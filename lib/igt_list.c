/*
 * Copyright © 2019 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include "igt_list.h"

void IGT_INIT_LIST_HEAD(struct igt_list_head *list)
{
	list->prev = list;
	list->next = list;
}

void igt_list_add(struct igt_list_head *elem, struct igt_list_head *head)
{
	elem->prev = head;
	elem->next = head->next;
	head->next = elem;
	elem->next->prev = elem;
}

void igt_list_del(struct igt_list_head *elem)
{
	elem->prev->next = elem->next;
	elem->next->prev = elem->prev;
	elem->next = NULL;
	elem->prev = NULL;
}

void igt_list_del_init(struct igt_list_head *elem)
{
	igt_list_del(elem);
	IGT_INIT_LIST_HEAD(elem);
}

void igt_list_move(struct igt_list_head *elem, struct igt_list_head *list)
{
	igt_list_del(elem);
	igt_list_add(elem, list);
}

void igt_list_move_tail(struct igt_list_head *elem, struct igt_list_head *list)
{
	igt_list_del(elem);
	igt_list_add_tail(elem, list);
}

int igt_list_length(const struct igt_list_head *head)
{
	struct  igt_list_head *e = head->next;
	int count = 0;

	while (e != head) {
		e = e->next;
		count++;
	}

	return count;
}

bool igt_list_empty(const struct igt_list_head *head)
{
	return head->next == head;
}
