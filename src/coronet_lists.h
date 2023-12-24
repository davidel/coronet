/*
 *  coronet by Davide Libenzi (coroutine/epoll network engine)
 *  Copyright (C) 2007  Davide Libenzi
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *  Davide Libenzi <davidel@xmailserver.org>
 *
 */

#if !defined(_CORONET_LISTS_H)
#define _CORONET_LISTS_H


#include "coronet.h"


#define CONET_OFFTOF(t, f) ((long) &((t *) 0)->f)
#define CONET_LLENT(p, t, f) ((t *) ((char *) (p) - CONET_OFFTOF(t, f)))



static inline void conet_llinit(struct ll_head *head) {

	head->prev = head->next = head;
}

static inline void conet_lladd(struct ll_head *ent, struct ll_head *prev,
			       struct ll_head *next) {

	prev->next = ent;
	next->prev = ent;
	ent->prev = prev;
	ent->next = next;
}

static inline void conet_lladdt(struct ll_head *ent, struct ll_head *head) {

	conet_lladd(ent, head->prev, head);
}

static inline void conet_lladdh(struct ll_head *ent, struct ll_head *head) {

	conet_lladd(ent, head, head->next);
}

static inline void conet_lldel(struct ll_head *ent) {
	struct ll_head *prev = ent->prev, *next = ent->next;

	prev->next = next;
	next->prev = prev;
}

static inline void conet_lldel_init(struct ll_head *ent) {

	conet_lldel(ent);
	conet_llinit(ent);
}

static inline struct ll_head *conet_llfirst(struct ll_head *head) {

	return head->next != head ? head->next: NULL;
}

static inline struct ll_head *conet_lllast(struct ll_head *head) {

	return head->prev != head ? head->prev: NULL;
}

static inline struct ll_head *conet_llnext(struct ll_head *ent,
					   struct ll_head *head) {

	return ent->next != head ? ent->next: NULL;
}

static inline struct ll_head *conet_llprev(struct ll_head *ent,
					   struct ll_head *head) {

	return ent->prev != head ? ent->prev: NULL;
}

static inline int conet_llempty(struct ll_head *head) {

	return conet_llfirst(head) == NULL;
}


#endif

