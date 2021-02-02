/*
 * Copyright © 2015 Intel Corporation
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

#ifndef IGT_LIST_H
#define IGT_LIST_H

#include <stdbool.h>
#include <stddef.h>

/**
 * SECTION:igt_list
 * @short_description: a list implementation inspired by the kernel
 * @title: IGT List
 * @include: igt_list.h
 *
 * This list data structure mimics the one we can find in the kernel. A few
 * bonus helpers are provided.
 *
 * igt_list is a doubly-linked list where an instance of igt_list_head is a
 * head sentinel and has to be initialized.
 *
 * Example usage:
 *
 * |[<!-- language="C" -->
 * struct igt_list_head foo_head;
 *
 * struct element {
 *         int foo;
 *         struct igt_list_head link;
 * };
 *
 * struct element e1, e2, e3;
 *
 * IGT_INIT_LIST_HEAD(&foo_head);
 * igt_list_add(&e1.link, &foo_head);   // e1 is the first element
 * igt_list_add(&e2.link, &foo_head);   // e2 is now the first element
 * igt_list_add(&e3.link, &e2.link);    // insert e3 after e2
 *
 * printf("list length: %d\n", igt_list_length(&foo_head));
 *
 * struct element *iter;
 * igt_list_for_each_entry(iter, &foo_head, link) {
 * 	printf("  %d\n", iter->foo);
 * }
 * ]|
 */

struct igt_list_head {
	struct igt_list_head *prev;
	struct igt_list_head *next;
};


void IGT_INIT_LIST_HEAD(struct igt_list_head *head);
void igt_list_add(struct igt_list_head *elem, struct igt_list_head *head);
void igt_list_del(struct igt_list_head *elem);
void igt_list_del_init(struct igt_list_head *elem);
void igt_list_move(struct igt_list_head *elem, struct igt_list_head *list);
void igt_list_move_tail(struct igt_list_head *elem, struct igt_list_head *list);
int igt_list_length(const struct igt_list_head *head);
bool igt_list_empty(const struct igt_list_head *head);

#define igt_container_of(ptr, sample, member)				\
	(__typeof__(sample))((char *)(ptr) -				\
				offsetof(__typeof__(*sample), member))

#define igt_list_for_each_entry(pos, head, member)			\
	for (pos = igt_container_of((head)->next, pos, member);		\
	     &pos->member != (head);					\
	     pos = igt_container_of((pos)->member.next, pos, member))

/**
 * igt_list_for_each_safe
 *
 * Safe against removel of the *current* list element. To achive this it
 * requires an extra helper variable `tmp` with the same type as `pos`.
 */
#define igt_list_for_each_entry_safe(pos, tmp, head, member)			\
	for (pos = igt_container_of((head)->next, pos, member),		\
	     tmp = igt_container_of((pos)->member.next, tmp, member); 	\
	     &pos->member != (head);					\
	     pos = tmp,							\
	     tmp = igt_container_of((pos)->member.next, tmp, member))

#define igt_list_for_each_entry_reverse(pos, head, member)		\
	for (pos = igt_container_of((head)->prev, pos, member);		\
	     &pos->member != (head);					\
	     pos = igt_container_of((pos)->member.prev, pos, member))


/* IGT custom helpers */

/**
 * IGT_LIST_HEAD
 *
 * Instead of having to use `IGT_INIT_LIST_HEAD()` list can be defined using
 * this helper, e.g. `static IGT_LIST_HEAD(list);`
 */
#define IGT_LIST_HEAD(name) struct igt_list_head name = { &(name), &(name) }

#define igt_list_add_tail(elem, head) igt_list_add(elem, (head)->prev)

#define igt_list_first_entry(head, type, member) \
	igt_container_of((head)->next, (type), member)

#define igt_list_last_entry(head, type, member) \
	igt_container_of((head)->prev, (type), member)

#endif /* IGT_LIST_H */
