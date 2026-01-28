/* Singly-linked list
 * – Two sentinel nodes (head and tail) eliminate edge-case tests.
 * – The list stores generic data pointers ('void *').
 * – All primitives are defined 'static inline' so they can live entirely
 *   in the header without multiple-definition issues.
 */

#pragma once

#include <lib/libc.h>
#include <lib/malloc.h>

#include "private/utils.h"

/* List node */
typedef struct list_node {
    struct list_node *next;
} list_node_t;

/* Public list descriptor */
typedef struct {
    list_node_t *head; /* dummy head sentinel (always non-NULL) */
    list_node_t *tail; /* dummy tail sentinel (always last) */
    size_t length;     /* number of data nodes */
} list_t;

static inline list_t *list_create(void)
{
    list_t *list = malloc(sizeof(*list));
    if (unlikely(!list))
        return NULL;

    list_node_t *head = malloc(sizeof(*head));
    if (unlikely(!head)) {
        free(list);
        return NULL;
    }

    list_node_t *tail = malloc(sizeof(*tail));
    if (unlikely(!tail)) {
        free(head);
        free(list);
        return NULL;
    }

    head->next = tail;

    tail->next = NULL;

    list->head = head;
    list->tail = tail;
    list->length = 0U;
    return list;
}

static inline int list_is_empty(const list_t *list)
{
    return !list || list->length == 0U;
}

/* Safe single-step successor: returns NULL for tail or NULL input */
static inline list_node_t *list_next(const list_node_t *node)
{
    return (node && node->next) ? node->next : NULL;
}

/* Circular next: wraps from tail sentinel to first data node */
static inline list_node_t *list_cnext(const list_t *list,
                                      const list_node_t *node)
{
    if (unlikely(!list || !node))
        return NULL;
    return (node->next == list->tail) ? list->head->next : node->next;
}

/* Push and pop */

static inline list_node_t *list_pushback(list_t *list, list_node_t *node)
{
    if (unlikely(!list || !node || node->next))
        return NULL;

    node->next = list->tail;

    /* Insert before tail sentinel */
    list_node_t *prev = list->head;
    while (prev->next != list->tail)
        prev = prev->next;
    prev->next = node;

    list->length++;
    return node;
}

static inline list_node_t *list_pop(list_t *list)
{
    if (unlikely(list_is_empty(list)))
        return NULL;

    list_node_t *first = list->head->next;
    list->head->next = first->next;
    first->next = NULL;

    list->length--;
    return first;
}

/* Remove a specific node from the list */
static inline list_node_t *list_remove(list_t *list, list_node_t *target)
{
    if (unlikely(!list || !target || list_is_empty(list)))
        return NULL;

    list_node_t *prev = list->head;
    while (prev->next != list->tail && prev->next != target)
        prev = prev->next;

    if (unlikely(prev->next != target))
        return NULL; /* node not found */

    prev->next = target->next;
    target->next = NULL;
    list->length--;
    return target;
}

/* Iteration */

/* Callback should return non-NULL to stop early, NULL to continue */
static inline list_node_t *list_foreach(list_t *list,
                                        list_node_t *(*cb)(list_node_t *,
                                                           void *),
                                        void *arg)
{
    if (unlikely(!list || !cb))
        return NULL;

    list_node_t *node = list->head->next;
    while (node != list->tail) {
        list_node_t *next = node->next; /* Save next before callback */
        list_node_t *res = cb(node, arg);
        if (res)
            return res;
        node = next;
    }
    return NULL;
}

/* Clear and destroy */

static inline void list_clear(list_t *list)
{
    if (unlikely(!list))
        return;
    while (!list_is_empty(list))
        list_pop(list);
}

static inline void list_destroy(list_t *list)
{
    if (!list)
        return;
    list_clear(list);
    free(list->head);
    free(list->tail);
    free(list);
}
