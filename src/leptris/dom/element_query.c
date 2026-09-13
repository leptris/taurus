/* dom/element_query.c — Public element/attribute/text/namespace query API.
 *
 * Extracted from leptris.c (TODO 42 phase 3). These are the read-only
 * inspection functions: leptris_element_*, leptris_element_attribute_*,
 * leptris_element_text_*, leptris_element_namespace_*.
 *
 * Mutation (set_attribute, add_namespace) lives here too — they're
 * typed wrappers around the underlying accessors.
 */

#include "../include/leptris.h"
#include "../leptris_internal.h"
#include "element.h"
#include "node.h"
#include "text.h"

/* Forward decls from leptris_memory.c — avoid pulling leptris_memory.h
 * directly because it conflicts with pi.h's leptris_pi_free. */
int leptris_element_add_namespace(struct leptris_element* elem,
                                  struct leptris_namespace* ns);
struct leptris_namespace* leptris_namespace_new_pooled(const char* prefix,
                                                      const char* uri,
                                                      LeptrisMemoryPool* pool);
#include "cdata.h"
#include "comment.h"
#include "pi.h"
#include "../common/string_view.h"
#include "../common/entities.h"
#include "../common/port.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
/* Forward decls (issue #542): the expanded-name matcher and prefix
 * resolver are defined in the attribute section below. */
static struct leptris_attribute* find_attr_expanded(
    LeptrisElement elem, const char* uri, const char* local);
static const char* elem_resolve_attr_prefix(LeptrisElement elem,
                                            const char* prefix);
#include <math.h>

/* ============================================================================
 * Element Functions
 * ============================================================================ */

/**
 * Get element name
 */
LEPTRIS_API const char* leptris_element_name(LeptrisElement elem) {
    if (!elem) return "";

    /* Use compact accessor to get element name */
    const char* name = leptris_element_get_name(elem);
    return name ? name : "";
}

/* Locate the owning document by walking up the parent chain.
 * Only the parse-time root gets `document` set on every node, but detached
 * subtrees still reach an attached ancestor through `parent`. */
static struct leptris_document* element_owning_document(LeptrisElement elem) {
    for (; elem; elem = leptris_elem_parent(elem)) {
        if (leptris_element_get_document(elem)) return leptris_element_get_document(elem);
    }
    return NULL;
}

/**
 * Get element text content (concatenated recursively)
 *
 * The public contract is a `const char*` owned by the element, so the result
 * must live as long as the document — it can never be a malloc'd buffer the
 * caller has no way to release.  Two paths satisfy that:
 *
 *   1. A lone text/CDATA child already holds the entire content, NUL-terminated,
 *      in document-owned storage: return it directly (zero allocation).
 *   2. Mixed content needs concatenation, so the joined string is interned in
 *      the document pool and released with leptris_document_free().
 */
LEPTRIS_API const char* leptris_element_text(LeptrisElement elem) {
    if (!elem) return "";

    LeptrisNode* child = leptris_node_first_child_internal((LeptrisNode*)elem);
    if (!child) return "";

    /* Path 1: single character-data child — hand back its own storage. */
    if (!leptris_node_get_next_sibling(child)) {
        if (child->type == LEPTRIS_NODE_TYPE_TEXT) {
            const char* content = leptris_text_get_content((LeptrisTextNode*)child);
            return content ? content : "";
        }
        if (child->type == LEPTRIS_NODE_TYPE_CDATA) {
            const char* content = ((LeptrisCDATANode*)child)->content;
            return content ? content : "";
        }
    }

    /* Path 2: concatenate, then intern in the document pool. */
    struct leptris_document* doc = element_owning_document(elem);
    if (!doc || !doc->pool) return "";

    char* text = leptris_element_get_text_content(elem);
    if (!text) return "";
    const char* owned = leptris_pool_strdup(doc->pool, text);
    leptris_free(text);
    return owned ? owned : "";
}

/**
 * Get child element text value (first text node only, not recursive)
 * This is different from leptris_element_text which concatenates all text recursively
 *
 * Returns nullptr if no text/CDATA child exists, not empty string ""
 *
 * Behavior: Looks at the first child node. If it's text/CDATA, returns its content.
 * If it's an element, recursively gets the child value from that element (one level deep).
 */
LEPTRIS_API const char* leptris_element_child_value(LeptrisElement elem) {
    if (!elem) return NULL;

    /* Get first child node - use node API to get ALL children (not just elements) */
    LeptrisNode* child = leptris_node_first_child((LeptrisNode*)elem);
    if (!child) return NULL;

    /* If first child is a text node, return its content */
    if (child->type == LEPTRIS_NODE_TYPE_TEXT) {
        LeptrisTextNode* text = (LeptrisTextNode*)child;
        return leptris_text_get_content(text);
    }

    /* If first child is a CDATA node, return its content */
    if (child->type == LEPTRIS_NODE_TYPE_CDATA) {
        LeptrisCDATANode* cdata = (LeptrisCDATANode*)child;
        return cdata->content;
    }

    /* If first child is an element, recursively get its child value (one level deep) */
    if (child->type == LEPTRIS_NODE_TYPE_ELEMENT) {
        return leptris_element_child_value((LeptrisElement)child);
    }

    return NULL;  /* Comment, PI, or other node types have no text value */
}

/**
 * Get attribute value by name (Public API)
 * PERFORMANCE: Uses linked list traversal (O(n) where n = attribute count)
 */
LEPTRIS_API const char* leptris_element_attribute(LeptrisElement elem, const char* name) {
    if (!elem || !name) return NULL;

    /* Validate node type - only elements have attributes */
    LeptrisNode* node = (LeptrisNode*)elem;
    if (node->type != LEPTRIS_NODE_TYPE_ELEMENT) {
        return NULL;  /* Not an element node */
    }

    /* Issue #542 semantics (XML Namespaces 1.0):
     *   1. bare name matches ONLY the no-namespace attribute —
     *      "type" never finds xmi:type (mirror of #525)
     *   2. qualified p:local resolves p in scope and matches by
     *      URI+local — p:type finds q:type when the URIs agree
     *   3. xml is prebound and needs no declaration
     *   4. undeclared prefix -> NULL, never a string fallback
     *   5. xmlns / xmlns:* are declarations — never matched here */
    const char* colon = strchr(name, ':');
    struct leptris_attribute* attr;
    if (!colon) {
        if (strcmp(name, "xmlns") == 0) return NULL;      /* (5) */
        attr = find_attr_expanded(elem, NULL, name);      /* (1) */
    } else {
        size_t pl = (size_t)(colon - name);
        if (pl == 5 && memcmp(name, "xmlns", 5) == 0) return NULL;  /* (5) */
        char pbuf[64];
        if (pl >= sizeof(pbuf)) return NULL;
        memcpy(pbuf, name, pl);
        pbuf[pl] = '\0';
        const char* uri = elem_resolve_attr_prefix(elem, pbuf);  /* (3)(4) */
        if (!uri) return NULL;                             /* (4) */
        attr = find_attr_expanded(elem, uri, colon + 1);   /* (2) */
    }
    if (!attr) return NULL;

    /* Single representation (round 4): entity values expand lazily
     * INTO the view (owned copy); no-entity views are already
     * NUL-terminated C strings. */
    if (attr_has_entities(attr)) {
        LeptrisMemoryPool* pool = leptris_element_get_pool(elem);
        LeptrisStringView raw_v = leptris_attr_value_sv(attr);
        char* decoded = pool
            ? leptris_decode_entities_view(&raw_v, pool)
            : NULL;
        if (decoded) {
            leptris_attr_value_set_heap(attr, leptris_sv_from_cstr(decoded));
            attr_set_entities(attr, 0);
        }
    }
    if (leptris_attr_value_sv(attr).data && leptris_attr_value_sv(attr).length > 0)
        return leptris_attr_value_sv(attr).data;
    return "";   /* present-but-empty (see attribute_value_at) */
}

/* ---- Expanded-name attribute support (issue #542) -----------------
 *
 * XML Namespaces 1.0 attribute semantics, mirroring #525 for
 * elements: the PREFIX never matters, only the (URI, local) pair it
 * resolves to through the OWNING element's in-scope declarations.
 * The xml prefix is prebound; xmlns/xmlns:* are declarations and are
 * invisible to the attribute accessors. */

#define LEPTRIS_XML_NS "http://www.w3.org/XML/1998/namespace"

/* Resolve a prefix through elem's in-scope declarations with the
 * reserved prefixes handled. NULL result = undeclared. */
static const char* elem_resolve_attr_prefix(LeptrisElement elem,
                                            const char* prefix) {
    if (!prefix || !prefix[0]) return NULL;      /* no namespace */
    if (strcmp(prefix, "xml") == 0) return LEPTRIS_XML_NS;
    if (strcmp(prefix, "xmlns") == 0) return NULL;  /* reserved */
    return leptris_element_lookup_namespace(elem, prefix);
}

/* The attribute's prefix as a NUL-terminated string, or NULL for a
 * no-namespace attribute. Name-derived, immutable. */
LEPTRIS_API const char* leptris_attribute_prefix(LeptrisAttribute attr) {
    if (!attr) return NULL;
    const char* n = attr->name_view.data;
    size_t nl = attr->name_view.length;
    const char* colon = nl ? memchr(n, ':', nl) : NULL;
    if (!colon) return NULL;
    struct leptris_attr_ns_cache* c = attr_get_ns_cache(attr);
    if (c && c->prefix) return c->prefix;
    /* Not stamped (e.g. mutation-created before #542 setters run):
     * derive on the fly via a bounded stack buffer is NOT possible
     * (lifetime) — callers should treat NULL-cache as no prefix
     * data; the element-level matchers below do their own split. */
    return NULL;
}

/* The attribute's namespace URI resolved through the OWNER element's
 * in-scope declarations. NULL = no namespace or undeclared prefix.
 * Resolved per read (mutation-correct), O(depth) in the worst case. */
LEPTRIS_API const char* leptris_attribute_namespace_uri(LeptrisAttribute attr) {
    if (!attr) return NULL;
    struct leptris_attr_ns_cache* c = attr_get_ns_cache(attr);
    if (!c || !c->owner_elem) return NULL;
    const char* pfx = leptris_attribute_prefix(attr);
    if (!pfx) return NULL;
    return elem_resolve_attr_prefix(c->owner_elem, pfx);
}

/* Static empty-URI check: NULL or "" both mean no namespace. */
static int uri_is_none(const char* uri) {
    return !uri || !uri[0];
}

/* Find an attribute by EXPANDED name. uri NULL/"" matches only
 * no-namespace attributes; otherwise the URI must match what the
 * attribute's prefix resolves to (prefix-agnostic). Local names
 * compare exactly. xmlns declarations are skipped defensively. */
static struct leptris_attribute* find_attr_expanded(
        LeptrisElement elem, const char* uri, const char* local) {
    size_t ll = strlen(local);
    struct leptris_attribute* attr = leptris_element_get_first_attribute(elem);
    while (attr) {
        const char* n = attr->name_view.data;
        size_t nl = attr->name_view.length;
        if (nl < ll) goto next;
        const char* colon = nl ? memchr(n, ':', nl) : NULL;
        if (uri_is_none(uri)) {
            if (colon) goto next;                    /* namespaced */
            if (nl == ll && memcmp(n, local, ll) == 0) return attr;
        } else {
            if (!colon) goto next;                   /* no namespace */
            size_t pl = (size_t)(colon - n);
            if (nl - pl - 1 != ll || memcmp(colon + 1, local, ll) != 0)
                goto next;
            /* Prefix is xmlns? that's a declaration, not an attr. */
            if (pl == 5 && memcmp(n, "xmlns", 5) == 0) goto next;
            /* Resolve this attr's prefix through elem in scope. */
            char pbuf[64];
            const char* pfx;
            struct leptris_attr_ns_cache* c = attr_get_ns_cache(attr);
            if (c && c->prefix) {
                pfx = c->prefix;
            } else if (pl < sizeof(pbuf)) {
                memcpy(pbuf, n, pl);
                pbuf[pl] = '\0';
                pfx = pbuf;
            } else {
                goto next;
            }
            const char* resolved = elem_resolve_attr_prefix(elem, pfx);
            if (resolved && strcmp(resolved, uri) == 0) return attr;
        }
    next:
        attr = leptris_attr_next(attr);
    }
    return NULL;
}

LEPTRIS_API const char* leptris_element_attribute_ns(LeptrisElement elem,
                                                     const char* uri,
                                                     const char* local) {
    if (!elem || !local) return NULL;
    if (((LeptrisNode*)elem)->type != LEPTRIS_NODE_TYPE_ELEMENT) return NULL;
    struct leptris_attribute* attr = find_attr_expanded(elem, uri, local);
    if (!attr) return NULL;
    if (attr_has_entities(attr)) {
        LeptrisMemoryPool* pool = leptris_element_get_pool(elem);
        LeptrisStringView raw_v = leptris_attr_value_sv(attr);
        char* decoded = pool
            ? leptris_decode_entities_view(&raw_v, pool)
            : NULL;
        if (decoded) {
            leptris_attr_value_set_heap(attr, leptris_sv_from_cstr(decoded));
            attr_set_entities(attr, 0);
        }
    }
    if (leptris_attr_value_sv(attr).data && leptris_attr_value_sv(attr).length > 0)
        return leptris_attr_value_sv(attr).data;
    return "";   /* present-but-empty (see attribute_value_at) */
}

LEPTRIS_API int leptris_element_has_attribute_ns(LeptrisElement elem,
                                                 const char* uri,
                                                 const char* local) {
    if (!elem || !local) return 0;
    return find_attr_expanded(elem, uri, local) != NULL;
}

LEPTRIS_API int leptris_element_has_attribute(LeptrisElement elem, const char* name) {
    if (!elem || !name) return 0;
    /* Same expanded-name semantics as leptris_element_attribute
     * (#542) — delegated so both stay in lockstep. */
    return leptris_element_attribute(elem, name) != NULL;
}

/**
 * Get attribute value as integer (Public API)
 */
/* ---- typed-conversion cores (architecture review C) ----
 *
 * The four attribute numeric getters share one contract: isspace-
 * trimmed whole-token numbers, base 10, partial parses rejected,
 * NULL/empty falls through to the caller's default. The element_
 * text_* getters are deliberately separate: their contracts differ
 * (0x hex forms, no trailing-whitespace tolerance, empty-text bool
 * is false) and unifying them would change behavior. */
static int attr_num_long(const char* s, long* out) {
    if (!s || !*s) return 0;
    while (isspace((unsigned char)*s)) s++;
    char* end;
    long v = strtol(s, &end, 10);
    while (*end && isspace((unsigned char)*end)) end++;
    if (*end != '\0') return 0;
    *out = v;
    return 1;
}

static int attr_num_ulong(const char* s, unsigned long* out) {
    if (!s || !*s) return 0;
    while (isspace((unsigned char)*s)) s++;
    char* end;
    unsigned long v = strtoul(s, &end, 10);
    while (*end && isspace((unsigned char)*end)) end++;
    if (*end != '\0') return 0;
    *out = v;
    return 1;
}

static int attr_num_double(const char* s, double* out) {
    if (!s || !*s) return 0;
    while (isspace((unsigned char)*s)) s++;
    char* end;
    double v = strtod(s, &end);
    while (*end && isspace((unsigned char)*end)) end++;
    if (*end != '\0') return 0;
    *out = v;
    return 1;
}

static int attr_num_float(const char* s, float* out) {
    if (!s || !*s) return 0;
    while (isspace((unsigned char)*s)) s++;
    char* end;
    float v = strtof(s, &end);
    while (*end && isspace((unsigned char)*end)) end++;
    if (*end != '\0') return 0;
    *out = v;
    return 1;
}

/**
 * Get attribute value as integer (Public API)
 */
LEPTRIS_API int leptris_element_attribute_int(LeptrisElement elem, const char* name, int default_value) {
    long v;
    return attr_num_long(leptris_element_attribute(elem, name), &v) ? (int)v : default_value;
}

/**
 * Get attribute value as unsigned integer (Public API)
 */
LEPTRIS_API unsigned int leptris_element_attribute_uint(LeptrisElement elem, const char* name, unsigned int default_value) {
    unsigned long v;
    return attr_num_ulong(leptris_element_attribute(elem, name), &v) ? (unsigned int)v : default_value;
}

/**
 * Get attribute value as double (Public API)
 */
LEPTRIS_API double leptris_element_attribute_double(LeptrisElement elem, const char* name, double default_value) {
    double v;
    return attr_num_double(leptris_element_attribute(elem, name), &v) ? v : default_value;
}

/**
 * Get attribute value as float (Public API)
 */
LEPTRIS_API float leptris_element_attribute_float(LeptrisElement elem, const char* name, float default_value) {
    float v;
    return attr_num_float(leptris_element_attribute(elem, name), &v) ? v : default_value;
}
/**
 * Get attribute value as boolean (Public API)
 */
LEPTRIS_API int leptris_element_attribute_bool(LeptrisElement elem, const char* name, int default_value) {
    const char* value = leptris_element_attribute(elem, name);
    if (!value || !*value) return default_value;

    /* Case-insensitive comparison for true/false/yes/no/on/off/1/0 */
    if (strcasecmp(value, "true") == 0 ||
        strcasecmp(value, "yes") == 0 ||
        strcasecmp(value, "on") == 0 ||
        strcmp(value, "1") == 0) {
        return 1;
    }
    if (strcasecmp(value, "false") == 0 ||
        strcasecmp(value, "no") == 0 ||
        strcasecmp(value, "off") == 0 ||
        strcmp(value, "0") == 0) {
        return 0;
    }

    return default_value;
}

/**
 * Get attribute value as string with default (Public API)
 */
LEPTRIS_API const char* leptris_element_attribute_string(LeptrisElement elem, const char* name, const char* default_value) {
    const char* value = leptris_element_attribute(elem, name);
    return (value != NULL) ? value : default_value;
}

/* ============================================================================
 * Attribute Setter Functions (Type-safe wrappers)
 * ============================================================================ */

/**
 * Set attribute value as double (Public API)
 */
LEPTRIS_API int leptris_element_set_attribute_double(LeptrisElement elem, const char* name, double value) {
    if (!elem || !name) return LEPTRIS_ERROR_NULL_ARG;

    /* Convert double to string with sufficient precision (17 significant digits for IEEE 754) */
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.17g", value);
    return leptris_element_set_attribute(elem, name, buffer);
}

/**
 * Set attribute value as float (Public API)
 */
LEPTRIS_API int leptris_element_set_attribute_float(LeptrisElement elem, const char* name, float value) {
    if (!elem || !name) return LEPTRIS_ERROR_NULL_ARG;

    /* Convert float to string */
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%g", value);
    return leptris_element_set_attribute(elem, name, buffer);
}

/**
 * Set attribute value as boolean (Public API)
 */
LEPTRIS_API int leptris_element_set_attribute_bool(LeptrisElement elem, const char* name, int value) {
    if (!elem || !name) return LEPTRIS_ERROR_NULL_ARG;

    /* Convert boolean to string */
    const char* bool_str = value ? "true" : "false";
    return leptris_element_set_attribute(elem, name, bool_str);
}

/**
 * Set attribute value as integer (Public API)
 */
LEPTRIS_API int leptris_element_set_attribute_int(LeptrisElement elem, const char* name, int value) {
    if (!elem || !name) return LEPTRIS_ERROR_NULL_ARG;

    /* Convert int to string */
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%d", value);
    return leptris_element_set_attribute(elem, name, buffer);
}

/**
 * Set attribute value as unsigned integer (Public API)
 */
LEPTRIS_API int leptris_element_set_attribute_uint(LeptrisElement elem, const char* name, unsigned int value) {
    if (!elem || !name) return LEPTRIS_ERROR_NULL_ARG;

    /* Convert unsigned int to string */
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%u", value);
    return leptris_element_set_attribute(elem, name, buffer);
}

/**
 * Get child element by index (Public API)
 * Returns the index-th ELEMENT child (skips text, comments, etc.)
 *
 * Performance: O(index) — walks the linked list from first_child.
 * For sequential iteration prefer leptris_element_first_child_any +
 * leptris_element_next_sibling_any (O(1) per step).
 */
LEPTRIS_API LeptrisElement leptris_element_child(LeptrisElement elem, size_t index) {
    if (!elem) return NULL;
    if (index >= elem->child_count) return NULL;

    LeptrisElement child = leptris_element_get_first_child(elem);
    for (size_t i = 0; i < index && child != NULL; i++) {
        child = leptris_element_get_next_sibling(child);
    }
    return child;
}

/**
 * Get parent element (Public API)
 */
LEPTRIS_API LeptrisElement leptris_element_parent(LeptrisElement elem) {
    if (!elem) return NULL;

    /* Use compact accessor to get parent */
    return leptris_element_get_parent(elem);
}

/**
 * Get root element of document (Public API)
 * Walks up the parent chain to find the element with no parent
 */
LEPTRIS_API LeptrisElement leptris_element_root(LeptrisElement elem) {
    if (!elem) return NULL;

    /* Walk up the parent chain until we find an element with no parent */
    LeptrisElement current = elem;
    LeptrisElement parent = leptris_element_get_parent(current);
    while (parent) {
        current = parent;
        parent = leptris_element_get_parent(current);
    }

    return current;
}

/**
 * Get first child element regardless of name (Public API)
 */
LEPTRIS_API LeptrisElement leptris_element_first_child_any(LeptrisElement elem) {
    if (!elem) return NULL;

    /* Iterate through children until we find an element */
    LeptrisNode* child = (LeptrisNode*)leptris_element_get_first_child(elem);
    while (child) {
        if (child->type == LEPTRIS_NODE_TYPE_ELEMENT) {
            return (LeptrisElement)child;
        }
        child = leptris_node_get_next_sibling(child);
    }

    return NULL;
}

/**
 * Get last child element regardless of name (Public API)
 */
LEPTRIS_API LeptrisElement leptris_element_last_child_any(LeptrisElement elem) {
    if (!elem) return NULL;

    /* Iterate through children to find the last element */
    LeptrisElement last_elem = NULL;

    LeptrisNode* child = (LeptrisNode*)leptris_element_get_first_child(elem);
    while (child) {
        if (child->type == LEPTRIS_NODE_TYPE_ELEMENT) {
            last_elem = (LeptrisElement)child;
        }
        child = leptris_node_get_next_sibling(child);
    }

    return last_elem;
}

/**
 * Get next sibling element regardless of name (Public API)
 */
LEPTRIS_API LeptrisElement leptris_element_next_sibling_any(LeptrisElement elem) {
    if (!elem) return NULL;

    /* Use compact accessor to get next sibling */
    LeptrisNode* sibling = (LeptrisNode*)leptris_element_get_next_sibling(elem);

    /* Keep traversing until we find an element (skip text/comment nodes) */
    while (sibling) {
        if (sibling->type == LEPTRIS_NODE_TYPE_ELEMENT) {
            return (LeptrisElement)sibling;
        }
        sibling = leptris_node_get_next_sibling(sibling);
    }

    return NULL;
}

/**
 * Get child elements in bulk (Public API)
 *
 * Fills out_children with up to max_count element children in
 * document order, skipping interleaved text/comment/CDATA nodes —
 * the same chain first_child_any/next_sibling_any walk, in one call.
 * Size the array from leptris_element_child_count (which counts
 * element children only).
 */
LEPTRIS_API size_t leptris_element_children(
    LeptrisElement elem, LeptrisElement* out_children, size_t max_count) {
    if (!elem || !out_children || max_count == 0) return 0;

    size_t written = 0;
    LeptrisNode* child = (LeptrisNode*)leptris_element_get_first_child(elem);
    while (child && written < max_count) {
        if (child->type == LEPTRIS_NODE_TYPE_ELEMENT) {
            out_children[written++] = (LeptrisElement)child;
        }
        child = leptris_node_get_next_sibling(child);
    }
    return written;
}

/* Issue #535 (3): node-level batch children — every child kind
 * (elements, text, comments, CDATA, PIs) in one call, matching
 * leptris_element_children's shape. Returns the number copied;
 * size with leptris_node_child_count. */
LEPTRIS_API size_t leptris_node_children(LeptrisNodeRef parent,
                                         LeptrisNodeRef* out_nodes,
                                         size_t max_count) {
    if (!parent) return 0;

    /* Count-query mode (out == NULL): the TOTAL across every child
     * kind — leptris_node_child_count counts elements only, so it
     * cannot size a mixed array. */
    size_t written = 0;
    LeptrisNode* child = leptris_node_first_child_internal((LeptrisNode*)parent);
    while (child) {
        if (out_nodes && written < max_count) {
            out_nodes[written] = (LeptrisNodeRef)child;
        }
        written++;
        child = leptris_node_get_next_sibling(child);
    }
    return out_nodes ? ((written < max_count) ? written : max_count) : written;
}

LEPTRIS_API size_t leptris_node_children_ex(LeptrisNodeRef parent,
                                            LeptrisNodeRef* out_nodes,
                                            LeptrisNodeKind* out_kinds,
                                            size_t max_count) {
    if (!parent) return 0;

    size_t written = 0;
    LeptrisNode* child = leptris_node_first_child_internal((LeptrisNode*)parent);
    while (child) {
        if (out_nodes && written < max_count) {
            out_nodes[written] = (LeptrisNodeRef)child;
            if (out_kinds)
                out_kinds[written] = (LeptrisNodeKind)child->type;
        }
        written++;
        child = leptris_node_get_next_sibling(child);
    }
    return out_nodes ? ((written < max_count) ? written : max_count) : written;
}

/**
 * Get previous sibling element regardless of name (Public API)
 * NOTE: Compact mode doesn't have prev_sibling pointer, so we search from parent
 */
LEPTRIS_API LeptrisElement leptris_element_previous_sibling_any(LeptrisElement elem) {
    if (!elem) return NULL;

    /* Get parent */
    LeptrisElement parent = leptris_element_get_parent(elem);
    if (!parent) return NULL;

    /* Find previous sibling by walking from parent's first child */
    LeptrisNode* child = (LeptrisNode*)leptris_element_get_first_child(parent);
    LeptrisElement prev = NULL;

    while (child && child != (LeptrisNode*)elem) {
        /* Only consider element nodes as previous siblings */
        if (child->type == LEPTRIS_NODE_TYPE_ELEMENT) {
            prev = (LeptrisElement)child;
        }
        child = leptris_node_get_next_sibling(child);
    }

    return prev;
}

/**
 * Get first child element with specific name (Public API)
 * If name is NULL, returns first child regardless of name
 */
LEPTRIS_API LeptrisElement leptris_element_first_child(LeptrisElement elem, const char* name) {
    if (!elem) return NULL;

    /* Get first child and check name if specified */
    LeptrisElement child = leptris_element_get_first_child(elem);
    if (!name) return child;

    /* Find first child with matching name via hash pre-filter (TODO 159) */
    uint16_t target_hash = leptris_name_hash_compute(name);
    while (child) {
        if (leptris_elem_name_is(child, name, target_hash)) {
            return child;
        }
        child = leptris_element_get_next_sibling(child);
    }

    return NULL;
}

/**
 * Get last child element with specific name (Public API)
 * If name is NULL, returns last child regardless of name
 */
LEPTRIS_API LeptrisElement leptris_element_last_child(LeptrisElement elem, const char* name) {
    if (!elem) return NULL;

    /* Get last child */
    LeptrisElement last = leptris_element_get_last_child(elem);
    if (!name || !last) return last;

    /* Walk backwards from last child to find one with matching name */
    /* Since we don't have prev_sibling, we need to walk from first child */
    LeptrisElement found = NULL;
    LeptrisElement child = leptris_element_get_first_child(elem);
    while (child) {
        const char* child_name = leptris_element_name(child);
        if (child_name && strcmp(child_name, name) == 0) {
            found = child;  /* Keep updating to get the last one */
        }
        child = leptris_element_get_next_sibling(child);
    }

    return found;
}

/**
 * Get next sibling element with specific name (Public API)
 * If name is NULL, returns next sibling regardless of name
 */
LEPTRIS_API LeptrisElement leptris_element_next_sibling(LeptrisElement elem, const char* name) {
    if (!elem) return NULL;

    /* Get next sibling */
    LeptrisElement sibling = leptris_element_get_next_sibling(elem);
    if (!name) return sibling;

    /* Find next sibling with matching name */
    while (sibling) {
        const char* sibling_name = leptris_element_name(sibling);
        if (sibling_name && strcmp(sibling_name, name) == 0) {
            return sibling;
        }
        sibling = leptris_element_get_next_sibling(sibling);
    }

    return NULL;
}

/**
 * Get previous sibling element with specific name (Public API)
 * If name is NULL, returns previous sibling regardless of name
 */
LEPTRIS_API LeptrisElement leptris_element_previous_sibling(LeptrisElement elem, const char* name) {
    if (!elem) return NULL;

    /* Get parent */
    LeptrisElement parent = leptris_element_get_parent(elem);
    if (!parent) return NULL;

    /* Find previous sibling by walking from parent's first child */
    LeptrisElement child = leptris_element_get_first_child(parent);
    LeptrisElement prev = NULL;
    LeptrisElement found = NULL;

    while (child && child != elem) {
        if (!name) {
            prev = child;
        } else {
            const char* child_name = leptris_element_name(child);
            if (child_name && strcmp(child_name, name) == 0) {
                found = child;  /* Keep updating to get the most recent matching one */
            }
        }
        child = leptris_element_get_next_sibling(child);
    }

    return name ? found : prev;
}

/**
 * Find child element by name (Public API)
 * Searches all children for one with matching name
 */
LEPTRIS_API LeptrisElement leptris_element_find_child(LeptrisElement elem, const char* name) {
    if (!elem || !name) return NULL;

    /* Walk all children to find one with matching name */
    LeptrisElement child = leptris_element_get_first_child(elem);
    while (child) {
        const char* child_name = leptris_element_name(child);
        if (child_name && strcmp(child_name, name) == 0) {
            return child;
        }
        child = leptris_element_get_next_sibling(child);
    }

    return NULL;
}

/**
 * Find child element by name and attribute value (Public API)
 * Searches all children for one with matching name and attribute value
 */
LEPTRIS_API LeptrisElement leptris_element_find_child_by_attr(LeptrisElement elem,
                                                           const char* child_name,
                                                           const char* attr_name,
                                                           const char* attr_value) {
    if (!elem || !attr_name || !attr_value) return NULL;

    /* Walk all children to find one with matching name and attribute */
    LeptrisElement child = leptris_element_get_first_child(elem);
    while (child) {
        /* If child_name is NULL, match any child. Otherwise check name matches. */
        int name_matches = (child_name == NULL);
        if (!name_matches) {
            const char* child_elem_name = leptris_element_name(child);
            name_matches = (child_elem_name && strcmp(child_elem_name, child_name) == 0);
        }

        if (name_matches) {
            /* Child has matching name (or any name if child_name is NULL), check attribute */
            const char* attr_val = leptris_element_attribute(child, attr_name);
            if (attr_val && strcmp(attr_val, attr_value) == 0) {
                return child;
            }
        }
        child = leptris_element_get_next_sibling(child);
    }

    return NULL;
}

/* ============================================================================
 * Text Content Conversion Functions (Public API)
 * ============================================================================ */

/**
 * Get element text content as integer (Public API)
 */
LEPTRIS_API int leptris_element_text_int(LeptrisElement elem, int default_value) {
    const char* text = leptris_element_text(elem);
    if (!text || !text[0]) return default_value;

    /* Skip leading whitespace */
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') {
        text++;
    }

    /* Check for hex prefix (0x or 0X) */
    int is_hex = 0;
    int is_negative = 0;

    if (*text == '-') {
        is_negative = 1;
        text++;
        /* Check for whitespace immediately after minus sign (invalid) */
        if (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') {
            return default_value;
        }
    }

    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        is_hex = 1;
        text += 2;
    }

    /* Parse the number */
    char* endptr;
    long value;
    if (is_hex) {
        value = strtol(text, &endptr, 16);
    } else {
        /* Use base 10 for decimal (no octal support) */
        value = strtol(text, &endptr, 10);
    }

    /* Check if entire string was consumed */
    if (endptr == text || *endptr != '\0') {
        return default_value;
    }

    return is_negative ? -(int)value : (int)value;
}

/**
 * Get element text content as unsigned integer (Public API)
 */
LEPTRIS_API unsigned int leptris_element_text_uint(LeptrisElement elem, unsigned int default_value) {
    const char* text = leptris_element_text(elem);
    if (!text || !text[0]) return default_value;

    /* Skip leading whitespace */
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') {
        text++;
    }

    /* Check for hex prefix (0x or 0X) */
    int is_hex = 0;

    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        is_hex = 1;
        text += 2;
    }

    /* Parse the number */
    char* endptr;
    unsigned long value;
    if (is_hex) {
        value = strtoul(text, &endptr, 16);
    } else {
        /* Use base 10 for decimal (no octal support) */
        value = strtoul(text, &endptr, 10);
    }

    /* Check if entire string was consumed */
    if (endptr == text || *endptr != '\0') {
        return default_value;
    }

    return (unsigned int)value;
}

/**
 * Get element text content as double (Public API)
 */
LEPTRIS_API double leptris_element_text_double(LeptrisElement elem, double default_value) {
    const char* text = leptris_element_text(elem);
    if (!text || !text[0]) return default_value;

    /* Try to parse as double */
    char* endptr;
    double value = strtod(text, &endptr);

    /* Check if entire string was consumed */
    if (endptr == text || *endptr != '\0') {
        return default_value;
    }

    return value;
}

/**
 * Get element text content as float (Public API)
 */
LEPTRIS_API float leptris_element_text_float(LeptrisElement elem, float default_value) {
    const char* text = leptris_element_text(elem);
    if (!text || !text[0]) return default_value;

    /* Try to parse as float */
    char* endptr;
    float value = strtof(text, &endptr);

    /* Check if entire string was consumed */
    if (endptr == text || *endptr != '\0') {
        return default_value;
    }

    return value;
}

/**
 * Get element text content as boolean (Public API)
 */
LEPTRIS_API int leptris_element_text_bool(LeptrisElement elem, int default_value) {
    const char* text = leptris_element_text(elem);
    if (!text || !text[0]) return 0;  /* Empty string is false */

    /* Skip leading whitespace */
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') {
        text++;
    }

    /* Skip trailing whitespace */
    size_t len = strlen(text);
    while (len > 0 && (text[len-1] == ' ' || text[len-1] == '\t' || text[len-1] == '\n' || text[len-1] == '\r')) {
        len--;
    }

    /* Case-insensitive comparison for various false values */
    if ((len == 5 && strncasecmp(text, "false", 5) == 0) ||
        (len == 2 && strncasecmp(text, "no", 2) == 0) ||
        (len == 3 && strncasecmp(text, "off", 3) == 0) ||
        (len == 1 && text[0] == '0')) {
        return 0;
    }

    /* Case-insensitive comparison for various true values */
    if ((len == 4 && strncasecmp(text, "true", 4) == 0) ||
        (len == 3 && strncasecmp(text, "yes", 3) == 0) ||
        (len == 2 && strncasecmp(text, "on", 2) == 0) ||
        (len == 1 && text[0] == '1')) {
        return 1;
    }

    /* Any non-empty text that's not explicitly false is true */
    return len > 0 ? 1 : 0;
}

/* ============================================================================
 * Namespace Operations (Public API)
 * ============================================================================ */

/**
 * Get element's active namespace URI
 * In compact mode, namespace is stored inline in the element.
 */
LEPTRIS_API LeptrisNamespace leptris_element_namespace(LeptrisElement elem) {
    if (!elem) return NULL;

    /* In compact mode, namespace_uri is cached from namespace_uri_view */
    /* Trigger lazy conversion if needed */
    return leptris_element_get_namespace_uri(elem);
}

LEPTRIS_API void leptris_element_expanded_name(LeptrisElement elem,
                                               const char** local_name,
                                               const char** prefix,
                                               const char** namespace_uri) {
    if (local_name) *local_name = elem ? leptris_element_get_name(elem) : NULL;
    if (prefix) *prefix = elem ? leptris_element_get_prefix(elem) : NULL;
    if (namespace_uri)
        *namespace_uri = elem ? leptris_element_get_namespace_uri(elem) : NULL;
}

LEPTRIS_API const char* leptris_element_prefix(LeptrisElement elem) {
    /* leptris_namespace_prefix(LeptrisNamespace) cannot answer this:
     * in the compact architecture the prefix lives on the ELEMENT,
     * not on the URI-string namespace handle. */
    return leptris_element_get_prefix(elem);
}

/**
 * Get namespace URI from namespace handle
 * In compact mode, LeptrisNamespace is the URI string itself.
 */
LEPTRIS_API const char* leptris_namespace_uri(LeptrisNamespace ns) {
    /* In compact mode, LeptrisNamespace IS the URI string */
    return ns;
}

/**
 * Get namespace prefix from namespace handle
 * In compact mode, namespaces are inline, so this returns NULL.
 * Use leptris_element_get_prefix() to get an element's prefix.
 */
LEPTRIS_API const char* leptris_namespace_prefix(LeptrisNamespace ns) {
    /* In compact mode, prefix information is stored per-element */
    /* Use leptris_element_get_prefix() instead */
    (void)ns; /* Unused */
    return NULL;
}

/**
 * Resolve namespace prefix with inheritance.
 * Issue #222: previously this only checked the element's OWN prefix
 * field (e.g. "foo" in <foo:child>) and returned the element's
 * namespace_uri — which was rarely what callers wanted and ignored
 * the xmlns:prefix declarations entirely. Now delegates to
 * leptris_element_lookup_namespace which walks the element's
 * namespace-declaration list and recurses up the tree. */
LEPTRIS_API const char* leptris_element_namespace_for_prefix(LeptrisElement elem, const char* prefix) {
    if (!elem) return NULL;
    return leptris_element_lookup_namespace(elem, prefix);
}



/**
 * Get hash value of element (Public API)
 * Returns a hash value based on the element's memory address
 */
LEPTRIS_API size_t leptris_element_hash_value(LeptrisElement elem) {
    if (!elem) return 0;

    /* Simple hash based on pointer address */
    /* This works because elements are pool-allocated and stable */
    return (size_t)elem;
}

/* ============================================================================
 * FFI Helpers: indexed attribute and namespace access (TODO 138)
 *
 * O(n) walk over the attribute linked list. Nokogiri and libxml2 do
 * the same. The Ruby binding caches results.
 * ============================================================================ */

LEPTRIS_API const char* leptris_element_attribute_name_at(LeptrisElement elem, size_t index) {
    struct leptris_attribute* attr = leptris_element_get_first_attribute(elem);
    size_t i = 0;
    while (attr) {
        if (i == index) {
            if (attr->name_view.data && attr->name_view.length > 0) {
                return attr->name_view.data;
            }
            return NULL;
        }
        i++;
        attr = leptris_attr_next(attr);
    }
    return NULL;
}

LEPTRIS_API const char* leptris_element_attribute_value_at(LeptrisElement elem, size_t index) {
    if (!elem) return NULL;
    struct leptris_attribute* attr = leptris_element_get_first_attribute(elem);
    size_t i = 0;
    while (attr) {
        if (i == index) {
            if (attr_has_entities(attr)) {
                LeptrisMemoryPool* pool = leptris_element_get_pool(elem);
                if (pool) {
                    LeptrisStringView rv_ = leptris_attr_value_sv(attr);
                    char* decoded = leptris_decode_entities_view(&rv_, pool);
                    if (decoded) {
                        leptris_attr_value_set_heap(attr, leptris_sv_from_cstr(decoded));
                        attr_set_entities(attr, 0);
                    }
                }
            }
            if (leptris_attr_value_sv(attr).data &&
                leptris_attr_value_sv(attr).length > 0) {
                return leptris_attr_value_sv(attr).data;
            }
            /* Present-but-empty value: a real empty string, not
             * "no attribute" (issue: eg:bar="" vanished in XSLT
             * attribute copies). */
            return "";
        }
        i++;
        attr = leptris_attr_next(attr);
    }
    return NULL;
}

LEPTRIS_API size_t leptris_element_attribute_count(LeptrisElement elem) {
    if (!elem) return 0;
    return elem->attr_count;
}

/* Handle-based attribute iteration (TODO.remaining/06). The _at
 * accessors above re-walk the list from the head per call — index
 * iteration is O(n^2); the handle pair is O(n) total and gives
 * bindings a stable object to carry between calls. */
LEPTRIS_API LeptrisAttribute leptris_element_first_attribute(LeptrisElement elem) {
    return leptris_element_get_first_attribute(elem);
}

LEPTRIS_API LeptrisAttribute leptris_attribute_next(LeptrisAttribute attr) {
    return attr ? leptris_attr_next(attr) : NULL;
}

LEPTRIS_API const char* leptris_attribute_get_name(LeptrisAttribute attr) {
    return attr ? attr_cname(attr) : "";
}

LEPTRIS_API const char* leptris_attribute_get_value(LeptrisElement elem, LeptrisAttribute attr) {
    if (!attr) return "";
    if (attr_has_entities(attr)) {
        LeptrisMemoryPool* pool = leptris_element_get_pool(elem);
        if (pool) {
            LeptrisStringView rv_ = leptris_attr_value_sv(attr);
            char* decoded = leptris_decode_entities_view(&rv_, pool);
            if (decoded) {
                leptris_attr_value_set_heap(attr, leptris_sv_from_cstr(decoded));
                attr_set_entities(attr, 0);
            }
        }
    }
    return attr_cvalue(attr);
}

LEPTRIS_API size_t leptris_element_namespace_count(LeptrisElement elem) {
    if (!elem) return 0;
    size_t count = 0;
    /* The parser strips xmlns declarations from the regular attribute
     * list and moves them to elem->namespaces. Walk THAT list. */
    for (struct leptris_namespace* ns = leptris_elem_namespaces(elem); ns; ns = ns->next) {
        count++;
    }
    /* Backward compat: also count xmlns attributes in case any caller
     * added them via leptris_element_add_attribute instead of the
     * namespace API. This is the original implementation. */
    struct leptris_attribute* attr = leptris_element_get_first_attribute(elem);
    while (attr) {
        LeptrisStringView nv = attr->name_view;
        if (nv.length >= 5 && nv.data &&
            nv.data[0] == 'x' && nv.data[1] == 'm' &&
            nv.data[2] == 'l' && nv.data[3] == 'n' &&
            nv.data[4] == 's') {
            count++;
        }
        attr = leptris_attr_next(attr);
    }
    return count;
}

/* Walk the union of elem->namespaces and any xmlns:* attributes to
 * the N-th declaration. The parser uses elem->namespaces; legacy
 * callers may have used the attribute list. Both are walked in order. */
static int element_namespace_decl_at(LeptrisElement elem, size_t index,
                                      const char** out_prefix,
                                      const char** out_uri) {
    if (!elem) return 0;
    size_t i = 0;
    for (struct leptris_namespace* ns = leptris_elem_namespaces(elem); ns; ns = ns->next) {
        if (i == index) {
            *out_prefix = ns->prefix;
            *out_uri = ns->uri;
            return 1;
        }
        i++;
    }
    struct leptris_attribute* attr = leptris_element_get_first_attribute(elem);
    while (attr) {
        LeptrisStringView nv = attr->name_view;
        if (nv.length >= 5 && nv.data &&
            nv.data[0] == 'x' && nv.data[1] == 'm' &&
            nv.data[2] == 'l' && nv.data[3] == 'n' &&
            nv.data[4] == 's') {
            if (i == index) {
                /* View data is NUL-terminated and document-lifetime —
                 * no materialization (TODO 184 round 3). */
                const char* full = (const char*)nv.data;
                if (full && nv.length > 5) {
                    *out_prefix = full + 6;
                } else {
                    *out_prefix = NULL;
                }
                *out_uri = attr_cvalue(attr);
                return 1;
            }
            i++;
        }
        attr = leptris_attr_next(attr);
    }
    return 0;
}

LEPTRIS_API const char* leptris_element_namespace_decl_prefix(
    LeptrisElement elem, size_t index) {
    const char* prefix = NULL;
    const char* uri = NULL;
    if (!element_namespace_decl_at(elem, index, &prefix, &uri)) return NULL;
    return prefix;
}

LEPTRIS_API const char* leptris_element_namespace_decl_uri(
    LeptrisElement elem, size_t index) {
    const char* prefix = NULL;
    const char* uri = NULL;
    if (!element_namespace_decl_at(elem, index, &prefix, &uri)) return NULL;
    return uri;
}

/* ============================================================================
 * Namespace mutation (issue #186).
 *
 * Three public entry points wrap the existing internal namespace
 * list. The internal list lives on elem->namespaces; the parser
 * populates it during parse. These mutators let callers add/remove
 * declarations programmatically.
 * ============================================================================ */

LEPTRIS_API LeptrisStatus leptris_element_add_namespace_definition(
    LeptrisElement elem, const char* prefix, const char* href) {
    if (!elem || !href) return LEPTRIS_ERROR_NULL_ARG;
    leptris_document_ensure_promoted(leptris_element_get_document(elem));

    /* Normalize empty-string prefix to NULL (default ns). */
    if (prefix && !*prefix) prefix = NULL;

    LeptrisMemoryPool* pool = leptris_element_get_pool(elem);
    struct leptris_namespace* ns;
    if (pool) {
        ns = leptris_namespace_new_pooled(prefix, href, pool);
    } else {
        /* No pool — fall back to heap-allocating the ns struct + copies. */
        ns = (struct leptris_namespace*)malloc(sizeof(*ns));
        if (!ns) return LEPTRIS_ERROR_MEMORY;
        ns->prefix = prefix ? strdup(prefix) : NULL;
        ns->uri = strdup(href);
        ns->next = NULL;
        if (!ns->uri || (prefix && !ns->prefix)) {
            free(ns->prefix);
            free(ns->uri);
            free(ns);
            return LEPTRIS_ERROR_MEMORY;
        }
    }
    if (!ns) return LEPTRIS_ERROR_MEMORY;

    /* #525: a declaration exists — unprefixed XPath name tests
     * must consult the resolver from here on. */
    {
        struct leptris_document* d = leptris_element_get_document(elem);
        if (d) d->has_namespaces = 1;
    }
    leptris_element_add_namespace(elem, ns);
    return LEPTRIS_OK;
}

LEPTRIS_API LeptrisStatus leptris_element_set_default_namespace(
    LeptrisElement elem, const char* href) {
    return leptris_element_add_namespace_definition(elem, NULL, href);
}

/* #817: the namespace-setter counterpart of
 * leptris_element_namespace (Nokogiri node.namespace= parity).
 *
 * NULL/"" detaches: the resolved link and the name's prefix slot
 * clear, and an xmlns="" UNDECLARATION is added so an in-scope
 * default namespace cannot capture the now-unqualified name. A
 * non-empty URI rebinds to an IN-SCOPE declaration carrying that
 * URI (the prefix is adopted); a URI with no in-scope declaration
 * is rejected — this API does not invent declarations (call
 * add_namespace_definition first).
 */
LEPTRIS_API LeptrisStatus leptris_element_set_namespace(
    LeptrisElement elem, const char* uri) {
    if (!elem) return LEPTRIS_ERROR_NULL_ARG;
    LeptrisMemoryPool* pool = leptris_element_get_pool(elem);
    if (!pool) return LEPTRIS_ERROR_INVALID_ARG;

    if (!uri || !*uri) {
        leptris_elem_set_prefix(elem, NULL, pool);
        leptris_elem_set_ns_uri(elem, NULL, pool);
        LeptrisStatus rc =
            leptris_element_add_namespace_definition(elem, NULL, "");
        if (rc == LEPTRIS_OK)
            leptris_node_increment_version(LEPTRIS_ELEMENT_AS_NODE(elem));
        return rc;
    }

    for (LeptrisElement a = elem; a;
         a = (LeptrisElement)leptris_node_parent((LeptrisNodeRef)a)) {
        for (int i = 0;; i++) {
            const char* p = leptris_element_namespace_decl_prefix(a, i);
            const char* u = leptris_element_namespace_decl_uri(a, i);
            if (!u) break;
            if (p && *p && strcmp(u, uri) == 0) {
                char* pc = leptris_pool_strdup(pool, p);
                char* uc = leptris_pool_strdup(pool, uri);
                if (!pc || !uc) return LEPTRIS_ERROR_MEMORY;
                leptris_elem_set_prefix(elem, pc, pool);
                leptris_elem_set_ns_uri(elem, uc, pool);
                leptris_node_increment_version(LEPTRIS_ELEMENT_AS_NODE(elem));
                return LEPTRIS_OK;
            }
        }
    }
    return LEPTRIS_ERROR_NOT_FOUND;
}

LEPTRIS_API LeptrisStatus leptris_element_remove_namespace_definition(
    LeptrisElement elem, const char* prefix) {
    if (!elem) return LEPTRIS_ERROR_NULL_ARG;
    /* Normalize empty prefix to NULL. */
    if (prefix && !*prefix) prefix = NULL;

    /* No ns_cache = no namespace declarations on this element. */
    if (!elem->ns_cache) return LEPTRIS_ERROR_NOT_FOUND;

    struct leptris_namespace** link = &elem->ns_cache->declarations;
    while (*link) {
        struct leptris_namespace* cur = *link;
        int match = (prefix == NULL) ? (cur->prefix == NULL)
            : (cur->prefix && strcmp(cur->prefix, prefix) == 0);
        if (match) {
            *link = cur->next;
            /* Don't free cur->prefix / cur->uri — they're pool-owned.
             * The struct itself may be pool-allocated too; skip free. */
            return LEPTRIS_OK;
        }
        link = &cur->next;
    }
    return LEPTRIS_ERROR_NOT_FOUND;
}

LEPTRIS_API size_t leptris_element_attributes_raw(
    LeptrisElement elem, const char** out_qnames, const char** out_values,
    size_t max_count) {
    if (!elem) return 0;
    if (!elem->ns_cache || !elem->ns_cache->raw_attrs) return 0;

    /* Count-only query. */
    if (!out_qnames && !out_values) {
        size_t n = 0;
        for (struct leptris_raw_attr* r = elem->ns_cache->raw_attrs; r;
             r = r->next)
            n++;
        return n;
    }

    size_t written = 0;
    for (struct leptris_raw_attr* r = elem->ns_cache->raw_attrs; r;
         r = r->next) {
        if (written >= max_count) break;
        if (out_qnames) out_qnames[written] = r->qname;
        if (out_values) out_values[written] = r->value;
        written++;
    }
    return written;
}
