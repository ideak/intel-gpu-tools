// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include "igt.h"
#include "igt_map.h"
#include "intel_allocator.h"
#include "intel_allocator_msgchannel.h"

//#define ALLOCDBG
#ifdef ALLOCDBG
#define alloc_info igt_info
#define alloc_debug igt_debug
static const char *reqtype_str[] = {
	[REQ_STOP]		= "stop",
	[REQ_OPEN]		= "open",
	[REQ_OPEN_AS]		= "open as",
	[REQ_CLOSE]		= "close",
	[REQ_ADDRESS_RANGE]	= "address range",
	[REQ_ALLOC]		= "alloc",
	[REQ_FREE]		= "free",
	[REQ_IS_ALLOCATED]	= "is allocated",
	[REQ_RESERVE]		= "reserve",
	[REQ_UNRESERVE]		= "unreserve",
	[REQ_RESERVE_IF_NOT_ALLOCATED] = "reserve-ina",
	[REQ_IS_RESERVED]	= "is reserved",
};
static inline const char *reqstr(enum reqtype request_type)
{
	igt_assert(request_type >= REQ_STOP && request_type <= REQ_IS_RESERVED);
	return reqtype_str[request_type];
}
#else
#define alloc_info(...) {}
#define alloc_debug(...) {}
#endif

struct allocator {
	int fd;
	uint32_t ctx;
	uint32_t vm;
	_Atomic(int32_t) refcount;
	struct intel_allocator *ial;
};

struct handle_entry {
	uint64_t handle;
	struct allocator *al;
};

struct intel_allocator *intel_allocator_reloc_create(int fd);
struct intel_allocator *intel_allocator_random_create(int fd);
struct intel_allocator *intel_allocator_simple_create(int fd);
struct intel_allocator *
intel_allocator_simple_create_full(int fd, uint64_t start, uint64_t end,
				   enum allocator_strategy strategy);

/*
 * Instead of trying to find first empty handle just get new one. Assuming
 * our counter is incremented 2^32 times per second (4GHz clock and handle
 * assignment takes single clock) 64-bit counter would wrap around after
 * ~68 years.
 *
 *                   allocator
 * handles           <fd, ctx>           intel allocator
 * +-----+           +--------+          +-------------+
 * |  1  +---------->+  fd: 3 +--------->+ data: ...   |
 * +-----+     +---->+ ctx: 1 |          | refcount: 2 |
 * |  2  +-----+     | ref: 2 |          +-------------+
 * +-----+           +--------+
 * |  3  +--+        +--------+          intel allocator
 * +-----+  |        |  fd: 3 |          +-------------+
 * | ... |  +------->| ctx: 2 +--------->+ data: ...   |
 * +-----+           | ref: 1 |          | refcount: 1 |
 * |  n  +--------+  +--------+          +-------------+
 * +-----+        |
 * | ... +-----+  |  allocator
 * +-----+     |  |  <fd, vm>            intel allocator
 * | ... +--+  |  |  +--------+          +-------------+
 * +     +  |  |  +->+  fd: 3 +-----+--->+ data: ...   |
 *          |  +---->+  vm: 1 |     |    | refcount: 3 |
 *          |        | ref: 2 |     |    +-------------+
 *          |        +--------+     |
 *          |        +--------+     |
 *          |        |  fd: 3 |     |
 *          +------->+  vm: 2 +-----+
 *                   | ref: 1 |
 *                   +--------+
 */
static _Atomic(uint64_t) next_handle;
static struct igt_map *handles;
static struct igt_map *ctx_map;
static struct igt_map *vm_map;
static pthread_mutex_t map_mutex = PTHREAD_MUTEX_INITIALIZER;
#define GET_MAP(vm) ((vm) ? vm_map : ctx_map)

static bool multiprocess;
static pthread_t allocator_thread;
static bool allocator_thread_running;

static bool warn_if_not_empty;

/* For allocator purposes we need to track pid/tid */
static pid_t allocator_pid = -1;
extern pid_t child_pid;
extern __thread pid_t child_tid;

/*
 * - for parent process we have child_pid == -1
 * - for child which calls intel_allocator_init() allocator_pid == child_pid
 */
static inline bool is_same_process(void)
{
	return child_pid == -1 || allocator_pid == child_pid;
}

static struct msg_channel *channel;

static int send_alloc_stop(struct msg_channel *msgchan)
{
	struct alloc_req req = {0};

	req.request_type = REQ_STOP;

	return msgchan->send_req(msgchan, &req);
}

static int send_req(struct msg_channel *msgchan, pid_t tid,
		    struct alloc_req *request)
{
	request->tid = tid;
	return msgchan->send_req(msgchan, request);
}

static int recv_req(struct msg_channel *msgchan, struct alloc_req *request)
{
	return msgchan->recv_req(msgchan, request);
}

static int send_resp(struct msg_channel *msgchan,
		     pid_t tid, struct alloc_resp *response)
{
	response->tid = tid;
	return msgchan->send_resp(msgchan, response);
}

static int recv_resp(struct msg_channel *msgchan,
		     pid_t tid, struct alloc_resp *response)
{
	response->tid = tid;
	return msgchan->recv_resp(msgchan, response);
}

static inline void map_entry_free_func(struct igt_map_entry *entry)
{
	free(entry->data);
}

static uint64_t __handle_create(struct allocator *al)
{
	struct handle_entry *h = malloc(sizeof(*h));

	igt_assert(h);
	h->handle = atomic_fetch_add(&next_handle, 1);
	h->al = al;
	igt_map_insert(handles, h, h);

	return h->handle;
}

static void __handle_destroy(uint64_t handle)
{
	struct handle_entry he = { .handle = handle };

	igt_map_remove(handles, &he, map_entry_free_func);
}

static struct allocator *__allocator_find(int fd, uint32_t ctx, uint32_t vm)
{
	struct allocator al = { .fd = fd, .ctx = ctx, .vm = vm };
	struct igt_map *map = GET_MAP(vm);

	return igt_map_search(map, &al);
}

static struct allocator *__allocator_find_by_handle(uint64_t handle)
{
	struct handle_entry *h, he = { .handle = handle };

	h = igt_map_search(handles, &he);
	if (!h)
		return NULL;

	return h->al;
}

static struct allocator *__allocator_create(int fd, uint32_t ctx, uint32_t vm,
					    struct intel_allocator *ial)
{
	struct igt_map *map = GET_MAP(vm);
	struct allocator *al = malloc(sizeof(*al));

	igt_assert(al);
	igt_assert(fd == ial->fd);
	al->fd = fd;
	al->ctx = ctx;
	al->vm = vm;
	atomic_init(&al->refcount, 0);
	al->ial = ial;

	igt_map_insert(map, al, al);

	return al;
}

static void __allocator_destroy(struct allocator *al)
{
	struct igt_map *map = GET_MAP(al->vm);

	igt_map_remove(map, al, map_entry_free_func);
}

static int __allocator_get(struct allocator *al)
{
	struct intel_allocator *ial = al->ial;
	int refcount;

	atomic_fetch_add(&al->refcount, 1);
	refcount = atomic_fetch_add(&ial->refcount, 1);
	igt_assert(refcount >= 0);

	return refcount;
}

static bool __allocator_put(struct allocator *al)
{
	struct intel_allocator *ial = al->ial;
	bool released = false;
	int refcount, al_refcount;

	al_refcount = atomic_fetch_sub(&al->refcount, 1);
	refcount = atomic_fetch_sub(&ial->refcount, 1);
	igt_assert(refcount >= 1);
	if (refcount == 1) {
		if (!ial->is_empty(ial) && warn_if_not_empty)
			igt_warn("Allocator not clear before destroy!\n");

		/* Check allocator has also refcount == 1 */
		igt_assert_eq(al_refcount, 1);

		released = true;
	}

	return released;
}

static struct intel_allocator *intel_allocator_create(int fd,
						      uint64_t start, uint64_t end,
						      uint8_t allocator_type,
						      uint8_t allocator_strategy)
{
	struct intel_allocator *ial = NULL;

	switch (allocator_type) {
	/*
	 * Few words of explanation is required here.
	 *
	 * INTEL_ALLOCATOR_NONE allows keeping information in the code (intel-bb
	 * is an example) we're not using IGT allocator itself and likely
	 * we rely on relocations.
	 * So trying to create NONE allocator doesn't makes sense and below
	 * assertion catches such invalid usage.
	 */
	case INTEL_ALLOCATOR_NONE:
		igt_assert_f(allocator_type != INTEL_ALLOCATOR_NONE,
			     "We cannot use NONE allocator\n");
		break;
	case INTEL_ALLOCATOR_RELOC:
		ial = intel_allocator_reloc_create(fd);
		break;
	case INTEL_ALLOCATOR_RANDOM:
		ial = intel_allocator_random_create(fd);
		break;
	case INTEL_ALLOCATOR_SIMPLE:
		if (!start && !end)
			ial = intel_allocator_simple_create(fd);
		else
			ial = intel_allocator_simple_create_full(fd, start, end,
								 allocator_strategy);
		break;
	default:
		igt_assert_f(ial, "Allocator type %d not implemented\n",
			     allocator_type);
		break;
	}

	igt_assert(ial);

	ial->type = allocator_type;
	ial->strategy = allocator_strategy;
	pthread_mutex_init(&ial->mutex, NULL);

	return ial;
}

static void intel_allocator_destroy(struct intel_allocator *ial)
{
	alloc_info("Destroying allocator (empty: %d)\n", ial->is_empty(ial));

	ial->destroy(ial);
}

static struct allocator *allocator_open(int fd, uint32_t ctx, uint32_t vm,
					uint64_t start, uint64_t end,
					uint8_t allocator_type,
					uint8_t allocator_strategy,
					uint64_t *ahndp)
{
	struct intel_allocator *ial;
	struct allocator *al;
	const char *idstr = vm ? "vm" : "ctx";

	igt_assert(ahndp);

	al = __allocator_find(fd, ctx, vm);
	if (!al) {
		alloc_info("Allocator fd: %d, ctx: %u, vm: %u, <0x%llx : 0x%llx> "
			    "not found, creating one\n",
			    fd, ctx, vm, (long long) start, (long long) end);
		ial = intel_allocator_create(fd, start, end, allocator_type,
					     allocator_strategy);
		al = __allocator_create(fd, ctx, vm, ial);
	}

	ial = al->ial;

	igt_assert_f(ial->type == allocator_type,
		     "Allocator type must be same for fd/%s\n", idstr);

	igt_assert_f(ial->strategy == allocator_strategy,
		     "Allocator strategy must be same or fd/%s\n", idstr);

	__allocator_get(al);
	*ahndp = __handle_create(al);

	return al;
}

static struct allocator *allocator_open_as(struct allocator *base,
					   uint32_t new_vm, uint64_t *ahndp)
{
	struct allocator *al;

	igt_assert(ahndp);
	al = __allocator_create(base->fd, base->ctx, new_vm, base->ial);
	__allocator_get(al);
	*ahndp = __handle_create(al);

	return al;
}

static bool allocator_close(uint64_t ahnd)
{
	struct allocator *al;
	bool released, is_empty = false;

	al = __allocator_find_by_handle(ahnd);
	if (!al) {
		igt_warn("Cannot find handle: %llx\n", (long long) ahnd);
		return false;
	}

	released = __allocator_put(al);
	if (released) {
		is_empty = al->ial->is_empty(al->ial);
		intel_allocator_destroy(al->ial);
	}

	if (!atomic_load(&al->refcount))
		__allocator_destroy(al);

	__handle_destroy(ahnd);

	return is_empty;
}

static int send_req_recv_resp(struct msg_channel *msgchan,
			      struct alloc_req *request,
			      struct alloc_resp *response)
{
	int ret;

	ret = send_req(msgchan, child_tid, request);
	if (ret < 0) {
		igt_warn("Error sending request [type: %d]: err = %d [%s]\n",
			 request->request_type, errno, strerror(errno));

		return ret;
	}

	ret = recv_resp(msgchan, child_tid, response);
	if (ret < 0)
		igt_warn("Error receiving response [type: %d]: err = %d [%s]\n",
			 request->request_type, errno, strerror(errno));

	/*
	 * This is main assumption - we receive message which size must be > 0.
	 * If this is fulfilled we return 0 as a success.
	 */
	if (ret > 0)
		ret = 0;

	return ret;
}

static int handle_request(struct alloc_req *req, struct alloc_resp *resp)
{
	int ret;
	long refcnt;

	memset(resp, 0, sizeof(*resp));

	if (is_same_process()) {
		struct intel_allocator *ial;
		struct allocator *al;
		uint64_t start, end, size, ahnd;
		uint32_t ctx, vm;
		bool allocated, reserved, unreserved;
		/* Used when debug is on, so avoid compilation warnings */
		(void) ctx;
		(void) vm;
		(void) refcnt;

		/*
		 * Mutex only work on allocator instance, not stop/open/close
		 */
		if (req->request_type > REQ_CLOSE) {
			/*
			 * We have to lock map mutex because concurrent open
			 * can lead to resizing the map.
			 */
			pthread_mutex_lock(&map_mutex);
			al = __allocator_find_by_handle(req->allocator_handle);
			pthread_mutex_unlock(&map_mutex);
			igt_assert(al);

			ial = al->ial;
			igt_assert(ial);
			pthread_mutex_lock(&ial->mutex);
		}

		switch (req->request_type) {
		case REQ_STOP:
			alloc_info("<stop>\n");
			break;

		case REQ_OPEN:
			pthread_mutex_lock(&map_mutex);
			al = allocator_open(req->open.fd,
					    req->open.ctx, req->open.vm,
					    req->open.start, req->open.end,
					    req->open.allocator_type,
					    req->open.allocator_strategy,
					    &ahnd);
			refcnt = atomic_load(&al->refcount);
			ret = atomic_load(&al->ial->refcount);
			pthread_mutex_unlock(&map_mutex);

			resp->response_type = RESP_OPEN;
			resp->open.allocator_handle = ahnd;

			alloc_info("<open> [tid: %ld] fd: %d, ahnd: %" PRIx64
				   ", ctx: %u, vm: %u"
				   ", alloc_type: %u, al->refcnt: %ld->%ld"
				   ", refcnt: %d->%d\n",
				   (long) req->tid, req->open.fd, ahnd,
				   req->open.ctx,
				   req->open.vm, req->open.allocator_type,
				   refcnt - 1, refcnt, ret - 1, ret);
			break;

		case REQ_OPEN_AS:
			/* lock first to avoid concurrent close */
			pthread_mutex_lock(&map_mutex);

			al = __allocator_find_by_handle(req->allocator_handle);
			resp->response_type = RESP_OPEN_AS;

			if (!al) {
				alloc_info("<open as> [tid: %ld] ahnd: %" PRIx64
					   " -> no handle\n",
					   (long) req->tid, req->allocator_handle);
				pthread_mutex_unlock(&map_mutex);
				break;
			}

			if (!al->vm) {
				alloc_info("<open as> [tid: %ld] ahnd: %" PRIx64
					   " -> only open as for <fd, vm> is possible\n",
					   (long) req->tid, req->allocator_handle);
				pthread_mutex_unlock(&map_mutex);
				break;
			}


			al = allocator_open_as(al, req->open_as.new_vm, &ahnd);
			refcnt = atomic_load(&al->refcount);
			ret = atomic_load(&al->ial->refcount);
			pthread_mutex_unlock(&map_mutex);

			resp->response_type = RESP_OPEN_AS;
			resp->open.allocator_handle = ahnd;

			alloc_info("<open as> [tid: %ld] fd: %d, ahnd: %" PRIx64
				   ", ctx: %u, vm: %u"
				   ", alloc_type: %u, al->refcnt: %ld->%ld"
				   ", refcnt: %d->%d\n",
				   (long) req->tid, al->fd, ahnd,
				   al->ctx, al->vm, al->ial->type,
				   refcnt - 1, refcnt, ret - 1, ret);
			break;

		case REQ_CLOSE:
			pthread_mutex_lock(&map_mutex);
			al = __allocator_find_by_handle(req->allocator_handle);
			resp->response_type = RESP_CLOSE;

			if (!al) {
				alloc_info("<close> [tid: %ld] ahnd: %" PRIx64
					   " -> no handle\n",
					   (long) req->tid, req->allocator_handle);
				pthread_mutex_unlock(&map_mutex);
				break;
			}

			resp->response_type = RESP_CLOSE;
			ctx = al->ctx;
			vm = al->vm;

			refcnt = atomic_load(&al->refcount);
			ret = atomic_load(&al->ial->refcount);
			resp->close.is_empty = allocator_close(req->allocator_handle);
			pthread_mutex_unlock(&map_mutex);

			alloc_info("<close> [tid: %ld] ahnd: %" PRIx64
				   ", ctx: %u, vm: %u"
				   ", is_empty: %d, al->refcount: %ld->%ld"
				   ", refcnt: %d->%d\n",
				   (long) req->tid, req->allocator_handle,
				   ctx, vm, resp->close.is_empty,
				   refcnt, refcnt - 1, ret, ret - 1);

			break;

		case REQ_ADDRESS_RANGE:
			resp->response_type = RESP_ADDRESS_RANGE;
			ial->get_address_range(ial, &start, &end);
			resp->address_range.start = start;
			resp->address_range.end = end;
			alloc_info("<address range> [tid: %ld] ahnd: %" PRIx64
				   ", ctx: %u, vm: %u"
				   ", start: 0x%" PRIx64 ", end: 0x%" PRId64 "\n",
				   (long) req->tid, req->allocator_handle,
				   al->ctx, al->vm, start, end);
			break;

		case REQ_ALLOC:
			resp->response_type = RESP_ALLOC;
			resp->alloc.offset = ial->alloc(ial,
							req->alloc.handle,
							req->alloc.size,
							req->alloc.alignment);
			alloc_info("<alloc> [tid: %ld] ahnd: %" PRIx64
				   ", ctx: %u, vm: %u, handle: %u"
				   ", size: 0x%" PRIx64 ", offset: 0x%" PRIx64
				   ", alignment: 0x%" PRIx64 "\n",
				   (long) req->tid, req->allocator_handle,
				   al->ctx, al->vm,
				   req->alloc.handle, req->alloc.size,
				   resp->alloc.offset, req->alloc.alignment);
			break;

		case REQ_FREE:
			resp->response_type = RESP_FREE;
			resp->free.freed = ial->free(ial, req->free.handle);
			alloc_info("<free> [tid: %ld] ahnd: %" PRIx64
				   ", ctx: %u, vm: %u"
				   ", handle: %u, freed: %d\n",
				   (long) req->tid, req->allocator_handle,
				   al->ctx, al->vm,
				   req->free.handle, resp->free.freed);
			break;

		case REQ_IS_ALLOCATED:
			resp->response_type = RESP_IS_ALLOCATED;
			allocated = ial->is_allocated(ial,
						      req->is_allocated.handle,
						      req->is_allocated.size,
						      req->is_allocated.offset);
			resp->is_allocated.allocated = allocated;
			alloc_info("<is allocated> [tid: %ld] ahnd: %" PRIx64
				   ", ctx: %u, vm: %u"
				   ", offset: 0x%" PRIx64
				   ", allocated: %d\n", (long) req->tid,
				   req->allocator_handle, al->ctx, al->vm,
				   req->is_allocated.offset, allocated);
			break;

		case REQ_RESERVE:
			resp->response_type = RESP_RESERVE;
			reserved = ial->reserve(ial,
						req->reserve.handle,
						req->reserve.start,
						req->reserve.end);
			resp->reserve.reserved = reserved;
			alloc_info("<reserve> [tid: %ld] ahnd: %" PRIx64
				   ", ctx: %u, vm: %u, handle: %u"
				   ", start: 0x%" PRIx64 ", end: 0x%" PRIx64
				   ", reserved: %d\n",
				   (long) req->tid, req->allocator_handle,
				   al->ctx, al->vm, req->reserve.handle,
				   req->reserve.start, req->reserve.end, reserved);
			break;

		case REQ_UNRESERVE:
			resp->response_type = RESP_UNRESERVE;
			unreserved = ial->unreserve(ial,
						    req->unreserve.handle,
						    req->unreserve.start,
						    req->unreserve.end);
			resp->unreserve.unreserved = unreserved;
			alloc_info("<unreserve> [tid: %ld] ahnd: %" PRIx64
				   ", ctx: %u, vm: %u, handle: %u"
				   ", start: 0x%" PRIx64 ", end: 0x%" PRIx64
				   ", unreserved: %d\n",
				   (long) req->tid, req->allocator_handle,
				   al->ctx, al->vm, req->unreserve.handle,
				   req->unreserve.start, req->unreserve.end,
				   unreserved);
			break;

		case REQ_IS_RESERVED:
			resp->response_type = RESP_IS_RESERVED;
			reserved = ial->is_reserved(ial,
						    req->is_reserved.start,
						    req->is_reserved.end);
			resp->is_reserved.reserved = reserved;
			alloc_info("<is reserved> [tid: %ld] ahnd: %" PRIx64
				   ", ctx: %u, vm: %u"
				   ", start: 0x%" PRIx64 ", end: 0x%" PRIx64
				   ", reserved: %d\n",
				   (long) req->tid, req->allocator_handle,
				   al->ctx, al->vm, req->is_reserved.start,
				   req->is_reserved.end, reserved);
			break;

		case REQ_RESERVE_IF_NOT_ALLOCATED:
			resp->response_type = RESP_RESERVE_IF_NOT_ALLOCATED;
			size = req->reserve.end - req->reserve.start;

			allocated = ial->is_allocated(ial, req->reserve.handle,
						      size, req->reserve.start);
			if (allocated) {
				resp->reserve_if_not_allocated.allocated = allocated;
				alloc_info("<reserve if not allocated> [tid: %ld] "
					   "ahnd: %" PRIx64 ", ctx: %u, vm: %u"
					   ", handle: %u, size: 0x%lx"
					   ", start: 0x%" PRIx64 ", end: 0x%" PRIx64
					   ", allocated: %d, reserved: %d\n",
					   (long) req->tid, req->allocator_handle,
					   al->ctx, al->vm, req->reserve.handle,
					   (long) size, req->reserve.start,
					   req->reserve.end, allocated, false);
				break;
			}

			reserved = ial->reserve(ial,
						req->reserve.handle,
						req->reserve.start,
						req->reserve.end);
			resp->reserve_if_not_allocated.reserved = reserved;
			alloc_info("<reserve if not allocated> [tid: %ld] "
				   "ahnd: %" PRIx64 ", ctx: %u, vm: %u"
				   ", handle: %u, start: 0x%" PRIx64 ", end: 0x%" PRIx64
				   ", allocated: %d, reserved: %d\n",
				   (long) req->tid, req->allocator_handle,
				   al->ctx, al->vm,
				   req->reserve.handle,
				   req->reserve.start, req->reserve.end,
				   false, reserved);
			break;
		}

		if (req->request_type > REQ_CLOSE)
			pthread_mutex_unlock(&ial->mutex);

		return 0;
	}

	ret = send_req_recv_resp(channel, req, resp);

	if (ret < 0)
		exit(0);

	return ret;
}

static void *allocator_thread_loop(void *data)
{
	struct alloc_req req;
	struct alloc_resp resp;
	int ret;
	(void) data;

	alloc_info("Allocator pid: %ld, tid: %ld\n",
		   (long) allocator_pid, (long) gettid());
	alloc_info("Entering allocator loop\n");

	WRITE_ONCE(allocator_thread_running, true);

	while (1) {
		ret = recv_req(channel, &req);

		if (ret == -1) {
			igt_warn("Error receiving request in thread, ret = %d [%s]\n",
				 ret, strerror(errno));
			igt_waitchildren_timeout(1, "Stopping children, error receiving request\n");
			return (void *) -1;
		}

		/* Fake message to stop the thread */
		if (req.request_type == REQ_STOP) {
			alloc_info("<stop request>\n");
			break;
		}

		ret = handle_request(&req, &resp);
		if (ret) {
			igt_warn("Error handling request in thread, ret = %d [%s]\n",
				 ret, strerror(errno));
			break;
		}

		ret = send_resp(channel, req.tid, &resp);
		if (ret) {
			igt_warn("Error sending response in thread, ret = %d [%s]\n",
				 ret, strerror(errno));

			igt_waitchildren_timeout(1, "Stopping children, error sending response\n");
			return (void *) -1;
		}
	}

	WRITE_ONCE(allocator_thread_running, false);

	return NULL;
}


/**
 * __intel_allocator_multiprocess_prepare:
 *
 * Prepares allocator infrastructure to work in multiprocess mode.
 *
 * Some description is required why prepare/start steps are separated.
 * When we write the code and we don't use address sanitizer simple
 * intel_allocator_multiprocess_start() call is enough. With address
 * sanitizer and using forking we can encounter situation where one
 * forked child called allocator alloc() (so parent has some poisoned
 * memory in shadow map), then second fork occurs. Second child will
 * get poisoned shadow map from parent (there allocator thread reside).
 * Checking shadow map in this child will report memory leak.
 *
 * How to separate initialization steps take a look into api_intel_allocator.c
 * fork_simple_stress() function.
 */
void __intel_allocator_multiprocess_prepare(void)
{
	intel_allocator_init();

	multiprocess = true;
	channel->init(channel);
}

void __intel_allocator_multiprocess_start(void)
{
	pthread_create(&allocator_thread, NULL,
		       allocator_thread_loop, NULL);
}

/**
 * intel_allocator_multiprocess_start:
 *
 * Function turns on intel_allocator multiprocess mode what means
 * all allocations from children processes are performed in a separate thread
 * within main igt process. Children are aware of the situation and use
 * some interprocess communication channel to send/receive messages
 * (open, close, alloc, free, ...) to/from allocator thread.
 *
 * Must be used when you want to use an allocator in non single-process code.
 * All allocations in threads spawned in main igt process are handled by
 * mutexing, not by sending/receiving messages to/from allocator thread.
 *
 * Note. This destroys all previously created allocators and theirs content.
 */
void intel_allocator_multiprocess_start(void)
{
	alloc_info("allocator multiprocess start\n");

	igt_assert_f(child_pid == -1,
		     "Allocator thread can be spawned only in main IGT process\n");
	__intel_allocator_multiprocess_prepare();
	__intel_allocator_multiprocess_start();
}

/**
 * intel_allocator_multiprocess_stop:
 *
 * Function turns off intel_allocator multiprocess mode what means
 * stopping allocator thread and deinitializing its data.
 */
#define STOP_TIMEOUT_MS 100
void intel_allocator_multiprocess_stop(void)
{
	int time_left = STOP_TIMEOUT_MS;

	alloc_info("allocator multiprocess stop\n");

	if (multiprocess) {
		send_alloc_stop(channel);

		/* Give allocator thread time to complete */
		while (time_left-- > 0 && READ_ONCE(allocator_thread_running))
			usleep(1000); /* coarse calculation */

		/* Deinit, this should stop all blocked syscalls, if any */
		channel->deinit(channel);
		pthread_join(allocator_thread, NULL);

		/* But we're not sure does child will stuck */
		igt_waitchildren_timeout(5, "Stopping children");
		multiprocess = false;
	}
}

static uint64_t __intel_allocator_open_full(int fd, uint32_t ctx,
					    uint32_t vm,
					    uint64_t start, uint64_t end,
					    uint8_t allocator_type,
					    enum allocator_strategy strategy)
{
	struct alloc_req req = { .request_type = REQ_OPEN,
				 .open.fd = fd,
				 .open.ctx = ctx,
				 .open.vm = vm,
				 .open.start = start,
				 .open.end = end,
				 .open.allocator_type = allocator_type,
				 .open.allocator_strategy = strategy };
	struct alloc_resp resp;

	/* Get child_tid only once at open() */
	if (child_tid == -1)
		child_tid = gettid();

	igt_assert(handle_request(&req, &resp) == 0);
	igt_assert(resp.open.allocator_handle);
	igt_assert(resp.response_type == RESP_OPEN);

	return resp.open.allocator_handle;
}

/**
 * intel_allocator_open_full:
 * @fd: i915 descriptor
 * @ctx: context
 * @start: address of the beginning
 * @end: address of the end
 * @allocator_type: one of INTEL_ALLOCATOR_* define
 * @strategy: passed to the allocator to define the strategy (like order
 * of allocation, see notes below).
 *
 * Function opens an allocator instance within <@start, @end) vm for given
 * @fd and @ctx and returns its handle. If the allocator for such pair
 * doesn't exist it is created with refcount = 1.
 * Parallel opens returns same handle bumping its refcount.
 *
 * Returns: unique handle to the currently opened allocator.
 *
 * Notes:
 * Strategy is generally used internally by the underlying allocator:
 *
 * For SIMPLE allocator:
 * - ALLOC_STRATEGY_HIGH_TO_LOW means topmost addresses are allocated first,
 * - ALLOC_STRATEGY_LOW_TO_HIGH opposite, allocation starts from lowest
 *   addresses.
 *
 * For RANDOM allocator:
 * - none of strategy is currently implemented.
 */
uint64_t intel_allocator_open_full(int fd, uint32_t ctx,
				   uint64_t start, uint64_t end,
				   uint8_t allocator_type,
				   enum allocator_strategy strategy)
{
	return __intel_allocator_open_full(fd, ctx, 0, start, end,
					   allocator_type, strategy);
}

uint64_t intel_allocator_open_vm_full(int fd, uint32_t vm,
				      uint64_t start, uint64_t end,
				      uint8_t allocator_type,
				      enum allocator_strategy strategy)
{
	igt_assert(vm != 0);
	return __intel_allocator_open_full(fd, 0, vm, start, end,
					   allocator_type, strategy);
}

/**
 * intel_allocator_open:
 * @fd: i915 descriptor
 * @ctx: context
 * @allocator_type: one of INTEL_ALLOCATOR_* define
 *
 * Function opens an allocator instance for given @fd and @ctx and returns
 * its handle. If the allocator for such pair doesn't exist it is created
 * with refcount = 1. Parallel opens returns same handle bumping its refcount.
 *
 * Returns: unique handle to the currently opened allocator.
 *
 * Notes: we pass ALLOC_STRATEGY_HIGH_TO_LOW as default, playing with higher
 * addresses makes easier to find addressing issues (like passing non-canonical
 * offsets, which won't be catched unless 47-bit is set).
 */
uint64_t intel_allocator_open(int fd, uint32_t ctx, uint8_t allocator_type)
{
	return intel_allocator_open_full(fd, ctx, 0, 0, allocator_type,
					 ALLOC_STRATEGY_HIGH_TO_LOW);
}

uint64_t intel_allocator_open_vm(int fd, uint32_t vm, uint8_t allocator_type)
{
	return intel_allocator_open_vm_full(fd, vm, 0, 0, allocator_type,
					    ALLOC_STRATEGY_HIGH_TO_LOW);
}

uint64_t intel_allocator_open_vm_as(uint64_t allocator_handle, uint32_t new_vm)
{
	struct alloc_req req = { .request_type = REQ_OPEN_AS,
				 .allocator_handle = allocator_handle,
				 .open_as.new_vm = new_vm };
	struct alloc_resp resp;

	/* Get child_tid only once at open() */
	if (child_tid == -1)
		child_tid = gettid();

	igt_assert(handle_request(&req, &resp) == 0);
	igt_assert(resp.open_as.allocator_handle);
	igt_assert(resp.response_type == RESP_OPEN_AS);

	return resp.open.allocator_handle;
}

/**
 * intel_allocator_close:
 * @allocator_handle: handle to the allocator that will be closed
 *
 * Function decreases an allocator refcount for the given @handle.
 * When refcount reaches zero allocator is closed (destroyed) and all
 * allocated / reserved areas are freed.
 *
 * Returns: true if closed allocator was empty, false otherwise.
 */
bool intel_allocator_close(uint64_t allocator_handle)
{
	struct alloc_req req = { .request_type = REQ_CLOSE,
				 .allocator_handle = allocator_handle };
	struct alloc_resp resp;

	igt_assert(handle_request(&req, &resp) == 0);
	igt_assert(resp.response_type == RESP_CLOSE);

	return resp.close.is_empty;
}

/**
 * intel_allocator_get_address_range:
 * @allocator_handle: handle to an allocator
 * @startp: pointer to the variable where function writes starting offset
 * @endp: pointer to the variable where function writes ending offset
 *
 * Function fills @startp, @endp with respectively, starting and ending offset
 * of the allocator working virtual address space range.
 *
 * Note. Allocators working ranges can differ depending on the device or
 * the allocator type so before reserving a specific offset a good practise
 * is to ensure that address is between accepted range.
 */
void intel_allocator_get_address_range(uint64_t allocator_handle,
				       uint64_t *startp, uint64_t *endp)
{
	struct alloc_req req = { .request_type = REQ_ADDRESS_RANGE,
				 .allocator_handle = allocator_handle };
	struct alloc_resp resp;

	igt_assert(handle_request(&req, &resp) == 0);
	igt_assert(resp.response_type == RESP_ADDRESS_RANGE);

	if (startp)
		*startp = resp.address_range.start;

	if (endp)
		*endp = resp.address_range.end;
}

/**
 * __intel_allocator_alloc:
 * @allocator_handle: handle to an allocator
 * @handle: handle to an object
 * @size: size of an object
 * @alignment: determines object alignment
 *
 * Function finds and returns the most suitable offset with given @alignment
 * for an object with @size identified by the @handle.
 *
 * Returns: currently assigned address for a given object. If an object was
 * already allocated returns same address. If allocator can't find suitable
 * range returns ALLOC_INVALID_ADDRESS.
 */
uint64_t __intel_allocator_alloc(uint64_t allocator_handle, uint32_t handle,
				 uint64_t size, uint64_t alignment)
{
	struct alloc_req req = { .request_type = REQ_ALLOC,
				 .allocator_handle = allocator_handle,
				 .alloc.handle = handle,
				 .alloc.size = size,
				 .alloc.alignment = alignment };
	struct alloc_resp resp;

	igt_assert(handle_request(&req, &resp) == 0);
	igt_assert(resp.response_type == RESP_ALLOC);

	return resp.alloc.offset;
}

/**
 * intel_allocator_alloc:
 * @allocator_handle: handle to an allocator
 * @handle: handle to an object
 * @size: size of an object
 * @alignment: determines object alignment
 *
 * Same as __intel_allocator_alloc() but asserts if allocator can't return
 * valid address.
 */
uint64_t intel_allocator_alloc(uint64_t allocator_handle, uint32_t handle,
			       uint64_t size, uint64_t alignment)
{
	uint64_t offset;

	offset = __intel_allocator_alloc(allocator_handle, handle,
					 size, alignment);
	igt_assert(offset != ALLOC_INVALID_ADDRESS);

	return offset;
}

/**
 * intel_allocator_free:
 * @allocator_handle: handle to an allocator
 * @handle: handle to an object to be freed
 *
 * Function free object identified by the @handle in allocator what makes it
 * offset again allocable.
 *
 * Note. Reserved objects can only be freed by an #intel_allocator_unreserve
 * function.
 *
 * Returns: true if the object was successfully freed, otherwise false.
 */
bool intel_allocator_free(uint64_t allocator_handle, uint32_t handle)
{
	struct alloc_req req = { .request_type = REQ_FREE,
				 .allocator_handle = allocator_handle,
				 .free.handle = handle };
	struct alloc_resp resp;

	igt_assert(handle_request(&req, &resp) == 0);
	igt_assert(resp.response_type == RESP_FREE);

	return resp.free.freed;
}

/**
 * intel_allocator_is_allocated:
 * @allocator_handle: handle to an allocator
 * @handle: handle to an object
 * @size: size of an object
 * @offset: address of an object
 *
 * Function checks whether the object identified by the @handle and @size
 * is allocated at the @offset.
 *
 * Returns: true if the object is currently allocated at the @offset,
 * otherwise false.
 */
bool intel_allocator_is_allocated(uint64_t allocator_handle, uint32_t handle,
				  uint64_t size, uint64_t offset)
{
	struct alloc_req req = { .request_type = REQ_IS_ALLOCATED,
				 .allocator_handle = allocator_handle,
				 .is_allocated.handle = handle,
				 .is_allocated.size = size,
				 .is_allocated.offset = offset };
	struct alloc_resp resp;

	igt_assert(handle_request(&req, &resp) == 0);
	igt_assert(resp.response_type == RESP_IS_ALLOCATED);

	return resp.is_allocated.allocated;
}

/**
 * intel_allocator_reserve:
 * @allocator_handle: handle to an allocator
 * @handle: handle to an object
 * @size: size of an object
 * @offset: address of an object
 *
 * Function reserves space that starts at the @offset and has @size.
 * Optionally we can pass @handle to mark that space is for a specific
 * object, otherwise pass -1.
 *
 * Note. Reserved space is identified by offset and size, not a handle.
 * So an object can have multiple reserved spaces with its handle.
 *
 * Returns: true if space is successfully reserved, otherwise false.
 */
bool intel_allocator_reserve(uint64_t allocator_handle, uint32_t handle,
			     uint64_t size, uint64_t offset)
{
	struct alloc_req req = { .request_type = REQ_RESERVE,
				 .allocator_handle = allocator_handle,
				 .reserve.handle = handle,
				 .reserve.start = offset,
				 .reserve.end = offset + size };
	struct alloc_resp resp;

	igt_assert(handle_request(&req, &resp) == 0);
	igt_assert(resp.response_type == RESP_RESERVE);

	return resp.reserve.reserved;
}

/**
 * intel_allocator_unreserve:
 * @allocator_handle: handle to an allocator
 * @handle: handle to an object
 * @size: size of an object
 * @offset: address of an object
 *
 * Function unreserves space that starts at the @offset, @size and @handle.
 *
 * Note. @handle, @size and @offset have to match those used in reservation.
 * i.e. check with the same offset but even smaller size will fail.
 *
 * Returns: true if the space is successfully unreserved, otherwise false.
 */
bool intel_allocator_unreserve(uint64_t allocator_handle, uint32_t handle,
			       uint64_t size, uint64_t offset)
{
	struct alloc_req req = { .request_type = REQ_UNRESERVE,
				 .allocator_handle = allocator_handle,
				 .unreserve.handle = handle,
				 .unreserve.start = offset,
				 .unreserve.end = offset + size };
	struct alloc_resp resp;

	igt_assert(handle_request(&req, &resp) == 0);
	igt_assert(resp.response_type == RESP_UNRESERVE);

	return resp.unreserve.unreserved;
}

/**
 * intel_allocator_is_reserved:
 * @allocator_handle: handle to an allocator
 * @size: size of an object
 * @offset: address of an object
 *
 * Function checks whether space starting at the @offset and @size is
 * currently under reservation.
 *
 * Note. @size and @offset have to match those used in reservation,
 * i.e. check with the same offset but even smaller size will fail.
 *
 * Returns: true if space is reserved, othwerise false.
 */
bool intel_allocator_is_reserved(uint64_t allocator_handle,
				 uint64_t size, uint64_t offset)
{
	struct alloc_req req = { .request_type = REQ_IS_RESERVED,
				 .allocator_handle = allocator_handle,
				 .is_reserved.start = offset,
				 .is_reserved.end = offset + size };
	struct alloc_resp resp;

	igt_assert(handle_request(&req, &resp) == 0);
	igt_assert(resp.response_type == RESP_IS_RESERVED);

	return resp.is_reserved.reserved;
}

/**
 * intel_allocator_reserve_if_not_allocated:
 * @allocator_handle: handle to an allocator
 * @handle: handle to an object
 * @size: size of an object
 * @offset: address of an object
 * @is_allocatedp: if not NULL function writes there object allocation status
 * (true/false)
 *
 * Function checks whether the object identified by the @handle and @size
 * is allocated at the @offset and writes the result to @is_allocatedp.
 * If it's not it reserves it at the given @offset.
 *
 * Returns: true if the space for an object was reserved, otherwise false.
 */
bool intel_allocator_reserve_if_not_allocated(uint64_t allocator_handle,
					      uint32_t handle,
					      uint64_t size, uint64_t offset,
					      bool *is_allocatedp)
{
	struct alloc_req req = { .request_type = REQ_RESERVE_IF_NOT_ALLOCATED,
				 .allocator_handle = allocator_handle,
				 .reserve.handle = handle,
				 .reserve.start = offset,
				 .reserve.end = offset + size };
	struct alloc_resp resp;

	igt_assert(handle_request(&req, &resp) == 0);
	igt_assert(resp.response_type == RESP_RESERVE_IF_NOT_ALLOCATED);

	if (is_allocatedp)
		*is_allocatedp = resp.reserve_if_not_allocated.allocated;

	return resp.reserve_if_not_allocated.reserved;
}

/**
 * intel_allocator_print:
 * @allocator_handle: handle to an allocator
 *
 * Function prints statistics and content of the allocator.
 * Mainly for debugging purposes.
 *
 * Note. Printing possible only in the main process.
 **/
void intel_allocator_print(uint64_t allocator_handle)
{
	igt_assert(allocator_handle);

	if (!multiprocess || is_same_process()) {
		struct allocator *al;

		al = __allocator_find_by_handle(allocator_handle);
		pthread_mutex_lock(&map_mutex);
		al->ial->print(al->ial, true);
		pthread_mutex_unlock(&map_mutex);
	} else {
		igt_warn("Print stats is in main process only\n");
	}
}

static int equal_handles(const void *key1, const void *key2)
{
	const struct handle_entry *h1 = key1, *h2 = key2;

	alloc_debug("h1: %llx, h2: %llx\n",
		   (long long) h1->handle, (long long) h2->handle);

	return h1->handle == h2->handle;
}

static int equal_ctx(const void *key1, const void *key2)
{
	const struct allocator *a1 = key1, *a2 = key2;

	alloc_debug("a1: <fd: %d, ctx: %u>, a2 <fd: %d, ctx: %u>\n",
		   a1->fd, a1->ctx, a2->fd, a2->ctx);

	return a1->fd == a2->fd && a1->ctx == a2->ctx;
}

static int equal_vm(const void *key1, const void *key2)
{
	const struct allocator *a1 = key1, *a2 = key2;

	alloc_debug("a1: <fd: %d, vm: %u>, a2 <fd: %d, vm: %u>\n",
		   a1->fd, a1->vm, a2->fd, a2->vm);

	return a1->fd == a2->fd && a1->vm == a2->vm;
}

/* 2^31 + 2^29 - 2^25 + 2^22 - 2^19 - 2^16 + 1 */
#define GOLDEN_RATIO_PRIME_32 0x9e370001UL

static inline uint32_t hash_handles(const void *val)
{
	uint32_t hash = ((struct handle_entry *) val)->handle;

	hash = hash * GOLDEN_RATIO_PRIME_32;
	return hash;
}

static inline uint32_t hash_instance(const void *val)
{
	uint64_t hash = ((struct allocator *) val)->fd;

	hash = hash * GOLDEN_RATIO_PRIME_32;
	return hash;
}

static void __free_maps(struct igt_map *map, bool close_allocators)
{
	struct igt_map_entry *pos;
	const struct handle_entry *h;

	if (!map)
		return;

	if (close_allocators)
		igt_map_foreach(map, pos) {
			h = pos->key;
			allocator_close(h->handle);
		}

	igt_map_destroy(map, map_entry_free_func);
}

/**
 * intel_allocator_init:
 *
 * Function initializes the allocators infrastructure. The second call will
 * override current infra and destroy existing there allocators. It is called
 * in igt_constructor.
 **/
void intel_allocator_init(void)
{
	alloc_info("Prepare an allocator infrastructure\n");

	allocator_pid = getpid();
	alloc_info("Allocator pid: %ld\n", (long) allocator_pid);

	__free_maps(handles, true);
	__free_maps(ctx_map, false);
	__free_maps(vm_map, false);

	atomic_init(&next_handle, 1);
	handles = igt_map_create(hash_handles, equal_handles);
	ctx_map = igt_map_create(hash_instance, equal_ctx);
	vm_map = igt_map_create(hash_instance, equal_vm);
	igt_assert(handles && ctx_map && vm_map);

	channel = intel_allocator_get_msgchannel(CHANNEL_SYSVIPC_MSGQUEUE);
}

igt_constructor {
	intel_allocator_init();
}
