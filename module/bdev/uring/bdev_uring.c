/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

#include "bdev_uring.h"

#include "spdk/stdinc.h"
#include "spdk/config.h"
#include "spdk/barrier.h"
#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/fd.h"
#include "spdk/likely.h"
#include "spdk/thread.h"
#include "spdk/json.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/file.h"

#include "spdk/log.h"
#include "spdk_internal/uring.h"

#include <linux/falloc.h>
#include <linux/loop.h>
#include <sys/sysmacros.h>

#ifdef SPDK_CONFIG_URING_ZNS
#include <linux/blkzoned.h>
#define SECTOR_SHIFT 9
#endif

#define URING_LOG_FMT "%s,uring:%p,filename:%s"
#define URING_LOG_ARGS(uring) \
  (uring)->bdev.name, \
  (uring), \
  (uring)->filename

#define URING_LOG(type, uring, format, ...) do { \
	SPDK_##type##LOG("["URING_LOG_FMT"] " format, URING_LOG_ARGS(uring), ##__VA_ARGS__); \
} while (0)

#define URING_LOG2(type, component, uring, format, ...) do { \
	SPDK_##type##LOG(component, "["URING_LOG_FMT"] " format, URING_LOG_ARGS(uring), ##__VA_ARGS__); \
} while (0)

#define URING_ERRLOG(uring, format, ...) URING_LOG(ERR, uring, format, ##__VA_ARGS__)
#define URING_WARNLOG(uring, format, ...) URING_LOG(WARN, uring, format, ##__VA_ARGS__)
#define URING_NOTICELOG(uring, format, ...) URING_LOG(NOTICE, uring, format, ##__VA_ARGS__)
#define URING_INFOLOG(uring, format, ...) URING_LOG2(INFO, uring, uring, format, ##__VA_ARGS__)

#ifdef DEBUG
#define URING_DEBUGLOG(uring, format, ...) URING_LOG2(DEBUG, uring, uring, format, ##__VA_ARGS__)
#else
#define URING_DEBUGLOG(...) do { } while (0)
#endif

struct bdev_uring_zoned_dev {
	uint64_t		num_zones;
	uint32_t		zone_shift;
	uint32_t		lba_shift;
};

struct bdev_uring_io_channel {
	struct bdev_uring_group_channel		*group_ch;
};

struct bdev_uring_group_channel {
	uint64_t				io_inflight;
	uint64_t				io_pending;
	struct spdk_poller			*poller;
	struct io_uring				uring;
	bool					detached;
};

struct bdev_uring_task {
	uint64_t			len;
	struct bdev_uring_io_channel	*ch;
	TAILQ_ENTRY(bdev_uring_task)	link;
};

struct bdev_uring {
	struct spdk_bdev	bdev;
	struct bdev_uring_zoned_dev	zd;
	char			*filename;
	int			fd;
	/* Whether UNMAP / WRITE_ZEROES are advertised, and (for UNMAP) whether the
	 * backing is a block device, so the syscall path can be chosen: files use
	 * IORING_OP_FALLOCATE(PUNCH_HOLE), block devices use BLOCK_URING_CMD_DISCARD. */
	bool			unmap;
	bool			zero;
	bool			is_blkdev;
	TAILQ_ENTRY(bdev_uring)  link;

	bool			hot_remove_in_progress;
};

static int bdev_uring_init(void);
static void bdev_uring_fini(void);
static void uring_free_bdev(struct bdev_uring *uring);
static TAILQ_HEAD(, bdev_uring) g_uring_bdev_head = TAILQ_HEAD_INITIALIZER(g_uring_bdev_head);

/* Kernel support for BLOCK_URING_CMD_DISCARD (Linux 6.12+): -1 unprobed, 0 no, 1 yes.
 * Probed at most once, on the first block-device uring bdev, never in the I/O path. */
static int g_uring_blk_discard = -1;

#define SPDK_URING_QUEUE_DEPTH 512
#define MAX_EVENTS_PER_POLL 32

static int
bdev_uring_get_ctx_size(void)
{
	return sizeof(struct bdev_uring_task);
}

static struct bdev_uring *
uring_from_bdev(struct spdk_bdev *bdev)
{
	return SPDK_CONTAINEROF(bdev, struct bdev_uring, bdev);
}

static struct spdk_bdev_module uring_if = {
	.name		= "uring",
	.module_init	= bdev_uring_init,
	.module_fini	= bdev_uring_fini,
	.get_ctx_size	= bdev_uring_get_ctx_size,
};

SPDK_BDEV_MODULE_REGISTER(uring, &uring_if)

static int
bdev_uring_open(struct bdev_uring *uring)
{
	int fd;

	fd = open(uring->filename, O_RDWR | O_DIRECT | O_NOATIME);
	if (fd < 0) {
		/* Try without O_DIRECT for non-disk files */
		fd = open(uring->filename, O_RDWR | O_NOATIME);
		if (fd < 0) {
			URING_ERRLOG(uring, "open() failed, rc %d: %s\n", fd, spdk_strerror(errno));
			uring->fd = -1;
			return -1;
		}
	}

	uring->fd = fd;

	return 0;
}

static void
bdev_uring_hot_remove(void *ctx)
{
	char *name = ctx;

	delete_uring_bdev(name, NULL, NULL);
	free(name);
}

static void
bdev_uring_try_hot_remove(struct bdev_uring *uring)
{
	char	*name;

	if (__atomic_test_and_set(&uring->hot_remove_in_progress, __ATOMIC_RELAXED)) {
		return;
	}

	name = strdup(uring->bdev.name);
	if (!name) {
		__atomic_clear(&uring->hot_remove_in_progress, __ATOMIC_RELAXED);
		return;
	}

	URING_ERRLOG(uring, "hot-remove detected, unregistering bdev...\n");
	spdk_thread_send_msg(spdk_thread_get_app_thread(), bdev_uring_hot_remove, name);
}

static void
dummy_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *ctx)
{
}

int
bdev_uring_rescan(const char *name)
{
	struct spdk_bdev_desc *desc;
	struct spdk_bdev *bdev;
	struct bdev_uring *uring;
	uint64_t uring_size, blockcnt;
	int rc;

	rc = spdk_bdev_open_ext(name, false, dummy_bdev_event_cb, NULL, &desc);
	if (rc != 0) {
		return rc;
	}

	bdev = spdk_bdev_desc_get_bdev(desc);
	if (bdev->module != &uring_if) {
		rc = -ENODEV;
		goto exit;
	}

	uring = uring_from_bdev(bdev);
	uring_size = spdk_fd_get_size(uring->fd);
	blockcnt = uring_size / bdev->blocklen;

	if (uring_size == 0) {
		bdev_uring_try_hot_remove(uring);
		goto exit;
	}

	if (bdev->blockcnt != blockcnt) {
		URING_NOTICELOG(uring, "URING device is resized: old block count %" PRIu64 ", new block count %"
				PRIu64 "\n",
				bdev->blockcnt,
				blockcnt);
		rc = spdk_bdev_notify_blockcnt_change(bdev, blockcnt);
		if (rc != 0) {
			URING_ERRLOG(uring, "Could not change num blocks, rc: %d\n", rc);
			goto exit;
		}
	}

exit:
	spdk_bdev_close(desc);
	return rc;
}

static int
bdev_uring_close(struct bdev_uring *uring)
{
	int rc;

	if (uring->fd == -1) {
		return 0;
	}

	rc = close(uring->fd);
	if (rc < 0) {
		URING_ERRLOG(uring, "close() failed (fd=%d), rc %d: %s\n",
			     uring->fd, rc, spdk_strerror(errno));
		return -1;
	}

	uring->fd = -1;

	return 0;
}

static int64_t
bdev_uring_readv(struct bdev_uring *uring, struct spdk_io_channel *ch,
		 struct bdev_uring_task *uring_task,
		 struct iovec *iov, int iovcnt, uint64_t nbytes, uint64_t offset)
{
	struct bdev_uring_io_channel *uring_ch = spdk_io_channel_get_ctx(ch);
	struct bdev_uring_group_channel *group_ch = uring_ch->group_ch;
	struct io_uring_sqe *sqe;

	sqe = io_uring_get_sqe(&group_ch->uring);
	if (!sqe) {
		URING_DEBUGLOG(uring, "get sqe failed as out of resource\n");
		return -ENOMEM;
	}

	io_uring_prep_readv(sqe, uring->fd, iov, iovcnt, offset);
	io_uring_sqe_set_data(sqe, uring_task);
	uring_task->len = nbytes;
	uring_task->ch = uring_ch;

	URING_DEBUGLOG(uring, "read %d iovs size %lu to off: %#lx\n", iovcnt, nbytes, offset);

	group_ch->io_pending++;
	return nbytes;
}

static int64_t
bdev_uring_writev(struct bdev_uring *uring, struct spdk_io_channel *ch,
		  struct bdev_uring_task *uring_task,
		  struct iovec *iov, int iovcnt, size_t nbytes, uint64_t offset)
{
	struct bdev_uring_io_channel *uring_ch = spdk_io_channel_get_ctx(ch);
	struct bdev_uring_group_channel *group_ch = uring_ch->group_ch;
	struct io_uring_sqe *sqe;

	sqe = io_uring_get_sqe(&group_ch->uring);
	if (!sqe) {
		URING_DEBUGLOG(uring, "get sqe failed as out of resource\n");
		return -ENOMEM;
	}

	io_uring_prep_writev(sqe, uring->fd, iov, iovcnt, offset);
	io_uring_sqe_set_data(sqe, uring_task);
	uring_task->len = nbytes;
	uring_task->ch = uring_ch;

	URING_DEBUGLOG(uring, "write %d iovs size %lu from off: %#lx\n", iovcnt, nbytes, offset);

	group_ch->io_pending++;
	return nbytes;
}

/*
 * Submit an async UNMAP or WRITE_ZEROES via IORING_OP_FALLOCATE on the existing
 * ring. io_uring is natively async, so no worker thread is needed and the
 * completion is reaped by bdev_uring_reap alongside reads and writes. fallocate
 * completes with res == 0 on success, so len is recorded as 0 for the reaper.
 */
static int64_t
bdev_uring_fallocate(struct bdev_uring *uring, struct spdk_io_channel *ch,
		     struct spdk_bdev_io *bdev_io, int mode)
{
	struct bdev_uring_task *uring_task = (struct bdev_uring_task *)bdev_io->driver_ctx;
	struct bdev_uring_io_channel *uring_ch = spdk_io_channel_get_ctx(ch);
	struct bdev_uring_group_channel *group_ch = uring_ch->group_ch;
	struct io_uring_sqe *sqe;

	sqe = io_uring_get_sqe(&group_ch->uring);
	if (!sqe) {
		URING_DEBUGLOG(uring, "get sqe failed as out of resource\n");
		return -ENOMEM;
	}

	io_uring_prep_fallocate(sqe, uring->fd, mode,
				bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen,
				bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
	io_uring_sqe_set_data(sqe, uring_task);
	uring_task->len = 0;
	uring_task->ch = uring_ch;

	group_ch->io_pending++;
	return 0;
}

/* Block-device UNMAP via BLOCK_URING_CMD_DISCARD (Linux 6.12+), reaped like the
 * others. Completes with res == 0 on success, so len is recorded as 0. */
static int64_t
bdev_uring_cmd_discard(struct bdev_uring *uring, struct spdk_io_channel *ch,
		       struct spdk_bdev_io *bdev_io)
{
	struct bdev_uring_task *uring_task = (struct bdev_uring_task *)bdev_io->driver_ctx;
	struct bdev_uring_io_channel *uring_ch = spdk_io_channel_get_ctx(ch);
	struct bdev_uring_group_channel *group_ch = uring_ch->group_ch;
	struct io_uring_sqe *sqe;

	sqe = io_uring_get_sqe(&group_ch->uring);
	if (!sqe) {
		URING_DEBUGLOG(uring, "get sqe failed as out of resource\n");
		return -ENOMEM;
	}

	io_uring_prep_cmd_discard(sqe, uring->fd,
				  bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen,
				  bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
	io_uring_sqe_set_data(sqe, uring_task);
	uring_task->len = 0;
	uring_task->ch = uring_ch;

	group_ch->io_pending++;
	return 0;
}

/*
 * Probe, at most once, whether this kernel supports BLOCK_URING_CMD_DISCARD
 * (6.12+). Support is a property of the kernel, not of any particular device
 * (every block device routes through the same blkdev_uring_cmd handler), so it
 * is checked on a disposable loop device rather than by issuing a destructive
 * discard on a real backing store. A zero-length discard cannot be used to
 * probe: the kernel rejects len == 0 with -EINVAL on every version, so only a
 * real, aligned discard returning 0 distinguishes a kernel that supports the
 * command (e.g. stable 6.12) from one that rejects it (observed on a
 * bleeding-edge io_uring development tree that reports a newer version). Cached
 * globally, so it runs only on the first block-device uring bdev, never in the
 * I/O path.
 */
static bool
bdev_uring_blk_discard_supported(void)
{
	char backing[] = "/tmp/spdk_uring_discard_probe.XXXXXX";
	char loopname[32];
	struct io_uring ring;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int lctl = -1, loopfd = -1, backfd = -1, devnr;
	bool ring_ok = false;

	if (g_uring_blk_discard >= 0) {
		return g_uring_blk_discard;
	}
	g_uring_blk_discard = 0;

	backfd = mkstemp(backing);
	if (backfd < 0) {
		SPDK_NOTICELOG("uring: discard probe: mkstemp failed (errno %d); "
			       "block-device discard disabled\n", errno);
		return false;
	}
	unlink(backing);				/* anonymous; freed on close */
	if (ftruncate(backfd, 1 << 20) != 0) {		/* 1 MiB backing store */
		goto out;
	}

	lctl = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
	if (lctl < 0) {
		SPDK_NOTICELOG("uring: discard probe: /dev/loop-control unavailable "
			       "(errno %d); block-device discard disabled\n", errno);
		goto out;
	}
	devnr = ioctl(lctl, LOOP_CTL_GET_FREE);
	if (devnr < 0) {
		goto out;
	}
	snprintf(loopname, sizeof(loopname), "/dev/loop%d", devnr);
	/* O_DIRECT is unsupported on some backing filesystems (e.g. tmpfs); the
	 * discard transfers no data, so plain buffered access probes identically. */
	loopfd = open(loopname, O_RDWR | O_CLOEXEC);
	if (loopfd < 0 || ioctl(loopfd, LOOP_SET_FD, backfd) != 0) {
		goto out;
	}

	if (io_uring_queue_init(1, &ring, 0) == 0) {
		ring_ok = true;
		sqe = io_uring_get_sqe(&ring);
		if (sqe != NULL) {
			/* real, aligned, single-block discard: the only submission a
			 * working kernel completes with res == 0 */
			io_uring_prep_cmd_discard(sqe, loopfd, 0, 512);
			if (io_uring_submit(&ring) == 1 &&
			    io_uring_wait_cqe(&ring, &cqe) == 0) {
				g_uring_blk_discard = (cqe->res == 0) ? 1 : 0;
				SPDK_NOTICELOG("uring: BLOCK_URING_CMD_DISCARD kernel probe "
					       "res=%d; block-device discard %s\n", cqe->res,
					       g_uring_blk_discard ? "enabled" : "disabled");
				io_uring_cqe_seen(&ring, cqe);
			}
		}
	}

out:
	if (ring_ok) {
		io_uring_queue_exit(&ring);
	}
	if (loopfd >= 0) {
		ioctl(loopfd, LOOP_CLR_FD, 0);
		close(loopfd);
	}
	if (lctl >= 0) {
		close(lctl);
	}
	if (backfd >= 0) {
		close(backfd);
	}
	return g_uring_blk_discard;
}

/*
 * Whether a specific block device advertises discard support, via
 * queue/discard_max_bytes. Non-destructive; gates per-device so UNMAP is never
 * advertised on a disk the kernel would reject with -EOPNOTSUPP.
 */
static bool
bdev_uring_dev_supports_discard(int fd)
{
	struct stat st;
	char path[64];
	char buf[32];
	int sfd;
	ssize_t n;

	if (fstat(fd, &st) != 0 || !S_ISBLK(st.st_mode)) {
		return false;
	}
	snprintf(path, sizeof(path), "/sys/dev/block/%u:%u/queue/discard_max_bytes",
		 major(st.st_rdev), minor(st.st_rdev));
	sfd = open(path, O_RDONLY);
	if (sfd < 0) {
		return false;
	}
	n = read(sfd, buf, sizeof(buf) - 1);
	close(sfd);
	if (n <= 0) {
		return false;
	}
	buf[n] = '\0';
	return strtoull(buf, NULL, 10) > 0;
}

static int
bdev_uring_destruct(void *ctx)
{
	struct bdev_uring *uring = ctx;
	int rc = 0;

	TAILQ_REMOVE(&g_uring_bdev_head, uring, link);
	rc = bdev_uring_close(uring);
	spdk_io_device_unregister(uring, NULL);
	uring_free_bdev(uring);
	return rc;
}

static int
bdev_uring_reap(struct bdev_uring_group_channel *group_ch, int max)
{
	int i, count, rc;
	struct io_uring_cqe *cqe;
	struct bdev_uring_task *uring_task;
	enum spdk_bdev_io_status status;
	struct spdk_bdev_io *bdev_io;
	struct bdev_uring *uring;
	struct io_uring *ring = &group_ch->uring;

	count = 0;
	for (i = 0; i < max; i++) {
		rc = io_uring_peek_cqe(ring, &cqe);
		if (rc != 0) {
			assert(rc == -EAGAIN || rc == -EWOULDBLOCK);
			return count;
		}

		assert(cqe != NULL);

		uring_task = (struct bdev_uring_task *)cqe->user_data;
		bdev_io = spdk_bdev_io_from_ctx(uring_task);
		rc = cqe->res;
		if (spdk_unlikely(rc != (signed)uring_task->len)) {
			uring = uring_from_bdev(bdev_io->bdev);

			/* Since spdk_fd_get_size is not cost-free, we prioritize the check for -EAGAIN/-EWOULDBLOCK
			 * as it's not likely that these errors are returned when a device is detached.
			 */
			if (rc == -EAGAIN || rc == -EWOULDBLOCK) {
				status = SPDK_BDEV_IO_STATUS_NOMEM;
			} else {
				/* When the block device device is detached from the system, IOs fail with different
				 * observed res such as 0, -EIO or -ENOSPC.
				 * In this case the ioctl BLKGETSIZE64 yields a device size of 0.
				 * Note that re-attaching the device will not correct this because the existing fd is
				 * still invalid.
				 */
				if (!group_ch->detached && spdk_fd_get_size(uring->fd) == 0) {
					group_ch->detached = true;
				}

				if (group_ch->detached) {
					bdev_uring_try_hot_remove(uring);
				} else {
					URING_ERRLOG(uring, "I/O failed with error %d\n", rc);
				}
				status = SPDK_BDEV_IO_STATUS_FAILED;
			}
		} else {
			status = SPDK_BDEV_IO_STATUS_SUCCESS;
		}

		uring_task->ch->group_ch->io_inflight--;
		io_uring_cqe_seen(ring, cqe);
		spdk_bdev_io_complete(bdev_io, status);
		count++;
	}

	return count;
}

static int
bdev_uring_group_poll(void *arg)
{
	struct bdev_uring_group_channel *group_ch = arg;
	int to_complete, to_submit;
	int count, ret;

	to_submit = group_ch->io_pending;

	if (to_submit > 0) {
		/* If there are I/O to submit, use io_uring_submit here.
		 * It will automatically call spdk_io_uring_enter appropriately. */
		ret = io_uring_submit(&group_ch->uring);
		if (ret < 0) {
			return SPDK_POLLER_BUSY;
		}

		group_ch->io_pending = 0;
		group_ch->io_inflight += to_submit;
	}

	to_complete = group_ch->io_inflight;
	count = 0;
	if (to_complete > 0) {
		count = bdev_uring_reap(group_ch, to_complete);
	}

	if (count + to_submit > 0) {
		return SPDK_POLLER_BUSY;
	} else {
		return SPDK_POLLER_IDLE;
	}
}

static void
bdev_uring_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		      bool success)
{
	int64_t ret = 0;
	struct bdev_uring *uring = uring_from_bdev(bdev_io->bdev);

	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		ret = bdev_uring_readv(uring,
				       ch,
				       (struct bdev_uring_task *)bdev_io->driver_ctx,
				       bdev_io->u.bdev.iovs,
				       bdev_io->u.bdev.iovcnt,
				       bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
				       bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		ret = bdev_uring_writev(uring,
					ch,
					(struct bdev_uring_task *)bdev_io->driver_ctx,
					bdev_io->u.bdev.iovs,
					bdev_io->u.bdev.iovcnt,
					bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
					bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen);
		break;
	default:
		URING_ERRLOG(uring, "Wrong io type\n");
		break;
	}

	if (ret == -ENOMEM) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
	}
}

#ifdef SPDK_CONFIG_URING_ZNS
static int
bdev_uring_fill_zone_type(struct bdev_uring *uring, struct spdk_bdev_zone_info *zone_info,
			  struct blk_zone *zones_rep)
{
	switch (zones_rep->type) {
	case BLK_ZONE_TYPE_CONVENTIONAL:
		zone_info->type = SPDK_BDEV_ZONE_TYPE_CNV;
		break;
	case BLK_ZONE_TYPE_SEQWRITE_REQ:
		zone_info->type = SPDK_BDEV_ZONE_TYPE_SEQWR;
		break;
	case BLK_ZONE_TYPE_SEQWRITE_PREF:
		zone_info->type = SPDK_BDEV_ZONE_TYPE_SEQWP;
		break;
	default:
		URING_ERRLOG(uring, "Invalid zone type: %#x in zone report\n", zones_rep->type);
		return -EIO;
	}
	return 0;
}

static int
bdev_uring_fill_zone_state(struct bdev_uring *uring, struct spdk_bdev_zone_info *zone_info,
			   struct blk_zone *zones_rep)
{
	switch (zones_rep->cond) {
	case BLK_ZONE_COND_EMPTY:
		zone_info->state = SPDK_BDEV_ZONE_STATE_EMPTY;
		break;
	case BLK_ZONE_COND_IMP_OPEN:
		zone_info->state = SPDK_BDEV_ZONE_STATE_IMP_OPEN;
		break;
	case BLK_ZONE_COND_EXP_OPEN:
		zone_info->state = SPDK_BDEV_ZONE_STATE_EXP_OPEN;
		break;
	case BLK_ZONE_COND_CLOSED:
		zone_info->state = SPDK_BDEV_ZONE_STATE_CLOSED;
		break;
	case BLK_ZONE_COND_READONLY:
		zone_info->state = SPDK_BDEV_ZONE_STATE_READ_ONLY;
		break;
	case BLK_ZONE_COND_FULL:
		zone_info->state = SPDK_BDEV_ZONE_STATE_FULL;
		break;
	case BLK_ZONE_COND_OFFLINE:
		zone_info->state = SPDK_BDEV_ZONE_STATE_OFFLINE;
		break;
	case BLK_ZONE_COND_NOT_WP:
		zone_info->state = SPDK_BDEV_ZONE_STATE_NOT_WP;
		break;
	default:
		URING_ERRLOG(uring, "Invalid zone state: %#x in zone report\n", zones_rep->cond);
		return -EIO;
	}
	return 0;
}

static int
bdev_uring_zone_management_op(struct spdk_bdev_io *bdev_io)
{
	struct bdev_uring *uring;
	struct blk_zone_range range;
	long unsigned zone_mgmt_op;
	uint64_t zone_id = bdev_io->u.zone_mgmt.zone_id;

	uring = uring_from_bdev(bdev_io->bdev);

	switch (bdev_io->u.zone_mgmt.zone_action) {
	case SPDK_BDEV_ZONE_RESET:
		zone_mgmt_op = BLKRESETZONE;
		break;
	case SPDK_BDEV_ZONE_OPEN:
		zone_mgmt_op = BLKOPENZONE;
		break;
	case SPDK_BDEV_ZONE_CLOSE:
		zone_mgmt_op = BLKCLOSEZONE;
		break;
	case SPDK_BDEV_ZONE_FINISH:
		zone_mgmt_op = BLKFINISHZONE;
		break;
	default:
		return -EINVAL;
	}

	range.sector = (zone_id << uring->zd.lba_shift);
	range.nr_sectors = (uring->bdev.zone_size << uring->zd.lba_shift);

	if (ioctl(uring->fd, zone_mgmt_op, &range)) {
		URING_ERRLOG(uring, "Ioctl BLKXXXZONE(%#x) failed errno: %d(%s)\n",
			     bdev_io->u.zone_mgmt.zone_action, errno, strerror(errno));
		return -EINVAL;
	}

	spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);

	return 0;
}

static int
bdev_uring_zone_get_info(struct spdk_bdev_io *bdev_io)
{
	struct bdev_uring *uring;
	struct blk_zone *zones;
	struct blk_zone_report *rep;
	struct spdk_bdev_zone_info *zone_info = bdev_io->u.zone_mgmt.buf;
	size_t repsize;
	uint32_t i, shift;
	uint32_t num_zones = bdev_io->u.zone_mgmt.num_zones;
	uint64_t zone_id = bdev_io->u.zone_mgmt.zone_id;

	uring = uring_from_bdev(bdev_io->bdev);
	shift = uring->zd.lba_shift;

	if ((num_zones > uring->zd.num_zones) || !num_zones) {
		return -EINVAL;
	}

	repsize = sizeof(struct blk_zone_report) + (sizeof(struct blk_zone) * num_zones);
	rep = (struct blk_zone_report *)malloc(repsize);
	if (!rep) {
		return -ENOMEM;
	}

	zones = (struct blk_zone *)(rep + 1);

	while (num_zones && ((zone_id >> uring->zd.zone_shift) <= num_zones)) {
		memset(rep, 0, repsize);
		rep->sector = zone_id;
		rep->nr_zones = num_zones;

		if (ioctl(uring->fd, BLKREPORTZONE, rep)) {
			URING_ERRLOG(uring, "Ioctl BLKREPORTZONE failed errno: %d(%s)\n",
				     errno, strerror(errno));
			free(rep);
			return -EINVAL;
		}

		if (!rep->nr_zones) {
			break;
		}

		for (i = 0; i < rep->nr_zones; i++) {
			zone_info->zone_id = ((zones + i)->start >> shift);
			zone_info->write_pointer = ((zones + i)->wp >> shift);
			zone_info->capacity = ((zones + i)->capacity >> shift);

			bdev_uring_fill_zone_state(uring, zone_info, zones + i);
			bdev_uring_fill_zone_type(uring, zone_info, zones + i);

			zone_id = ((zones + i)->start + (zones + i)->len) >> shift;
			zone_info++;
			num_zones--;
		}
	}

	spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	free(rep);
	return 0;
}

static int
bdev_uring_check_zoned_support(struct bdev_uring *uring, const char *name, const char *filename)
{
	char *filename_dup = NULL, *base;
	char *str = NULL;
	uint32_t val;
	uint32_t zinfo;
	int retval = -1;
	struct stat sb;
	char resolved_path[PATH_MAX], *rp;
	char *sysfs_path = NULL;

	uring->bdev.zoned = false;

	/* Follow symlink */
	if ((rp = realpath(filename, resolved_path))) {
		filename = rp;
	}

	/* Perform check on block devices only */
	if (stat(filename, &sb) == 0 && S_ISBLK(sb.st_mode)) {
		return 0;
	}

	/* strdup() because basename() may modify the passed parameter */
	filename_dup = strdup(filename);
	if (filename_dup == NULL) {
		URING_ERRLOG(uring, "Could not duplicate string %s\n", filename);
		return -1;
	}

	base = basename(filename_dup);
	free(filename_dup);
	sysfs_path = spdk_sprintf_alloc("/sys/block/%s/queue/zoned", base);
	retval = spdk_read_sysfs_attribute(&str, "%s", sysfs_path);
	/* Check if this is a zoned block device */
	if (retval < 0) {
		URING_ERRLOG(uring, "Unable to open file %s. errno: %d\n", sysfs_path, retval);
	} else if (strcmp(str, "host-aware") == 0 || strcmp(str, "host-managed") == 0) {
		/* Only host-aware & host-managed zns devices */
		uring->bdev.zoned = true;

		if (ioctl(uring->fd, BLKGETNRZONES, &zinfo)) {
			URING_ERRLOG(uring, "ioctl BLKNRZONES failed %d (%s)\n", errno, strerror(errno));
			goto err_ret;
		}
		uring->zd.num_zones = zinfo;

		if (ioctl(uring->fd, BLKGETZONESZ, &zinfo)) {
			URING_ERRLOG(uring, "ioctl BLKGETZONESZ failed %d (%s)\n", errno, strerror(errno));
			goto err_ret;
		}

		uring->zd.lba_shift = uring->bdev.required_alignment - SECTOR_SHIFT;
		uring->bdev.zone_size = (zinfo >> uring->zd.lba_shift);
		uring->zd.zone_shift = spdk_u32log2(zinfo >> uring->zd.lba_shift);

		retval = spdk_read_sysfs_attribute_uint32(&val, "/sys/block/%s/queue/max_open_zones", base);
		if (retval < 0) {
			URING_ERRLOG(uring, "Failed to get max open zones %d (%s)\n", retval, strerror(-retval));
			goto err_ret;
		}
		uring->bdev.max_open_zones = uring->bdev.optimal_open_zones = val;

		retval = spdk_read_sysfs_attribute_uint32(&val, "/sys/block/%s/queue/max_active_zones", base);
		if (retval < 0) {
			URING_ERRLOG(uring, "Failed to get max active zones %d (%s)\n", retval, strerror(-retval));
			goto err_ret;
		}
		uring->bdev.max_active_zones = val;
		retval = 0;
	} else {
		retval = 0;        /* queue/zoned=none */
	}
err_ret:
	free(str);
	free(sysfs_path);
	return retval;
}
#else
/* No support for zoned devices */
static int
bdev_uring_zone_management_op(struct spdk_bdev_io *bdev_io)
{
	return -1;
}

static int
bdev_uring_zone_get_info(struct spdk_bdev_io *bdev_io)
{
	return -1;
}

static int
bdev_uring_check_zoned_support(struct bdev_uring *uring, const char *name, const char *filename)
{
	return 0;
}
#endif

static int
_bdev_uring_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct bdev_uring *uring = uring_from_bdev(bdev_io->bdev);

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_GET_ZONE_INFO:
		return bdev_uring_zone_get_info(bdev_io);
	case SPDK_BDEV_IO_TYPE_ZONE_MANAGEMENT:
		return bdev_uring_zone_management_op(bdev_io);
	/* Read and write operations must be performed on buffers aligned to
	 * bdev->required_alignment. If user specified unaligned buffers,
	 * get the aligned buffer from the pool by calling spdk_bdev_io_get_buf. */
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		spdk_bdev_io_get_buf(bdev_io, bdev_uring_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		return 0;
	case SPDK_BDEV_IO_TYPE_UNMAP: {
		int64_t rc = uring->is_blkdev ?
			     bdev_uring_cmd_discard(uring, ch, bdev_io) :
			     bdev_uring_fallocate(uring, ch, bdev_io,
						  FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE);
		if (rc == -ENOMEM) {
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
		}
		return 0;
	}
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		if (bdev_uring_fallocate(uring, ch, bdev_io, FALLOC_FL_ZERO_RANGE) == -ENOMEM) {
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
		}
		return 0;
	default:
		return -1;
	}
}

static void
bdev_uring_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	if (_bdev_uring_submit_request(ch, bdev_io) < 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static bool
bdev_uring_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
#ifdef SPDK_CONFIG_URING_ZNS
	case SPDK_BDEV_IO_TYPE_GET_ZONE_INFO:
	case SPDK_BDEV_IO_TYPE_ZONE_MANAGEMENT:
#endif
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		return true;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		return ((struct bdev_uring *)ctx)->unmap;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		return ((struct bdev_uring *)ctx)->zero;
	default:
		return false;
	}
}

static int
bdev_uring_create_cb(void *io_device, void *ctx_buf)
{
	struct bdev_uring_io_channel *ch = ctx_buf;

	ch->group_ch = spdk_io_channel_get_ctx(spdk_get_io_channel(&uring_if));

	return 0;
}

static void
bdev_uring_destroy_cb(void *io_device, void *ctx_buf)
{
	struct bdev_uring_io_channel *ch = ctx_buf;

	spdk_put_io_channel(spdk_io_channel_from_ctx(ch->group_ch));
}

static struct spdk_io_channel *
bdev_uring_get_io_channel(void *ctx)
{
	struct bdev_uring *uring = ctx;

	return spdk_get_io_channel(uring);
}

static int
bdev_uring_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct bdev_uring *uring = ctx;

	spdk_json_write_named_object_begin(w, "uring");

	spdk_json_write_named_string(w, "filename", uring->filename);

	spdk_json_write_object_end(w);

	return 0;
}

static void
bdev_uring_write_json_config(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct bdev_uring *uring = uring_from_bdev(bdev);
	char uuid_str[SPDK_UUID_STRING_LEN];

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_uring_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	spdk_json_write_named_uint32(w, "block_size", bdev->blocklen);
	spdk_json_write_named_string(w, "filename", uring->filename);
	spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &bdev->uuid);
	spdk_json_write_named_string(w, "uuid", uuid_str);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static const struct spdk_bdev_fn_table uring_fn_table = {
	.destruct		= bdev_uring_destruct,
	.submit_request		= bdev_uring_submit_request,
	.io_type_supported	= bdev_uring_io_type_supported,
	.get_io_channel		= bdev_uring_get_io_channel,
	.dump_info_json		= bdev_uring_dump_info_json,
	.write_config_json	= bdev_uring_write_json_config,
};

static void
uring_free_bdev(struct bdev_uring *uring)
{
	if (uring == NULL) {
		return;
	}
	free(uring->filename);
	free(uring->bdev.name);
	free(uring);
}

static int
bdev_uring_group_create_cb(void *io_device, void *ctx_buf)
{
	struct bdev_uring_group_channel *ch = ctx_buf;

	/* Do not use IORING_SETUP_IOPOLL until the Linux kernel can support not only
	 * local devices but also devices attached from remote target */
	if (io_uring_queue_init(SPDK_URING_QUEUE_DEPTH, &ch->uring, 0) < 0) {
		SPDK_ERRLOG("uring I/O context setup failure\n");
		return -1;
	}

	ch->poller = SPDK_POLLER_REGISTER(bdev_uring_group_poll, ch, 0);
	return 0;
}

static void
bdev_uring_group_destroy_cb(void *io_device, void *ctx_buf)
{
	struct bdev_uring_group_channel *ch = ctx_buf;

	io_uring_queue_exit(&ch->uring);

	spdk_poller_unregister(&ch->poller);
}

struct spdk_bdev *
create_uring_bdev(const struct bdev_uring_opts *opts)
{
	struct bdev_uring *uring;
	uint32_t detected_block_size;
	uint64_t bdev_size;
	int rc;
	uint32_t block_size = opts->block_size;
	struct stat st;

	uring = calloc(1, sizeof(*uring));
	if (!uring) {
		SPDK_ERRLOG("Unable to allocate enough memory for uring backend\n");
		return NULL;
	}

	uring->filename = strdup(opts->filename);
	if (!uring->filename) {
		goto error_return;
	}

	uring->bdev.name = strdup(opts->name);
	if (!uring->bdev.name) {
		goto error_return;
	}

	if (bdev_uring_open(uring)) {
		goto error_return;
	}

	/* Files: UNMAP/WRITE_ZEROES via fallocate (punch-hole / zero-range), always
	 * available above the 5.13 uring floor. Block devices: UNMAP via
	 * BLOCK_URING_CMD_DISCARD when the kernel supports it (probed once); there is
	 * no io_uring write-zeroes op for block devices, so that stays file-only. */
	if (fstat(uring->fd, &st) == 0) {
		if (S_ISREG(st.st_mode)) {
			uring->unmap = true;
			uring->zero = true;
		} else if (S_ISBLK(st.st_mode)) {
			uring->is_blkdev = true;
			uring->unmap = bdev_uring_blk_discard_supported() &&
				       bdev_uring_dev_supports_discard(uring->fd);
		}
	}

	bdev_size = spdk_fd_get_size(uring->fd);

	uring->bdev.product_name = "URING bdev";
	uring->bdev.module = &uring_if;

	uring->bdev.write_cache = 0;

	detected_block_size = spdk_fd_get_blocklen(uring->fd);
	if (block_size == 0) {
		/* User did not specify block size - use autodetected block size. */
		if (detected_block_size == 0) {
			URING_ERRLOG(uring, "Block size could not be auto-detected\n");
			goto error_return;
		}
		block_size = detected_block_size;
	} else {
		if (block_size < detected_block_size) {
			URING_ERRLOG(uring, "Specified block size %" PRIu32 " is smaller than "
				     "auto-detected block size %" PRIu32 "\n",
				     block_size, detected_block_size);
			goto error_return;
		} else if (detected_block_size != 0 && block_size != detected_block_size) {
			URING_WARNLOG(uring, "Specified block size %" PRIu32 " does not match "
				      "auto-detected block size %" PRIu32 "\n",
				      block_size, detected_block_size);
		}
	}

	if (block_size < 512) {
		URING_ERRLOG(uring, "Invalid block size %" PRIu32 " (must be at least 512).\n", block_size);
		goto error_return;
	}

	if (!spdk_u32_is_pow2(block_size)) {
		URING_ERRLOG(uring, "Invalid block size %" PRIu32 " (must be a power of 2.)\n", block_size);
		goto error_return;
	}

	uring->bdev.blocklen = block_size;
	uring->bdev.required_alignment = spdk_u32log2(block_size);

	rc = bdev_uring_check_zoned_support(uring, opts->name, opts->filename);
	if (rc) {
		goto error_return;
	}

	if (bdev_size % uring->bdev.blocklen != 0) {
		URING_ERRLOG(uring, "Disk size %" PRIu64 " is not a multiple of block size %" PRIu32 "\n",
			     bdev_size, uring->bdev.blocklen);
		goto error_return;
	}

	uring->bdev.blockcnt = bdev_size / uring->bdev.blocklen;
	uring->bdev.ctxt = uring;

	uring->bdev.fn_table = &uring_fn_table;

	if (!spdk_mem_all_zero(&opts->uuid, sizeof(opts->uuid))) {
		spdk_uuid_copy(&uring->bdev.uuid, &opts->uuid);
	}

	spdk_io_device_register(uring, bdev_uring_create_cb, bdev_uring_destroy_cb,
				sizeof(struct bdev_uring_io_channel),
				uring->bdev.name);
	rc = spdk_bdev_register(&uring->bdev);
	if (rc) {
		spdk_io_device_unregister(uring, NULL);
		goto error_return;
	}

	TAILQ_INSERT_TAIL(&g_uring_bdev_head, uring, link);
	return &uring->bdev;

error_return:
	bdev_uring_close(uring);
	uring_free_bdev(uring);
	return NULL;
}

struct delete_uring_bdev_ctx {
	spdk_delete_uring_complete cb_fn;
	void *cb_arg;
};

static void
uring_bdev_unregister_cb(void *arg, int bdeverrno)
{
	struct delete_uring_bdev_ctx *ctx = arg;

	if (ctx->cb_fn) {
		ctx->cb_fn(ctx->cb_arg, bdeverrno);
	}
	free(ctx);
}

void
delete_uring_bdev(const char *name, spdk_delete_uring_complete cb_fn, void *cb_arg)
{
	struct delete_uring_bdev_ctx *ctx;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		if (cb_fn) {
			cb_fn(cb_arg, -ENOMEM);
		}
		return;
	}

	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	rc = spdk_bdev_unregister_by_name(name, &uring_if, uring_bdev_unregister_cb, ctx);
	if (rc != 0) {
		uring_bdev_unregister_cb(ctx, rc);
	}
}

static int
bdev_uring_init(void)
{
	spdk_io_device_register(&uring_if, bdev_uring_group_create_cb, bdev_uring_group_destroy_cb,
				sizeof(struct bdev_uring_group_channel), "uring_module");

	return 0;
}

static void
bdev_uring_fini(void)
{
	spdk_io_device_unregister(&uring_if, NULL);
}

SPDK_LOG_REGISTER_COMPONENT(uring)
