/*    Copyright 2023 Davide Libenzi
 * 
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 * 
 *        http://www.apache.org/licenses/LICENSE-2.0
 * 
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
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

