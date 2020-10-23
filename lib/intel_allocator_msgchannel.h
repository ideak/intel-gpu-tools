// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#ifndef __INTEL_ALLOCATOR_MSGCHANNEL_H__
#define __INTEL_ALLOCATOR_MSGCHANNEL_H__

#include <sys/types.h>
#include <unistd.h>
#include <stdint.h>

enum reqtype {
	REQ_STOP,
	REQ_OPEN,
	REQ_OPEN_AS,
	REQ_CLOSE,
	REQ_ADDRESS_RANGE,
	REQ_ALLOC,
	REQ_FREE,
	REQ_IS_ALLOCATED,
	REQ_RESERVE,
	REQ_UNRESERVE,
	REQ_RESERVE_IF_NOT_ALLOCATED,
	REQ_IS_RESERVED,
};

enum resptype {
	RESP_OPEN,
	RESP_OPEN_AS,
	RESP_CLOSE,
	RESP_ADDRESS_RANGE,
	RESP_ALLOC,
	RESP_FREE,
	RESP_IS_ALLOCATED,
	RESP_RESERVE,
	RESP_UNRESERVE,
	RESP_IS_RESERVED,
	RESP_RESERVE_IF_NOT_ALLOCATED,
};

struct alloc_req {
	enum reqtype request_type;

	/* Common */
	pid_t tid;
	uint64_t allocator_handle;

	union {
		struct {
			int fd;
			uint32_t ctx;
			uint32_t vm;
			uint64_t start;
			uint64_t end;
			uint8_t allocator_type;
			uint8_t allocator_strategy;
		} open;

		struct {
			uint32_t new_vm;
		} open_as;

		struct {
			uint32_t handle;
			uint64_t size;
			uint64_t alignment;
		} alloc;

		struct {
			uint32_t handle;
		} free;

		struct {
			uint32_t handle;
			uint64_t size;
			uint64_t offset;
		} is_allocated;

		struct {
			uint32_t handle;
			uint64_t start;
			uint64_t end;
		} reserve, unreserve;

		struct {
			uint64_t start;
			uint64_t end;
		} is_reserved;

	};
};

struct alloc_resp {
	enum resptype response_type;
	pid_t tid;

	union {
		struct {
			uint64_t allocator_handle;
		} open, open_as;

		struct {
			bool is_empty;
		} close;

		struct {
			uint64_t start;
			uint64_t end;
			uint8_t direction;
		} address_range;

		struct {
			uint64_t offset;
		} alloc;

		struct {
			bool freed;
		} free;

		struct {
			bool allocated;
		} is_allocated;

		struct {
			bool reserved;
		} reserve, is_reserved;

		struct {
			bool unreserved;
		} unreserve;

		struct {
			bool allocated;
			bool reserved;
		} reserve_if_not_allocated;
	};
};

struct msg_channel {
	void *priv;
	void (*init)(struct msg_channel *channel);
	void (*deinit)(struct msg_channel *channel);
	int (*send_req)(struct msg_channel *channel, struct alloc_req *request);
	int (*recv_req)(struct msg_channel *channel, struct alloc_req *request);
	int (*send_resp)(struct msg_channel *channel, struct alloc_resp *response);
	int (*recv_resp)(struct msg_channel *channel, struct alloc_resp *response);
};

enum msg_channel_type {
	CHANNEL_SYSVIPC_MSGQUEUE
};

struct msg_channel *intel_allocator_get_msgchannel(enum msg_channel_type type);

#endif
