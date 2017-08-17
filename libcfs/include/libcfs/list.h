/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License version 2 for more details.  A copy is
 * included in the COPYING file that accompanied this code.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * GPL HEADER END
 */

#ifndef __LIBCFS_LIST_H__
#define __LIBCFS_LIST_H__

#if defined (__linux__) && defined(__KERNEL__)

#include <linux/list.h>

typedef struct list_head cfs_list_t;

#define __cfs_list_add(new, prev, next)      __list_add(new, prev, next)
#define cfs_list_add(new, head)              list_add(new, head)

#define cfs_list_add_tail(new, head)         list_add_tail(new, head)

#define __cfs_list_del(prev, next)           __list_del(prev, next)
#define cfs_list_del(entry)                  list_del(entry)
#define cfs_list_del_init(entry)             list_del_init(entry)

#define cfs_list_move(list, head)            list_move(list, head)
#define cfs_list_move_tail(list, head)       list_move_tail(list, head)

#define cfs_list_empty(head)                 list_empty(head)
#define cfs_list_empty_careful(head)         list_empty_careful(head)

#define __cfs_list_splice(list, head)        __list_splice(list, head)
#define cfs_list_splice(list, head)          list_splice(list, head)

#define cfs_list_splice_init(list, head)     list_splice_init(list, head)

#define cfs_list_entry(ptr, type, member)    list_entry(ptr, type, member)
#define cfs_list_for_each(pos, head)         list_for_each(pos, head)
#define cfs_list_for_each_safe(pos, n, head) list_for_each_safe(pos, n, head)
#define cfs_list_for_each_prev(pos, head)    list_for_each_prev(pos, head)
#define cfs_list_for_each_entry(pos, head, member) \
        list_for_each_entry(pos, head, member)
#define cfs_list_for_each_entry_reverse(pos, head, member) \
        list_for_each_entry_reverse(pos, head, member)
#define cfs_list_for_each_entry_safe_reverse(pos, n, head, member) \
	list_for_each_entry_safe_reverse(pos, n, head, member)
#define cfs_list_for_each_entry_safe(pos, n, head, member) \
        list_for_each_entry_safe(pos, n, head, member)
#ifdef list_for_each_entry_safe_from
#define cfs_list_for_each_entry_safe_from(pos, n, head, member) \
        list_for_each_entry_safe_from(pos, n, head, member)
#endif /* list_for_each_entry_safe_from */
#define cfs_list_for_each_entry_continue(pos, head, member) \
        list_for_each_entry_continue(pos, head, member)

#define CFS_LIST_HEAD_INIT(n)		     LIST_HEAD_INIT(n)
#define CFS_LIST_HEAD(n)		     LIST_HEAD(n)
#define CFS_INIT_LIST_HEAD(p)		     INIT_LIST_HEAD(p)

typedef struct hlist_head cfs_hlist_head_t;
typedef struct hlist_node cfs_hlist_node_t;

#define cfs_hlist_unhashed(h)              hlist_unhashed(h)

#define cfs_hlist_empty(h)                 hlist_empty(h)

#define __cfs_hlist_del(n)                 __hlist_del(n)
#define cfs_hlist_del(n)                   hlist_del(n)
#define cfs_hlist_del_init(n)              hlist_del_init(n)

#define cfs_hlist_add_head(n, next)        hlist_add_head(n, next)
#define cfs_hlist_add_before(n, next)      hlist_add_before(n, next)
#define cfs_hlist_add_after(n, next)       hlist_add_behind(next, n)

#define cfs_hlist_entry(ptr, type, member) hlist_entry(ptr, type, member)
#define cfs_hlist_for_each(pos, head)      hlist_for_each(pos, head)
#define cfs_hlist_for_each_safe(pos, n, head) \
        hlist_for_each_safe(pos, n, head)
#ifdef HAVE_HLIST_FOR_EACH_3ARG
#define cfs_hlist_for_each_entry(tpos, pos, head, member) \
	pos = NULL; hlist_for_each_entry(tpos, head, member)
#else
#define cfs_hlist_for_each_entry(tpos, pos, head, member) \
        hlist_for_each_entry(tpos, pos, head, member)
#endif
#define cfs_hlist_for_each_entry_continue(tpos, pos, member) \
        hlist_for_each_entry_continue(tpos, pos, member)
#define cfs_hlist_for_each_entry_from(tpos, pos, member) \
        hlist_for_each_entry_from(tpos, pos, member)
#ifdef HAVE_HLIST_FOR_EACH_3ARG
#define cfs_hlist_for_each_entry_safe(tpos, pos, n, head, member) \
	pos = NULL; hlist_for_each_entry_safe(tpos, n, head, member)
#else
#define cfs_hlist_for_each_entry_safe(tpos, pos, n, head, member) \
        hlist_for_each_entry_safe(tpos, pos, n, head, member)
#endif

#define CFS_HLIST_HEAD_INIT		   HLIST_HEAD_INIT
#define CFS_HLIST_HEAD(n)		   HLIST_HEAD(n)
#define CFS_INIT_HLIST_HEAD(p)		   INIT_HLIST_HEAD(p)
#define CFS_INIT_HLIST_NODE(p)		   INIT_HLIST_NODE(p)

#else /* !defined (__linux__) || !defined(__KERNEL__) */

/*
 * Simple doubly linked list implementation.
 *
 * Some of the internal functions ("__xxx") are useful when
 * manipulating whole lists rather than single entries, as
 * sometimes we already know the next/prev entries and we can
 * generate better code by using them directly rather than
 * using the generic single-entry routines.
 */

#define prefetch(a) ((void)a)

struct cfs_list_head {
	struct cfs_list_head *next, *prev;
};

typedef struct cfs_list_head cfs_list_t;

#define CFS_LIST_HEAD_INIT(name) { &(name), &(name) }

#define CFS_LIST_HEAD(name) \
	cfs_list_t name = CFS_LIST_HEAD_INIT(name)

#define CFS_INIT_LIST_HEAD(ptr) do { \
	(ptr)->next = (ptr); (ptr)->prev = (ptr); \
} while (0)

/**
 * Insert a new entry between two known consecutive entries.
 *
 * This is only for internal list manipulation where we know
 * the prev/next entries already!
 */
static inline void __cfs_list_add(cfs_list_t * new,
                                  cfs_list_t * prev,
                                  cfs_list_t * next)
{
	next->prev = new;
	new->next = next;
	new->prev = prev;
	prev->next = new;
}

/**
 * Insert an entry at the start of a list.
 * \param new  new entry to be inserted
 * \param head list to add it to
 *
 * Insert a new entry after the specified head.
 * This is good for implementing stacks.
 */
static inline void cfs_list_add(cfs_list_t *new,
                                cfs_list_t *head)
{
	__cfs_list_add(new, head, head->next);
}

/**
 * Insert an entry at the end of a list.
 * \param new  new entry to be inserted
 * \param head list to add it to
 *
 * Insert a new entry before the specified head.
 * This is useful for implementing queues.
 */
static inline void cfs_list_add_tail(cfs_list_t *new,
                                     cfs_list_t *head)
{
	__cfs_list_add(new, head->prev, head);
}

/*
 * Delete a list entry by making the prev/next entries
 * point to each other.
 *
 * This is only for internal list manipulation where we know
 * the prev/next entries already!
 */
static inline void __cfs_list_del(cfs_list_t *prev,
                                  cfs_list_t *next)
{
	next->prev = prev;
	prev->next = next;
}

/**
 * Remove an entry from the list it is currently in.
 * \param entry the entry to remove
 * Note: list_empty(entry) does not return true after this, the entry is in an
 * undefined state.
 */
static inline void cfs_list_del(cfs_list_t *entry)
{
	__cfs_list_del(entry->prev, entry->next);
}

/**
 * Remove an entry from the list it is currently in and reinitialize it.
 * \param entry the entry to remove.
 */
static inline void cfs_list_del_init(cfs_list_t *entry)
{
	__cfs_list_del(entry->prev, entry->next);
	CFS_INIT_LIST_HEAD(entry);
}

/**
 * Remove an entry from the list it is currently in and insert it at the start
 * of another list.
 * \param list the entry to move
 * \param head the list to move it to
 */
static inline void cfs_list_move(cfs_list_t *list,
                                 cfs_list_t *head)
{
	__cfs_list_del(list->prev, list->next);
	cfs_list_add(list, head);
}

/**
 * Remove an entry from the list it is currently in and insert it at the end of
 * another list.
 * \param list the entry to move
 * \param head the list to move it to
 */
static inline void cfs_list_move_tail(cfs_list_t *list,
                                      cfs_list_t *head)
{
	__cfs_list_del(list->prev, list->next);
	cfs_list_add_tail(list, head);
}

/**
 * Test whether a list is empty
 * \param head the list to test.
 */
static inline int cfs_list_empty(cfs_list_t *head)
{
	return head->next == head;
}

/**
 * Test whether a list is empty and not being modified
 * \param head the list to test
 *
 * Tests whether a list is empty _and_ checks that no other CPU might be
 * in the process of modifying either member (next or prev)
 *
 * NOTE: using cfs_list_empty_careful() without synchronization
 * can only be safe if the only activity that can happen
 * to the list entry is cfs_list_del_init(). Eg. it cannot be used
 * if another CPU could re-list_add() it.
 */
static inline int cfs_list_empty_careful(const cfs_list_t *head)
{
        cfs_list_t *next = head->next;
        return (next == head) && (next == head->prev);
}

static inline void __cfs_list_splice(cfs_list_t *list,
                                     cfs_list_t *head)
{
	cfs_list_t *first = list->next;
	cfs_list_t *last = list->prev;
	cfs_list_t *at = head->next;

	first->prev = head;
	head->next = first;

	last->next = at;
	at->prev = last;
}

/**
 * Join two lists
 * \param list the new list to add.
 * \param head the place to add it in the first list.
 *
 * The contents of \a list are added at the start of \a head.  \a list is in an
 * undefined state on return.
 */
static inline void cfs_list_splice(cfs_list_t *list,
                                   cfs_list_t *head)
{
	if (!cfs_list_empty(list))
		__cfs_list_splice(list, head);
}

/**
 * Join two lists and reinitialise the emptied list.
 * \param list the new list to add.
 * \param head the place to add it in the first list.
 *
 * The contents of \a list are added at the start of \a head.  \a list is empty
 * on return.
 */
static inline void cfs_list_splice_init(cfs_list_t *list,
                                        cfs_list_t *head)
{
	if (!cfs_list_empty(list)) {
		__cfs_list_splice(list, head);
		CFS_INIT_LIST_HEAD(list);
	}
}

/**
 * Get the container of a list
 * \param ptr	 the embedded list.
 * \param type	 the type of the struct this is embedded in.
 * \param member the member name of the list within the struct.
 */
#define cfs_list_entry(ptr, type, member) \
	((type *)((char *)(ptr)-(char *)(&((type *)0)->member)))

/**
 * Iterate over a list
 * \param pos	the iterator
 * \param head	the list to iterate over
 *
 * Behaviour is undefined if \a pos is removed from the list in the body of the
 * loop.
 */
#define cfs_list_for_each(pos, head) \
	for (pos = (head)->next, prefetch(pos->next); pos != (head); \
		pos = pos->next, prefetch(pos->next))

/**
 * Iterate over a list safely
 * \param pos	the iterator
 * \param n     temporary storage
 * \param head	the list to iterate over
 *
 * This is safe to use if \a pos could be removed from the list in the body of
 * the loop.
 */
#define cfs_list_for_each_safe(pos, n, head) \
	for (pos = (head)->next, n = pos->next; pos != (head); \
		pos = n, n = pos->next)

/**
 * Iterate over a list continuing after existing point
 * \param pos    the type * to use as a loop counter
 * \param head   the list head
 * \param member the name of the list_struct within the struct  
 */
#define cfs_list_for_each_entry_continue(pos, head, member)                 \
        for (pos = cfs_list_entry(pos->member.next, typeof(*pos), member);  \
             prefetch(pos->member.next), &pos->member != (head);            \
             pos = cfs_list_entry(pos->member.next, typeof(*pos), member))

/**
 * \defgroup hlist Hash List
 * Double linked lists with a single pointer list head.
 * Mostly useful for hash tables where the two pointer list head is too
 * wasteful.  You lose the ability to access the tail in O(1).
 * @{
 */

typedef struct cfs_hlist_node {
	struct cfs_hlist_node *next, **pprev;
} cfs_hlist_node_t;

typedef struct cfs_hlist_head {
	cfs_hlist_node_t *first;
} cfs_hlist_head_t;

/* @} */

/*
 * "NULL" might not be defined at this point
 */
#ifdef NULL
#define NULL_P NULL
#else
#define NULL_P ((void *)0)
#endif

/**
 * \addtogroup hlist
 * @{
 */

#define CFS_HLIST_HEAD_INIT { NULL_P }
#define CFS_HLIST_HEAD(name) cfs_hlist_head_t name = { NULL_P }
#define CFS_INIT_HLIST_HEAD(ptr) ((ptr)->first = NULL_P)
#define CFS_INIT_HLIST_NODE(ptr) ((ptr)->next = NULL_P, (ptr)->pprev = NULL_P)

static inline int cfs_hlist_unhashed(const cfs_hlist_node_t *h)
{
	return !h->pprev;
}

static inline int cfs_hlist_empty(const cfs_hlist_head_t *h)
{
	return !h->first;
}

static inline void __cfs_hlist_del(cfs_hlist_node_t *n)
{
	cfs_hlist_node_t *next = n->next;
	cfs_hlist_node_t **pprev = n->pprev;
	*pprev = next;
	if (next)
		next->pprev = pprev;
}

static inline void cfs_hlist_del(cfs_hlist_node_t *n)
{
	__cfs_hlist_del(n);
}

static inline void cfs_hlist_del_init(cfs_hlist_node_t *n)
{
	if (n->pprev)  {
		__cfs_hlist_del(n);
		CFS_INIT_HLIST_NODE(n);
	}
}

static inline void cfs_hlist_add_head(cfs_hlist_node_t *n,
                                      cfs_hlist_head_t *h)
{
	cfs_hlist_node_t *first = h->first;
	n->next = first;
	if (first)
		first->pprev = &n->next;
	h->first = n;
	n->pprev = &h->first;
}

/* next must be != NULL */
static inline void cfs_hlist_add_before(cfs_hlist_node_t *n,
					cfs_hlist_node_t *next)
{
	n->pprev = next->pprev;
	n->next = next;
	next->pprev = &n->next;
	*(n->pprev) = n;
}

static inline void cfs_hlist_add_after(cfs_hlist_node_t *n,
                                       cfs_hlist_node_t *next)
{
	next->next = n->next;
	n->next = next;
	next->pprev = &n->next;

	if(next->next)
		next->next->pprev  = &next->next;
}

#define cfs_hlist_entry(ptr, type, member) container_of(ptr,type,member)

#define cfs_hlist_for_each(pos, head) \
	for (pos = (head)->first; pos && (prefetch(pos->next), 1); \
	     pos = pos->next)

#define cfs_hlist_for_each_safe(pos, n, head) \
	for (pos = (head)->first; pos && (n = pos->next, 1); \
	     pos = n)

/**
 * Iterate over an hlist of given type
 * \param tpos	 the type * to use as a loop counter.
 * \param pos	 the &struct hlist_node to use as a loop counter.
 * \param head	 the head for your list.
 * \param member the name of the hlist_node within the struct.
 */
#define cfs_hlist_for_each_entry(tpos, pos, head, member)                    \
	for (pos = (head)->first;                                            \
	     pos && ({ prefetch(pos->next); 1;}) &&                          \
		({ tpos = cfs_hlist_entry(pos, typeof(*tpos), member); 1;}); \
	     pos = pos->next)

/**
 * Iterate over an hlist continuing after existing point
 * \param tpos	 the type * to use as a loop counter.
 * \param pos	 the &struct hlist_node to use as a loop counter.
 * \param member the name of the hlist_node within the struct.
 */
#define cfs_hlist_for_each_entry_continue(tpos, pos, member)                 \
	for (pos = (pos)->next;                                              \
	     pos && ({ prefetch(pos->next); 1;}) &&                          \
		({ tpos = cfs_hlist_entry(pos, typeof(*tpos), member); 1;}); \
	     pos = pos->next)

/**
 * Iterate over an hlist continuing from an existing point
 * \param tpos	 the type * to use as a loop counter.
 * \param pos	 the &struct hlist_node to use as a loop counter.
 * \param member the name of the hlist_node within the struct.
 */
#define cfs_hlist_for_each_entry_from(tpos, pos, member)			 \
	for (; pos && ({ prefetch(pos->next); 1;}) &&                        \
		({ tpos = cfs_hlist_entry(pos, typeof(*tpos), member); 1;}); \
	     pos = pos->next)

/**
 * Iterate over an hlist of given type safe against removal of list entry
 * \param tpos	 the type * to use as a loop counter.
 * \param pos	 the &struct hlist_node to use as a loop counter.
 * \param n	 another &struct hlist_node to use as temporary storage
 * \param head	 the head for your list.
 * \param member the name of the hlist_node within the struct.
 */
#define cfs_hlist_for_each_entry_safe(tpos, pos, n, head, member)            \
	for (pos = (head)->first;                                            \
	     pos && ({ n = pos->next; 1; }) &&                               \
		({ tpos = cfs_hlist_entry(pos, typeof(*tpos), member); 1;}); \
	     pos = n)

/* @} */

#endif /* __linux__ && __KERNEL__ */

#ifndef cfs_list_for_each_prev
/**
 * Iterate over a list in reverse order
 * \param pos	the &struct list_head to use as a loop counter.
 * \param head	the head for your list.
 */
#define cfs_list_for_each_prev(pos, head) \
	for (pos = (head)->prev, prefetch(pos->prev); pos != (head);     \
		pos = pos->prev, prefetch(pos->prev))

#endif /* cfs_list_for_each_prev */

#ifndef cfs_list_for_each_entry
/**
 * Iterate over a list of given type
 * \param pos        the type * to use as a loop counter.
 * \param head       the head for your list.
 * \param member     the name of the list_struct within the struct.
 */
#define cfs_list_for_each_entry(pos, head, member)                          \
        for (pos = cfs_list_entry((head)->next, typeof(*pos), member),      \
		     prefetch(pos->member.next);                            \
	     &pos->member != (head);                                        \
	     pos = cfs_list_entry(pos->member.next, typeof(*pos), member),  \
	     prefetch(pos->member.next))
#endif /* cfs_list_for_each_entry */

#ifndef cfs_list_for_each_entry_rcu
#define cfs_list_for_each_entry_rcu(pos, head, member) \
       list_for_each_entry(pos, head, member)
#endif

#ifndef cfs_list_for_each_entry_rcu
#define cfs_list_for_each_entry_rcu(pos, head, member) \
       list_for_each_entry(pos, head, member)
#endif

#ifndef cfs_list_for_each_entry_reverse
/**
 * Iterate backwards over a list of given type.
 * \param pos        the type * to use as a loop counter.
 * \param head       the head for your list.
 * \param member     the name of the list_struct within the struct.
 */
#define cfs_list_for_each_entry_reverse(pos, head, member)                  \
	for (pos = cfs_list_entry((head)->prev, typeof(*pos), member);      \
	     prefetch(pos->member.prev), &pos->member != (head);            \
	     pos = cfs_list_entry(pos->member.prev, typeof(*pos), member))
#endif /* cfs_list_for_each_entry_reverse */

#ifndef cfs_list_for_each_entry_safe
/**
 * Iterate over a list of given type safe against removal of list entry
 * \param pos        the type * to use as a loop counter.
 * \param n          another type * to use as temporary storage
 * \param head       the head for your list.
 * \param member     the name of the list_struct within the struct.
 */
#define cfs_list_for_each_entry_safe(pos, n, head, member)                   \
        for (pos = cfs_list_entry((head)->next, typeof(*pos), member),       \
		n = cfs_list_entry(pos->member.next, typeof(*pos), member);  \
	     &pos->member != (head);                                         \
	     pos = n, n = cfs_list_entry(n->member.next, typeof(*n), member))

#endif /* cfs_list_for_each_entry_safe */

#ifndef cfs_list_for_each_entry_safe_from
/**
 * Iterate over a list continuing from an existing point
 * \param pos        the type * to use as a loop cursor.
 * \param n          another type * to use as temporary storage
 * \param head       the head for your list.
 * \param member     the name of the list_struct within the struct.
 *
 * Iterate over list of given type from current point, safe against
 * removal of list entry.
 */
#define cfs_list_for_each_entry_safe_from(pos, n, head, member)             \
        for (n = cfs_list_entry(pos->member.next, typeof(*pos), member);    \
             &pos->member != (head);                                        \
             pos = n, n = cfs_list_entry(n->member.next, typeof(*n), member))
#endif /* cfs_list_for_each_entry_safe_from */

#define cfs_list_for_each_entry_typed(pos, head, type, member)		\
        for (pos = cfs_list_entry((head)->next, type, member),		\
		     prefetch(pos->member.next);                        \
	     &pos->member != (head);                                    \
	     pos = cfs_list_entry(pos->member.next, type, member),	\
	     prefetch(pos->member.next))

#define cfs_list_for_each_entry_reverse_typed(pos, head, type, member)	\
	for (pos = cfs_list_entry((head)->prev, type, member);		\
	     prefetch(pos->member.prev), &pos->member != (head);	\
	     pos = cfs_list_entry(pos->member.prev, type, member))

#define cfs_list_for_each_entry_safe_typed(pos, n, head, type, member)	\
    for (pos = cfs_list_entry((head)->next, type, member),		\
		n = cfs_list_entry(pos->member.next, type, member);	\
	     &pos->member != (head);                                    \
	     pos = n, n = cfs_list_entry(n->member.next, type, member))

#define cfs_list_for_each_entry_safe_from_typed(pos, n, head, type, member)  \
        for (n = cfs_list_entry(pos->member.next, type, member);             \
             &pos->member != (head);                                         \
             pos = n, n = cfs_list_entry(n->member.next, type, member))

#define cfs_hlist_for_each_entry_typed(tpos, pos, head, type, member)   \
	for (pos = (head)->first;                                       \
	     pos && (prefetch(pos->next), 1) &&                         \
		(tpos = cfs_hlist_entry(pos, type, member), 1);         \
	     pos = pos->next)

#define cfs_hlist_for_each_entry_safe_typed(tpos, pos, n, head, type, member) \
	for (pos = (head)->first;                                             \
	     pos && (n = pos->next, 1) &&                                     \
		(tpos = cfs_hlist_entry(pos, type, member), 1);               \
	     pos = n)

#ifdef HAVE_HLIST_ADD_AFTER
#define hlist_add_behind(hnode, tail)  hlist_add_after(tail, hnode)
#endif /* HAVE_HLIST_ADD_AFTER */

#endif /* __LIBCFS_LUSTRE_LIST_H__ */
