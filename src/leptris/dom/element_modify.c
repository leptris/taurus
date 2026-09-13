/* lib/src/dom/element_modify.c - DOM Modification API (Compact Mode)
 * Copyright (c) 2024, Ribose Inc.
 *
 * Public API for modifying DOM trees in-place.
 * COMPACT MODE: Uses compact pointer encoding and accessor functions.
 */

#include "../../include/leptris.h"
#include "../leptris_internal.h"
#include "../common/string_view.h"
#include "../memory/pool.h"
#include "element.h"
#include "element_index.h"
#include "compact.h"
#include "root_doc_map.h"
#include "node.h"
#include "text.h"
#include "comment.h"
#include "cdata.h"
#include "pi.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ===========================================================================
 * Public API Implementation - DOM Modification (Compact Mode)
 * =========================================================================== */

/* Round 21: carve mutation element names from a per-document
 * contiguous block — replaces leptris_pool_strdup on the create path
 * (call chain + arena slack checks cost ~9ns for a 2-byte name;
 * this is bump + copy). Oversized names (> block size) fall back
 * to the pool. */
#define MUT_NAME_BLOCK_BYTES 4096

static char* mut_name_carve(struct leptris_document* doc,
                            const char* name, size_t name_len) {
    /* Slot layout: [8B doc backpointer][name bytes][NUL]. The
     * backpointer + header bit 6 let get_document resolve this
     * element while unattached with zero registration (see
     * leptris_elem_namebp_doc). */
    size_t need = sizeof(struct leptris_document*) + name_len + 1;
    if (name_len > 254 || need > MUT_NAME_BLOCK_BYTES / 4) {
        /* Long names: pool path (rare; element names are short).
         * No backpointer — caller must register in the root map. */
        return NULL;
    }
    if (doc->mut_name_cursor + need > doc->mut_name_end) {
        struct leptris_mut_name_block* blk =
            (struct leptris_mut_name_block*)malloc(
                sizeof(struct leptris_mut_name_block) + MUT_NAME_BLOCK_BYTES);
        if (!blk) return NULL;
        blk->next = doc->mut_name_blocks;
        doc->mut_name_blocks = blk;
        doc->mut_name_cursor = blk->bytes;
        doc->mut_name_end = blk->bytes + MUT_NAME_BLOCK_BYTES;
    }
    char* slot = doc->mut_name_cursor;
    doc->mut_name_cursor += need;
    *(struct leptris_document**)slot = doc;
    memcpy(slot + sizeof(struct leptris_document*), name, name_len);
    slot[sizeof(struct leptris_document*) + name_len] = '\0';
    return slot + sizeof(struct leptris_document*);
}

/* Round 22: carve mutation attrs from a per-document contiguous
 * 40-byte-stride block (adjacent attrs keep their cp16 next-edges
 * in-range — no compact-overflow traffic for programmatic builds).
 * Falls back to the pool when a block can't be allocated. */
#define MUT_ATTR_BLOCK_COUNT 128

static struct leptris_attribute* mut_attr_carve(struct leptris_document* doc) {
    if (doc->mut_attr_cursor && doc->mut_attr_cursor < doc->mut_attr_end) {
        return doc->mut_attr_cursor++;
    }
    struct leptris_mut_attr_block* blk =
        (struct leptris_mut_attr_block*)malloc(
            sizeof(struct leptris_mut_attr_block) +
            (size_t)MUT_ATTR_BLOCK_COUNT * sizeof(struct leptris_attribute));
    if (!blk) return NULL;
    blk->next = doc->mut_attr_blocks;
    doc->mut_attr_blocks = blk;
    doc->mut_attr_cursor = (struct leptris_attribute*)blk->bytes;
    doc->mut_attr_end = doc->mut_attr_cursor + MUT_ATTR_BLOCK_COUNT;
    return doc->mut_attr_cursor++;
}

/* Round 22: short string carve for attr names/values — reuses the
 * name block (its 8-byte doc header is unused here; harmless).
 * Returns NULL for long strings — caller falls back to the pool. */
static char* mut_str_carve(struct leptris_document* doc, const char* s,
                           size_t len) {
    if (len + 1 > MUT_NAME_BLOCK_BYTES / 4) return NULL;
    size_t need = 8 + len + 1;
    if (doc->mut_name_cursor + need > doc->mut_name_end) {
        struct leptris_mut_name_block* blk =
            (struct leptris_mut_name_block*)malloc(
                sizeof(struct leptris_mut_name_block) + MUT_NAME_BLOCK_BYTES);
        if (!blk) return NULL;
        blk->next = doc->mut_name_blocks;
        doc->mut_name_blocks = blk;
        doc->mut_name_cursor = blk->bytes;
        doc->mut_name_end = blk->bytes + MUT_NAME_BLOCK_BYTES;
    }
    char* p = doc->mut_name_cursor + 8;
    doc->mut_name_cursor += need;
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

/* Round 18: carve mutation elements from a per-document contiguous
 * bump block. The pool extension path malloc'd each element
 * separately, scattering them across heap regions — sibling edges
 * (self-relative offsets) still worked but landed far apart, and
 * every fresh malloc paid allocator + page costs. Contiguous
 * carving keeps sequential-append elements cache-adjacent. Blocks
 * chain via ->next and are freed with the document. */
#define MUT_ELEM_BLOCK_COUNT 1024

static LeptrisElement mut_elem_carve(struct leptris_document* doc) {
    if (doc->mut_elem_cursor && doc->mut_elem_cursor < doc->mut_elem_end) {
        return doc->mut_elem_cursor++;
    }

    struct leptris_mut_elem_block* blk =
        (struct leptris_mut_elem_block*)malloc(
            sizeof(struct leptris_mut_elem_block) +
            (size_t)MUT_ELEM_BLOCK_COUNT * sizeof(struct leptris_element));
    if (!blk) return NULL;

    blk->next = doc->mut_elem_blocks;
    doc->mut_elem_blocks = blk;
    doc->mut_elem_cursor = (LeptrisElement)blk->bytes;
    doc->mut_elem_end = doc->mut_elem_cursor + MUT_ELEM_BLOCK_COUNT;
    return doc->mut_elem_cursor++;
}

/**
 * Create new element in document (Public API)
 */
LeptrisElement leptris_element_create(LeptrisDocument doc, const char* name) {
    if (!doc || !name) return NULL;

    /* Issue #187: trigger lazy promote if the doc was produced by
     * the flat-parse fast path. The compact-pointer tree must exist
     * before we can allocate a new element into it. */
    leptris_document_ensure_promoted(doc);

    LeptrisElement elem = NULL;
    if (doc->pool) {
        /* Round 18: struct from the bump block, name from the pool.
         * Same init contract as leptris_element_create_with_view
         * (memset-zero + type/name/name_hash/name_len). Falls back
         * to the plain pool path when a block can't be allocated. */
        size_t name_len = strlen(name);
        elem = mut_elem_carve(doc);
        if (elem) {
            char* name_copy = mut_name_carve(doc, name, name_len);
            if (name_copy) {
                memset(elem, 0, sizeof(struct leptris_element));
                elem->base.type = LEPTRIS_NODE_TYPE_ELEMENT;
                elem->header.flags |= LEPTRIS_NAMEBP_FLAG;
                elem->name = name_copy;
                elem->name_hash = leptris_name_hash_compute(name_copy);
                elem->name_len = (name_len > 254) ? 0xFF : (uint8_t)name_len;
                /* Round 20 contract: create registers every new element
                 * so pre-attach ops (set_root validation, get_document
                 * on detached elements) can resolve the doc. The bump-
                 * block fast path omitted it, leaving detached elements
                 * unresolvable until attached. Register is O(1) with
                 * the ROOTMAP_FLAG fast-out, so no measurable cost. */
                /* Lane 18 P1: NO map registration — mut_name_carve
                 * stamped the doc backpointer and header bit 6, and
                 * get_document resolves namebp BEFORE the map, so
                 * the entry is pure redundancy here (the #905 class
                 * only broke paths where namebp was NOT guaranteed;
                 * this branch guarantees it). The pool fallbacks
                 * below keep registering — their names carry no
                 * backpointer. */
                /* Mut-block copy is writable: a QName splits in
                 * place (#846). */
                leptris_elem_split_qname(elem, doc->pool);
            } else {
                /* Long name or block alloc failure: fall through to
                 * the pool path (registration follows below — pool
                 * names carry no backpointer). */
                memset(elem, 0, sizeof(struct leptris_element));
                char* pooled = leptris_pool_strdup(doc->pool, name);
                if (!pooled) return NULL;
                elem->base.type = LEPTRIS_NODE_TYPE_ELEMENT;
                elem->name = pooled;
                elem->name_hash = leptris_name_hash_compute(pooled);
                elem->name_len = (name_len > 254) ? 0xFF : (uint8_t)name_len;
                leptris_root_doc_register(elem, doc);
            }
        } else {
            elem = leptris_element_create_pooled(name, doc->pool);
            if (elem) leptris_root_doc_register(elem, doc);
        }
    } else {
        elem = leptris_element_create_pooled(name, doc->pool);
        if (elem) leptris_root_doc_register(elem, doc);
    }
    return elem;
}

/**
 * Append child element (Public API)
 */
/* Lane 18: the fused create+append. Shares the exact create and
 * append internals with the two-call pair — one doc resolution and
 * one public frame instead of two of each. */
LeptrisElement leptris_element_create_child(LeptrisElement parent, const char* name) {
    if (!parent || !name) return NULL;
    struct leptris_document* doc = leptris_element_get_document(parent);
    if (!doc || !doc->pool) return NULL;
    LeptrisElement elem = leptris_element_create(doc, name);
    if (!elem) return NULL;
    leptris_element_append_child_internal_doc(parent, (LeptrisNode*)elem, doc);
    leptris_element_invalidate_child_cache(parent);
    leptris_element_index_invalidate(doc);
    return elem;
}

LeptrisStatus leptris_element_append_child(LeptrisElement parent, LeptrisElement child) {
    if (!parent || !child) return LEPTRIS_ERROR_NULL_ARG;

    /* Single document resolution per mutation (TODO 195c): the
     * root walk + map lookup was paid three times per append
     * (twice here, once inside the internal for the tail cache). */
    struct leptris_document* doc = leptris_element_get_document(parent);

    /* Call internal void function, assume success */
    leptris_element_append_child_internal_doc(parent, (LeptrisNode*)child, doc);
    leptris_element_invalidate_child_cache(parent);



    /* Invalidate element index (TODO 132): the new child changes
     * the document's element set. The index will be rebuilt lazily
     * on the next descendant-axis query. */
    if (doc) {
        leptris_element_index_invalidate(doc);
    }
    return LEPTRIS_OK;
}

/**
 * Prepend child element at the beginning (Public API)
 */
LeptrisStatus leptris_element_prepend_child(LeptrisElement parent, LeptrisElement child) {
    if (!parent || !child) return LEPTRIS_ERROR_NULL_ARG;

    /* Call internal void function, assume success */
    leptris_element_prepend_child_internal(parent, (LeptrisNode*)child);
    leptris_element_invalidate_child_cache(parent);
    return LEPTRIS_OK;
}

/**
 * Insert new node before a sibling (Public API)
 * Issue #216: supports all child node types (element, text, comment,
 * cdata, pi), not just elements. The LeptrisElement signature is kept
 * for ABI stability; callers cast non-element node pointers.
 */
LeptrisStatus leptris_element_insert_before(LeptrisElement sibling, LeptrisElement new_node) {
    if (!sibling || !new_node) return LEPTRIS_ERROR_NULL_ARG;

    LeptrisNode* new_node_ptr = (LeptrisNode*)new_node;
    LeptrisNode* sibling_ptr = (LeptrisNode*)sibling;

    /* Validate new_node type. */
    if (new_node_ptr->type != LEPTRIS_NODE_TYPE_ELEMENT &&
        new_node_ptr->type != LEPTRIS_NODE_TYPE_TEXT &&
        new_node_ptr->type != LEPTRIS_NODE_TYPE_CDATA &&
        new_node_ptr->type != LEPTRIS_NODE_TYPE_COMMENT &&
        new_node_ptr->type != LEPTRIS_NODE_TYPE_PI) {
        return LEPTRIS_ERROR_INVALID_ARG;
    }

    /* Get parent using the type-dispatching accessor — sibling may
     * be any child node type. Issue #540: a DETACHED sibling is
     * legal — link-only mode below chains the two nodes so a later
     * append of the chain head under a parent carries the whole
     * sequence (libxml2 xmlAddPrevSibling semantics for unlinked
     * nodes; the flat next-sibling layout needs no extra state). */
    LeptrisElement parent = leptris_node_parent(sibling_ptr);
    if (!parent) {
        if (leptris_node_parent(new_node_ptr)) {
            leptris_node_unlink(new_node_ptr);
        }
        /* new_node becomes the HEAD of the detached chain. */
        leptris_node_set_next_sibling(new_node_ptr, sibling_ptr);
        return LEPTRIS_OK;
    }

    /* Issue #217 + #518: unlink new_node from ANY current parent —
     * including THIS one. A same-parent move without the unlink left
     * the old next-sibling link in place and the splice below built
     * a cycle in the child chain (later inserts/serializes hung
     * forever). leptris_node_unlink also decrements the old parent's
     * child_count, so the ++ below stays balanced for moves. */
    if (leptris_node_parent(new_node_ptr)) {
        leptris_node_unlink(new_node_ptr);
    }

    /* Walk the parent's true child chain (including non-element
     * children) to find the node before sibling. MUST run after the
     * unlink — new_node may have been in this chain. */
    LeptrisNode* prev_child = NULL;
    LeptrisNode* current = leptris_node_first_child_internal((LeptrisNode*)parent);
    while (current && current != sibling_ptr) {
        prev_child = current;
        current = leptris_node_get_next_sibling(current);
    }
    if (!current) return LEPTRIS_ERROR_INVALID_ARG;

    /* Splice new_node in between prev_child and sibling. */
    if (prev_child) {
        leptris_node_set_next_sibling(prev_child, new_node_ptr);
    } else {
        leptris_elem_set_first_child(parent, new_node_ptr);
    }
    leptris_node_set_next_sibling(new_node_ptr, sibling_ptr);

    /* Set parent (type-dispatching). TODO 155 Phase A: document field
     * removed; non-root elements reach doc via walk to root. */
    if (new_node_ptr->type == LEPTRIS_NODE_TYPE_ELEMENT) {
        leptris_element_set_parent((LeptrisElement)new_node_ptr, parent);
    } else {
        switch (new_node_ptr->type) {
            case LEPTRIS_NODE_TYPE_TEXT:
                leptris_textnode_set_parent((LeptrisTextNode*)new_node_ptr, parent);
                break;
            case LEPTRIS_NODE_TYPE_COMMENT:
                leptris_comment_set_parent((LeptrisCommentNode*)new_node_ptr, parent);
                break;
            case LEPTRIS_NODE_TYPE_CDATA:
                leptris_cdata_set_parent((LeptrisCDATANode*)new_node_ptr, parent);
                break;
            case LEPTRIS_NODE_TYPE_PI:
                leptris_pi_set_parent((LeptrisPINode*)new_node_ptr, parent);
                break;
            default: break;
        }
    }

    /* Issue #213: maintain child_count for element children only,
     * matching leptris_element_append_child_internal. */
    if (new_node_ptr->type == LEPTRIS_NODE_TYPE_ELEMENT) {
        parent->child_count++;
    }

    leptris_node_increment_version(LEPTRIS_ELEMENT_AS_NODE(parent));
    leptris_element_invalidate_child_cache(parent);
    return LEPTRIS_OK;
}

/**
 * Insert new node after a sibling (Public API)
 * Issue #216: supports all child node types.
 */
LeptrisStatus leptris_element_insert_after(LeptrisElement sibling, LeptrisElement new_node) {
    if (!sibling || !new_node) return LEPTRIS_ERROR_NULL_ARG;

    LeptrisNode* new_node_ptr = (LeptrisNode*)new_node;
    LeptrisNode* sibling_ptr = (LeptrisNode*)sibling;

    if (new_node_ptr->type != LEPTRIS_NODE_TYPE_ELEMENT &&
        new_node_ptr->type != LEPTRIS_NODE_TYPE_TEXT &&
        new_node_ptr->type != LEPTRIS_NODE_TYPE_CDATA &&
        new_node_ptr->type != LEPTRIS_NODE_TYPE_COMMENT &&
        new_node_ptr->type != LEPTRIS_NODE_TYPE_PI) {
        return LEPTRIS_ERROR_INVALID_ARG;
    }

    LeptrisElement parent = leptris_node_parent(sibling_ptr);
    if (!parent) {
        /* Issue #540: detached sibling — link-only append after it. */
        if (leptris_node_parent(new_node_ptr)) {
            leptris_node_unlink(new_node_ptr);
        }
        LeptrisNode* after = leptris_node_get_next_sibling(sibling_ptr);
        leptris_node_set_next_sibling(sibling_ptr, new_node_ptr);
        leptris_node_set_next_sibling(new_node_ptr, after);
        return LEPTRIS_OK;
    }

    /* Issue #217 + #518: unlink from ANY current parent — including
     * this one (a same-parent move must first come out of the chain,
     * else the splice below cycles it; see insert_before). */
    if (leptris_node_parent(new_node_ptr)) {
        leptris_node_unlink(new_node_ptr);
    }

    /* Read AFTER the unlink: if new_node was sibling's next sibling,
     * reading before would capture new_node itself and the splice
     * would link it to itself. */
    LeptrisNode* next_sibling = leptris_node_get_next_sibling(sibling_ptr);

    /* Splice new_node between sibling and next_sibling. Use the
     * type-dispatching setter on new_node — leptris_elem_set_next_sibling
     * only writes the element-form of the field. */
    leptris_node_set_next_sibling(sibling_ptr, new_node_ptr);
    leptris_node_set_next_sibling(new_node_ptr, next_sibling);

    /* Set parent (type-dispatching). TODO 155 Phase A: document field
     * removed; non-root elements reach doc via walk to root. */
    if (new_node_ptr->type == LEPTRIS_NODE_TYPE_ELEMENT) {
        leptris_element_set_parent((LeptrisElement)new_node_ptr, parent);
    } else {
        switch (new_node_ptr->type) {
            case LEPTRIS_NODE_TYPE_TEXT:
                leptris_textnode_set_parent((LeptrisTextNode*)new_node_ptr, parent);
                break;
            case LEPTRIS_NODE_TYPE_COMMENT:
                leptris_comment_set_parent((LeptrisCommentNode*)new_node_ptr, parent);
                break;
            case LEPTRIS_NODE_TYPE_CDATA:
                leptris_cdata_set_parent((LeptrisCDATANode*)new_node_ptr, parent);
                break;
            case LEPTRIS_NODE_TYPE_PI:
                leptris_pi_set_parent((LeptrisPINode*)new_node_ptr, parent);
                break;
            default: break;
        }
    }

    /* Update last_child if sibling was the last child. */
    LeptrisNode* last = leptris_node_last_child_internal((LeptrisNode*)parent);
    if (last == sibling_ptr) {
        leptris_elem_set_last_child(parent, new_node_ptr);
    }

    if (new_node_ptr->type == LEPTRIS_NODE_TYPE_ELEMENT) {
        parent->child_count++;
    }

    leptris_node_increment_version(LEPTRIS_ELEMENT_AS_NODE(parent));
    leptris_element_invalidate_child_cache(parent);
    return LEPTRIS_OK;
}

/**
 * Remove child element (Public API)
 * COMPACT MODE: Simplified implementation
 */
LeptrisStatus leptris_element_remove_child(LeptrisElement parent, LeptrisElement child) {
    if (!parent || !child) return LEPTRIS_ERROR_NULL_ARG;

    /* Verify child is actually a child of parent */
    LeptrisElement found = NULL;
    LeptrisElement current = leptris_element_get_first_child(parent);
    while (current) {
        if (current == child) {
            found = current;
            break;
        }
        current = leptris_element_get_next_sibling(current);
    }

    if (!found) {
        return LEPTRIS_ERROR_INVALID_ARG;
    }

    /* Find the node before child in the list */
    LeptrisElement prev_child = NULL;
    current = leptris_element_get_first_child(parent);
    while (current && current != child) {
        prev_child = current;
        current = leptris_element_get_next_sibling(current);
    }

    LeptrisElement next_child = leptris_element_get_next_sibling(child);

    /* Unlink child from the list */
    if (prev_child) {
        leptris_elem_set_next_sibling(prev_child, (LeptrisNode*)next_child);
    } else {
        /* Child was first child */
        leptris_elem_set_first_child(parent, (LeptrisNode*)next_child);
    }

    /* Update last_child pointer - directly check instead of using get_last_child */
    if (leptris_elem_last_child(parent) == (LeptrisNode*)child) {
        /* Child was last child */
        leptris_elem_set_last_child(parent, (LeptrisNode*)prev_child);
    }

    /* Clear parent and decrement count */
    leptris_elem_set_parent(child, NULL);
    parent->child_count--;

    /* COW: Increment version */
    leptris_node_increment_version(LEPTRIS_ELEMENT_AS_NODE(parent));

    leptris_element_invalidate_child_cache(parent);

    /* Structural mutation: a cached element index would describe the
     * pre-removal tree (append_child invalidates; removal must too). */
    if (leptris_element_get_document(parent)) {
        leptris_element_index_invalidate(leptris_element_get_document(parent));
    }
    return LEPTRIS_OK;
}

/**
 * Remove all children (Public API)
 */
LeptrisStatus leptris_element_remove_all_children(LeptrisElement elem) {
    if (!elem) return LEPTRIS_ERROR_NULL_ARG;

    /* Walk through all children and clear parent references */
    LeptrisElement child = leptris_element_get_first_child(elem);
    while (child) {
        LeptrisElement next = leptris_element_get_next_sibling(child);
        leptris_elem_set_parent(child, NULL);
        child = next;
    }

    /* Clear child pointers */
    leptris_elem_set_first_child(elem, NULL);
    leptris_elem_set_last_child(elem, NULL);
    elem->child_count = 0;

    /* COW: Increment version */
    leptris_node_increment_version(LEPTRIS_ELEMENT_AS_NODE(elem));

    leptris_element_invalidate_child_cache(elem);

    /* Structural mutation: invalidate any cached element index. */
    if (leptris_element_get_document(elem)) {
        leptris_element_index_invalidate(leptris_element_get_document(elem));
    }
    return LEPTRIS_OK;
}

/**
 * Set element name (Public API)
 */
LeptrisStatus leptris_element_set_name(LeptrisElement elem, const char* name) {
    if (!elem) return LEPTRIS_ERROR_NULL_ARG;
    if (!name) return LEPTRIS_ERROR_INVALID_ARG;

    /* CRITICAL FIX: Don't free old name - it might be pool-allocated!
     * Pool-allocated strings will be freed when the pool is destroyed.
     * If we free() a pool-allocated string, we get undefined behavior. */

    /* Set new name - prefer pool allocation if document is available.
     * Round 21: resolve the doc BEFORE clearing the mutation name-
     * backpointer bit — the element was reachable only through that
     * backpointer, and clearing it first sends the element down the
     * standalone/malloc path (breaking every later mutation op).
     * A replacement name is pool storage with no backpointer: clear
     * the bit and register in the root map instead. */
    struct leptris_document* sn_doc = leptris_element_get_document(elem);
    elem->header.flags &= (uint8_t)(~LEPTRIS_NAMEBP_FLAG & 0xFFu);
    if (sn_doc && sn_doc->pool) {
        /* Use pool allocation for consistency with parsing */
        elem->name = leptris_pool_strdup(sn_doc->pool, name);
        if (elem->name) leptris_root_doc_register(elem, sn_doc);
    } else {
        /* Fallback to malloc for standalone elements */
        elem->name = leptris_strdup(name);
    }
    if (!elem->name) return LEPTRIS_ERROR_MEMORY;
    /* #817: keep the prefix cache coherent with the new name — an
     * UNQUALIFIED rename must not let the serializer resurrect the
     * old prefix (p:child renamed to child emitted p:child). A
     * qualified rename adopts the lexical prefix; its URI stays
     * whatever is in scope (set_namespace rebinds explicitly). */
    {
        const char* colon = strchr(elem->name, ':');
        LeptrisMemoryPool* npool = leptris_element_get_pool(elem);
        if (colon && colon > elem->name) {
            size_t pl = (size_t)(colon - elem->name);
            char* pfx = (char*)leptris_pool_alloc(
                npool, pl + 1);
            if (pfx) {
                memcpy(pfx, elem->name, pl);
                pfx[pl] = 0;
                leptris_elem_set_prefix(elem, pfx, npool);
                /* Name storage is the LOCAL part; slide past the
                 * prefix. */
                elem->name = (char*)colon + 1;
            }
        } else {
            leptris_elem_set_prefix(elem, NULL, npool);
        }
    }
    elem->name_hash = leptris_name_hash_compute(elem->name);
    elem->name_len = (uint8_t)(strlen(elem->name) > 254
                                   ? 0xFF : strlen(elem->name));

    /* COW: Increment version */
    leptris_node_increment_version(LEPTRIS_ELEMENT_AS_NODE(elem));

    return LEPTRIS_OK;
}

/**
 * Set element text content (Public API)
 * COMPACT MODE: Removes all existing children and adds a single text node
 */
LeptrisStatus leptris_element_set_text(LeptrisElement elem, const char* text) {
    if (!elem) return LEPTRIS_ERROR_NULL_ARG;

    /* Remove all existing children */
    leptris_element_remove_all_children(elem);

    if (text) {
        /* Create new text node (even if empty to match pugixml behavior) */
        LeptrisMemoryPool* pool = leptris_element_get_pool(elem);
        LeptrisTextNode* text_node = leptris_text_create(text, strlen(text), pool);
        if (!text_node) {
            return LEPTRIS_ERROR_MEMORY;
        }

        /* Add as child */
        leptris_element_append_child_internal(elem, (LeptrisNode*)text_node);
    }

    return LEPTRIS_OK;
}

/**
 * Set attribute (Public API)
 * COMPACT MODE: Uses linked list attribute storage
 */

/* ---- Doc-level attribute-name index (mutation path) --------------------
 *
 * leptris_element_set_attribute walks the attr list per call to
 * reject duplicates — O(N) per set, O(N^2) for programmatic builds
 * (11 ms at 2000 attrs on one element). This open-addressed index
 * keys (element pointer, 32-bit name hash) -> attribute so the
 * duplicate check is O(1). Safety: nodes are arena-backed and
 * element removal only unlinks (never frees), so raw attr pointers
 * cannot dangle before leptris_document_free — which frees the
 * index. Entries: attr != NULL live; attr == NULL with elem != NULL
 * is a tombstone (probe continues); all-NULL slot ends the probe.
 * The (elem, hash == 0) sentinel marks an element as registered:
 * parse-created attrs are bulk-inserted on the element's first
 * mutation so the index is authoritative without ever touching the
 * parse path. ------------------------------------------------------------------ */

/* struct leptris_attr_index_entry / _index: leptris_internal.h
 * (document_free releases the table). */

#define ATTR_INDEX_INIT_CAP 16u

/* Walk-first threshold: elements at or below this attr count use the
 * hash-prefiltered list walk for the duplicate check instead of the
 * doc-level index. XSLT result elements typically carry 1–3 attrs —
 * register + probe + put costs more than walking a handful of nodes,
 * and the index never re-pays for a short-lived result tree. Elements
 * that grow past the threshold lazily bulk-register on their next
 * mutation (attr_index_register inserts every existing attr). */
#define ATTR_INDEX_WALK_MAX 8

/* Index keys use the FULL 32-bit FNV of the name — independent of
 * the attr field's 15-bit lazy hash (round 20). The u15 field feeds
 * per-element walk pre-filters; the doc-level open-addressed index
 * needs full-width keys so distinct names never share a probe key
 * (a u15 key space turns each hash collision into an O(N) fallback
 * walk, re-quadraticizing programmatic builds). */
static uint32_t attr_index_hash(const char* s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t attr_index_slot(LeptrisElement elem, uint32_t name_hash,
                                size_t cap) {
    uint64_t h = (uint64_t)(uintptr_t)elem;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    /* Round 20: fold AFTER the multiply. The old `h2 ^= h2 >> 15`
     * was a no-op for the 15-bit attr hashes — with stride-256 hash
     * progressions (pre-finalizer bug) that piled whole arithmetic
     * sequences onto single probe chains. */
    uint64_t h2 = (uint64_t)name_hash * 0x9E3779B97F4A7C15ULL;
    h2 ^= h2 >> 32;
    return (uint32_t)((h ^ h2) & (cap - 1));
}

static struct leptris_attr_index* attr_index_get(struct leptris_document* doc) {
    if (!doc->attr_index) {
        doc->attr_index = (struct leptris_attr_index*)malloc(
            sizeof(struct leptris_attr_index));
        if (!doc->attr_index) return NULL;
        doc->attr_index->slots = (struct leptris_attr_index_entry*)calloc(
            ATTR_INDEX_INIT_CAP, sizeof(struct leptris_attr_index_entry));
        if (!doc->attr_index->slots) {
            free(doc->attr_index);
            doc->attr_index = NULL;
            return NULL;
        }
        doc->attr_index->cap = ATTR_INDEX_INIT_CAP;
        doc->attr_index->used = 0;
    }
    return doc->attr_index;
}

/* Probe for (elem, name_hash). On hit returns the slot index with
 * *attr_out set (NULL when the slot is the registration sentinel).
 * On miss returns the empty-slot index for insertion. */
static size_t attr_index_probe(struct leptris_attr_index* ix,
                               LeptrisElement elem, uint32_t name_hash,
                               struct leptris_attribute** attr_out,
                               int* found) {
    size_t cap = ix->cap;
    size_t i = attr_index_slot(elem, name_hash, cap);
    *found = 0;
    for (size_t n_ = 0; n_ < cap; n_++) {
        struct leptris_attr_index_entry* e = &ix->slots[i];
        if (!e->elem && !e->attr) return i;         /* never used: miss */
        if (e->elem == elem && e->name_hash == name_hash) {
            if (e->attr) { *attr_out = e->attr; *found = 1; return i; }
            if (name_hash == 0) { *attr_out = NULL; *found = 1; return i; }
        }
        i = (i + 1) & (cap - 1);
    }
    return (size_t)-1; /* full (cannot happen: rehash keeps load < 0.7) */
}

static void attr_index_rehash(struct leptris_attr_index* ix) {
    size_t ncap = ix->cap * 2;
    struct leptris_attr_index_entry* ns = (struct leptris_attr_index_entry*)
        calloc(ncap, sizeof(struct leptris_attr_index_entry));
    if (!ns) return; /* stay at old size; probe loop tolerates it */
    for (size_t i = 0; i < ix->cap; i++) {
        struct leptris_attr_index_entry e = ix->slots[i];
        if (!e.elem && !e.attr) continue;
        if (!e.attr && e.name_hash != 0) continue;  /* drop tombstones */
        size_t j = attr_index_slot(e.elem, e.name_hash, ncap);
        while (ns[j].elem || ns[j].attr) j = (j + 1) & (ncap - 1);
        ns[j] = e;
    }
    free(ix->slots);
    ix->slots = ns;
    ix->cap = ncap;
    ix->used = 0;
    for (size_t i = 0; i < ncap; i++) {
        if (ns[i].elem || ns[i].attr) ix->used++;
    }
}

static void attr_index_put(struct leptris_attr_index* ix, LeptrisElement elem,
                           uint32_t name_hash,
                           struct leptris_attribute* attr, size_t slot) {
    if (slot >= ix->cap) {
        attr_index_rehash(ix);
        struct leptris_attribute* dummy;
        int f;
        slot = attr_index_probe(ix, elem, name_hash, &dummy, &f);
        if (slot == (size_t)-1) return;
    }
    struct leptris_attr_index_entry* e = &ix->slots[slot];
    if (!e->elem && !e->attr) ix->used++;
    e->elem = elem;
    e->name_hash = name_hash;
    e->attr = attr;
    if (ix->used * 10 >= ix->cap * 7) attr_index_rehash(ix);
}

/* Bulk-register every attr of `elem` plus the (elem, 0) sentinel so
 * later probes are authoritative. Falls back silently (index stays
 * partial) only on allocation failure — callers must then use the
 * list walk. */
static void attr_index_register(struct leptris_document* doc,
                                struct leptris_attr_index* ix,
                                LeptrisElement elem) {
    struct leptris_attribute* dummy;
    int found;
    size_t slot = attr_index_probe(ix, elem, 0, &dummy, &found);
    if (found || slot == (size_t)-1) return;
    attr_index_put(ix, elem, 0, NULL, slot);
    struct leptris_attribute* a = leptris_element_get_first_attribute(elem);
    while (a) {
        uint32_t h = attr_index_hash(attr_cname(a), a->name_view.length);
        slot = attr_index_probe(ix, elem, h, &dummy, &found);
        if (!found && slot != (size_t)-1) {
            attr_index_put(ix, elem, h, a, slot);
        }
        a = leptris_attr_next(a);
    }
}

/* O(1) duplicate check with lazy registration. Returns the existing
 * attr or NULL; *ix_out/registered bookkeeping lets the caller
 * insert the new attr without re-probing. */
static struct leptris_attribute* attr_index_lookup(
    struct leptris_document* doc, LeptrisElement elem, const char* name,
    size_t name_len, uint32_t name_hash,
    struct leptris_attr_index** ix_out, size_t* insert_slot) {
    *ix_out = NULL;
    *insert_slot = (size_t)-1;
    struct leptris_attr_index* ix = attr_index_get(doc);
    if (!ix) return NULL;
    attr_index_register(doc, ix, elem);
    struct leptris_attribute* hit = NULL;
    int found;
    size_t slot = attr_index_probe(ix, elem, name_hash, &hit, &found);
    if (slot == (size_t)-1) return NULL;
    if (!found) {
        *ix_out = ix;
        *insert_slot = slot;
        return NULL;
    }
    if (!hit) return NULL;                 /* sentinel-only element */
    /* Confirm with the string (hash collisions). */
    if (hit->name_view.length == name_len &&
        memcmp(attr_cname(hit), name, name_len) == 0) {
        return hit;
    }
    /* Collision with a different name: fall back to the walk. */
    return leptris_element_get_attribute_by_name(elem, name);
}

LeptrisStatus leptris_element_set_attribute(LeptrisElement elem, const char* name, const char* value) {
    if (!elem || !name) return LEPTRIS_ERROR_NULL_ARG;

    /* Mutation-path perf: skip the pool's string interning hash table
     * (TODO 106 Phase 1).  Interning deduplicates attribute NAMES that
     * recur across many elements during parsing; on the public mutation
     * path the user knows the attr is unique, and the hash lookup/insert
     * is pure overhead — ~200ns × N attrs.  Inline pool_alloc + memcpy
     * is faster than leptris_sv_to_cstr_pooled for this case. */

    /* Check if attribute already exists — O(1) via the doc-level
     * attr-name index (lazy registration covers parse-created attrs);
     * NULL index (alloc failure) falls back to the list walk. */
    size_t set_name_len = strlen(name);
    uint32_t set_name_hash = 0;
    struct leptris_document* set_doc = leptris_element_get_document(elem);
    struct leptris_attr_index* set_ix = NULL;
    size_t set_slot = (size_t)-1;
    struct leptris_attribute* existing;
    if (set_doc &&
        ((elem->header.flags & LEPTRIS_ATTR_INDEXED_FLAG) != 0 ||
         elem->attr_count > ATTR_INDEX_WALK_MAX)) {
        /* Lane 18: the flag latches — attr_count wraps at 255 and
         * must never flip a high-attr element back to the walk. */
        elem->header.flags |= LEPTRIS_ATTR_INDEXED_FLAG;
        set_name_hash = attr_index_hash(name, set_name_len);
        existing = attr_index_lookup(set_doc, elem, name, set_name_len,
                                     set_name_hash, &set_ix, &set_slot);
    } else {
        /* Walk-first (small elements): set_ix stays NULL so the
         * insert below skips the index put too. */
        existing = leptris_element_get_attribute_by_name(elem, name);
    }
    if (existing) {
        /* Update existing attribute's value. One resolution (the
         * set_doc from the top of the function) — the old path
         * re-climbed get_document/get_pool twice per overwrite. */
        LeptrisMemoryPool* pool = set_doc ? set_doc->pool : NULL;

        if (value) {
            size_t vlen = strlen(value);
            if (vlen <= LEPTRIS_ATTR_VALUE_MAX_INLINE) {
                /* lane18 S1: small values store INLINE in the slot —
                 * zero allocation per overwrite. */
                leptris_attr_value_set_inline(existing, value, vlen);
                attr_set_entities(existing, 0);
                return LEPTRIS_OK;
            }
        }
        if (pool) {
            /* Pool-allocated document: pool_strdup the new value (no
             * interning — see header comment).  Old value is pool-
             * allocated and reclaims when the pool frees. */
            if (value) {
                size_t vlen = strlen(value);
                char* storage = (char*)leptris_pool_alloc(pool, vlen + 1);
                if (!storage) return LEPTRIS_ERROR_MEMORY;
                memcpy(storage, value, vlen);
                storage[vlen] = '\0';
                leptris_attr_value_set_heap(existing, leptris_sv_from_ptr(storage, vlen));
            } else {
                leptris_attr_value_set_heap(existing, leptris_sv_empty());
            }
            attr_set_entities(existing, 0);
        } else {
            /* No pool available: views into the caller's string
             * (fallback for edge cases — mutation API contract says
             * the value string must outlive the attribute here). */
            leptris_attr_value_set_heap(existing, value ? leptris_sv_from_cstr(value) : leptris_sv_empty());
            attr_set_entities(existing, 0);
        }
    } else {
        /* Get the memory pool from the document */
        LeptrisMemoryPool* pool = NULL;
        if (leptris_element_get_document(elem) && leptris_element_get_pool(elem)) {
            pool = leptris_element_get_pool(elem);
        } else {
            /* CRITICAL: No pool available - cannot allocate attributes without memory pool */
            return LEPTRIS_ERROR_MEMORY;
        }

        /* Round 22: struct from the per-doc attr bump block (adjacent
         * attrs keep cp16 next-edges in-range); strings from the name
         * block. Pool fallbacks preserve the never-fail contract. */
        struct leptris_attribute* attr = mut_attr_carve(set_doc);
        if (!attr) attr = (struct leptris_attribute*)leptris_pool_alloc(
            pool, sizeof(struct leptris_attribute));
        if (!attr) {
            return LEPTRIS_ERROR_MEMORY;
        }

        size_t nlen = strlen(name);
        char* name_storage = mut_str_carve(set_doc, name, nlen);
        if (!name_storage) {
            name_storage = (char*)leptris_pool_alloc(pool, nlen + 1);
            if (!name_storage) return LEPTRIS_ERROR_MEMORY;
            memcpy(name_storage, name, nlen);
            name_storage[nlen] = '\0';
        }
        attr->name_view = leptris_sv_from_ptr(name_storage, nlen);

        /* Pre-compute name hash for O(1) lookup filtering (TODO 113).
         * Round 19: 15-bit + entity flag share the field. */
        attr->name_hash = attr_hash15(name_storage, nlen);

        if (value) {
            size_t vlen = strlen(value);
            if (vlen <= LEPTRIS_ATTR_VALUE_MAX_INLINE) {
                /* lane18 S1: small new values inline in the slot —
                 * no carve, no pool round-trip. */
                leptris_attr_value_set_inline(attr, value, vlen);
            } else {
                char* value_storage = mut_str_carve(set_doc, value, vlen);
                if (!value_storage) {
                    value_storage = (char*)leptris_pool_alloc(pool, vlen + 1);
                    if (!value_storage) return LEPTRIS_ERROR_MEMORY;
                    memcpy(value_storage, value, vlen);
                    value_storage[vlen] = '\0';
                }
                leptris_attr_value_set_heap(
                    attr, leptris_sv_from_ptr(value_storage, vlen));
            }
        } else {
            leptris_attr_value_set_heap(attr, leptris_sv_empty());
        }

        attr->ns_cache_off = 0;  /* TODO 173 */
        leptris_attr_set_next(attr, NULL);

        /* Append via the doc-level attr-tail cache — O(1) for the
         * sequential case (the element struct carries no last-attr
         * edge by the 64-byte layout law; the walk is O(attr_count)
         * and made programmatic builds quadratic). */
        struct leptris_attribute* last =
            (set_doc && set_doc->mut_attr_elem == elem)
                ? set_doc->mut_attr_tail
                : leptris_elem_last_attribute(elem);
        if (last) {
            leptris_attr_set_next(last, attr);
        } else {
            leptris_elem_set_first_attribute(elem, attr);
        }
        leptris_elem_set_last_attribute(elem, attr);
        if (set_doc) {
            set_doc->mut_attr_elem = elem;
            set_doc->mut_attr_tail = attr;
        }

        elem->attr_count++;

        if (set_ix && set_slot != (size_t)-1) {
            attr_index_put(set_ix, elem, set_name_hash, attr, set_slot);
        }
    }

    /* COW: Increment version */
    leptris_node_increment_version(LEPTRIS_ELEMENT_AS_NODE(elem));

    return LEPTRIS_OK;
}

/**
 * Remove attribute (Public API)
 */
LeptrisStatus leptris_element_remove_attribute(LeptrisElement elem, const char* name) {
    if (!elem || !name) return LEPTRIS_ERROR_NULL_ARG;

    struct leptris_document* rm_doc = leptris_element_get_document(elem);
    uint32_t rm_hash = attr_index_hash(name, strlen(name));

    struct leptris_attribute* attr = leptris_element_get_first_attribute(elem);
    struct leptris_attribute* prev = NULL;

    while (attr) {
        const char* attr_name = attr_cname(attr);

        int match = 0;
        if (attr_name) {
            match = (strcmp(attr_name, name) == 0);
        }

        if (match) {
            /* Found - remove from linked list */
            if (prev) {
                leptris_attr_set_next(prev, leptris_attr_next(attr));
            } else {
                /* Was first attribute - update first_attribute pointer */
                if (leptris_attr_next(attr)) {
                    leptris_element_set_first_attribute(elem, leptris_attr_next(attr));
                } else {
                    /* No more attributes */
                    leptris_element_set_first_attribute(elem, NULL);
                }
            }

            /* CRITICAL: Don't free pool-allocated attributes!
             * Attributes are allocated from the memory pool and will be freed
             * when the pool is destroyed. If we free() them here, we get undefined behavior.
             * Just decrement the count and let the pool handle cleanup. */

            /* CRITICAL: Don't free attribute strings!
             * Attributes created by parser or programmatically use pool allocation.
             * Pool-allocated strings cannot be individually freed - they will be
             * reclaimed when the entire document/pool is freed.
             * Trying to free() them here causes undefined behavior (Abort trap). */

            /* Tombstone the index entry: the attr memory stays valid
             * (pool-backed) but is detached — a later set_attribute
             * must not resurrect it. */
            if (rm_doc && rm_doc->mut_attr_elem == elem &&
                rm_doc->mut_attr_tail == attr) {
                rm_doc->mut_attr_elem = NULL;
                rm_doc->mut_attr_tail = NULL;
            }
            if (rm_doc && rm_doc->attr_index) {
                struct leptris_attribute* dummy;
                int found;
                size_t slot = attr_index_probe(rm_doc->attr_index, elem,
                                               rm_hash, &dummy, &found);
                if (found && rm_doc->attr_index->slots[slot].attr == attr) {
                    rm_doc->attr_index->slots[slot].attr = NULL;
                }
            }

            elem->attr_count--;
            leptris_node_increment_version(LEPTRIS_ELEMENT_AS_NODE(elem));
            return LEPTRIS_OK;
        }

        prev = attr;
        attr = leptris_attr_next(attr);
    }

    return LEPTRIS_ERROR_NOT_FOUND;  /* Attribute not found */
}

/* element.c: attach a namespace declaration node to the element's
 * list (takes ownership; the pooled deep-copy path builds its own
 * pooled declarations). */
int leptris_element_add_namespace(struct leptris_element* elem,
                                  struct leptris_namespace* ns);

/* #721: copy an element's namespace identity — the cached prefix and
 * namespace URI (the name is stored LOCAL; the prefix is a separate
 * cache slot) plus every xmlns:* declaration. Strings are duplicated
 * into the target pool (leptris_element_name_view shares the source
 * buffer cross-doc, and so do these). Call AFTER the copy is attached
 * or registered so get_pool/get_document resolve through the chain. */
/* #804 perf: pool-threaded core of copy_element_namespaces — the
 * deep copier already knows the pool; resolving it through the
 * parent chain (root-map walk + TLS) per element dominated subtree
 * duplication profiles. */
/* Returns 1 when any xmlns declaration was attached (the caller
 * sets the document's has_namespaces resolution gate). */
static int copy_element_namespaces_pooled(LeptrisElement copy,
                                          LeptrisElement source,
                                          LeptrisMemoryPool* pool) {
    int any_decl = 0;
    if (!pool) return 0;
    char* pfx = leptris_elem_prefix(source);
    if (pfx) {
        LeptrisStringView pv = leptris_sv_from_cstr(pfx);
        leptris_elem_set_prefix(
            copy, leptris_sv_to_cstr_pooled(&pv, pool), pool);
    }
    char* uri = leptris_elem_ns_uri(source);
    if (uri) {
        LeptrisStringView uv = leptris_sv_from_cstr(uri);
        leptris_elem_set_ns_uri(
            copy, leptris_sv_to_cstr_pooled(&uv, pool), pool);
    }
    for (int i = 0;; i++) {
        const char* np = leptris_element_namespace_decl_prefix(source, i);
        const char* nu = leptris_element_namespace_decl_uri(source, i);
        if (!nu) break;
        /* Pool-owned declaration linked through the THREADED pool.
         * The public leptris_element_add_namespace re-resolves the
         * pool via get_document — the detached copy is NOT
         * root-registered mid-recursion, so that path silently
         * dropped every declaration (#812) or heap-leaked (the
         * mutator's fallback, PR #806). The head accessor is the
         * same one add_namespace writes. */
        const char* norm = (np && !*np) ? NULL : np;
        struct leptris_namespace* ns =
            leptris_namespace_new_pooled(norm, nu, pool);
        if (!ns) break;
        ns->next = NULL;
        struct leptris_namespace** head =
            leptris_elem_namespaces_ptr(copy, pool);
        if (!head) break;
        if (!*head) {
            *head = ns;
        } else {
            struct leptris_namespace* tail = *head;
            while (tail->next) tail = tail->next;
            tail->next = ns;
        }
        any_decl = 1;
    }
    return any_decl;
}

static void copy_element_namespaces(LeptrisElement copy, LeptrisElement source,
                                    LeptrisElement parent) {
    LeptrisMemoryPool* pool = leptris_element_get_pool(parent);
    if (!pool) return;
    copy_element_namespaces_pooled(copy, source, pool);
}

/**
 * Append copy of element (Public API)
 */
LeptrisElement leptris_element_append_copy(LeptrisElement parent, LeptrisElement source) {
    if (!parent || !source) return NULL;

    /* CRITICAL: Check document pointer FIRST before any other access!
     * Use volatile to prevent compiler from reordering this check. */
    volatile struct leptris_document* parent_doc = leptris_element_get_document(parent);
    if (!parent_doc) {
        return NULL;
    }

    if (!parent_doc->pool) {
        return NULL;
    }

    /* Verify source has document too */
    if (!leptris_element_get_document(source)) {
        return NULL;  /* Source element must have a document */
    }

    /* Get source name as StringView (zero-copy, O(1) access) */
    LeptrisStringView name_view = leptris_element_name_view(source);
    if (leptris_sv_is_empty(&name_view)) return NULL;

    /* Check if this is a cross-document copy */
    int is_cross_doc = (leptris_element_get_document(source) != leptris_element_get_document(parent));

    /* For cross-document copies, we need to copy the name string data
     * because the StringView points to the source document's XML buffer */
    LeptrisStringView name_copy_view = name_view;  /* Default: use original StringView */
    if (is_cross_doc) {
        /* Double-check that target pool is valid before allocating */
        if (!leptris_element_get_pool(parent)) {
            return NULL;  /* Pool became NULL somehow */
        }

        /* Allocate and copy name string to target document's pool */
        char* name_copy = leptris_sv_to_cstr_pooled(&name_view, leptris_element_get_pool(parent));
        if (!name_copy) return NULL;
        name_copy_view = leptris_sv_from_cstr(name_copy);
    }

    /* Fast path: Simple element (no attributes, no children) - skip all loops */
    if (!leptris_element_get_first_attribute(source) && !leptris_elem_first_child(source)) {
        LeptrisElement copy = leptris_element_create_with_view(name_copy_view, leptris_element_get_pool(parent));
        if (!copy) return NULL;
        if (leptris_element_append_child(parent, copy) != LEPTRIS_OK) {
            return NULL;
        }
        /* #721: namespaces — after append_child so pool/doc lookups
         * resolve through the parent chain. A bare <p:c/> takes this
         * path and must keep its binding. */
        copy_element_namespaces(copy, source, parent);
        return copy;
    }

    /* Create new element with StringView (no C string conversion!) */
    LeptrisElement copy = leptris_element_create_with_view(name_copy_view, leptris_element_get_pool(parent));
    if (!copy) return NULL;

    /* TODO 155 Phase A: register copy as a temporary root so recursive
     * child-copy calls can reach the pool via leptris_element_get_pool. */
    leptris_root_doc_register(copy, leptris_element_get_document(parent));

    /* #721: namespaces — after registration so the internal
     * accessors can allocate the ns_cache through the pool. */
    copy_element_namespaces(copy, source, parent);

    /* Copy attributes - optimized: direct StringView copy when same document */
    uint8_t attr_count = leptris_element_attribute_count(source);
    for (uint8_t i = 0; i < attr_count; i++) {
        struct leptris_attribute* attr = leptris_element_get_attribute_by_index(source, i);
        if (attr && !leptris_sv_is_empty(&attr->name_view)) {
            LeptrisStringView attr_name_view = attr->name_view;
            LeptrisStringView attr_value_view = leptris_attr_value_sv(attr);

            /* For cross-document copies, copy the attribute string data */
            if (is_cross_doc) {
                /* Double-check that target pool is valid */
                if (!leptris_element_get_pool(parent)) {
                    continue;  /* Skip this attribute if pool is invalid */
                }

                char* name_copy = leptris_sv_to_cstr_pooled(&attr->name_view, leptris_element_get_pool(parent));
                LeptrisStringView av_ = leptris_attr_value_sv(attr);
                char* value_copy =
                    leptris_sv_to_cstr_pooled(&av_, leptris_element_get_pool(parent));
                if (!name_copy || !value_copy) {
                    /* Allocation failed - skip this attribute */
                    if (name_copy) /* Nothing to free, pool-allocated */
                    if (value_copy) /* Nothing to free, pool-allocated */
                    continue;
                }
                attr_name_view = leptris_sv_from_cstr(name_copy);
                attr_value_view = leptris_sv_from_cstr(value_copy);
            }

            /* Use pool-based fast path if available, otherwise fallback to set_attribute
             * Pool path is 2-3x faster (no hash lookup, no string conversion) */
            if (leptris_element_get_pool(parent)) {
                leptris_element_add_attribute(copy, attr_name_view, attr_value_view, leptris_element_get_pool(parent));
            } else {
                /* Fallback: C-string API (views are always valid C
                 * strings under the single representation). */
                const char* attr_name = attr_cname(attr);
                const char* attr_value = attr_cvalue(attr);
                if (attr_name && attr_value) {
                    leptris_element_set_attribute(copy, attr_name, attr_value);
                }
            }
        }
    }

    /* Copy children recursively */
    LeptrisElement child = (LeptrisElement)leptris_elem_first_child(source);

    /* SAFETY: Verify child pointer is valid before accessing.
     * Small values like 0x4 indicate memory corruption or uninitialized fields. */
    while (child) {
        /* Check if pointer looks valid (not obviously corrupted)
         * Values less than 0x1000 are likely invalid pointers or offsets */
        if ((uintptr_t)child < 0x1000) {
            /* Invalid pointer - skip this child to prevent crash */
            break;
        }

        /* Additional safety: Check if pointer points to readable memory
         * by verifying the type field is within valid range */
        LeptrisNode* child_node = (LeptrisNode*)child;

        if (child_node->type < LEPTRIS_NODE_TYPE_ELEMENT ||
            child_node->type > LEPTRIS_NODE_TYPE_DOCTYPE) {
            /* Invalid type field - likely corrupted or pointing to wrong memory */
            break;
        }

        if (child_node->type == LEPTRIS_NODE_TYPE_ELEMENT) {
            /* Recursively copy element child */
            leptris_element_append_copy(copy, child);
        } else if (child_node->type == LEPTRIS_NODE_TYPE_TEXT) {
            LeptrisTextNode* text = (LeptrisTextNode*)child;
            /* SAFETY: Verify text pointer is valid before accessing content */
            if ((uintptr_t)text > 0x1000) {
                LeptrisTextNode* text_copy = leptris_text_create(text->content,
                    text->content_len,
                    leptris_element_get_pool(copy));
                if (text_copy) {
                    leptris_element_append_child_internal(copy, (LeptrisNode*)text_copy);
                }
            }
        } else if (child_node->type == LEPTRIS_NODE_TYPE_CDATA) {
            LeptrisCDATANode* cdata = (LeptrisCDATANode*)child;
            if ((uintptr_t)cdata > 0x1000) {
                LeptrisCDATANode* cdata_copy = leptris_cdata_create(cdata->content,
                    cdata->content ? strlen(cdata->content) : 0,
                    leptris_element_get_pool(copy));
                if (cdata_copy) {
                    leptris_element_append_child_internal(copy, (LeptrisNode*)cdata_copy);
                }
            }
        }
        /* COMMENT and PI children copy too (issue #696 — the copy
         * loop used to drop them silently at every level). */
        else if (child_node->type == LEPTRIS_NODE_TYPE_COMMENT) {
            LeptrisCommentNode* cm = (LeptrisCommentNode*)child;
            if ((uintptr_t)cm > 0x1000 && cm->content) {
                LeptrisCommentNode* cc = leptris_comment_create(
                    cm->content, strlen(cm->content),
                    leptris_element_get_pool(copy));
                if (cc)
                    leptris_element_append_child_internal(copy,
                                                          (LeptrisNode*)cc);
            }
        } else if (child_node->type == LEPTRIS_NODE_TYPE_PI) {
            LeptrisPINode* pi = (LeptrisPINode*)child;
            if ((uintptr_t)pi > 0x1000) {
                LeptrisPINode* pc = leptris_pi_create(
                    pi->target, pi->target ? strlen(pi->target) : 0,
                    pi->data ? pi->data : "",
                    pi->data ? strlen(pi->data) : 0,
                    leptris_element_get_pool(copy));
                if (pc)
                    leptris_element_append_child_internal(copy,
                                                          (LeptrisNode*)pc);
            }
        }

        /* CRITICAL FIX: Get next sibling using generic accessor
         * The child variable is declared as LeptrisElement but might actually
         * point to a text node or other node type. Each node type has next_sibling
         * at a different offset, so we must use the generic accessor.
         * Note: child_node is already declared above at line 598 */
        LeptrisNode* next = leptris_node_get_next_sibling(child_node);
        if (!next) {
            /* No more siblings */
            break;
        }
        child = (LeptrisElement)next;
    }

    /* Append to parent */
    if (leptris_element_append_child(parent, copy) != LEPTRIS_OK) {
        /* Note: copy is pool-allocated, will be freed with document */
        return NULL;
    }

    return copy;
}

/**
 * Prepend copy of element (Public API)
 */
LeptrisElement leptris_element_prepend_copy(LeptrisElement parent, LeptrisElement source) {
    if (!parent || !source) return NULL;

    /* Get source name as StringView (zero-copy, O(1) access) */
    LeptrisStringView name_view = leptris_element_name_view(source);
    if (leptris_sv_is_empty(&name_view)) return NULL;

    /* Check if this is a cross-document copy */
    int is_cross_doc = (leptris_element_get_document(source) != leptris_element_get_document(parent));

    /* For cross-document copies, we need to copy the name string data
     * because the StringView points to the source document's XML buffer */
    LeptrisStringView name_copy_view = name_view;  /* Default: use original StringView */
    if (is_cross_doc) {
        /* Allocate and copy name string to target document's pool */
        char* name_copy = leptris_sv_to_cstr_pooled(&name_view, leptris_element_get_pool(parent));
        if (!name_copy) return NULL;
        name_copy_view = leptris_sv_from_cstr(name_copy);
    }

    /* Fast path: Simple element (no attributes, no children) */
    if (!leptris_element_get_first_attribute(source) && !leptris_elem_first_child(source)) {
        LeptrisElement copy = leptris_element_create_with_view(name_copy_view, leptris_element_get_pool(parent));
        if (!copy) return NULL;
        if (leptris_element_prepend_child(parent, copy) != LEPTRIS_OK) {
            return NULL;
        }
        return copy;
    }

    /* Create new element with StringView (no C string conversion!) */
    LeptrisElement copy = leptris_element_create_with_view(name_copy_view, leptris_element_get_pool(parent));
    if (!copy) return NULL;

    /* TODO 155 Phase A: register copy as a temporary root so recursive
     * child-copy calls can reach the pool via leptris_element_get_pool. */
    leptris_root_doc_register(copy, leptris_element_get_document(parent));

    /* Copy attributes - optimized: direct StringView copy when same document */
    uint8_t attr_count = leptris_element_attribute_count(source);
    for (uint8_t i = 0; i < attr_count; i++) {
        struct leptris_attribute* attr = leptris_element_get_attribute_by_index(source, i);
        if (attr && !leptris_sv_is_empty(&attr->name_view)) {
            LeptrisStringView attr_name_view = attr->name_view;
            LeptrisStringView attr_value_view = leptris_attr_value_sv(attr);

            /* For cross-document copies, copy the attribute string data */
            if (is_cross_doc) {
                char* name_copy = leptris_sv_to_cstr_pooled(&attr->name_view, leptris_element_get_pool(parent));
                LeptrisStringView av_ = leptris_attr_value_sv(attr);
                char* value_copy =
                    leptris_sv_to_cstr_pooled(&av_, leptris_element_get_pool(parent));
                if (!name_copy || !value_copy) {
                    /* Allocation failed - skip this attribute */
                    if (name_copy) /* Nothing to free, pool-allocated */
                    if (value_copy) /* Nothing to free, pool-allocated */
                    continue;
                }
                attr_name_view = leptris_sv_from_cstr(name_copy);
                attr_value_view = leptris_sv_from_cstr(value_copy);
            }

            /* Use pool-based fast path if available, otherwise fallback to set_attribute
             * Pool path is 2-3x faster (no hash lookup, no string conversion) */
            if (leptris_element_get_pool(parent)) {
                leptris_element_add_attribute(copy, attr_name_view, attr_value_view, leptris_element_get_pool(parent));
            } else {
                /* Fallback: C-string API (views are always valid C
                 * strings under the single representation). */
                const char* attr_name = attr_cname(attr);
                const char* attr_value = attr_cvalue(attr);
                if (attr_name && attr_value) {
                    leptris_element_set_attribute(copy, attr_name, attr_value);
                }
            }
        }
    }

    /* Copy children recursively */
    LeptrisElement child = (LeptrisElement)leptris_elem_first_child(source);
    while (child) {
        LeptrisNode* child_node = (LeptrisNode*)child;
        if (child_node->type == LEPTRIS_NODE_TYPE_ELEMENT) {
            /* Recursively copy element child */
            leptris_element_append_copy(copy, child);
        } else if (child_node->type == LEPTRIS_NODE_TYPE_TEXT) {
            LeptrisTextNode* text = (LeptrisTextNode*)child;
            LeptrisTextNode* text_copy = leptris_text_create(text->content,
                text->content_len,
                leptris_element_get_pool(copy));
            if (text_copy) {
                leptris_element_append_child_internal(copy, (LeptrisNode*)text_copy);
            }
        }
        /* Use generic accessor to get next sibling - child might be any node type */
        LeptrisNode* next = leptris_node_get_next_sibling(child_node);
        if (!next) break;
        child = (LeptrisElement)next;
    }

    /* Prepend to parent */
    if (leptris_element_prepend_child(parent, copy) != LEPTRIS_OK) {
        return NULL;
    }

    return copy;
}

/**
 * Insert copy after sibling (Public API)
 */
LeptrisElement leptris_element_insert_copy_after(LeptrisElement sibling, LeptrisElement source) {
    if (!sibling || !source) return NULL;

    /* Get parent of sibling */
    LeptrisElement parent = leptris_element_parent(sibling);
    if (!parent) return NULL;

    /* Get source name as StringView (zero-copy, O(1) access) */
    LeptrisStringView name_view = leptris_element_name_view(source);
    if (leptris_sv_is_empty(&name_view)) return NULL;

    /* Check if this is a cross-document copy */
    int is_cross_doc = (leptris_element_get_document(source) != leptris_element_get_document(parent));

    /* For cross-document copies, we need to copy the name string data
     * because the StringView points to the source document's XML buffer */
    LeptrisStringView name_copy_view = name_view;  /* Default: use original StringView */
    if (is_cross_doc) {
        /* Allocate and copy name string to target document's pool */
        char* name_copy = leptris_sv_to_cstr_pooled(&name_view, leptris_element_get_pool(parent));
        if (!name_copy) return NULL;
        name_copy_view = leptris_sv_from_cstr(name_copy);
    }

    /* Fast path: Simple element (no attributes, no children) */
    if (!leptris_element_get_first_attribute(source) && !leptris_elem_first_child(source)) {
        LeptrisElement copy = leptris_element_create_with_view(name_copy_view, leptris_element_get_pool(parent));
        if (!copy) return NULL;
        if (leptris_element_insert_after(sibling, copy) != LEPTRIS_OK) {
            return NULL;
        }
        return copy;
    }

    /* Create new element with StringView (no C string conversion!) */
    LeptrisElement copy = leptris_element_create_with_view(name_copy_view, leptris_element_get_pool(parent));
    if (!copy) return NULL;

    /* TODO 155 Phase A: register copy as a temporary root so recursive
     * child-copy calls can reach the pool via leptris_element_get_pool. */
    leptris_root_doc_register(copy, leptris_element_get_document(parent));

    /* Copy attributes - optimized: direct StringView copy when same document */
    uint8_t attr_count = leptris_element_attribute_count(source);
    for (uint8_t i = 0; i < attr_count; i++) {
        struct leptris_attribute* attr = leptris_element_get_attribute_by_index(source, i);
        if (attr && !leptris_sv_is_empty(&attr->name_view)) {
            LeptrisStringView attr_name_view = attr->name_view;
            LeptrisStringView attr_value_view = leptris_attr_value_sv(attr);

            /* For cross-document copies, copy the attribute string data */
            if (is_cross_doc) {
                char* name_copy = leptris_sv_to_cstr_pooled(&attr->name_view, leptris_element_get_pool(parent));
                LeptrisStringView av_ = leptris_attr_value_sv(attr);
                char* value_copy =
                    leptris_sv_to_cstr_pooled(&av_, leptris_element_get_pool(parent));
                if (!name_copy || !value_copy) {
                    /* Allocation failed - skip this attribute */
                    if (name_copy) /* Nothing to free, pool-allocated */
                    if (value_copy) /* Nothing to free, pool-allocated */
                    continue;
                }
                attr_name_view = leptris_sv_from_cstr(name_copy);
                attr_value_view = leptris_sv_from_cstr(value_copy);
            }

            /* Use pool-based fast path if available, otherwise fallback to set_attribute
             * Pool path is 2-3x faster (no hash lookup, no string conversion) */
            if (leptris_element_get_pool(parent)) {
                leptris_element_add_attribute(copy, attr_name_view, attr_value_view, leptris_element_get_pool(parent));
            } else {
                /* Fallback: C-string API (views are always valid C
                 * strings under the single representation). */
                const char* attr_name = attr_cname(attr);
                const char* attr_value = attr_cvalue(attr);
                if (attr_name && attr_value) {
                    leptris_element_set_attribute(copy, attr_name, attr_value);
                }
            }
        }
    }

    /* Copy children recursively */
    LeptrisElement child = (LeptrisElement)leptris_elem_first_child(source);
    while (child) {
        LeptrisNode* child_node = (LeptrisNode*)child;
        if (child_node->type == LEPTRIS_NODE_TYPE_ELEMENT) {
            /* Recursively copy element child */
            leptris_element_append_copy(copy, child);
        } else if (child_node->type == LEPTRIS_NODE_TYPE_TEXT) {
            LeptrisTextNode* text = (LeptrisTextNode*)child;
            LeptrisTextNode* text_copy = leptris_text_create(text->content,
                text->content_len,
                leptris_element_get_pool(copy));
            if (text_copy) {
                leptris_element_append_child_internal(copy, (LeptrisNode*)text_copy);
            }
        }
        /* Use generic accessor to get next sibling - child might be any node type */
        LeptrisNode* next = leptris_node_get_next_sibling(child_node);
        if (!next) break;
        child = (LeptrisElement)next;
    }

    /* Insert after sibling */
    if (leptris_element_insert_after(sibling, copy) != LEPTRIS_OK) {
        return NULL;
    }

    return copy;
}

/**
 * Insert copy before sibling (Public API)
 */
LeptrisElement leptris_element_insert_copy_before(LeptrisElement sibling, LeptrisElement source) {
    if (!sibling || !source) return NULL;

    /* Get parent of sibling */
    LeptrisElement parent = leptris_element_parent(sibling);
    if (!parent) return NULL;

    /* Get source name as StringView (zero-copy, O(1) access) */
    LeptrisStringView name_view = leptris_element_name_view(source);
    if (leptris_sv_is_empty(&name_view)) return NULL;

    /* Check if this is a cross-document copy */
    int is_cross_doc = (leptris_element_get_document(source) != leptris_element_get_document(parent));

    /* For cross-document copies, we need to copy the name string data
     * because the StringView points to the source document's XML buffer */
    LeptrisStringView name_copy_view = name_view;  /* Default: use original StringView */
    if (is_cross_doc) {
        /* Allocate and copy name string to target document's pool */
        char* name_copy = leptris_sv_to_cstr_pooled(&name_view, leptris_element_get_pool(parent));
        if (!name_copy) return NULL;
        name_copy_view = leptris_sv_from_cstr(name_copy);
    }

    /* Fast path: Simple element (no attributes, no children) */
    if (!leptris_element_get_first_attribute(source) && !leptris_elem_first_child(source)) {
        LeptrisElement copy = leptris_element_create_with_view(name_copy_view, leptris_element_get_pool(parent));
        if (!copy) return NULL;
        if (leptris_element_insert_before(sibling, copy) != LEPTRIS_OK) {
            return NULL;
        }
        return copy;
    }

    /* Create new element with StringView (no C string conversion!) */
    LeptrisElement copy = leptris_element_create_with_view(name_copy_view, leptris_element_get_pool(parent));
    if (!copy) return NULL;

    /* TODO 155 Phase A: register copy as a temporary root so recursive
     * child-copy calls can reach the pool via leptris_element_get_pool. */
    leptris_root_doc_register(copy, leptris_element_get_document(parent));

    /* Copy attributes - optimized: direct StringView copy when same document */
    uint8_t attr_count = leptris_element_attribute_count(source);
    for (uint8_t i = 0; i < attr_count; i++) {
        struct leptris_attribute* attr = leptris_element_get_attribute_by_index(source, i);
        if (attr && !leptris_sv_is_empty(&attr->name_view)) {
            LeptrisStringView attr_name_view = attr->name_view;
            LeptrisStringView attr_value_view = leptris_attr_value_sv(attr);

            /* For cross-document copies, copy the attribute string data */
            if (is_cross_doc) {
                char* name_copy = leptris_sv_to_cstr_pooled(&attr->name_view, leptris_element_get_pool(parent));
                LeptrisStringView av_ = leptris_attr_value_sv(attr);
                char* value_copy =
                    leptris_sv_to_cstr_pooled(&av_, leptris_element_get_pool(parent));
                if (!name_copy || !value_copy) {
                    /* Allocation failed - skip this attribute */
                    if (name_copy) /* Nothing to free, pool-allocated */
                    if (value_copy) /* Nothing to free, pool-allocated */
                    continue;
                }
                attr_name_view = leptris_sv_from_cstr(name_copy);
                attr_value_view = leptris_sv_from_cstr(value_copy);
            }

            /* Use pool-based fast path if available, otherwise fallback to set_attribute
             * Pool path is 2-3x faster (no hash lookup, no string conversion) */
            if (leptris_element_get_pool(parent)) {
                leptris_element_add_attribute(copy, attr_name_view, attr_value_view, leptris_element_get_pool(parent));
            } else {
                /* Fallback: C-string API (views are always valid C
                 * strings under the single representation). */
                const char* attr_name = attr_cname(attr);
                const char* attr_value = attr_cvalue(attr);
                if (attr_name && attr_value) {
                    leptris_element_set_attribute(copy, attr_name, attr_value);
                }
            }
        }
    }

    /* Copy children recursively */
    LeptrisElement child = (LeptrisElement)leptris_elem_first_child(source);
    while (child) {
        LeptrisNode* child_node = (LeptrisNode*)child;
        if (child_node->type == LEPTRIS_NODE_TYPE_ELEMENT) {
            /* Recursively copy element child */
            leptris_element_append_copy(copy, child);
        } else if (child_node->type == LEPTRIS_NODE_TYPE_TEXT) {
            LeptrisTextNode* text = (LeptrisTextNode*)child;
            LeptrisTextNode* text_copy = leptris_text_create(text->content,
                text->content_len,
                leptris_element_get_pool(copy));
            if (text_copy) {
                leptris_element_append_child_internal(copy, (LeptrisNode*)text_copy);
            }
        }
        /* Use generic accessor to get next sibling - child might be any node type */
        LeptrisNode* next = leptris_node_get_next_sibling(child_node);
        if (!next) break;
        child = (LeptrisElement)next;
    }

    /* Insert before sibling */
    if (leptris_element_insert_before(sibling, copy) != LEPTRIS_OK) {
        return NULL;
    }

    return copy;
}


/**
 * Remove all children (Public API) - Alias for remove_all_children
 */
LeptrisStatus leptris_element_remove_children(LeptrisElement elem) {
    return leptris_element_remove_all_children(elem);
}

/* ============================================================================
 * Bulk Allocation for Subtree Copy (Task 40)
 * ============================================================================ */

/**
 * Internal: Build a map from source elements to pre-allocated copies
 *
 * This is used during bulk copy to link elements without knowing their
 * final addresses ahead of time. We allocate space for the map, then
 * build it while copying.
 *
 * @param pool Memory pool for allocations
 * @param count Number of elements in subtree
 * @return Pointer to element array, or NULL on failure
 */
static LeptrisElement* leptris_element_create_copy_map(LeptrisMemoryPool* pool, size_t count) {
    if (!pool || count == 0) return NULL;
    return (LeptrisElement*)leptris_pool_alloc(pool, sizeof(LeptrisElement) * count);
}

/**
 * Internal: Copy subtree using pre-allocated elements (bulk allocation)
 *
 * This function assumes elements have already been allocated and stored
 * in the copy_map array. It initializes each element and builds parent-child
 * relationships.
 *
 * @param parent_copy Parent element to attach copies to
 * @param source Source element to copy
 * @param copy_map Array of pre-allocated element copies
 * @param copy_index Pointer to current index in copy_map (updated during traversal)
 * @param pool Memory pool for additional allocations (attributes, strings)
 * @return The copy of the source element, or NULL on failure
 */
static LeptrisElement leptris_element_copy_subtree_bulk_internal(
    LeptrisElement parent_copy,
    LeptrisElement source,
    LeptrisElement* copy_map,
    size_t* copy_index,
    LeptrisMemoryPool* pool
) {
    if (!source || !copy_map || !copy_index) return NULL;

    /* Get current index and advance for children */
    size_t this_index = *copy_index;
    (*copy_index)++;

    LeptrisElement copy = copy_map[this_index];
    if (!copy) return NULL;

    /* Initialize the copy element with minimal memset (only what we need) */
    memset(copy, 0, sizeof(struct leptris_element));

    /* Copy base node type */
    copy->base.type = LEPTRIS_NODE_TYPE_ELEMENT;

    /* name_view removed (TODO 90) — name is already pool-strdup'd
     * by create_with_view during the deep_copy_element call above. */

    /* Copy prefix and namespace as StringView (TODO 90) */
    /* prefix_view removed (TODO 90) — prefix is copied as char* below */;
    /* Phase 2e-B: copy namespace_uri through ns_cache. */
    {
        char* src_uri = leptris_elem_ns_uri(source);
        if (src_uri) {
            LeptrisMemoryPool* p = leptris_element_get_pool(copy);
            leptris_elem_set_ns_uri(copy, src_uri, p);
        }
    }

    /* Set parent pointer */
    leptris_elem_set_parent(copy, parent_copy);

    /* Copy attributes */
    copy->attr_count = source->attr_count;

    /* Copy attribute linked list - we need to allocate attributes individually
     * since they're stored as a compact pointer linked list */
    struct leptris_attribute* src_attr = leptris_element_get_first_attribute(source);
    struct leptris_attribute* prev_dst_attr = NULL;

    while (src_attr) {
        /* Allocate attribute from pool */
        struct leptris_attribute* dst_attr = (struct leptris_attribute*)leptris_pool_alloc(pool, sizeof(struct leptris_attribute));
        if (!dst_attr) break;

        /* Initialize attribute */
        memset(dst_attr, 0, sizeof(struct leptris_attribute));

        /* Owned copies in the DESTINATION pool (single representation,
         * round 4): the source's views may point into the source
         * document's buffer, which can be freed before this copy. */
        if (!leptris_sv_is_empty(&src_attr->name_view)) {
            char* n = leptris_pool_strdup(pool, src_attr->name_view.data);
            if (n) dst_attr->name_view = leptris_sv_from_cstr(n);
        }
        LeptrisStringView svv_ = leptris_attr_value_sv(src_attr);
        if (!leptris_sv_is_empty(&svv_)) {
            char* v = leptris_pool_strdup(pool, leptris_attr_value_sv(src_attr).data);
            if (v) leptris_attr_value_set_heap(dst_attr, leptris_sv_from_cstr(v));
        }
        dst_attr->name_hash = src_attr->name_hash;  /* hash + entity flag */
        dst_attr->ns_cache_off = 0;  /* set below if source has cache */

        /* Copy namespace cache if present (TODO 173). */
        struct leptris_attr_ns_cache* src_ns = attr_get_ns_cache(src_attr);
        if (src_ns) {
            struct leptris_attr_ns_cache* dst_ns =
                (struct leptris_attr_ns_cache*)leptris_pool_alloc(
                    pool, sizeof(struct leptris_attr_ns_cache));
            if (dst_ns) {
                dst_ns->prefix_view = src_ns->prefix_view;
                dst_ns->namespace_uri_view = src_ns->namespace_uri_view;
                dst_ns->prefix = src_ns->prefix
                    ? leptris_pool_strdup(pool, src_ns->prefix) : NULL;
                dst_ns->namespace_uri = src_ns->namespace_uri
                    ? leptris_pool_strdup(pool, src_ns->namespace_uri) : NULL;
                attr_set_ns_cache(dst_attr, dst_ns);
            }
        }

        /* Entity flag: already carried in the copied name_hash
         * (bit 15, round 19). */

        /* Link to previous attribute or set as first attribute */
        if (!prev_dst_attr) {
            leptris_elem_set_first_attribute(copy, dst_attr);
        } else {
            leptris_attr_set_next(prev_dst_attr, dst_attr);
        }
        leptris_elem_set_last_attribute(copy, dst_attr);
        prev_dst_attr = dst_attr;
        src_attr = leptris_attr_next(src_attr);
    }

    /* Copy children recursively and link them */
    LeptrisElement first_child = NULL;
    LeptrisElement last_child = NULL;

    LeptrisElement child = (LeptrisElement)leptris_elem_first_child(source);
    while (child) {
        LeptrisNode* child_node = (LeptrisNode*)child;

        if (child_node->type == LEPTRIS_NODE_TYPE_ELEMENT) {
            /* Recursively copy element child using bulk allocation */
            LeptrisElement child_copy = leptris_element_copy_subtree_bulk_internal(
                copy, child, copy_map, copy_index, pool
            );

            if (child_copy) {
                /* Link to parent's child list */
                if (!first_child) {
                    first_child = child_copy;
                }
                last_child = child_copy;
            }
        } else if (child_node->type == LEPTRIS_NODE_TYPE_TEXT) {
            /* Copy text node (not bulk-allocated) */
            LeptrisTextNode* text = (LeptrisTextNode*)child;
            LeptrisTextNode* text_copy = leptris_text_create(text->content,
                text->content_len,
                leptris_element_get_pool(copy));
            if (text_copy) {
                /* Link as child using internal function */
                leptris_element_append_child_internal(copy, (LeptrisNode*)text_copy);
                if (!first_child) {
                    first_child = (LeptrisElement)text_copy;
                }
                if (last_child) {
                    leptris_elem_set_next_sibling(last_child, (LeptrisNode*)text_copy);
                }
                last_child = (LeptrisElement)text_copy;
            }
        }

        /* Use generic accessor to get next sibling - child might be any node type */
        LeptrisNode* next = leptris_node_get_next_sibling(child_node);
        if (!next) break;
        child = (LeptrisElement)next;
    }

    /* Set child pointers */
    leptris_elem_set_first_child(copy, (LeptrisNode*)first_child);
    leptris_elem_set_last_child(copy, (LeptrisNode*)last_child);

    /* Copy child count */
    copy->child_count = source->child_count;

    return copy;
}

/**
 * Copy subtree using bulk allocation (10-15% faster for large subtrees)
 *
 * Pre-allocates all element structures in a single contiguous block,
 * then initializes them. This reduces:
 * - Pool allocation overhead (1 call vs N calls)
 * - memset calls (1 call vs N calls)
 * - Cache misses (contiguous memory access)
 *
 * @param parent Parent element to attach copy to
 * @param source Source element to copy
 * @return Copy of the source element, or NULL on failure
 */
LeptrisElement leptris_element_append_copy_bulk(LeptrisElement parent, LeptrisElement source) {
    if (!parent || !source) return NULL;
    if (!leptris_element_get_document(parent) || !leptris_element_get_pool(parent)) {
        /* Fallback to regular copy if no pool */
        return leptris_element_append_copy(parent, source);
    }

    LeptrisMemoryPool* pool = leptris_element_get_pool(parent);

    /* Count subtree nodes to determine allocation size */
    LeptrisSubtreeStats stats;
    leptris_element_count_subtree(source, &stats);

    size_t total_elements = stats.element_count;
    if (total_elements == 0) return NULL;

    /* Allocate all element structures in one batch */
    LeptrisElement* copy_map = leptris_element_create_copy_map(pool, total_elements);
    if (!copy_map) {
        /* Fallback to regular copy if batch allocation fails */
        return leptris_element_append_copy(parent, source);
    }

    /* Copy the subtree using pre-allocated elements */
    size_t copy_index = 0;
    LeptrisElement copy = leptris_element_copy_subtree_bulk_internal(
        NULL, source, copy_map, &copy_index, pool
    );

    if (!copy) {
        return NULL;
    }

    /* TODO 155 Phase A: document field removed; copy reaches doc
     * via parent chain once appended below. */

    /* Attach to parent */
    if (leptris_element_append_child(parent, copy) != LEPTRIS_OK) {
        return NULL;
    }

    return copy;
}

/* #804 perf: detached deep-copy core. Threads the destination pool
 * through the recursion — no per-node document/pool resolution, no
 * per-element root-map registration (a malloc each), no public-API
 * mutation dispatch. The TOP copy registers in the root map once so
 * the detached tree still resolves its document; descendants reach
 * it through the parent chain they already carry. Fidelity matches
 * leptris_element_append_copy: elements, text, cdata, comment, PI
 * children (issue #696), attributes, and namespace declarations
 * (issue #721). */
static LeptrisElement copy_subtree_detached(LeptrisElement source,
                                            LeptrisElement parent_copy,
                                            LeptrisMemoryPool* pool,
                                            struct leptris_document* doc) {
    LeptrisStringView name_view = leptris_element_name_view(source);
    if (leptris_sv_is_empty(&name_view)) return NULL;
    char* name_copy = leptris_sv_to_cstr_pooled(&name_view, pool);
    if (!name_copy) return NULL;
    LeptrisElement copy = leptris_element_create_with_view(
        leptris_sv_from_cstr(name_copy), pool);
    if (!copy) return NULL;
    leptris_elem_set_parent(copy, parent_copy);

    if (copy_element_namespaces_pooled(copy, source, pool))
        doc->has_namespaces = 1;

    for (struct leptris_attribute* sa =
             leptris_element_get_first_attribute(source);
         sa; sa = leptris_attr_next(sa)) {
        if (leptris_sv_is_empty(&sa->name_view)) continue;
        char* n = leptris_pool_strdup(pool, sa->name_view.data);
        if (!n) continue;
        LeptrisStringView sav_ = leptris_attr_value_sv(sa);
        char* v = leptris_sv_is_empty(&sav_)
                      ? NULL
                      : leptris_pool_strdup(pool, sav_.data);
        LeptrisStringView nv = leptris_sv_from_cstr(n);
        LeptrisStringView vv =
            v ? leptris_sv_from_cstr(v) : leptris_sv_from_cstr("");
        leptris_element_add_attribute(copy, nv, vv, pool);
    }

    LeptrisNodeRef first = NULL;
    LeptrisNodeRef last = NULL;
    for (LeptrisNodeRef c = leptris_elem_first_child(source); c;
         c = leptris_node_get_next_sibling(c)) {
        int ty = leptris_node_get_type(c);
        LeptrisNodeRef cc = NULL;
        if (ty == LEPTRIS_NODE_TYPE_ELEMENT) {
            cc = (LeptrisNodeRef)copy_subtree_detached((LeptrisElement)c,
                                                       copy, pool, doc);
        } else if (ty == LEPTRIS_NODE_TYPE_TEXT) {
            LeptrisTextNode* t = (LeptrisTextNode*)c;
            cc = (LeptrisNodeRef)leptris_text_create(t->content,
                                                     t->content_len, pool);
            if (cc) leptris_textnode_set_parent((LeptrisTextNode*)cc, copy);
        } else if (ty == LEPTRIS_NODE_TYPE_CDATA) {
            LeptrisCDATANode* cd = (LeptrisCDATANode*)c;
            cc = (LeptrisNodeRef)leptris_cdata_create(
                cd->content, cd->content ? strlen(cd->content) : 0, pool);
            if (cc) leptris_cdata_set_parent((LeptrisCDATANode*)cc, copy);
        } else if (ty == LEPTRIS_NODE_TYPE_COMMENT) {
            LeptrisCommentNode* cm = (LeptrisCommentNode*)c;
            cc = (LeptrisNodeRef)leptris_comment_create(
                cm->content, cm->content ? strlen(cm->content) : 0, pool);
            if (cc)
                leptris_comment_set_parent((LeptrisCommentNode*)cc, copy);
        } else if (ty == LEPTRIS_NODE_TYPE_PI) {
            LeptrisPINode* pi = (LeptrisPINode*)c;
            cc = (LeptrisNodeRef)leptris_pi_create(
                pi->target, pi->target ? strlen(pi->target) : 0,
                pi->data ? pi->data : "",
                pi->data ? strlen(pi->data) : 0, pool);
            if (cc) leptris_pi_set_parent((LeptrisPINode*)cc, copy);
        }
        if (!cc) continue;
        if (!first) {
            first = cc;
        } else {
            leptris_node_set_next_sibling(last, cc);
        }
        last = cc;
    }
    leptris_elem_set_first_child(copy, first);
    leptris_elem_set_last_child(copy, last);
    /* Issue #213 semantics: child_count counts ELEMENT children. */
    copy->child_count = source->child_count;
    return copy;
}

/* Issue #148 Phase 1: detached deep copy.
 *
 * Copies the subtree into dest_doc's pool (one register of the copy
 * root keeps document resolution working while detached) and
 * returns it with no parent — the caller attaches it. All node
 * kinds, attributes, and namespace declarations survive (issues
 * #696/#721); leptris_document_free on dest_doc releases
 * everything. */
LEPTRIS_API LeptrisElement leptris_element_copy(LeptrisElement src,
                                              LeptrisDocument dest_doc) {
    if (!src || !dest_doc) return NULL;
    if (!dest_doc->pool) return NULL;

    /* Trigger lazy promote on dest so the pool is initialized. */
    leptris_document_ensure_promoted(dest_doc);

    LeptrisElement copy =
        copy_subtree_detached(src, NULL, dest_doc->pool, dest_doc);
    if (!copy) return NULL;

    /* Pool-named copies carry no name backpointer — one root-map
     * registration keeps the detached tree resolving to dest_doc. */
    leptris_root_doc_register(copy, dest_doc);
    return copy;
}


/* Full-document deep copy (Issue #148 Phase 1).
 *
 * Builds a fresh LeptrisDocument then uses leptris_element_copy to
 * duplicate the root. Carries the XML declaration
 * (version/encoding/standalone) and the document-level PIs.
 */
LEPTRIS_API LeptrisDocument leptris_document_copy(LeptrisDocument src) {
    if (!src) return NULL;

    /* Force lazy promotion so src->new_dom_root is populated. */
    leptris_document_ensure_promoted(src);

    LeptrisDocument dest = (LeptrisDocument)calloc(1, sizeof(*dest));
    if (!dest) return NULL;
    dest->strict_mode = src->strict_mode;
    dest->ref_count = 1;

    /* Pool — sized to source pool's used bytes for one-shot alloc. */
    size_t page_size = 4096;
    if (src->pool) {
        /* Estimate: same size as source's current page. */
        page_size = src->pool->page_size;
    }
    dest->pool = leptris_pool_create_with_page_size(page_size);
    if (!dest->pool) { free(dest); return NULL; }
    dest->page_base = leptris_pool_get_base(dest->pool);

    /* XML declaration fields — heap-strdup; freed by leptris_document_free. */
    if (src->encoding) {
        dest->encoding = strdup(src->encoding);
        if (!dest->encoding) goto fail;
    }
    if (src->xml_version) {
        dest->xml_version = strdup(src->xml_version);
        if (!dest->xml_version) goto fail;
    }
    dest->standalone = src->standalone;
    dest->had_declaration = src->had_declaration;

    /* Root tree. */
    LeptrisElement root_copy = NULL;
    if (src->new_dom_root) {
        root_copy = leptris_element_copy((LeptrisElement)src->new_dom_root,
                                         dest);
        if (!root_copy) goto fail;
        dest->new_dom_root = root_copy;
    }

    /* Document children (issue #580): copy each doc-level node and
     * splice the root copy in at its chain position — the dest chain
     * mirrors [prolog..., root, epilog...]. */
    {
        LeptrisNode* tail = NULL;
        for (LeptrisNode* c = (LeptrisNode*)src->doc_children_head; c;
             c = leptris_node_get_next_sibling(c)) {
            LeptrisNode* dup = NULL;
            if (c->type == LEPTRIS_NODE_TYPE_ELEMENT) {
                if ((LeptrisElement)c == (LeptrisElement)src->new_dom_root)
                    dup = (LeptrisNode*)root_copy;
                if (!dup) continue;
            } else if (c->type == LEPTRIS_NODE_TYPE_PI) {
                LeptrisPINode* pi = (LeptrisPINode*)c;
                dup = (LeptrisNode*)leptris_pi_node_create(
                    dest,
                    pi->target ? pi->target : "",
                    pi->data ? pi->data : "");
            } else if (c->type == LEPTRIS_NODE_TYPE_COMMENT) {
                dup = (LeptrisNode*)leptris_comment_node_create(
                    dest,
                    leptris_comment_get_content((LeptrisCommentNode*)c));
            }
            if (!dup) goto fail;
            if (tail) leptris_node_set_next_sibling(tail, dup);
            else dest->doc_children_head = dup;
            tail = dup;
        }
        dest->doc_children_tail = tail;
    }

    return dest;

fail:
    leptris_document_free(dest);
    return NULL;
}


