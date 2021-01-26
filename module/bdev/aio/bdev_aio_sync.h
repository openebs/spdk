
typedef int (*aio_request_fn)(void *arg);

struct aio_request_ctx {
	struct bdev_aio_task	*aio_task;
	struct spdk_thread	*thread;
	aio_request_fn		fn;
	int			status;
	int			aio_errno;
	char			ctx[0];
};

struct aio_flush_ctx {
	struct file_disk	*fdisk;
};

struct aio_range_ctx {
	struct file_disk	*fdisk;
	uint64_t		offset_blocks;
	uint64_t		num_blocks;
	uint32_t		blocklen;
};

int aio_send_request(void *message);

int aio_sync_init(void);
void aio_sync_fini(void);
