#include "bdev_aio.h"
#include "spdk/env.h"
#include "spdk/stdinc.h"
#include "spdk/thread.h"
#include "spdk/bdev_module.h"
#include "spdk/log.h"

#include <libaio.h>

#include "bdev_aio_task.h"
#include "bdev_aio_sync.h"

#define MAX_QUEUE_LEN 1024

static struct spdk_ring *g_blocking_ring = NULL;
static pthread_t g_blocking_worker_thread;
static pthread_mutex_t g_mutex;
static pthread_cond_t g_cond;
static bool g_exit;

static void
aio_complete(struct spdk_bdev_io *bdev_io, int status, int aio_errno)
{
	if (status == 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	} else {
		spdk_bdev_io_complete_aio_status(bdev_io, -aio_errno);
	}
}

static void
aio_call_complete_fn(void *arg)
{
	struct aio_request_ctx *request = arg;

	aio_complete(spdk_bdev_io_from_ctx(request->aio_task), request->status, request->aio_errno);

	free(arg);
}

static void
aio_call_request_fn(void *arg)
{
	struct aio_request_ctx *request = arg;

	request->status = request->fn(request->ctx);
	request->aio_errno = errno;

	spdk_thread_send_msg(request->thread, aio_call_complete_fn, request);
}

#define BATCH_SIZE 64

static void *
aio_blocking_worker(void *arg)
{
	void *message[BATCH_SIZE];
	size_t count;
	size_t i;

	SPDK_NOTICELOG("aio_blocking_worker started on core:%d\n", sched_getcpu());

	pthread_mutex_lock(&g_mutex);

	for (;;) {
		for (;;) {
			count = spdk_ring_dequeue(g_blocking_ring, message, BATCH_SIZE);

			if (count == 0) {
				break;
			}

			pthread_mutex_unlock(&g_mutex);

			for (i = 0; i < count; i++) {
				aio_call_request_fn(message[i]);
			}

			pthread_mutex_lock(&g_mutex);
		}

		if (g_exit) {
			break;
		}

		pthread_cond_wait(&g_cond, &g_mutex);
	}

	pthread_mutex_unlock(&g_mutex);

	return NULL;
}

int
aio_send_request(void *message)
{
	size_t count = spdk_ring_count(g_blocking_ring);

	if (spdk_ring_enqueue(g_blocking_ring, &message, 1, NULL) == 0) {
		return -1;
	}

	if (count == 0) {
		/* wake up worker thread */
		pthread_mutex_lock(&g_mutex);
		pthread_cond_signal(&g_cond);
		pthread_mutex_unlock(&g_mutex);
	}

	return 0;
}

static
void _set_affinity(pthread_attr_t *attr)
{
	cpu_set_t cpuset;
	unsigned i, cores;

	CPU_ZERO(&cpuset);

	cores = sysconf(_SC_NPROCESSORS_CONF);

	for (i = 0; i < cores; i++) {
		CPU_SET(i, &cpuset);
	}

	SPDK_ENV_FOREACH_CORE(i) {
		CPU_CLR(i, &cpuset);
	}

	if (CPU_COUNT(&cpuset) > 0) {
		pthread_attr_setaffinity_np(attr, sizeof(cpu_set_t), &cpuset);
	}
}

int
aio_sync_init(void)
{
	pthread_attr_t attr;

	g_blocking_ring = spdk_ring_create(SPDK_RING_TYPE_MP_SC, MAX_QUEUE_LEN, SPDK_ENV_SOCKET_ID_ANY);

	if (g_blocking_ring == NULL) {
		return -1;
	}

	g_exit = false;

	pthread_mutex_init(&g_mutex, NULL);

	pthread_cond_init(&g_cond, NULL);

	pthread_attr_init(&attr);

	_set_affinity(&attr);

	pthread_create(&g_blocking_worker_thread, &attr, aio_blocking_worker, NULL);

	pthread_attr_destroy(&attr);

	return 0;
}

void
aio_sync_fini(void)
{
	pthread_mutex_lock(&g_mutex);
	g_exit = true;
	pthread_cond_signal(&g_cond);
	pthread_mutex_unlock(&g_mutex);

	pthread_join(g_blocking_worker_thread, NULL);

	spdk_ring_free(g_blocking_ring);

	pthread_cond_destroy(&g_cond);

	pthread_mutex_destroy(&g_mutex);
}
