/* libleptris - Memory management implementation
 * Copyright (c) 2024, Ribose Inc.
 * All rights reserved.
 */

#include "leptris_memory.h"
#include "dom/element.h"
#include "memory/pool.h"
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Attribute Management (moved from old location for compatibility)
 * ============================================================================ */

struct leptris_attribute* leptris_attribute_new(const char* name, const char* value) {
    if (!name) return NULL;

    struct leptris_attribute* attr = LEPTRIS_ALLOC(struct leptris_attribute);
    if (!attr) return NULL;
    /* Zero the whole struct: next_cp/has_entities/name_hash have no
     * owners on this heap-allocated lifecycle. */
    memset(attr, 0, sizeof(*attr));

    /* Single representation (TODO 184 round 4): owned NUL-terminated
     * copies stored AS views. leptris_attribute_free releases them. */
    char* name_copy = leptris_strdup(name);
    if (!name_copy) {
        free(attr);
        return NULL;
    }
    attr->name_view = leptris_sv_from_cstr(name_copy);

    if (value) {
        char* value_copy = leptris_strdup(value);
        if (!value_copy) {
            free(name_copy);
            free(attr);
            return NULL;
        }
        leptris_attr_value_set_heap(attr, leptris_sv_from_cstr(value_copy));
    }

    return attr;
}

void leptris_attribute_free(struct leptris_attribute* attr) {
    if (!attr) return;

    /* The views hold owned heap copies on this lifecycle — cast away
     * const for free (the data IS mutable storage; the view type is
     * const only for read-side safety). */
    free((void*)(uintptr_t)attr->name_view.data);
    struct leptris_attr_ns_cache* nsc = attr_get_ns_cache(attr);
    if (nsc) {
        if (nsc->prefix) free(nsc->prefix);
        if (nsc->namespace_uri) free(nsc->namespace_uri);
        free(nsc);
    }
    free((void*)(uintptr_t)leptris_attr_value_sv(attr).data);

    free(attr);
}

/* ============================================================================
 * Namespace Management
 * ============================================================================ */

struct leptris_namespace* leptris_namespace_new(const char* prefix, const char* uri) {
    if (!uri) return NULL;

    struct leptris_namespace* ns = LEPTRIS_ALLOC(struct leptris_namespace);
    if (!ns) return NULL;

    ns->prefix = prefix ? leptris_strdup(prefix) : NULL;
    ns->uri = leptris_strdup(uri);
    ns->next = NULL;

    if (!ns->uri || (prefix && !ns->prefix)) {
        if (ns->prefix) free(ns->prefix);
        if (ns->uri) free(ns->uri);
        free(ns);
        return NULL;
    }

    return ns;
}

struct leptris_namespace* leptris_namespace_new_pooled(const char* prefix,
                                                      const char* uri,
                                                      LeptrisMemoryPool* pool) {
    if (!uri || !pool) return NULL;

    struct leptris_namespace* ns = (struct leptris_namespace*)leptris_pool_alloc(pool, sizeof(struct leptris_namespace));
    if (!ns) return NULL;

    ns->prefix = prefix ? leptris_pool_strdup(pool, prefix) : NULL;
    ns->uri = leptris_pool_strdup(pool, uri);
    ns->next = NULL;

    /* If allocation failed, return NULL - pool will be cleaned up on document free */
    if (!ns->uri) {
        return NULL;
    }

    return ns;
}

void leptris_namespace_free_single(struct leptris_namespace* ns) {
    if (!ns) return;

    if (ns->prefix) free(ns->prefix);
    if (ns->uri) free(ns->uri);
    free(ns);
}

void leptris_namespace_free_chain(struct leptris_namespace* ns) {
    struct leptris_namespace* next;

    while (ns) {
        next = ns->next;
        leptris_namespace_free_single(ns);
        ns = next;
    }
}

struct leptris_namespace* leptris_namespace_find(struct leptris_element* elem, const char* prefix) {
    struct leptris_namespace* ns;

    if (!elem) return NULL;

    /* Check current element */
    ns = leptris_elem_namespaces(elem);
    while (ns) {
        if ((prefix == NULL && ns->prefix == NULL) ||
            (prefix && ns->prefix && strcmp(prefix, ns->prefix) == 0)) {
            return ns;
        }
        ns = ns->next;
    }

    /* Search parent */
    struct leptris_element* parent = leptris_element_get_parent(elem);
    if (parent) {
        return leptris_namespace_find(parent, prefix);
    }

    return NULL;
}

/* Add namespace to element's namespace linked list.
 *
 * Issue #171: append in source order so leptris_element_namespace_decl_*
 * returns declarations in the order they appear in the document.
 * Previously this prepended, giving consumers a reversed view. */
int leptris_element_add_namespace(struct leptris_element* elem, struct leptris_namespace* ns) {
    /* Issue #525: a namespace declaration now exists — mark the
     * document so unprefixed XPath name tests consult the resolver. */
    if (elem && ns) {
        struct leptris_document* d = leptris_element_get_document((LeptrisElement)elem);
        if (d) d->has_namespaces = 1;
    }
    if (!elem || !ns) return -1;

    ns->next = NULL;
    /* ns_cache is required to hold the declarations head. Allocate
     * on demand from the document's pool. TODO 155 Phase B. */
    struct leptris_memory_pool* pool = leptris_element_get_pool(elem);
    struct leptris_namespace** head_ptr = leptris_elem_namespaces_ptr(elem, pool);
    if (!head_ptr) return -1;
    if (!*head_ptr) {
        *head_ptr = ns;
        return 0;
    }
    struct leptris_namespace* tail = *head_ptr;
    while (tail->next) tail = tail->next;
    tail->next = ns;
    return 0;
}
