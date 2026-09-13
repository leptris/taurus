/* dom/node_public.c — Public LeptrisNodeRef API.
 *
 * Extracted from leptris.c (TODO 42 phase 2). These are the public-facing
 * wrappers around the node-navigation helpers in dom/node.c.
 */

#include "../include/leptris.h"
#include "../leptris_internal.h"
#include "element.h"
#include "element_index.h"
#include "node.h"
#include "text.h"
#include "comment.h"
#include "cdata.h"
#include "pi.h"
#include "document_node.h"
#include "root_doc_map.h"
#include <stdio.h>
#include <string.h>

LEPTRIS_API int leptris_node_get_type(LeptrisNodeRef node) {
    if (!node) return 0; /* LEPTRIS_NODE_TYPE_ELEMENT */
    return (int)node->type;
}

LEPTRIS_API LeptrisNodeRef leptris_node_first_child(LeptrisNodeRef node) {
    /* Use the internal accessor that returns ANY child type (text,
     * element, CDATA, etc.) — the public leptris_element_get_first_child
     * would skip non-element children and break node-level traversal. */
    return (LeptrisNodeRef)leptris_node_first_child_internal((LeptrisNode*)node);
}

LEPTRIS_API LeptrisNodeRef leptris_node_last_child(LeptrisNodeRef node) {
    return (LeptrisNodeRef)leptris_node_last_child_internal((LeptrisNode*)node);
}

LEPTRIS_API LeptrisNodeRef leptris_node_next_sibling(LeptrisNodeRef node) {
    if (!node) return NULL;
    return (LeptrisNodeRef)leptris_node_get_next_sibling(node);
}

LEPTRIS_API LeptrisNodeRef leptris_node_previous_sibling(LeptrisNodeRef node) {
    if (!node) return NULL;

    /* Issue #182: previous_sibling must work for any node type, not
     * just elements. leptris_node_parent (added in #168) gives us the
     * parent for any node; we then walk the parent's child chain to
     * find the node immediately preceding this one. O(N) over the
     * number of siblings, which is acceptable for typical docs. */
    LeptrisElement parent = leptris_node_parent(node);
    if (!parent) return NULL;

    LeptrisNodeRef prev = NULL;
    LeptrisNodeRef child = (LeptrisNodeRef)leptris_elem_first_child(parent);
    while (child && child != node) {
        prev = child;
        child = (LeptrisNodeRef)leptris_node_get_next_sibling(child);
    }
    /* If child == node, we found it; return prev. If child == NULL,
     * node isn't in parent's chain (corrupt tree) -- return NULL. */
    return (child == node) ? prev : NULL;
}

LEPTRIS_API size_t leptris_node_child_count(LeptrisNodeRef node) {
    if (!node) return 0;
    return leptris_node_child_count_internal(node);
}

LEPTRIS_API LeptrisElement leptris_node_as_element(LeptrisNodeRef node) {
    if (!node || node->type != LEPTRIS_NODE_TYPE_ELEMENT) return NULL;
    return (LeptrisElement)node;
}

LEPTRIS_API LeptrisNodeRef leptris_element_as_node(LeptrisElement elem) {
    return (LeptrisNodeRef)elem;
}

LEPTRIS_API const char* leptris_text_node_get_content(LeptrisNodeRef node) {
    if (!node) return NULL;
    if (node->type == LEPTRIS_NODE_TYPE_TEXT) {
        return leptris_text_get_content((LeptrisTextNode*)node);
    }
    if (node->type == LEPTRIS_NODE_TYPE_CDATA) {
        return ((LeptrisCDATANode*)node)->content;
    }
    return NULL;
}

LEPTRIS_API const char* leptris_comment_node_get_content(LeptrisNodeRef node) {
    if (!node || node->type != LEPTRIS_NODE_TYPE_COMMENT) return NULL;
    return ((LeptrisCommentNode*)node)->content;
}

LEPTRIS_API const char* leptris_cdata_node_get_content(LeptrisNodeRef node) {
    if (!node || node->type != LEPTRIS_NODE_TYPE_CDATA) return NULL;
    return ((LeptrisCDATANode*)node)->content;
}

LEPTRIS_API const char* leptris_pi_node_get_target(LeptrisNodeRef node) {
    if (!node || node->type != LEPTRIS_NODE_TYPE_PI) return NULL;
    return ((LeptrisPINode*)node)->target;
}

LEPTRIS_API const char* leptris_pi_node_get_data(LeptrisNodeRef node) {
    if (!node || node->type != LEPTRIS_NODE_TYPE_PI) return NULL;
    return ((LeptrisPINode*)node)->data;
}

/* ============================================================================
 * Typed node creators (issue #167).
 *
 * Each creator allocates from the document's pool, copies the content
 * into the pool, and returns an unattached node. Use
 * leptris_element_append_child to attach.
 *
 * For setter functions, the new content is pool-copied so the caller
 * may free or modify the input immediately. The previous content is
 * NOT freed separately -- it lives in the pool and is reclaimed when
 * the document is freed.
 * ============================================================================ */

/* Look up the document that owns a node. Goes via the parent element
 * (every node reachable from a tree was attached via append_child,
 * which sets parent_off and propagates the document pointer on
 * element children). For unattached nodes, returns NULL. */
/* ---- Document-level processing instructions (issue #526) -------
 * Document-level comments and PIs are tree children of the document
 * node (issue #580): one node chain, [prolog..., root, epilog...].
 * The #526 flat accessors below are the cheap flat view over that
 * chain. */

LEPTRIS_API LeptrisNodeRef leptris_document_node(LeptrisDocument doc) {
    if (!doc) return NULL;
    return (LeptrisNodeRef)leptris_document_get_node(doc);
}

LEPTRIS_API size_t leptris_document_pi_count(LeptrisDocument doc) {
    if (!doc) return 0;
    size_t n = 0;
    for (LeptrisNode* c = (LeptrisNode*)doc->doc_children_head; c;
         c = leptris_node_get_next_sibling(c))
        if (c->type == LEPTRIS_NODE_TYPE_PI) n++;
    return n;
}

LEPTRIS_API const char* leptris_document_pi_target(LeptrisDocument doc,
                                                   size_t index) {
    if (!doc) return NULL;
    size_t i = 0;
    for (LeptrisNode* c = (LeptrisNode*)doc->doc_children_head; c;
         c = leptris_node_get_next_sibling(c)) {
        if (c->type != LEPTRIS_NODE_TYPE_PI) continue;
        if (i == index)
            return leptris_pi_get_target((LeptrisPINode*)c);
        i++;
    }
    return NULL;
}

LEPTRIS_API const char* leptris_document_pi_data(LeptrisDocument doc,
                                                 size_t index) {
    if (!doc) return NULL;
    size_t i = 0;
    for (LeptrisNode* c = (LeptrisNode*)doc->doc_children_head; c;
         c = leptris_node_get_next_sibling(c)) {
        if (c->type != LEPTRIS_NODE_TYPE_PI) continue;
        if (i == index)
            return leptris_pi_get_data((LeptrisPINode*)c);
        i++;
    }
    return NULL;
}

/* ---- Document-level comments (issue #578) ------------------------ */
LEPTRIS_API size_t leptris_document_comment_count(LeptrisDocument doc) {
    if (!doc) return 0;
    size_t n = 0;
    for (LeptrisNode* c = (LeptrisNode*)doc->doc_children_head; c;
         c = leptris_node_get_next_sibling(c))
        if (c->type == LEPTRIS_NODE_TYPE_COMMENT) n++;
    return n;
}

LEPTRIS_API const char* leptris_document_comment_content(LeptrisDocument doc,
                                                         size_t index) {
    if (!doc) return NULL;
    size_t i = 0;
    for (LeptrisNode* c = (LeptrisNode*)doc->doc_children_head; c;
         c = leptris_node_get_next_sibling(c)) {
        if (c->type != LEPTRIS_NODE_TYPE_COMMENT) continue;
        if (i == index)
            return leptris_comment_get_content((LeptrisCommentNode*)c);
        i++;
    }
    return NULL;
}

LEPTRIS_API LeptrisNodeRef leptris_document_add_pi(LeptrisDocument doc,
                                                   const char* target,
                                                   const char* data) {
    if (!doc || !target || !*target) return NULL;
    LeptrisNodeRef n = leptris_pi_node_create(doc, target, data);
    if (!n) return NULL;
    /* Insert at the end of the PROLOG (before the root element) —
     * the #526 contract: added PIs serialize before the root. On a
     * rootless document the chain is empty; append. */
    LeptrisNode* rootn = (LeptrisNode*)doc->new_dom_root;
    if (!rootn) rootn = (LeptrisNode*)doc->root;
    if (!rootn) {
        LeptrisNode* tail = (LeptrisNode*)doc->doc_children_tail;
        if (tail) leptris_node_set_next_sibling(tail, (LeptrisNode*)n);
        else doc->doc_children_head = n;
        doc->doc_children_tail = n;
        return n;
    }
    LeptrisNode* prev = NULL;
    LeptrisNode* c = (LeptrisNode*)doc->doc_children_head;
    while (c && c != rootn) {
        prev = c;
        c = leptris_node_get_next_sibling(c);
    }
    if (prev) leptris_node_set_next_sibling(prev, (LeptrisNode*)n);
    else doc->doc_children_head = n;
    leptris_node_set_next_sibling((LeptrisNode*)n, rootn);
    return n;
}

/* #1032: the add_pi twin for comments — document-level comments
 * parse and serialize (#578) but had no writer. Appends at the END
 * of the document children chain (epilog, after the root element):
 * the Document#add_child parity moxml asks for, so rebuilding a
 * parsed `<root/><!-- tail -->` round-trips. */
LEPTRIS_API LeptrisNodeRef leptris_document_add_comment(
    LeptrisDocument doc, const char* content) {
    if (!doc) return NULL;
    LeptrisNodeRef n = leptris_comment_node_create(doc, content);
    if (!n) return NULL;
    LeptrisNode* tail = (LeptrisNode*)doc->doc_children_tail;
    if (tail) leptris_node_set_next_sibling(tail, (LeptrisNode*)n);
    else doc->doc_children_head = n;
    doc->doc_children_tail = n;
    return n;
}

static LeptrisDocument node_public_document(LeptrisNodeRef node) {
    if (!node) return NULL;
    if (node->type == LEPTRIS_NODE_TYPE_ELEMENT) {
        return leptris_element_get_document((LeptrisElement)node);
    }
    LeptrisElement parent = leptris_node_parent(node);
    if (parent) return leptris_element_get_document(parent);
    /* Issue #519: detached non-element nodes carry their owning
     * document — mutations must work before any attach. */
    switch (node->type) {
        case LEPTRIS_NODE_TYPE_PI:
            return ((LeptrisPINode*)node)->owner_doc;
        case LEPTRIS_NODE_TYPE_COMMENT:
            return ((LeptrisCommentNode*)node)->owner_doc;
        case LEPTRIS_NODE_TYPE_CDATA:
            return ((LeptrisCDATANode*)node)->owner_doc;
        default:
            return NULL;
    }
}

/* Pool-strdup helper scoped to this TU. */
static char* node_public_pool_strdup(LeptrisMemoryPool* pool,
                                      const char* s, size_t len) {
    if (!pool) return NULL;
    char* copy = (char*)leptris_pool_alloc(pool, len + 1);
    if (!copy) return NULL;
    if (len > 0) memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

LEPTRIS_API LeptrisNodeRef leptris_text_node_create(LeptrisDocument doc,
                                                  const char* content) {
    if (!doc) return NULL;
    /* TODO 139 Phase D: trigger lazy promote so doc->pool is set. */
    leptris_document_ensure_promoted(doc);
    if (!doc->pool) return NULL;
    if (!content) content = "";
    size_t len = strlen(content);
    LeptrisTextNode* n = leptris_text_create_borrowed(content, len, doc->pool);
    if (!n) return NULL;
    /* Materialize a pool-owned NUL-terminated copy so future reads via
     * leptris_text_node_get_content don't have to lazy-alloc. */
    char* copy = node_public_pool_strdup(doc->pool, content, len);
    if (copy) {
        n->content = copy;
        n->content_len = len;
        n->borrowed = 0;
    }
    return (LeptrisNodeRef)n;
}

LEPTRIS_API LeptrisNodeRef leptris_comment_node_create(LeptrisDocument doc,
                                                     const char* content) {
    if (!doc) return NULL;
    leptris_document_ensure_promoted(doc);
    if (!doc->pool) return NULL;
    if (!content) content = "";
    LeptrisCommentNode* n = leptris_comment_create(content, strlen(content),
                                                   doc->pool);
    if (n) n->owner_doc = doc;
    return (LeptrisNodeRef)n;
}

LEPTRIS_API LeptrisNodeRef leptris_cdata_node_create(LeptrisDocument doc,
                                                   const char* content) {
    if (!doc) return NULL;
    leptris_document_ensure_promoted(doc);
    if (!doc->pool) return NULL;
    if (!content) content = "";
    LeptrisCDATANode* n = leptris_cdata_create(content, strlen(content),
                                              doc->pool);
    if (n) n->owner_doc = doc;
    return (LeptrisNodeRef)n;
}

LEPTRIS_API LeptrisNodeRef leptris_pi_node_create(LeptrisDocument doc,
                                                const char* target,
                                                const char* data) {
    if (!doc) return NULL;
    leptris_document_ensure_promoted(doc);
    if (!doc->pool || !target) return NULL;
    if (!data) data = "";
    LeptrisPINode* n = leptris_pi_create(target, strlen(target),
                                         data, strlen(data), doc->pool);
    if (n) n->owner_doc = doc;
    return (LeptrisNodeRef)n;
}

LEPTRIS_API LeptrisStatus leptris_text_node_set_content(LeptrisNodeRef node,
                                                      const char* content) {
    if (!node) return LEPTRIS_ERROR_NULL_ARG;
    if (node->type != LEPTRIS_NODE_TYPE_TEXT) return LEPTRIS_ERROR_INVALID_ARG;
    if (!content) content = "";
    size_t len = strlen(content);
    LeptrisTextNode* t = (LeptrisTextNode*)node;
    if (!t->pool) {
        /* Use parent doc pool if the node's own pool is unset
         * (happens after manual construction in some paths). */
        LeptrisDocument doc = node_public_document(node);
        if (!doc || !doc->pool) return LEPTRIS_ERROR_INVALID_ARG;
        t->pool = doc->pool;
    }
    char* copy = node_public_pool_strdup(t->pool, content, len);
    if (!copy) return LEPTRIS_ERROR_MEMORY;
    t->content = copy;
    t->content_len = len;
    t->borrowed = 0;
    return LEPTRIS_OK;
}

LEPTRIS_API LeptrisStatus leptris_cdata_node_set_content(LeptrisNodeRef node,
                                                       const char* content) {
    if (!node) return LEPTRIS_ERROR_NULL_ARG;
    if (node->type != LEPTRIS_NODE_TYPE_CDATA) return LEPTRIS_ERROR_INVALID_ARG;
    if (!content) content = "";
    size_t len = strlen(content);
    LeptrisCDATANode* c = (LeptrisCDATANode*)node;
    LeptrisDocument doc = node_public_document(node);
    if (!doc || !doc->pool) return LEPTRIS_ERROR_INVALID_ARG;
    char* copy = node_public_pool_strdup(doc->pool, content, len);
    if (!copy) return LEPTRIS_ERROR_MEMORY;
    c->content = copy;
    return LEPTRIS_OK;
}

LEPTRIS_API LeptrisStatus leptris_comment_node_set_content(LeptrisNodeRef node,
                                                         const char* content) {
    if (!node) return LEPTRIS_ERROR_NULL_ARG;
    if (node->type != LEPTRIS_NODE_TYPE_COMMENT) return LEPTRIS_ERROR_INVALID_ARG;
    if (!content) content = "";
    size_t len = strlen(content);
    LeptrisCommentNode* c = (LeptrisCommentNode*)node;
    LeptrisDocument doc = node_public_document(node);
    if (!doc || !doc->pool) return LEPTRIS_ERROR_INVALID_ARG;
    char* copy = node_public_pool_strdup(doc->pool, content, len);
    if (!copy) return LEPTRIS_ERROR_MEMORY;
    c->content = copy;
    return LEPTRIS_OK;
}

LEPTRIS_API LeptrisStatus leptris_pi_node_set_target(LeptrisNodeRef node,
                                                   const char* target) {
    if (!node) return LEPTRIS_ERROR_NULL_ARG;
    if (node->type != LEPTRIS_NODE_TYPE_PI) return LEPTRIS_ERROR_INVALID_ARG;
    if (!target) return LEPTRIS_ERROR_NULL_ARG;
    size_t len = strlen(target);
    LeptrisPINode* p = (LeptrisPINode*)node;
    LeptrisDocument doc = node_public_document(node);
    if (!doc || !doc->pool) return LEPTRIS_ERROR_INVALID_ARG;
    char* copy = node_public_pool_strdup(doc->pool, target, len);
    if (!copy) return LEPTRIS_ERROR_MEMORY;
    p->target = copy;
    return LEPTRIS_OK;
}

LEPTRIS_API LeptrisStatus leptris_pi_node_set_data(LeptrisNodeRef node,
                                                 const char* data) {
    if (!node) return LEPTRIS_ERROR_NULL_ARG;
    if (node->type != LEPTRIS_NODE_TYPE_PI) return LEPTRIS_ERROR_INVALID_ARG;
    if (!data) data = "";
    size_t len = strlen(data);
    LeptrisPINode* p = (LeptrisPINode*)node;
    LeptrisDocument doc = node_public_document(node);
    if (!doc || !doc->pool) return LEPTRIS_ERROR_INVALID_ARG;
    char* copy = node_public_pool_strdup(doc->pool, data, len);
    if (!copy) return LEPTRIS_ERROR_MEMORY;
    p->data = copy;
    return LEPTRIS_OK;
}

/* ============================================================================
 * Generic parent + unlink (issue #168).
 * ============================================================================ */

LEPTRIS_API LeptrisElement leptris_node_parent(LeptrisNodeRef node) {
    if (!node) return NULL;
    if (node->type == LEPTRIS_NODE_TYPE_ELEMENT) {
        return leptris_element_get_parent((LeptrisElement)node);
    }
    /* Non-element nodes carry parent_off (issue #168). The parser
     * and append_child_internal both populate it when attaching. */
    switch (node->type) {
        case LEPTRIS_NODE_TYPE_TEXT:
            return leptris_textnode_parent((LeptrisTextNode*)node);
        case LEPTRIS_NODE_TYPE_COMMENT:
            return leptris_comment_parent((LeptrisCommentNode*)node);
        case LEPTRIS_NODE_TYPE_CDATA:
            return leptris_cdata_parent((LeptrisCDATANode*)node);
        case LEPTRIS_NODE_TYPE_PI:
            return leptris_pi_parent((LeptrisPINode*)node);
        default:
            return NULL;
    }
}

LEPTRIS_API LeptrisStatus leptris_node_unlink(LeptrisNodeRef node) {
    if (!node) return LEPTRIS_ERROR_NULL_ARG;
    LeptrisElement parent = leptris_node_parent(node);
    if (!parent) return LEPTRIS_ERROR_NOT_FOUND;

    /* Round 21: mutation-carved elements carry their doc in the name
     * backpointer (header bit 6) — they resolve statelessly whether
     * attached or orphaned. Pool-named elements have no backpointer;
     * register those in the root map so the orphan keeps resolving. */
    struct leptris_document* orphan_doc =
        (node->type == LEPTRIS_NODE_TYPE_ELEMENT &&
         !leptris_elem_has_namebp((LeptrisElement)node))
            ? leptris_element_get_document((LeptrisElement)node) : NULL;

    /* Splice node out of parent's child chain. We need the previous
     * sibling to update its next_sibling pointer; if node is the
     * first child, update parent.first_child instead. */
    LeptrisNodeRef prev = NULL;
    LeptrisNodeRef cur = (LeptrisNodeRef)leptris_elem_first_child(parent);
    while (cur && cur != node) {
        prev = cur;
        cur = (LeptrisNodeRef)leptris_node_get_next_sibling(cur);
    }
    if (!cur) {
        /* Node is not in parent's child chain -- corrupt tree state. */
        return LEPTRIS_ERROR_NOT_FOUND;
    }

    LeptrisNodeRef next = (LeptrisNodeRef)leptris_node_get_next_sibling(node);
    if (prev) {
        leptris_node_set_next_sibling(prev, next);
    } else {
        /* Node was first child. */
        leptris_elem_set_first_child(parent, next);
        /* If also last child (only child), clear last too. */
        if (leptris_elem_last_child(parent) == (LeptrisNode*)node) {
            leptris_elem_set_last_child(parent, prev ? (LeptrisNode*)prev : NULL);
        }
    }
    /* If node was last child, update last to prev. */
    if (leptris_elem_last_child(parent) == (LeptrisNode*)node) {
        leptris_elem_set_last_child(parent, prev ? (LeptrisNode*)prev : NULL);
    }

    /* Clear node's own sibling + parent links so it's a clean orphan. */
    leptris_node_set_next_sibling(node, NULL);
    if (node->type == LEPTRIS_NODE_TYPE_ELEMENT) {
        leptris_element_set_parent((LeptrisElement)node, NULL);
        if (orphan_doc) leptris_root_doc_register((LeptrisElement)node, orphan_doc);
    } else {
        switch (node->type) {
            case LEPTRIS_NODE_TYPE_TEXT:
                leptris_textnode_set_parent((LeptrisTextNode*)node, NULL);
                break;
            case LEPTRIS_NODE_TYPE_COMMENT:
                leptris_comment_set_parent((LeptrisCommentNode*)node, NULL);
                break;
            case LEPTRIS_NODE_TYPE_CDATA:
                leptris_cdata_set_parent((LeptrisCDATANode*)node, NULL);
                break;
            case LEPTRIS_NODE_TYPE_PI:
                leptris_pi_set_parent((LeptrisPINode*)node, NULL);
                break;
            default:
                break;
        }
    }

    if (node->type == LEPTRIS_NODE_TYPE_ELEMENT) {
        if (parent->child_count > 0) parent->child_count--;
    }
    /* Invalidate element index (mutation). */
    if (leptris_element_get_document(parent)) {
        leptris_element_index_invalidate(leptris_element_get_document(parent));
    }
    return LEPTRIS_OK;
}

/* ============================================================================
 * node_line + node_compare (issue #172).
 * ============================================================================ */

/* High bit of base.line marks a RESOLVED line; otherwise the field
 * holds byteOffset+1 into doc->xml_buffer (lazy line tracking — the
 * parse scans carry no '\n' compares). */
#define LEPTRIS_LINE_RESOLVED 0x80000000u

/* Reach the owning document from any node: elements go through the
 * root map; other node types hop their parent edge first. */
static struct leptris_document* node_document(LeptrisNodeRef node) {
    if (!node) return NULL;
    switch (node->type) {
    case LEPTRIS_NODE_TYPE_ELEMENT:
        return leptris_element_get_document((LeptrisElement)node);
    case LEPTRIS_NODE_TYPE_TEXT:
        return leptris_element_get_document(
            leptris_textnode_parent((const LeptrisTextNode*)node));
    case LEPTRIS_NODE_TYPE_COMMENT:
        return leptris_element_get_document(
            leptris_comment_parent((const LeptrisCommentNode*)node));
    case LEPTRIS_NODE_TYPE_CDATA:
        return leptris_element_get_document(
            leptris_cdata_parent((const LeptrisCDATANode*)node));
    case LEPTRIS_NODE_TYPE_PI:
        return leptris_element_get_document(
            leptris_pi_parent((const LeptrisPINode*)node));
    default:
        return NULL;
    }
}

/* Build (once) the per-document table of '\n' byte offsets. The
 * buffer is required to stay unmodified for the document's lifetime
 * (the same contract that keeps StringViews valid), so the table
 * never goes stale. */
static const uint32_t* doc_line_breaks(struct leptris_document* doc,
                                       size_t* count) {
    if (!doc->line_breaks) {
        size_t cap = 256, n_ = 0;
        uint32_t* a = (uint32_t*)malloc(cap * sizeof(uint32_t));
        const char* b = doc->xml_buffer;
        const char* end = b + doc->xml_buffer_len;
        const char* q = b;
        while (q < end) {
            const char* hit = (const char*)memchr(q, '\n',
                                                  (size_t)(end - q));
            if (!hit) break;
            if (n_ == cap) {
                cap *= 2;
                uint32_t* grown = (uint32_t*)realloc(
                    a, cap * sizeof(uint32_t));
                if (!grown) { free(a); return NULL; }
                a = grown;
            }
            a[n_++] = (uint32_t)(size_t)(hit - b);
            q = hit + 1;
        }
        doc->line_breaks = a;
        doc->line_break_count = n_;
    }
    *count = doc->line_break_count;
    return doc->line_breaks;
}

LEPTRIS_API int leptris_node_line(LeptrisNodeRef node) {
    if (!node) return 0;
    uint32_t v = node->line;
    if (v == 0) return 0;                       /* unknown */
    if (v & LEPTRIS_LINE_RESOLVED) return (int)(v & ~LEPTRIS_LINE_RESOLVED);
    /* Offset-encoded: resolve against the document buffer. */
    struct leptris_document* doc = node_document(node);
    if (!doc || !doc->xml_buffer) return 0;
    size_t off = (size_t)(v - 1);
    if (off > doc->xml_buffer_len) return 0;    /* defensive */
    size_t n_ = 0;
    const uint32_t* brks = doc_line_breaks(doc, &n_);
    if (!brks) return 0;
    /* line = 1 + number of newlines strictly before the node start */
    size_t lo = 0, hi = n_;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (brks[mid] < off) lo = mid + 1; else hi = mid;
    }
    node->line = LEPTRIS_LINE_RESOLVED | (uint32_t)(lo + 1);
    return (int)(lo + 1);
}

LEPTRIS_API void* leptris_node_get_binding_wrapper(LeptrisNodeRef node) {
    return node ? node->binding_wrapper : NULL;
}

LEPTRIS_API void leptris_node_set_binding_wrapper(LeptrisNodeRef node, void* wrapper) {
    if (node) node->binding_wrapper = wrapper;
}

LEPTRIS_API int leptris_node_compare(LeptrisNodeRef a, LeptrisNodeRef b) {
    if (a == b) return 0;
    if (!a || !b) return 0;
    /* Document-order comparison requires walking ancestor chains to
     * find the common ancestor and comparing sibling positions. The
     * compact-pointer tree supports this via parent accessors on
     * elements; non-element nodes lack a parent pointer (see
     * leptris_node_parent above). For elements, fall back to pointer
     * identity for now -- a correct implementation is a future
     * enhancement. */
    if (a < b) return -1;
    return 1;
}

/* ----- leptris_node_get_xpath helpers (TODO 148 Phase 3) ----- */

/* Count element siblings with the same name that appear at or before
 * `elem` in document order. Returns the 1-based position and writes
 * the total same-named count to *total (so the caller can decide
 * whether the [N] suffix is needed). */
static uint32_t count_same_name_element_position(LeptrisElement elem,
                                                  uint32_t* total) {
    uint32_t pos = 0;
    uint32_t same = 0;
    const char* name = leptris_element_name(elem);
    LeptrisElement parent = leptris_element_parent(elem);
    if (!parent) {
        if (total) *total = 1;
        return 1;
    }
    LeptrisNodeRef child = leptris_node_first_child(leptris_element_as_node(parent));
    while (child) {
        if (leptris_node_get_type(child) == 0 /* ELEMENT */) {
            LeptrisElement e = (LeptrisElement)child;
            const char* cn = leptris_element_name(e);
            if (cn && name && strcmp(cn, name) == 0) {
                same++;
                if (e == elem) pos = same;
            }
        }
        child = leptris_node_next_sibling(child);
    }
    if (total) *total = same;
    return pos;
}

LEPTRIS_API char* leptris_node_get_xpath(LeptrisNodeRef node) {
    if (!node) return NULL;

    /* Walk up collecting ancestors; the path is built deepest-first
     * then reversed. The buffer is heap-grown as needed. */
    char* buf = (char*)malloc(64);
    if (!buf) return NULL;
    size_t len = 0;
    size_t cap = 64;
    buf[0] = '\0';

    /* For non-element nodes, render the type-test marker as the
     * DEEPEST segment. We walk up the element chain from the parent
     * and append the marker last (after the reverse concat). */
    LeptrisNodeRef cur = node;
    int leaf_type = leptris_node_get_type(cur);
    LeptrisNodeRef start_elem;
    if (leaf_type == 0 /* ELEMENT */) {
        start_elem = cur;
    } else {
        LeptrisElement parent = leptris_node_parent(cur);
        if (!parent) {
            /* Detached non-element node — emit just the marker. */
            const char* marker = "text()";
            if (leaf_type == 2) marker = "comment()";
            else if (leaf_type == 4) marker = "processing-instruction()";
            size_t mlen = strlen(marker);
            if (mlen + 2 > cap) {
                free(buf);
                buf = (char*)malloc(mlen + 2);
                if (!buf) return NULL;
                cap = mlen + 2;
            }
            buf[len++] = '/';
            memcpy(buf + len, marker, mlen);
            len += mlen;
            buf[len] = '\0';
            return buf;
        }
        start_elem = leptris_element_as_node(parent);
    }

    /* Walk up the element chain, rendering each segment into a
     * temp list, then concat in reverse. */
    char* segs[256];
    size_t seg_lens[256];
    int nseg = 0;
    LeptrisNodeRef walk = start_elem;
    while (walk && nseg < 256) {
        LeptrisElement e = (LeptrisElement)walk;
        uint32_t total = 0;
        uint32_t pos = count_same_name_element_position(e, &total);
        const char* name = leptris_element_name(e);
        if (!name) break;

        char tmp[256];
        int written;
        if (total > 1) {
            written = snprintf(tmp, sizeof(tmp), "/%s[%u]", name, pos);
        } else {
            written = snprintf(tmp, sizeof(tmp), "/%s", name);
        }
        if (written < 0) break;
        size_t sl = (size_t)written;
        char* seg = (char*)malloc(sl + 1);
        if (!seg) {
            for (int i = 0; i < nseg; i++) free(segs[i]);
            free(buf);
            return NULL;
        }
        memcpy(seg, tmp, sl + 1);
        segs[nseg] = seg;
        seg_lens[nseg] = sl;
        nseg++;

        LeptrisElement parent = leptris_element_parent(e);
        if (!parent) break;
        walk = leptris_element_as_node(parent);
    }

    /* Concat in reverse order (root first). */
    for (int i = nseg - 1; i >= 0; i--) {
        if (len + seg_lens[i] + 1 > cap) {
            while (cap < len + seg_lens[i] + 1) cap *= 2;
            char* new_buf = (char*)realloc(buf, cap);
            if (!new_buf) {
                for (int j = 0; j < nseg; j++) free(segs[j]);
                free(buf);
                return NULL;
            }
            buf = new_buf;
        }
        memcpy(buf + len, segs[i], seg_lens[i]);
        len += seg_lens[i];
        buf[len] = '\0';
        free(segs[i]);
    }

    /* Append the leaf marker for non-element nodes. */
    if (leaf_type != 0) {
        const char* marker = "text()";
        if (leaf_type == 2) marker = "comment()";
        else if (leaf_type == 4) marker = "processing-instruction()";
        size_t mlen = strlen(marker);
        if (len + mlen + 2 > cap) {
            while (cap < len + mlen + 2) cap *= 2;
            char* new_buf = (char*)realloc(buf, cap);
            if (!new_buf) return buf;  /* best-effort: return what we have */
            buf = new_buf;
        }
        buf[len++] = '/';
        memcpy(buf + len, marker, mlen);
        len += mlen;
        buf[len] = '\0';
    }

    return buf;
}

LEPTRIS_API int leptris_node_traverse(LeptrisNodeRef root,
                                     LeptrisTraverseOrder order,
                                     int (*callback)(LeptrisNodeRef node,
                                                     void* user_data),
                                     void* user_data) {
    if (!root || !callback) return -1;
    if (order != LEPTRIS_TRAVERSE_PRE_ORDER &&
        order != LEPTRIS_TRAVERSE_POST_ORDER) {
        return -1;
    }

    /* Iterative DFS with an explicit 256-deep stack (matches
     * leptris_node_freeze). Each frame carries a "done" flag:
     *   false → still need to descend into first child
     *   true  → children exhausted, ready to visit (post-order) or pop
     * Stack depth is bounded by tree depth, not branching factor —
     * siblings are visited via next-sibling on pop, not pushed eagerly.
     * This keeps a 4KB stack frame regardless of fan-out. */
    typedef struct { LeptrisNode* node; int done; } TraverseFrame;
    TraverseFrame stack[256];
    int depth = 0;
    stack[depth].node = (LeptrisNode*)root;
    stack[depth].done = 0;
    depth++;

    int count = 0;
    while (depth > 0) {
        TraverseFrame* f = &stack[depth - 1];
        if (!f->done) {
            if (order == LEPTRIS_TRAVERSE_PRE_ORDER) {
                if (callback((LeptrisNodeRef)f->node, user_data) != 0) {
                    return count;
                }
                count++;
            }
            f->done = 1;
            LeptrisNode* child = leptris_node_first_child_internal(f->node);
            if (child && depth < 256) {
                stack[depth].node = child;
                stack[depth].done = 0;
                depth++;
            }
        } else {
            if (order == LEPTRIS_TRAVERSE_POST_ORDER) {
                if (callback((LeptrisNodeRef)f->node, user_data) != 0) {
                    return count;
                }
                count++;
            }
            LeptrisNode* sib = leptris_node_get_next_sibling(f->node);
            depth--;
            if (sib && depth < 256) {
                stack[depth].node = sib;
                stack[depth].done = 0;
                depth++;
            }
        }
    }

    return count;
}
