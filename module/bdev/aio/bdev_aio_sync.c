#include "bdev_aio.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/bdev_module.h"

#include <libaio.h>

#include "bdev_aio_task.h"
#include "bdev_aio_sync.h"

static pthread_t g_blocking_worker_thread;
static pthread_mutex_t g_mutex;
static pthread_cond_t g_cond;
static bool g_exit;

#define MAX_QUEUE_LEN 1024

struct queue {
	void *message[MAX_QUEUE_LEN];
	size_t used;
};

static struct queue g_queue;

static void
_init_queue(void)
{
	g_queue.used = 0;
}

static int
_enqueue(void *message)
{
	if (g_queue.used < MAX_QUEUE_LEN) {
		if (g_queue.used == 0) {
			pthread_cond_signal(&g_cond);
		}

		g_queue.message[g_queue.used++] = message;

		return 0;
	}

	return -1;
}

static size_t
_dequeue(void *message[], size_t n)
{
	size_t i;

	if (g_queue.used > n) {
		for (i = 0; i < n; i++) {
			message[i] = g_queue.message[i];
		}

		g_queue.used -= n;

		for (i = 0; i < g_queue.used; i++) {
			g_queue.message[i] = g_queue.message[n + i];
		}

		return n;
	}

	if (g_queue.used > 0) {
		n = g_queue.used;
		g_queue.used = 0;

		for (i = 0; i < n; i++) {
			message[i] = g_queue.message[i];
		}

		return n;
	}

	return 0;
}

static void
aio_complete(struct spdk_bdev_io *bdev_io, int status)
{
	spdk_bdev_io_complete(bdev_io,
			      (status == 0) ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED);
}

static void
aio_call_complete_fn(void *arg)
{
	struct aio_request_ctx *request = arg;

	aio_complete(spdk_bdev_io_from_ctx(request->aio_task), request->status);

	free(arg);
}

static void
aio_call_request_fn(void *arg)
{
	struct aio_request_ctx *request = arg;

	request->status = request->fn(request->ctx);

	spdk_set_thread(request->thread);

	spdk_thread_send_msg(request->thread, aio_call_complete_fn, request);

	spdk_set_thread(NULL);
}

#define BATCH_SIZE 64

static void *
aio_blocking_worker(void *arg)
{
	void *message[BATCH_SIZE];
	size_t count;
	size_t i;

	pthread_mutex_lock(&g_mutex);

	for (;;) {
		for (;;) {
			count = _dequeue(message, BATCH_SIZE);

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
	int status;

	pthread_mutex_lock(&g_mutex);

	status = _enqueue(message);

	pthread_mutex_unlock(&g_mutex);

	return status;
}

void
aio_sync_init(void)
{
	_init_queue();

	g_exit = false;

	pthread_mutex_init(&g_mutex, NULL);

	pthread_cond_init(&g_cond, NULL);

	pthread_create(&g_blocking_worker_thread, NULL, aio_blocking_worker, NULL);
}

void
aio_sync_fini(void)
{
	g_exit = true;

	pthread_cond_signal(&g_cond);

	pthread_join(g_blocking_worker_thread, NULL);

	pthread_cond_destroy(&g_cond);

	pthread_mutex_destroy(&g_mutex);
}
