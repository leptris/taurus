/* lib/src/dom/element.c - Element node implementation (COMPACT ONLY)
 * Copyright (c) 2024, Ribose Inc.
 *
 * HYBRID ARCHITECTURE:
 * Uses regular pointers for hot navigation paths (parent, first_child, next_sibling)
 * to achieve 1.2x performance target vs pugixml.
 * - ~117 bytes per element (vs 192 bytes in legacy design = 1.6x reduction!)
 * - Direct pointer access (no page_base calculation)
 * - Better cache locality than pure compact design
 * - Compact pointers only for cold paths (attributes)
 *
 * This is the ONLY element implementation - no backwards compatibility.
 */

#include "element.h"
#include "compact.h"
#include "root_doc_map.h"
#include "text.h"
#include "comment.h"
#include "cdata.h"
#include "pi.h"
#include "../../include/leptris.h"  /* for LEPTRIS_API on public exports */
#include "doctype.h"
#include "node.h"  /* For leptris_node_get_next_sibling */
#include "../common/entities.h"
#include "../common/string_view.h"
#include "../memory/pool.h"
#include "../leptris_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* From leptris_memory.c — declared here to avoid pulling in the
 * full leptris_memory.h, which conflicts with pi.h's leptris_pi_free. */
int leptris_element_add_namespace(struct leptris_element* elem, struct leptris_namespace* ns);
struct leptris_namespace* leptris_namespace_new_pooled(const char* prefix, const char* uri, LeptrisMemoryPool* pool);

/* ============================================================================
 * Element Creation
 * ============================================================================ */

/* Create element with StringView (true zero-copy!) */
LeptrisElement leptris_element_create_with_view(
    LeptrisStringView name_view,
    LeptrisMemoryPool* pool
) {
    if (leptris_sv_is_empty(&name_view) || !pool) return NULL;

    /* Allocate + zero element from pool. memset handles ALL field
     * initialization to 0/NULL. Only type and name need non-zero
     * values. This eliminates ~20 redundant stores that the old
     * per-field init did after memset. */
    LeptrisElement elem = (LeptrisElement)leptris_pool_alloc(pool, sizeof(struct leptris_element));
    if (!elem) return NULL;

    memset(elem, 0, sizeof(struct leptris_element));
    elem->base.type = LEPTRIS_NODE_TYPE_ELEMENT;
    elem->name = leptris_sv_to_cstr_pooled(&name_view, pool);
    elem->name_hash = leptris_name_hash_compute(elem->name);
    elem->name_len = (name_view.length > 254)
        ? 0xFF : (uint8_t)name_view.length;
    /* Pool copy is writable: a QName splits in place (#846). */
    leptris_elem_split_qname(elem, pool);

    return elem;
}

/* Create element with in-place string (zero-copy) */
LeptrisElement leptris_element_create_pooled_inplace(char* name, LeptrisMemoryPool* pool) {
    if (!name || !pool) return NULL;

    /* Create StringView from in-place string */
    LeptrisStringView name_view = leptris_sv_from_cstr(name);
    return leptris_element_create_with_view(name_view, pool);
}

/* Create an element skeleton with no name — for the deferred-NUL
 * zero-copy parser path (TODO 113 Phase 5). The parser fills in
 * elem->name/prefix after consuming the opening tag's terminator,
 * at which point writing a NUL at name_view.data[name_view.length]
 * is safe (the parser has moved past that byte).
 *
 * Caller MUST finalize via the parser before any code reads name. */
LeptrisElement leptris_element_create_zero_copy(LeptrisMemoryPool* pool) {
    if (!pool) return NULL;

    LeptrisElement elem = (LeptrisElement)leptris_pool_alloc(
        pool, sizeof(struct leptris_element));
    if (!elem) return NULL;

    memset(elem, 0, sizeof(struct leptris_element));
    elem->base.type = LEPTRIS_NODE_TYPE_ELEMENT;
    return elem;
}

/* Create element using memory pool.
 *
 * The name is COPIED into the pool — callers can free or reuse their
 * buffer immediately.  This is the safe contract for the public
 * leptris_element_create() and for cross-document copies; the parser
 * uses leptris_element_create_with_view directly for true zero-copy. */
LeptrisElement leptris_element_create_pooled(const char* name, LeptrisMemoryPool* pool) {
    if (!name || !pool) return NULL;

    /* Pool-owned copy; lives for the document's lifetime. */
    char* name_copy = leptris_pool_strdup(pool, name);
    if (!name_copy) return NULL;

    /* Hand ownership to create_with_view via the inplace path so the
     * name_view points into pool-owned storage, not the caller's buffer. */
    return leptris_element_create_pooled_inplace(name_copy, pool);
}

/* Free element - pool allocated, so this is a no-op */
void leptris_element_free(LeptrisElement elem) {
    /* Pool-allocated elements don't need individual free */
    /* This function exists for API compatibility only */
    (void)elem;
}

/* ============================================================================
 * Regular Pointer Access Functions (no page_base calculation!)
 * ============================================================================ */

/* Legacy public setter entrypoints — delegate to the compact-offset
 * setters in element.h (TODO 90 Phase 2b). Kept as out-of-line
 * symbols so the public API doesn't depend on the inline bodies. */
void leptris_element_set_parent(LeptrisElement elem, LeptrisElement parent) {
    leptris_elem_set_parent(elem, parent);
}

void leptris_element_set_first_child(LeptrisElement elem, LeptrisElement child) {
    leptris_elem_set_first_child(elem, (LeptrisNode*)child);
}

void leptris_element_set_last_child(LeptrisElement elem, LeptrisElement child) {
    leptris_elem_set_last_child(elem, (LeptrisNode*)child);
}

void leptris_element_set_next_sibling(LeptrisElement elem, LeptrisElement sibling) {
    leptris_elem_set_next_sibling(elem, (LeptrisNode*)sibling);
}

/* ============================================================================
 * Attribute Access Functions (still uses compact pointers - cold path)
 * ============================================================================ */

/* Get first attribute — decodes the int32_t offset (TODO 90 Phase 2d). */
struct leptris_attribute* leptris_element_get_first_attribute(LeptrisElement elem) {
    return leptris_elem_first_attribute(elem);
}

/* Set first attribute — encodes the int32_t offset (TODO 90 Phase 2d). */
void leptris_element_set_first_attribute(LeptrisElement elem, struct leptris_attribute* attr) {
    leptris_elem_set_first_attribute(elem, attr);
}

/* Get attribute by index */
struct leptris_attribute* leptris_element_get_attribute_by_index(LeptrisElement elem, uint8_t index) {
    if (!elem || index >= elem->attr_count) return NULL;

    /* Walk the linked list to find the attribute at index */
    struct leptris_attribute* attr = leptris_element_get_first_attribute(elem);
    for (uint8_t i = 0; i < index && attr; i++) {
        /* Validate attr pointer before accessing next */
        if ((uintptr_t)attr < 0x1000) return NULL;  /* Invalid pointer */
        attr = leptris_attr_next(attr);
    }

    /* Final validation before returning */
    if ((uintptr_t)attr < 0x1000) return NULL;

    return attr;
}

/* Get attribute by name */
struct leptris_attribute* leptris_element_get_attribute_by_name(LeptrisElement elem, const char* name) {
    if (!elem || !name) return NULL;

    /* Hash-filtered attribute lookup (TODO 113 Phase 4).
     * Compute the 15-bit hash of the search name once (round 19 —
     * attr_name_hash truncates identically via attr_hash15), then
     * compare small integers in the loop before touching string
     * data. This turns O(N × strlen) into O(N × uint16) for the
     * non-matching case. */
    size_t name_len = strlen(name);
    uint16_t name_hash = attr_hash15(name, name_len);

    struct leptris_attribute* attr = leptris_element_get_first_attribute(elem);
    while (attr) {
        /* Hash pre-filter: reject most non-matching attrs in one
         * integer comparison. Lazy compute on first read (TODO 172).
         * Only when hash AND length match do we do the full memcmp. */
        if (attr_name_hash(attr) == name_hash &&
            attr->name_view.length == name_len) {
            if (!leptris_sv_is_empty(&attr->name_view) &&
                memcmp(attr->name_view.data, name, name_len) == 0) {
                return attr;
            }
        }
        attr = leptris_attr_next(attr);
    }

    return NULL;
}

/* Get attribute by name (StringView version - internal, faster)
 * This is 2-3x faster than the C string version because:
 * 1. No strlen() call needed
 * 2. Uses length-based comparison first (O(1) mismatch check)
 * 3. No C string conversion needed
 */
struct leptris_attribute* leptris_element_get_attribute_by_name_view(LeptrisElement elem, LeptrisStringView name) {
    if (!elem || leptris_sv_is_empty(&name)) return NULL;

    /* Walk the attribute linked list */
    struct leptris_attribute* attr = leptris_element_get_first_attribute(elem);
    while (attr) {
        /* Direct StringView comparison (O(1) length check + memcmp) */
        if (leptris_sv_equals(&attr->name_view, &name)) {
            return attr;
        }
        attr = leptris_attr_next(attr);
    }

    return NULL;
}

/* Add attribute to element */
int leptris_element_add_attribute(LeptrisElement elem,
                                LeptrisStringView name_view,
                                LeptrisStringView value_view,
                                LeptrisMemoryPool* pool) {
    if (!elem || leptris_sv_is_empty(&name_view) || !pool) return -1;

    /* Allocate attribute from pool */
    struct leptris_attribute* attr = (struct leptris_attribute*)leptris_pool_alloc(
        pool, sizeof(struct leptris_attribute));
    if (!attr) return -1;

    /* Initialize attribute */
    attr->name_view = name_view;
    attr->value_view = value_view;

    /* Pre-compute the 15-bit hash of the attribute name for O(1)
     * lookup filtering (TODO 113 Phase 4). Entity flag starts
     * clear; the decode below sets it only when expansion fails.
     * Round 19: hash and flag share the field (bit 15). */
    attr->name_hash = attr_hash15(name_view.data, name_view.length);

    /* CRITICAL FIX: Initialize namespace/prefix fields to prevent stale data.
     * TODO 173: these now live in the side-cache struct (ns_cache_off).
     * 0 = no prefix / namespace_uri = empty/NULL. */
    attr->ns_cache_off = 0;

    /* EAGER STRING CONVERSION: Convert attribute name and value to NULL-terminated C-strings.
     *
     * PERFORMANCE (TODO 22): attribute NAMES are interned via the pool's
     * hash table (they recur across elements, so dedup pays off).
     * attribute VALUES bypass interning — values are almost always
     * unique per element, so the hash-table lookup/insert cost is
     * wasted.  Direct pool allocation instead.
     *
     * CRITICAL: Decode XML entities in attribute values BEFORE converting to C string.
     * Entities like &lt; &gt; &amp; must be decoded to < > & during parsing. */
    /* SINGLE representation (TODO 184 round 4): the caller's views may
     * point at temporary memory, so this path REPLACES them with owned
     * pool copies. Parse-path attrs keep their zero-copy buffer views.
     *
     * Names: interned via the pool's hash table (they recur across
     * elements). Values: direct pool allocation (values are almost
     * always unique — interning cost is wasted).
     *
     * Entities decode BEFORE storing; &lt; &gt; &amp; must read as
     * < > & in the value. */
    char* name_storage = leptris_sv_to_cstr_pooled(&name_view, pool);
    if (name_storage) {
        attr->name_view = leptris_sv_from_ptr(
            name_storage, name_view.length);
    }

    /* Allocate value storage directly from the pool — no interning. */
    char* value_storage = (char*)leptris_pool_alloc(pool, value_view.length + 1);
    if (value_storage) {
        memcpy(value_storage, value_view.data, value_view.length);
        value_storage[value_view.length] = '\0';

        /* Check if value contains entities and decode them */
        if (memchr(value_storage, '&', value_view.length) != NULL) {
            /* Value contains entities - decode them first.
             * leptris_decode_entities_view allocates from the pool, so
             * the value_storage we just allocated becomes garbage
             * (pool will reclaim it eventually).  This is a minor
             * waste; entity-bearing attribute values are rare. */
            LeptrisStringView decoded_sv = { value_storage, value_view.length };
            char* decoded = leptris_decode_entities_view(&decoded_sv, pool);
            if (decoded) {
                attr->value_view = leptris_sv_from_cstr(decoded);
            } else {
                attr->value_view =
                    leptris_sv_from_ptr(value_storage, value_view.length);
                attr_set_entities(attr, 1);
            }
        } else {
            attr->value_view =
                leptris_sv_from_ptr(value_storage, value_view.length);
        }
    } else {
        attr->value_view = leptris_sv_from_ptr(NULL, 0);
    }

    leptris_attr_set_next(attr, NULL);

    /* Append via cached last_attribute pointer (TODO 106).
     * Maintains the same invariant as leptris_element_set_attribute.
     * Decode the int32_t offset to access the struct, then update via
     * the encoder (TODO 90 Phase 2d). */
    struct leptris_attribute* last = leptris_elem_last_attribute(elem);
    if (last) {
        leptris_attr_set_next(last, attr);
    } else {
        leptris_elem_set_first_attribute(elem, attr);
    }
    leptris_elem_set_last_attribute(elem, attr);

    /* Increment attribute count */
    elem->attr_count++;

    return 0;
}

/* Add attribute in deferred-NUL mode (TODO 113 Phase 5).
 *
 * Mirrors leptris_element_add_attribute but leaves attr->name and
 * attr->value NULL when no entity decoding is needed. The parser
 * fills them in by NUL-terminating in the writable XML buffer after
 * the opening tag is consumed — saving two pool allocations per
 * attribute on the common (no-entity) path. Entity-bearing values
 * are still pool-allocated eagerly (rare path, can't be zero-copied
 * because the decoded bytes don't exist in the source buffer). */
int leptris_element_add_attribute_zero_copy(LeptrisElement elem,
                                           LeptrisStringView name_view,
                                           LeptrisStringView value_view,
                                           LeptrisMemoryPool* pool) {
    if (!elem || leptris_sv_is_empty(&name_view) || !pool) return -1;

    struct leptris_attribute* attr = (struct leptris_attribute*)leptris_pool_alloc(
        pool, sizeof(struct leptris_attribute));
    if (!attr) return -1;

    attr->name_view = name_view;
    attr->value_view = value_view;

    attr->name_hash = attr_hash15(name_view.data, name_view.length);

    attr->ns_cache_off = 0;  /* TODO 173: side cache allocated on demand. */

    if (memchr(value_view.data, '&', value_view.length) != NULL) {
        char* value_storage = (char*)leptris_pool_alloc(pool, value_view.length + 1);
        if (value_storage) {
            memcpy(value_storage, value_view.data, value_view.length);
            value_storage[value_view.length] = '\0';
            LeptrisStringView decoded_sv = { value_storage, value_view.length };
            char* decoded = leptris_decode_entities_view(&decoded_sv, pool);
            if (decoded) {
                attr->value_view = leptris_sv_from_cstr(decoded);
                attr_set_entities(attr, 0);
            } else {
                attr->value_view =
                    leptris_sv_from_ptr(value_storage, value_view.length);
                attr_set_entities(attr, 1);
            }
        } else {
            attr_set_entities(attr, 0);
        }
    } else {
        attr_set_entities(attr, 0);
    }

    leptris_attr_set_next(attr, NULL);

    struct leptris_attribute* last = leptris_elem_last_attribute(elem);
    if (last) {
        leptris_attr_set_next(last, attr);
    } else {
        leptris_elem_set_first_attribute(elem, attr);
    }
    leptris_elem_set_last_attribute(elem, attr);

    elem->attr_count++;
    return 0;
}

/* ============================================================================
 * Name and Namespace Access
 * ============================================================================ */

/* Get element name (lazy conversion from StringView to C string) */
const char* leptris_element_get_name(LeptrisElement elem) {
    if (!elem) return NULL;

    /* Validate node type - only elements have names */
    LeptrisNode* node = (LeptrisNode*)elem;
    if (node->type != LEPTRIS_NODE_TYPE_ELEMENT) {
        return NULL;  /* Not an element node */
    }

    /* name_view removed (TODO 90) — name is set eagerly by create_with_view. */
    return elem->name;
}

/* Set prefix using StringView (zero-copy!) */

/* Set namespace URI from StringView (eager pool-strdup — no lazy staging).
 * TODO 90: namespace_uri_view removed from struct; this setter is now
 * the only path. It does the conversion eagerly so subsequent
 * leptris_element_get_namespace_uri() just returns the cached char*. */
void leptris_element_set_namespace_uri_view(LeptrisElement elem, LeptrisStringView uri_view) {
    if (!elem) return;
    if (leptris_sv_is_empty(&uri_view)) {
        if (elem->ns_cache) elem->ns_cache->namespace_uri = NULL;
        return;
    }
    /* Issue #525: a namespace now exists — unprefixed XPath name
     * tests must consult the resolver from here on. */
    {
        struct leptris_document* d = leptris_element_get_document(elem);
        if (d) d->has_namespaces = 1;
    }
    LeptrisMemoryPool* pool = leptris_element_get_pool(elem);
    if (pool) {
        char* storage = (char*)leptris_pool_alloc(pool, uri_view.length + 1);
        if (!storage) return;
        memcpy(storage, uri_view.data, uri_view.length);
        storage[uri_view.length] = '\0';
        leptris_elem_set_ns_uri(elem, storage, pool);
    } else {
        leptris_elem_set_ns_uri(elem, (char*)uri_view.data, NULL);
    }
}

/* Get element prefix */
const char* leptris_element_get_prefix(LeptrisElement elem) {
    return leptris_elem_prefix(elem);
}

/* Get element namespace URI */
const char* leptris_element_get_namespace_uri(LeptrisElement elem) {
    if (!elem) return NULL;

    char* cached = leptris_elem_ns_uri(elem);
    if (cached) return cached;

    /* LAZY NAMESPACE RESOLUTION: resolve via ancestors. An
     * xmlns="" UNDECLARATION resolves to the empty string — the
     * element deliberately has NO namespace (the #817 detach and
     * the default-ns undeclare path), not an empty-URI one. */
    const char* prefix = leptris_elem_prefix(elem);
    const char* uri = leptris_element_lookup_namespace(elem, prefix);
    if (uri && !*uri) return NULL;
    if (uri) {
        LeptrisMemoryPool* pool = leptris_element_get_pool(elem);
        if (pool) {
            size_t len = strlen(uri);
            char* copy = (char*)leptris_pool_alloc(pool, len + 1);
            if (copy) {
                memcpy(copy, uri, len);
                copy[len] = '\0';
                leptris_elem_set_ns_uri(elem, copy, pool);
            }
        } else {
            leptris_elem_set_ns_uri(elem, leptris_strdup(uri), NULL);
        }
    }
    return leptris_elem_ns_uri(elem);
}


/* Legacy functions for C string input.
 *
 * POOL-STORAGE (Linux LSan leak): these used to heap-strdup into a
 * POOL-allocated cache — document_free never freed the strings, so
 * every XSLT result element with a namespace leaked them. The
 * copies are pool-allocated now: the pool owns cache AND strings
 * and frees both at teardown, so a re-set simply orphans the old
 * pool bytes (bounded; prefix/URI re-assignment is rare). Pool-less
 * (detached) elements keep the heap copy — nothing else could own
 * it — which the (rare) explicit free contract covers. */
void leptris_element_set_prefix(LeptrisElement elem, const char* prefix) {
    if (!elem) return;
    LeptrisMemoryPool* pool = leptris_element_get_pool(elem);
    char* copy = (prefix && *prefix)
        ? (pool ? leptris_pool_strdup(pool, prefix)
                : leptris_strdup(prefix))
        : NULL;
    leptris_elem_set_prefix(elem, copy, pool);
}

void leptris_element_set_namespace_uri(LeptrisElement elem, const char* uri) {
    if (!elem) return;
    LeptrisMemoryPool* pool = leptris_element_get_pool(elem);
    char* copy = (uri && *uri)
        ? (pool ? leptris_pool_strdup(pool, uri)
                : leptris_strdup(uri))
        : NULL;
    leptris_elem_set_ns_uri(elem, copy, pool);
}

/* ============================================================================
 * Child Access Functions
 * ============================================================================ */

/* Get child count */
size_t leptris_element_child_count(LeptrisElement elem) {
    if (!elem) return 0;
    return elem->child_count;
}

/* Get child by index */
LeptrisElement leptris_element_get_child(LeptrisElement elem, uint16_t index) {
    if (!elem || index >= elem->child_count) return NULL;

    /* Walk the child linked list */
    LeptrisElement child = leptris_element_get_first_child(elem);
    /* size_t counter: a uint8_t wraps for index > 255 and the walk
     * never terminates early as intended (CodeQL comparison-with-
     * wider-type). */
    for (size_t i = 0; i < index && child; i++) {
        child = leptris_element_get_next_sibling(child);
    }

    return child;
}

/* ============================================================================
 * Legacy Public API Functions (compatibility wrappers)
 * ============================================================================ */

/* Add attribute (legacy C string API) */
void leptris_element_add_attribute_legacy(
    LeptrisElement elem,
    const char* name,
    const char* value
) {
    if (!elem || !name) return;

    /* Create StringViews from C strings */
    LeptrisStringView name_view = leptris_sv_from_cstr(name);
    LeptrisStringView value_view = leptris_sv_from_cstr(value ? value : "");

    /* For now, we need a pool. Use element's document pool if available */
    LeptrisMemoryPool* pool = leptris_element_get_pool(elem);
    if (!pool) {
        /* No pool available - can't add attribute in compact mode */
        return;
    }

    leptris_element_add_attribute(elem, name_view, value_view, pool);
}

/* Add attribute using memory pool (fast) */
void leptris_element_add_attribute_pooled(
    LeptrisElement elem,
    const char* name,
    const char* value,
    LeptrisMemoryPool* pool
) {
    if (!elem || !name || !pool) return;

    /* Create StringViews from C strings */
    LeptrisStringView name_view = leptris_sv_from_cstr(name);
    LeptrisStringView value_view = leptris_sv_from_cstr(value ? value : "");

    leptris_element_add_attribute(elem, name_view, value_view, pool);
}

/* Add attribute with in-place strings (zero-copy) */
void leptris_element_add_attribute_pooled_inplace(
    LeptrisElement elem,
    char* name,
    char* value,
    LeptrisMemoryPool* pool
) {
    if (!elem || !name || !pool) return;

    /* Create StringViews from in-place strings */
    LeptrisStringView name_view = leptris_sv_from_cstr(name);
    LeptrisStringView value_view = leptris_sv_from_cstr(value ? value : "");

    leptris_element_add_attribute(elem, name_view, value_view, pool);
}

/* Get attribute value by name (legacy API) */
const char* leptris_element_get_attribute_legacy(LeptrisElement elem, const char* name) {
    if (!elem || !name) return NULL;

    struct leptris_attribute* attr = leptris_element_get_attribute_by_name(elem, name);
    if (!attr) return NULL;

    /* Single representation (TODO 184 round 4): entity values expand
     * lazily into the view (owned copy); no-entity views are already
     * NUL-terminated. */
    if (attr_has_entities(attr) && !leptris_sv_is_empty(&attr->value_view)) {
        if (leptris_element_get_document(elem) && leptris_element_get_pool(elem)) {
            char* decoded = leptris_decode_entities_view(
                &attr->value_view, leptris_element_get_pool(elem));
            if (decoded) {
                attr->value_view = leptris_sv_from_cstr(decoded);
                attr_set_entities(attr, 0);
            }
        } else {
            char* expanded = leptris_sv_to_cstr(&attr->value_view);
            if (expanded) {
                attr->value_view = leptris_sv_from_cstr(expanded);
                attr_set_entities(attr, 0);
            }
        }
    }

    return attr->value_view.data;
}

/* Add namespace with in-place strings (zero-copy).
 *
 * Stores prefix/uri in the element's own namespace-declaration fields
 * AND adds a leptris_namespace entry to elem->namespaces so that
 * leptris_element_lookup_namespace() can find it. The strings are
 * NOT copied; caller must ensure they outlive the document (typically
 * pool-allocated). */
void leptris_element_add_namespace_inplace(LeptrisElement elem,
                                           char* prefix,
                                           char* uri,
                                           LeptrisMemoryPool* pool) {
    if (!elem || !uri) return;

    if (prefix) {
        leptris_elem_set_prefix(elem, prefix, pool);
    }
    leptris_elem_set_ns_uri(elem, uri, pool);

    /* Also register the namespace declaration on elem->namespaces so
     * descendant lookups via leptris_element_lookup_namespace find it.
     * If pool allocation fails, the in-place fields are still set; the
     * namespace just won't be discoverable via prefix lookup. */
    if (pool) {
        struct leptris_namespace* ns = leptris_namespace_new_pooled(prefix, uri, pool);
        if (ns) {
            leptris_element_add_namespace(elem, ns);
        }
    }
}

/* Lookup namespace URI by prefix */
const char* leptris_element_lookup_namespace(LeptrisElement elem,
                                             const char* prefix) {
    if (!elem) return NULL;

    /* Search namespaces linked list on current element */
    struct leptris_namespace* ns = leptris_elem_namespaces(elem);
    while (ns) {
        if ((prefix == NULL && ns->prefix == NULL) ||
            (prefix && ns->prefix && strcmp(prefix, ns->prefix) == 0)) {
            return ns->uri;
        }
        ns = ns->next;
    }

    /* Search parent */
    LeptrisElement parent = leptris_element_get_parent(elem);
    if (parent) {
        return leptris_element_lookup_namespace(parent, prefix);
    }

    return NULL;
}

/* Children manipulation */
void leptris_element_append_child_internal_doc(LeptrisElement elem, LeptrisNode* child,
                                              struct leptris_document* doc);
void leptris_element_append_child_internal(LeptrisElement elem, LeptrisNode* child) {
    if (!elem || !child) return;
    leptris_element_append_child_internal_doc(elem, child,
                                             leptris_element_get_document(elem));
}

/* doc is the caller-resolved document (NULL for detached trees) —
 * the public mutation path resolves it once instead of paying the
 * root walk + map lookup on every append (TODO 195c). */
void leptris_element_append_child_internal_doc(LeptrisElement elem, LeptrisNode* child,
                                              struct leptris_document* doc) {
    if (!elem || !child) return;

    /* SAFETY: Verify node type field is valid before accessing it
     * Small values like 0x4 or 0x1 in the type field suggest the pointer is corrupted */
    if (child->type < LEPTRIS_NODE_TYPE_ELEMENT ||
        child->type > LEPTRIS_NODE_TYPE_DOCTYPE) {
        /* Invalid node type - corrupted pointer */
        return;
    }

    if (child->type != LEPTRIS_NODE_TYPE_ELEMENT &&
        child->type != LEPTRIS_NODE_TYPE_TEXT &&
        child->type != LEPTRIS_NODE_TYPE_CDATA &&
        child->type != LEPTRIS_NODE_TYPE_COMMENT &&
        child->type != LEPTRIS_NODE_TYPE_PI) {
        return;  /* Only allow these node types as children */
    }

    /* Issue #217: if child is already attached to a parent, unlink
     * it first — even if the parent is the SAME element (re-ordering
     * within the same parent). Without this, the child appears twice
     * in the chain and child_count is inflated. */
    LeptrisElement old_parent = leptris_node_parent(child);
    if (old_parent) {
        leptris_node_unlink(child);
    }

    /* Mutation tail cache (TODO 195): sequential appends target the
     * same parent; the cached tail skips the O(N) walk. Direct-
     * mapped by element address so INTERLEAVED parents (result-tree
     * construction) each keep a slot — a single slot thrashed on
     * every other append (#682). Validated by the child's parent
     * back-pointer: a removed or re-parented cached tail fails the
     * check and falls back to the walk. */
    struct leptris_document* mut_doc = doc;
    LeptrisNode* mut_tail = NULL;
    if (mut_doc) {
        struct leptris_mut_tail* s =
            &mut_doc->mut_tail[(((uintptr_t)elem) >> 4) & 63];
        if (s->parent == elem && s->child &&
            leptris_node_parent(s->child) == elem)
            mut_tail = s->child;
    }

    /* For element children, set up linked list structure */
    if (child->type == LEPTRIS_NODE_TYPE_ELEMENT) {
        LeptrisElement child_elem = (LeptrisElement)child;

        /* Set parent relationship */
        leptris_element_set_parent(child_elem, elem);

        /* TODO 155 Phase A: document field removed; non-root walks to root. */
        /* Append to end of children list.
         * last_child may point to any node type (text/comment/etc.) so
         * we set its next_sibling via the type-dispatching setter. */
        LeptrisNode* last_node = mut_tail ? mut_tail : leptris_elem_last_child(elem);
        if (last_node) {
            /* Lane 18 P3: sequential appends' tail is an element —
             * the offset store directly; the type-dispatching node
             * setter only pays off for mixed-kind tails. */
            if (last_node->type == LEPTRIS_NODE_TYPE_ELEMENT)
                leptris_elem_set_next_sibling((LeptrisElement)last_node,
                                              (LeptrisNode*)child_elem);
            else
                leptris_node_set_next_sibling(last_node,
                                              (LeptrisNode*)child_elem);
        } else {
            leptris_elem_set_first_child(elem, (LeptrisNode*)child_elem);
        }

        /* Increment child count */
        elem->child_count++;
    } else {
        /* For non-element children (text, cdata, comment, pi), append to linked list */
        LeptrisNode* last = mut_tail ? mut_tail : leptris_elem_last_child(elem);
        if (last) {
            leptris_node_set_next_sibling(last, (LeptrisNode*)child);
            leptris_elem_set_last_child(elem, (LeptrisNode*)child);
        } else {
            /* No children yet - set first and last child using direct pointer assignment */
            leptris_elem_set_first_child(elem, (LeptrisNode*)child);
            leptris_elem_set_last_child(elem, (LeptrisNode*)child);
        }

        /* Issue #168: set parent_off on the non-element child so
         * leptris_node_parent can resolve in O(1). */
        switch (child->type) {
            case LEPTRIS_NODE_TYPE_TEXT:
                leptris_textnode_set_parent((LeptrisTextNode*)child, elem);
                break;
            case LEPTRIS_NODE_TYPE_COMMENT:
                leptris_comment_set_parent((LeptrisCommentNode*)child, elem);
                break;
            case LEPTRIS_NODE_TYPE_CDATA:
                leptris_cdata_set_parent((LeptrisCDATANode*)child, elem);
                break;
            case LEPTRIS_NODE_TYPE_PI:
                leptris_pi_set_parent((LeptrisPINode*)child, elem);
                break;
            default:
                break;
        }

        /* Don't increment child_count for non-element children */
    }

    if (mut_doc) {
        struct leptris_mut_tail* s =
            &mut_doc->mut_tail[(((uintptr_t)elem) >> 4) & 63];
        s->parent = elem;
        s->child = child;
    }

    /* COW: Increment version on modification */
    leptris_node_increment_version(LEPTRIS_ELEMENT_AS_NODE(elem));
}

void leptris_element_prepend_child_internal(LeptrisElement elem, LeptrisNode* child) {
    if (!elem || !child) return;

    /* SAFETY: Verify node type field is valid before accessing it
     * Small values like 0x4 or 0x1 in the type field suggest the pointer is corrupted */
    if (child->type < LEPTRIS_NODE_TYPE_ELEMENT ||
        child->type > LEPTRIS_NODE_TYPE_DOCTYPE) {
        /* Invalid node type - corrupted pointer */
        return;
    }

    if (child->type != LEPTRIS_NODE_TYPE_ELEMENT &&
        child->type != LEPTRIS_NODE_TYPE_TEXT &&
        child->type != LEPTRIS_NODE_TYPE_CDATA &&
        child->type != LEPTRIS_NODE_TYPE_COMMENT &&
        child->type != LEPTRIS_NODE_TYPE_PI) {
        return;  /* Only allow these node types as children */
    }

    /* Issue #217: if child is already attached to a parent, unlink
     * it first — even if the parent is the SAME element (re-ordering
     * within the same parent). Without this, the child appears twice
     * in the chain and child_count is inflated. */
    LeptrisElement old_parent = leptris_node_parent(child);
    if (old_parent) {
        leptris_node_unlink(child);
    }

    /* For element children, set up linked list structure */
    if (child->type == LEPTRIS_NODE_TYPE_ELEMENT) {
        LeptrisElement child_elem = (LeptrisElement)child;

        /* Set parent relationship */
        leptris_element_set_parent(child_elem, elem);

        /* TODO 155 Phase A: document field removed; non-root walks to root. */
        /* Insert at beginning of children list.
         * first_child may point to a non-element node; the new element's
         * next_sibling is set via the compact-offset setter. */
        LeptrisNode* first_node = leptris_elem_first_child(elem);
        if (first_node) {
            leptris_elem_set_next_sibling(child_elem, first_node);
            leptris_elem_set_first_child(elem, (LeptrisNode*)child_elem);
        } else {
            /* No children yet - set first and last child */
            leptris_elem_set_first_child(elem, (LeptrisNode*)child_elem);
            leptris_elem_set_last_child(elem, (LeptrisNode*)child_elem);
        }

        /* Increment child count */
        elem->child_count++;
    } else {
        /* For non-element children (text, cdata, comment, pi), insert at beginning */
        LeptrisNode* first = leptris_elem_first_child(elem);
        if (first) {
            /* Set the new child's next_sibling via the type-dispatching setter */
            leptris_node_set_next_sibling(child, first);
            leptris_elem_set_first_child(elem, (LeptrisNode*)child);
        } else {
            /* No children yet - set first and last child using direct pointer assignment */
            leptris_elem_set_first_child(elem, (LeptrisNode*)child);
            leptris_elem_set_last_child(elem, (LeptrisNode*)child);
        }

        /* Issue #168: set parent_off on the non-element child. */
        switch (child->type) {
            case LEPTRIS_NODE_TYPE_TEXT:
                leptris_textnode_set_parent((LeptrisTextNode*)child, elem);
                break;
            case LEPTRIS_NODE_TYPE_COMMENT:
                leptris_comment_set_parent((LeptrisCommentNode*)child, elem);
                break;
            case LEPTRIS_NODE_TYPE_CDATA:
                leptris_cdata_set_parent((LeptrisCDATANode*)child, elem);
                break;
            case LEPTRIS_NODE_TYPE_PI:
                leptris_pi_set_parent((LeptrisPINode*)child, elem);
                break;
            default:
                break;
        }

        /* Don't increment child_count for non-element children */
    }

    /* COW: Increment version on modification */
    leptris_node_increment_version(LEPTRIS_ELEMENT_AS_NODE(elem));
}

/* Helper function to calculate text length recursively */
static size_t calculate_text_length_recursive(LeptrisNode* node) {
    if (!node) return 0;

    size_t len = 0;
    LeptrisNode* child = leptris_node_first_child_internal(node);
    while (child) {
        if (child->type == LEPTRIS_NODE_TYPE_TEXT) {
            LeptrisTextNode* text = (LeptrisTextNode*)child;
            /* Route through leptris_text_get_content so entity-
             * containing borrowed text (the fast-parse path) is
             * expanded before measuring. After the call the node
             * is materialized, so the copy pass can use
             * text->content_len directly. */
            const char* content = leptris_text_get_content(text);
            if (content) {
                len += strlen(content);
            }
        } else if (child->type == LEPTRIS_NODE_TYPE_CDATA) {
            LeptrisCDATANode* cdata = (LeptrisCDATANode*)child;
            if (cdata->content) {
                len += strlen(cdata->content);
            }
        } else if (child->type == LEPTRIS_NODE_TYPE_ELEMENT) {
            /* Recursively include text from child elements */
            len += calculate_text_length_recursive(child);
        }

        child = leptris_node_get_next_sibling(child);
    }

    return len;
}

/* Helper function to copy text content recursively */
static void copy_text_content_recursive(LeptrisNode* node, char* result, size_t* offset) {
    if (!node || !result || !offset) return;

    LeptrisNode* child = leptris_node_first_child_internal(node);
    while (child) {
        if (child->type == LEPTRIS_NODE_TYPE_TEXT) {
            LeptrisTextNode* text = (LeptrisTextNode*)child;
            const char* content = leptris_text_get_content(text);
            if (content) {
                size_t clen = strlen(content);
                memcpy(result + *offset, content, clen);
                *offset += clen;
            }
        } else if (child->type == LEPTRIS_NODE_TYPE_CDATA) {
            LeptrisCDATANode* cdata = (LeptrisCDATANode*)child;
            if (cdata->content) {
                size_t len = strlen(cdata->content);
                memcpy(result + *offset, cdata->content, len);
                *offset += len;
            }
        } else if (child->type == LEPTRIS_NODE_TYPE_ELEMENT) {
            /* Recursively include text from child elements */
            copy_text_content_recursive(child, result, offset);
        }

        child = leptris_node_get_next_sibling(child);
    }
}

/* Text content extraction (concatenates ALL text nodes recursively) */
char* leptris_element_get_text_content(LeptrisElement elem) {
    if (!elem) return NULL;

    /* First pass: calculate total length needed recursively */
    size_t total_len = calculate_text_length_recursive((LeptrisNode*)elem);

    if (total_len == 0) {
        return leptris_strdup("");
    }

    /* Allocate buffer for concatenated text */
    char* result = (char*)leptris_malloc(total_len + 1);
    if (!result) return NULL;

    /* Second pass: copy text content recursively */
    size_t offset = 0;
    copy_text_content_recursive((LeptrisNode*)elem, result, &offset);

    result[offset] = '\0';
    return result;
}

/* Document tree operations */
void leptris_element_set_document_tree(LeptrisElement elem, struct leptris_document* doc) {
    /* TODO 155 Phase A: the document field is removed. We only need
     * to register the ROOT in the thread-local root→doc map; non-root
     * elements walk parent_off to find the root and look up there. */
    if (!elem) return;
    leptris_root_doc_register(elem, doc);
}

/* Remove all attributes from an element */
int leptris_element_remove_all_attributes(LeptrisElement elem) {
    if (!elem) return LEPTRIS_ERROR_NULL_INPUT;

    /* CRITICAL: Don't free pool-allocated attributes directly!
     * Attributes are allocated from the memory pool and will be freed
     * when the pool is destroyed. Just clear the pointers and let the pool handle cleanup.
     * We DON'T free any attribute strings - they're all pool-allocated. */

    /* Just clear the attribute linked list pointers */
    struct leptris_attribute* attr = leptris_element_get_first_attribute(elem);
    while (attr) {
        struct leptris_attribute* next = leptris_attr_next(attr);
        /* Don't free attr or any of its strings - all pool-allocated!
         * They will be reclaimed when the document/pool is freed. */
        attr = next;
    }

    /* Clear the attribute-list head/tail offsets and count */
    leptris_elem_set_first_attribute(elem, NULL);
    leptris_elem_set_last_attribute(elem, NULL);
    elem->attr_count = 0;

    return 0; /* LEPTRIS_OK */
}

/* ============================================================================
 * Subtree Analysis (for bulk allocation planning)
 * ============================================================================ */

/**
 * Count all nodes in subtree (for bulk allocation planning)
 *
 * Recursively traverses element tree and counts all nodes by type.
 * Used to pre-calculate allocation size for bulk operations.
 *
 * @param elem Root element to count
 * @param stats Output structure to fill with counts
 */
void leptris_element_count_subtree(LeptrisElement elem, LeptrisSubtreeStats* stats) {
    if (!elem || !stats) return;

    /* Initialize counts to zero */
    memset(stats, 0, sizeof(LeptrisSubtreeStats));

    /* Count this element */
    stats->element_count = 1;

    /* Count attributes on this element */
    stats->attribute_count = elem->attr_count;

    /* Recursively traverse all children */
    LeptrisNode* child = leptris_elem_first_child(elem);
    while (child) {
        switch (child->type) {
            case LEPTRIS_NODE_TYPE_ELEMENT:
                stats->element_count++;
                leptris_element_count_subtree((LeptrisElement)child, stats);
                break;

            case LEPTRIS_NODE_TYPE_TEXT:
                stats->text_count++;
                break;

            case LEPTRIS_NODE_TYPE_COMMENT:
                stats->comment_count++;
                break;

            case LEPTRIS_NODE_TYPE_CDATA:
                stats->cdata_count++;
                break;

            case LEPTRIS_NODE_TYPE_PI:
                stats->pi_count++;
                break;

            default:
                /* Unknown node type, skip */
                break;
        }

        child = leptris_node_get_next_sibling(child);
    }
}
