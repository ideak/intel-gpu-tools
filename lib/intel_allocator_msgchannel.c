// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "igt.h"
#include "intel_allocator_msgchannel.h"

extern __thread pid_t child_tid;

/* ----- SYSVIPC MSGQUEUE ----- */

#define FTOK_IGT_ALLOCATOR_KEY "/tmp/igt.allocator.key"
#define FTOK_IGT_ALLOCATOR_PROJID 2020

#define ALLOCATOR_REQUEST 1

struct msgqueue_data {
	key_t key;
	int queue;
};

struct msgqueue_buf {
	long mtype;
	union {
		struct alloc_req request;
		struct alloc_resp response;
	} data;
};

static void msgqueue_init(struct msg_channel *channel)
{
	struct msgqueue_data *msgdata;
	struct msqid_ds qstat;
	key_t key;
	int fd, queue;

	igt_debug("Init msgqueue\n");

	/* Create ftok key only if not exists */
	fd = open(FTOK_IGT_ALLOCATOR_KEY, O_CREAT | O_EXCL | O_WRONLY, 0600);
	igt_assert(fd >= 0 || errno == EEXIST);
	if (fd >= 0)
		close(fd);

	key = ftok(FTOK_IGT_ALLOCATOR_KEY, FTOK_IGT_ALLOCATOR_PROJID);
	igt_assert(key != -1);
	igt_debug("Queue key: %x\n", (int) key);

	queue = msgget(key, 0);
	if (queue != -1) {
		igt_assert(msgctl(queue, IPC_STAT, &qstat) == 0);
		igt_debug("old messages: %lu\n", qstat.msg_qnum);
		igt_assert(msgctl(queue, IPC_RMID, NULL) == 0);
	}

	queue = msgget(key, IPC_CREAT);
	igt_debug("msg queue: %d\n", queue);

	msgdata = calloc(1, sizeof(*msgdata));
	igt_assert(msgdata);
	msgdata->key = key;
	msgdata->queue = queue;
	channel->priv = msgdata;
}

static void msgqueue_deinit(struct msg_channel *channel)
{
	struct msgqueue_data *msgdata = channel->priv;

	igt_debug("Deinit msgqueue\n");
	msgctl(msgdata->queue, IPC_RMID, NULL);
	free(channel->priv);
}

static int msgqueue_send_req(struct msg_channel *channel,
			     struct alloc_req *request)
{
	struct msgqueue_data *msgdata = channel->priv;
	struct msgqueue_buf buf = {0};
	int ret;

	buf.mtype = ALLOCATOR_REQUEST;
	buf.data.request.request_type = 1;
	memcpy(&buf.data.request, request, sizeof(*request));

retry:
	ret = msgsnd(msgdata->queue, &buf, sizeof(buf) - sizeof(long), 0);
	if (ret == -1 && errno == EINTR)
		goto retry;

	if (ret == -1)
		igt_warn("Error: %s\n", strerror(errno));

	return ret;
}

static int msgqueue_recv_req(struct msg_channel *channel,
			     struct alloc_req *request)
{
	struct msgqueue_data *msgdata = channel->priv;
	struct msgqueue_buf buf = {0};
	int ret, size = sizeof(buf) - sizeof(long);

retry:
	ret = msgrcv(msgdata->queue, &buf, size, ALLOCATOR_REQUEST, 0);
	if (ret == -1 && errno == EINTR)
		goto retry;

	if (ret == size)
		memcpy(request, &buf.data.request, sizeof(*request));
	else if (ret == -1)
		igt_warn("Error: %s\n", strerror(errno));

	return ret;
}

static int msgqueue_send_resp(struct msg_channel *channel,
			      struct alloc_resp *response)
{
	struct msgqueue_data *msgdata = channel->priv;
	struct msgqueue_buf buf = {0};
	int ret;

	buf.mtype = response->tid;
	memcpy(&buf.data.response, response, sizeof(*response));

retry:
	ret = msgsnd(msgdata->queue, &buf, sizeof(buf) - sizeof(long), 0);
	if (ret == -1 && errno == EINTR)
		goto retry;

	if (ret == -1)
		igt_warn("Error: %s\n", strerror(errno));

	return ret;
}

static int msgqueue_recv_resp(struct msg_channel *channel,
			      struct alloc_resp *response)
{
	struct msgqueue_data *msgdata = channel->priv;
	struct msgqueue_buf buf = {0};
	int ret, size = sizeof(buf) - sizeof(long);

retry:
	ret = msgrcv(msgdata->queue, &buf, sizeof(buf) - sizeof(long),
		     response->tid, 0);
	if (ret == -1 && errno == EINTR)
		goto retry;

	if (ret == size)
		memcpy(response, &buf.data.response, sizeof(*response));
	else if (ret == -1)
		igt_warn("Error: %s\n", strerror(errno));

	return ret;
}

static struct msg_channel msgqueue_channel = {
	.priv = NULL,
	.init = msgqueue_init,
	.deinit = msgqueue_deinit,
	.send_req = msgqueue_send_req,
	.recv_req = msgqueue_recv_req,
	.send_resp = msgqueue_send_resp,
	.recv_resp = msgqueue_recv_resp,
};

struct msg_channel *intel_allocator_get_msgchannel(enum msg_channel_type type)
{
	struct msg_channel *channel = NULL;

	switch (type) {
	case CHANNEL_SYSVIPC_MSGQUEUE:
		channel = &msgqueue_channel;
	}

	igt_assert(channel);

	return channel;
}
