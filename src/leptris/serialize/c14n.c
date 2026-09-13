/* serialize/c14n.c — Canonical XML (C14N 1.0) implementation.
 *
 * Extracted from leptris.c (TODO 42 phase 4). Implements
 * leptris_c14n_canonicalize and its supporting helpers.
 */

#include "../include/leptris.h"
#include "../leptris_internal.h"
#include "../dom/element.h"
#include "../dom/node.h"
#include "../dom/text.h"
#include "../dom/comment.h"
#include "../dom/cdata.h"
#include "../dom/pi.h"
#include "../common/string_view.h"
#include "../common/port.h"
#include "../common/entities.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Macro for appending strings to a dynamic buffer.
 * Mirrors the definition in leptris.c — these helpers are local to
 * the document-lifecycle / C14N translation units. */
#define APPEND_STRING(str, len) do { \
    if (!buffer) goto cleanup; \
    while (*size + (len) + 1 > *capacity) { \
        size_t new_cap = *capacity * 2; \
        char* new_buf = (char*)realloc(*buffer, new_cap); \
        if (!new_buf) { \
            free(*buffer); \
            *buffer = NULL; \
            goto cleanup; \
        } \
        *buffer = new_buf; \
        *capacity = new_cap; \
    } \
    memcpy(*buffer + *size, str, len); \
    *size += len; \
    (*buffer)[*size] = '\0'; \
} while(0)

/* ============================================================================
 * Canonical XML (C14N) Operations
 * ============================================================================ */

/**
 * Helper function to escape text for C14N output
 *
 * C14N 1.0 escaping rules:
 * - In text content: < and & must be escaped
 * - In attribute values: <, &, and " must be escaped
 *
 * Returns a newly allocated string that must be freed
 */
static char* c14n_escape_text(const char* text, int is_attribute_value) {
    if (!text) return NULL;

    /* Count how much space we need
     * C14N spec requires: <, &, \r always escaped; " escaped in attributes */
    size_t len = strlen(text);
    size_t escape_count = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '<') {
            escape_count += 4;  /* &lt; */
        } else if (text[i] == '&') {
            escape_count += 5;  /* &amp; */
        } else if (text[i] == '\r') {
            escape_count += 5;  /* &#xD; (C14N requires \r to be escaped) */
        } else if (is_attribute_value && text[i] == '"') {
            escape_count += 6;  /* &quot; */
        }
    }

    /* If no escaping needed, return a copy */
    if (escape_count == 0) {
        return leptris_strdup(text);
    }

    /* Allocate buffer */
    size_t new_len = len + escape_count;
    char* escaped = (char*)malloc(new_len + 1);
    if (!escaped) return NULL;

    /* Escape characters (C14N spec: <, &, \r always escaped; " escaped in attributes) */
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '<') {
            memcpy(&escaped[j], "&lt;", 4);
            j += 4;
        } else if (text[i] == '&') {
            memcpy(&escaped[j], "&amp;", 5);
            j += 5;
        } else if (text[i] == '\r') {
            memcpy(&escaped[j], "&#xD;", 5);
            j += 5;
        } else if (is_attribute_value && text[i] == '"') {
            memcpy(&escaped[j], "&quot;", 6);
            j += 6;
        } else {
            escaped[j++] = text[i];
        }
    }
    escaped[j] = '\0';

    return escaped;
}

/**
 * Compare function for sorting attributes lexicographically (for qsort)
 */
static int compare_attributes(const void* a, const void* b) {
    const struct leptris_attribute* attr_a = *(const struct leptris_attribute**)a;
    const struct leptris_attribute* attr_b = *(const struct leptris_attribute**)b;

    /* #919: REC-xml-c14n section 2.3 — attribute nodes sort by
     * namespace URI (no-namespace = empty URI, sorts FIRST), then
     * by local name. The URI resolves through the OWNER element's
     * in-scope declarations (#542 semantics). */
    const char* uri_a = leptris_attribute_namespace_uri(
        (LeptrisAttribute)attr_a);
    const char* uri_b = leptris_attribute_namespace_uri(
        (LeptrisAttribute)attr_b);
    int cmp = strcmp(uri_a ? uri_a : "", uri_b ? uri_b : "");
    if (cmp != 0) return cmp;

    /* Same URI: local name (the part after any colon). */
    const char* name_a = attr_a->name_view.data;
    const char* name_b = attr_b->name_view.data;
    size_t len_a = attr_a->name_view.length;
    size_t len_b = attr_b->name_view.length;
    const char* colon_a = name_a ? memchr(name_a, ':', len_a) : NULL;
    const char* colon_b = name_b ? memchr(name_b, ':', len_b) : NULL;
    if (colon_a) { len_a -= (size_t)(colon_a - name_a) + 1; name_a = colon_a + 1; }
    if (colon_b) { len_b -= (size_t)(colon_b - name_b) + 1; name_b = colon_b + 1; }

    size_t min_len = len_a < len_b ? len_a : len_b;
    cmp = memcmp(name_a, name_b, min_len);
    if (cmp != 0) return cmp;
    if (len_a < len_b) return -1;
    if (len_a > len_b) return 1;
    return 0;
}

/**
 * Compare function for sorting namespace declarations (for qsort).
 * REC-xml-c14n section 2.2: namespace nodes sort lexicographically
 * by prefix; the default namespace (no prefix) sorts first.
 */
static int compare_namespaces(const void* a, const void* b) {
    const struct leptris_namespace* ns_a = *(const struct leptris_namespace**)a;
    const struct leptris_namespace* ns_b = *(const struct leptris_namespace**)b;

    if (!ns_a->prefix && !ns_b->prefix) return 0;
    if (!ns_a->prefix) return -1;
    if (!ns_b->prefix) return 1;
    return strcmp(ns_a->prefix, ns_b->prefix);
}

/* Issue #183: thread-local with_comments flag for the extended API.
 * Default 0 = strip comments (matches C14N 1.0 canonical spec). The
 * _ex variants flip this around their call to the walk. */
static LEPTRIS_THREAD_LOCAL int c14n_include_comments = 0;

/**
 * Recursive helper to serialize element in C14N format
 */
static void c14n_serialize_element(LeptrisElement elem, char** buffer, size_t* size, size_t* capacity) {
    if (!elem) return;

    /* Get element name */
    const char* name = leptris_element_get_name(elem);

    /* Get attribute count using accessor */
    uint8_t attr_count = leptris_element_attribute_count(elem);

    /* Allocate temporary array for sorting attributes */
    struct leptris_attribute** sorted_attrs = NULL;
    if (attr_count > 0) {
        sorted_attrs = (struct leptris_attribute**)malloc(attr_count * sizeof(struct leptris_attribute*));
        if (sorted_attrs) {
            /* Copy attributes — direct walk (TODO 185: indexed access
             * re-walks the list per index, O(K²)). */
            uint8_t si = 0;
            for (struct leptris_attribute* a = leptris_element_get_first_attribute(elem);
                 a && si < attr_count; a = leptris_attr_next(a)) {
                sorted_attrs[si++] = a;
            }
            /* Sort attributes lexicographically */
            qsort(sorted_attrs, si, sizeof(struct leptris_attribute*), compare_attributes);
        }
    }

    /* For namespace declarations - TODO: Implement namespace tracking in compact mode */
    /* Currently namespaces are stored in prefix/namespace_uri fields, not as a list */

    /* Append opening tag: <name */
    char temp[4096];
    int len;

    /* Check if element has content (including text nodes) */
    int has_content = 0;
    int has_children = 0;

    LeptrisNode* child = (LeptrisNode*)leptris_node_first_child_internal((LeptrisNode*)elem);
    while (child) {
        has_children = 1;
        /* Check if child is text or CDATA by checking node type */
        if (child->type == LEPTRIS_NODE_TYPE_TEXT) {
            const char* text = leptris_text_get_content((LeptrisTextNode*)child);
            if (text && *text) {
                has_content = 1;
                break;
            }
        }
        /* Get next sibling - the generic dispatch handles every node type */
        child = leptris_node_get_next_sibling(child);
    }

    /* Start opening tag */
    len = snprintf(temp, sizeof(temp), "<%s", name ? name : "element");
    APPEND_STRING(temp, len);

    /* Add namespace declarations, sorted by prefix with the default
     * namespace first (REC-xml-c14n section 2.2, issue #881). */
    size_t ns_count = 0;
    for (struct leptris_namespace* n = leptris_elem_namespaces(elem); n; n = n->next) {
        ns_count++;
    }
    if (ns_count > 0) {
        struct leptris_namespace** sorted_ns =
            (struct leptris_namespace**)malloc(ns_count * sizeof(struct leptris_namespace*));
        if (sorted_ns) {
            size_t ni = 0;
            for (struct leptris_namespace* n = leptris_elem_namespaces(elem); n && ni < ns_count; n = n->next) {
                sorted_ns[ni++] = n;
            }
            qsort(sorted_ns, ni, sizeof(struct leptris_namespace*), compare_namespaces);
            for (size_t i = 0; i < ni; i++) {
                struct leptris_namespace* ns = sorted_ns[i];
                /* Serialize namespace as xmlns:prefix="uri" or xmlns="uri" for default */
                if (ns->prefix) {
                    len = snprintf(temp, sizeof(temp), " xmlns:%s=\"%s\"", ns->prefix, ns->uri);
                } else {
                    len = snprintf(temp, sizeof(temp), " xmlns=\"%s\"", ns->uri);
                }
                APPEND_STRING(temp, len);
            }
            free(sorted_ns);
        }
    }

    /* Add sorted attributes */
    if (sorted_attrs) {
        for (size_t i = 0; i < attr_count; i++) {
            struct leptris_attribute* attr = sorted_attrs[i];
            const char* attr_name = attr->name_view.data;
            size_t attr_name_len = attr->name_view.length;

            /* Get attribute value. Single representation (round 4):
             * no-entity values read the view directly (NUL-terminated
             * in the doc buffer); entity values decode to a temporary
             * that this loop frees. */
            const char* attr_value;
            int attr_value_owned = 0;
            LeptrisStringView av = leptris_attr_value_sv(attr);
            if (attr_has_entities(attr) && !leptris_sv_is_empty(&av)) {
                /* Use lenient mode for C14N to handle edge cases like "&" in attributes */
                int old_strict = leptris_get_strict_mode();
                leptris_set_strict_mode(0);  /* Enable lenient mode */
                attr_value = leptris_decode_entities_view(&av, NULL);
                leptris_set_strict_mode(old_strict);  /* Restore strict mode */
                attr_value_owned = (attr_value != NULL);
            } else {
                attr_value = attr_cvalue(attr);
            }

            if (attr_name && attr_value) {
                /* Escape attribute value for C14N (<, &, and " need escaping) */
                char* escaped_value = c14n_escape_text(attr_value, 1);
                if (escaped_value) {
                    len = snprintf(temp, sizeof(temp), " %.*s=\"%s\"", (int)attr_name_len, attr_name, escaped_value);
                    APPEND_STRING(temp, len);
                    free(escaped_value);
                }

                /* Free temporary value only if we allocated it */
                if (attr_value_owned) {
                    free((void*)attr_value);
                }
            }
        }
    }

    /* Close opening tag */
    if (!has_children && !has_content) {
        /* Empty element: <tag></tag> (NOT <tag/>) */
        len = snprintf(temp, sizeof(temp), "></%s>", name ? name : "element");
        APPEND_STRING(temp, len);
    } else {
        len = snprintf(temp, sizeof(temp), ">");
        APPEND_STRING(temp, len);

        /* Add children - traverse ALL children including text nodes */
        LeptrisNode* sub_child = (LeptrisNode*)leptris_node_first_child_internal((LeptrisNode*)elem);
        while (sub_child) {
            if (sub_child->type == LEPTRIS_NODE_TYPE_ELEMENT) {
                c14n_serialize_element((LeptrisElement)sub_child, buffer, size, capacity);
                sub_child = leptris_node_get_next_sibling(sub_child);
            } else if (sub_child->type == LEPTRIS_NODE_TYPE_TEXT) {
                const char* text = leptris_text_get_content((LeptrisTextNode*)sub_child);
                if (text) {
                    /* Escape text content for C14N (only < and & need escaping) */
                    char* escaped_text = c14n_escape_text(text, 0);
                    if (escaped_text) {
                        APPEND_STRING(escaped_text, (int)strlen(escaped_text));
                        free(escaped_text);
                    }
                }
                sub_child = leptris_node_get_next_sibling(sub_child);
            } else if (sub_child->type == LEPTRIS_NODE_TYPE_CDATA) {
                /* CDATA content is treated as character data in C14N */
                const char* text = leptris_cdata_get_content((LeptrisCDATANode*)sub_child);
                if (text) {
                    /* Escape CDATA content for C14N (same rules as text content) */
                    char* escaped_text = c14n_escape_text(text, 0);
                    if (escaped_text) {
                        APPEND_STRING(escaped_text, (int)strlen(escaped_text));
                        free(escaped_text);
                    }
                }
                sub_child = leptris_node_get_next_sibling(sub_child);
            } else if (sub_child->type == LEPTRIS_NODE_TYPE_PI) {
                /* Processing Instruction: <?target data?> */
                LeptrisPINode* pi = (LeptrisPINode*)sub_child;
                const char* target = leptris_pi_get_target(pi);
                const char* data = leptris_pi_get_data(pi);
                if (target) {
                    len = snprintf(temp, sizeof(temp), "<?%s", target);
                    APPEND_STRING(temp, len);
                    if (data && *data) {
                        APPEND_STRING(" ", 1);
                        APPEND_STRING(data, (int)strlen(data));
                    }
                    APPEND_STRING("?>", 2);
                }
                sub_child = leptris_node_get_next_sibling(sub_child);
            } else if (sub_child->type == LEPTRIS_NODE_TYPE_COMMENT) {
                /* Issue #183: emit comments when with_comments=1. */
                if (c14n_include_comments) {
                    const char* text = leptris_comment_get_content(
                        (LeptrisCommentNode*)sub_child);
                    if (text) {
                        len = snprintf(temp, sizeof(temp),
                                       "<!--%s-->", text);
                        APPEND_STRING(temp, len);
                    }
                }
                sub_child = leptris_node_get_next_sibling(sub_child);
            } else {
                /* Unknown node type - stop */
                sub_child = NULL;
            }
        }

        /* Closing tag */
        len = snprintf(temp, sizeof(temp), "</%s>", name ? name : "element");
        APPEND_STRING(temp, len);
    }

    /* Cleanup */
    if (sorted_attrs) free(sorted_attrs);

cleanup:
    return;
}

/**
 * Canonicalize document to C14N format (Public API)
 */
LEPTRIS_API char* leptris_c14n_canonicalize(struct leptris_document* doc, int version, int flags) {
    (void)version;  /* Version reserved for future C14N 1.1 differences */
    (void)flags;    /* Flags reserved for future use */

    if (!doc) return NULL;

    /* Get root element */
    /* TODO 139 Phase D: trigger lazy promote if the doc was produced
     * by the flat-parse fast path. */
    leptris_document_ensure_promoted(doc);
    LeptrisElement root = (LeptrisElement)doc->new_dom_root;
    if (!root) return NULL;

    /* Allocate initial buffer */
    size_t capacity = 4096;
    size_t size = 0;
    char* buf = (char*)malloc(capacity);
    if (!buf) return NULL;
    buf[0] = '\0';

    /* Add document-level processing instructions before the root
     * element (issue #580: the document child chain, prolog nodes
     * only — C14N document order). */
    LeptrisNode* c14n_root_node = (LeptrisNode*)root;
    for (LeptrisNode* c = (LeptrisNode*)doc->doc_children_head; c;
         c = leptris_node_get_next_sibling(c)) {
        if (c == c14n_root_node) break;
        if (c->type != LEPTRIS_NODE_TYPE_PI) continue;
        LeptrisPINode* pi = (LeptrisPINode*)c;
        char temp[1024];
        int len;
        if (pi->target) {
            len = snprintf(temp, sizeof(temp), "<?%s", pi->target);
            while (size + len + 1 > capacity) {
                size_t new_cap = capacity * 2;
                char* new_buf = (char*)realloc(buf, new_cap);
                if (!new_buf) {
                    free(buf);
                    return NULL;
                }
                buf = new_buf;
                capacity = new_cap;
            }
            memcpy(buf + size, temp, len);
            size += len;
            buf[size] = '\0';

            if (pi->data && *pi->data) {
                len = 1;
                while (size + len + 1 > capacity) {
                    size_t new_cap = capacity * 2;
                    char* new_buf = (char*)realloc(buf, new_cap);
                    if (!new_buf) {
                        free(buf);
                        return NULL;
                    }
                    buf = new_buf;
                    capacity = new_cap;
                }
                memcpy(buf + size, " ", len);
                size += len;
                buf[size] = '\0';

                len = (int)strlen(pi->data);
                while (size + len + 1 > capacity) {
                    size_t new_cap = capacity * 2;
                    char* new_buf = (char*)realloc(buf, new_cap);
                    if (!new_buf) {
                        free(buf);
                        return NULL;
                    }
                    buf = new_buf;
                    capacity = new_cap;
                }
                memcpy(buf + size, pi->data, len);
                size += len;
                buf[size] = '\0';
            }
            len = 2;
            while (size + len + 1 > capacity) {
                size_t new_cap = capacity * 2;
                char* new_buf = (char*)realloc(buf, new_cap);
                if (!new_buf) {
                    free(buf);
                    return NULL;
                }
                buf = new_buf;
                capacity = new_cap;
            }
            memcpy(buf + size, "?>", len);
            size += len;
            buf[size] = '\0';
        }
    }

    /* Serialize document in C14N format */
    char* buffer = buf;
    c14n_serialize_element(root, &buffer, &size, &capacity);
    buf = buffer;

    /* Note: Line ending normalization is done during parsing per XML 1.0 spec.
     * Any remaining \r in the document should be escaped as &#xD; by the parser.
     * We don't do additional line ending transformation here. */
    buf[size] = '\0';

    return buf;
}

LEPTRIS_API char* leptris_c14n_canonicalize_subtree(LeptrisElement elem,
                                                    int version,
                                                    int flags) {
    (void)version;
    (void)flags;
    if (!elem) return NULL;

    /* Allocate initial buffer. */
    size_t capacity = 4096;
    size_t size = 0;
    char* buf = (char*)malloc(capacity);
    if (!buf) return NULL;
    buf[0] = '\0';

    /* Subtree C14N: serialize just this element + descendants. The
     * document-level PIs and XML declaration are NOT included
     * (subtree C14N is element-scoped, matching xmlsec and Nokogiri
     * semantics). */
    char* buffer = buf;
    c14n_serialize_element(elem, &buffer, &size, &capacity);
    buf = buffer;
    buf[size] = '\0';
    return buf;
}

/* ============================================================================
 * Extended C14N (issue #183)
 *
 * Adds:
 *   - Exclusive mode (drops unused namespace declarations)
 *   - Inclusive namespace prefixes (force-include list)
 *   - with_comments toggle (strip comments when 0)
 *
 * The implementation reuses c14n_serialize_element for the heavy
 * lifting. The new behavior is layered as post-processing:
 *   1. Canonicalize with the existing algorithm
 *   2. If with_comments == 0, strip <!-- ... --> from the output
 *   3. If mode == EXCLUSIVE, this implementation falls back to
 *      CANONICAL — full exclusive C14N requires tracking namespace
 *      use during the walk, which is a follow-up. The API surface
 *      is here so bindings can target it; exclusive semantics land
 *      in a future minor release.
 * ============================================================================ */

/* Strip <!-- ... --> from buf in-place. Comments cannot be nested
 * in XML so a single-pass scan is safe. */
static void c14n_strip_comments(char* buf, size_t* size) {
    if (!buf || !size) return;
    char* read = buf;
    char* write = buf;
    const char* end = buf + *size;
    while (read < end) {
        if (read + 3 < end && read[0] == '<' && read[1] == '!' &&
            read[2] == '-' && read[3] == '-') {
            /* Find the closing -->. */
            char* close = NULL;
            for (char* p = read + 4; p + 2 < end; p++) {
                if (p[0] == '-' && p[1] == '-' && p[2] == '>') {
                    close = p + 3;
                    break;
                }
            }
            if (!close) {
                /* Unterminated comment -- copy literally to avoid
                 * silent data loss. */
                *write++ = *read++;
            } else {
                read = close;
            }
        } else {
            *write++ = *read++;
        }
    }
    *write = '\0';
    *size = (size_t)(write - buf);
}

/* ----- Exclusive C14N (W3C exc-c14n 1.0) ----- */

/* Visible-prefix stack: tracks prefixes already emitted by output
 * ancestors so children don't re-emit them. The spec cap is
 * realistically < 32 nested namespace contexts. */
#define C14N_EXCL_STACK_SIZE 64

typedef struct {
    const char* prefixes[C14N_EXCL_STACK_SIZE];
    int count;
} C14nPrefixStack;

static int prefix_stack_find(const C14nPrefixStack* s, const char* p) {
    if (!p) return 0;
    for (int i = 0; i < s->count; i++) {
        if (s->prefixes[i] && strcmp(s->prefixes[i], p) == 0) return 1;
    }
    return 0;
}

static void prefix_stack_push(C14nPrefixStack* s, const char* p) {
    if (s->count < C14N_EXCL_STACK_SIZE) {
        s->prefixes[s->count++] = p;
    }
}

/* Returns 1 if the prefix is newly-visible (not in `emitted`),
 * 0 if already covered by an output ancestor. */
static int mark_visible_prefix(C14nPrefixStack* emitted, const char* prefix) {
    if (!prefix || !*prefix) return 0;
    if (prefix_stack_find(emitted, prefix)) return 0;
    return 1;
}

/* Recursive exclusive-C14N walker. */
static void c14n_serialize_element_excl(LeptrisElement elem,
                                         char** buffer, size_t* size,
                                         size_t* capacity,
                                         C14nPrefixStack* emitted,
                                         const char** inclusive) {
    if (!elem) return;

    const char* name = leptris_element_get_name(elem);
    const char* elem_prefix = leptris_element_get_prefix(elem);
    char temp[4096];
    int len;

    /* Pass 1: collect visibly-used prefixes at this element. */
    int need_elem_prefix = mark_visible_prefix(emitted, elem_prefix);
    int inclusive_count = 0;
    char needed_inclusive[16][64];
    if (inclusive) {
        for (const char** p = inclusive; *p && inclusive_count < 16; p++) {
            if (mark_visible_prefix(emitted, *p)) {
                strncpy(needed_inclusive[inclusive_count], *p, 63);
                needed_inclusive[inclusive_count][63] = '\0';
                inclusive_count++;
            }
        }
    }
    uint8_t attr_count = leptris_element_attribute_count(elem);
    char needed_attr_prefix[16][64];
    int attr_prefix_count = 0;
    for (struct leptris_attribute* a = leptris_element_get_first_attribute(elem);
         a && attr_prefix_count < 16; a = leptris_attr_next(a)) {
        LeptrisStringView nv = a->name_view;
        if (nv.length > 0 && nv.data) {
            const char* colon = (const char*)memchr(nv.data, ':', nv.length);
            if (colon && colon > nv.data) {
                size_t pl = (size_t)(colon - nv.data);
                if (pl < 64) {
                    char prefix[65];
                    memcpy(prefix, nv.data, pl);
                    prefix[pl] = '\0';
                    if (mark_visible_prefix(emitted, prefix)) {
                        memcpy(needed_attr_prefix[attr_prefix_count], prefix, pl + 1);
                        attr_prefix_count++;
                    }
                }
            }
        }
    }

    /* Push the new prefixes so children see them as inherited. */
    int pushed_count = 0;
    if (need_elem_prefix) { prefix_stack_push(emitted, elem_prefix); pushed_count++; }
    for (int i = 0; i < inclusive_count; i++) {
        prefix_stack_push(emitted, needed_inclusive[i]); pushed_count++;
    }
    for (int i = 0; i < attr_prefix_count; i++) {
        prefix_stack_push(emitted, needed_attr_prefix[i]); pushed_count++;
    }

    /* Open tag. */
    if (elem_prefix && elem_prefix[0]) {
        len = snprintf(temp, sizeof(temp), "<%s:%s", elem_prefix,
                       name ? name : "element");
    } else {
        len = snprintf(temp, sizeof(temp), "<%s", name ? name : "element");
    }
    APPEND_STRING(temp, len);

    /* Emit xmlns:prefix declarations for the newly-visible prefixes,
     * sorted lexicographically. Issue #194: deduplicate — the same
     * prefix may be both visibly-used AND in the inclusive list, in
     * which case it must be emitted exactly once. */
    const char* to_emit[40];
    int to_emit_count = 0;
    /* Helper: push if not already present. */
    #define EMIT_ONCE(p) do { \
        if ((p) && to_emit_count < 40) { \
            int dup = 0; \
            for (int k = 0; k < to_emit_count; k++) { \
                if (to_emit[k] && strcmp(to_emit[k], (p)) == 0) { dup = 1; break; } \
            } \
            if (!dup) to_emit[to_emit_count++] = (p); \
        } \
    } while (0)
    if (need_elem_prefix) EMIT_ONCE(elem_prefix);
    for (int i = 0; i < attr_prefix_count; i++) EMIT_ONCE(needed_attr_prefix[i]);
    for (int i = 0; i < inclusive_count; i++) EMIT_ONCE(needed_inclusive[i]);
    #undef EMIT_ONCE
    for (int i = 0; i < to_emit_count; i++) {
        for (int j = i + 1; j < to_emit_count; j++) {
            if (strcmp(to_emit[i], to_emit[j]) > 0) {
                const char* t = to_emit[i]; to_emit[i] = to_emit[j]; to_emit[j] = t;
            }
        }
    }
    for (int i = 0; i < to_emit_count; i++) {
        /* Resolve via xmlns declaration walk (not element prefix walk). */
        const char* uri = NULL;
        for (LeptrisElement p = elem; p && !uri; ) {
            for (struct leptris_namespace* ns = leptris_elem_namespaces(p); ns; ns = ns->next) {
                if (ns->prefix && strcmp(ns->prefix, to_emit[i]) == 0) {
                    uri = ns->uri;
                    break;
                }
            }
            if (!uri) {
                const char* pp = leptris_element_get_prefix(p);
                if (pp && strcmp(pp, to_emit[i]) == 0) {
                    uri = leptris_element_get_namespace_uri(p);
                }
            }
            p = leptris_element_get_parent(p);
        }
        if (uri) {
            len = snprintf(temp, sizeof(temp),
                           " xmlns:%s=\"%s\"", to_emit[i], uri);
            APPEND_STRING(temp, len);
        }
    }

    /* Attributes (sorted lexicographically as in canonical mode). */
    struct leptris_attribute** sorted_attrs = NULL;
    if (attr_count > 0) {
        sorted_attrs = (struct leptris_attribute**)malloc(
            attr_count * sizeof(*sorted_attrs));
        if (sorted_attrs) {
            uint8_t si = 0;
            for (struct leptris_attribute* a = leptris_element_get_first_attribute(elem);
                 a && si < attr_count; a = leptris_attr_next(a)) {
                sorted_attrs[si++] = a;
            }
            qsort(sorted_attrs, si, sizeof(*sorted_attrs), compare_attributes);
            for (size_t i = 0; i < attr_count; i++) {
                struct leptris_attribute* a = sorted_attrs[i];
                if (!a) continue;
                const char* an = a->name_view.data;
                size_t an_len = a->name_view.length;
                /* Single representation: entity values decode to a
                 * temporary this loop frees; others read the view. */
                const char* av = NULL;
                int av_owned = 0;
                LeptrisStringView av2 = leptris_attr_value_sv(a);
                if (attr_has_entities(a) && !leptris_sv_is_empty(&av2)) {
                    av = leptris_decode_entities_view(&av2, NULL);
                    av_owned = (av != NULL);
                }
                if (!av) av = attr_cvalue(a);
                if (an && av) {
                    char* escaped = c14n_escape_text(av, 1);
                    if (escaped) {
                        len = snprintf(temp, sizeof(temp),
                                       " %.*s=\"%s\"",
                                       (int)an_len, an, escaped);
                        APPEND_STRING(temp, len);
                        free(escaped);
                    }
                    if (av_owned) free((void*)av);
                }
            }
            free(sorted_attrs);
        }
    }

    /* Check for empty body. */
    int has_content = 0;
    int has_children = 0;
    LeptrisNode* child = (LeptrisNode*)leptris_node_first_child_internal((LeptrisNode*)elem);
    while (child) {
        has_children = 1;
        if (child->type == LEPTRIS_NODE_TYPE_TEXT) {
            const char* text = leptris_text_get_content((LeptrisTextNode*)child);
            if (text && *text) { has_content = 1; break; }
        }
        child = leptris_node_get_next_sibling(child);
    }

    if (!has_children && !has_content) {
        if (elem_prefix && elem_prefix[0]) {
            len = snprintf(temp, sizeof(temp), "></%s:%s>",
                           elem_prefix, name ? name : "element");
        } else {
            len = snprintf(temp, sizeof(temp), "></%s>",
                           name ? name : "element");
        }
        APPEND_STRING(temp, len);
        emitted->count -= pushed_count;
        return;
    }

    APPEND_STRING(">", 1);

    /* Walk children. */
    child = (LeptrisNode*)leptris_node_first_child_internal((LeptrisNode*)elem);
    while (child) {
        if (child->type == LEPTRIS_NODE_TYPE_ELEMENT) {
            c14n_serialize_element_excl((LeptrisElement)child, buffer, size,
                                         capacity, emitted, inclusive);
        } else if (child->type == LEPTRIS_NODE_TYPE_TEXT) {
            const char* text = leptris_text_get_content((LeptrisTextNode*)child);
            if (text) {
                char* escaped = c14n_escape_text(text, 0);
                if (escaped) {
                    APPEND_STRING(escaped, (int)strlen(escaped));
                    free(escaped);
                }
            }
        } else if (child->type == LEPTRIS_NODE_TYPE_CDATA) {
            const char* text = leptris_cdata_get_content((LeptrisCDATANode*)child);
            if (text) {
                char* escaped = c14n_escape_text(text, 0);
                if (escaped) {
                    APPEND_STRING(escaped, (int)strlen(escaped));
                    free(escaped);
                }
            }
        } else if (child->type == LEPTRIS_NODE_TYPE_PI) {
            LeptrisPINode* pi = (LeptrisPINode*)child;
            const char* t = leptris_pi_get_target(pi);
            const char* d = leptris_pi_get_data(pi);
            if (t) {
                len = snprintf(temp, sizeof(temp), "<?%s", t);
                APPEND_STRING(temp, len);
                if (d && *d) { APPEND_STRING(" ", 1); APPEND_STRING(d, (int)strlen(d)); }
                APPEND_STRING("?>", 2);
            }
        } else if (child->type == LEPTRIS_NODE_TYPE_COMMENT) {
            if (c14n_include_comments) {
                const char* text = leptris_comment_get_content(
                    (LeptrisCommentNode*)child);
                if (text) {
                    len = snprintf(temp, sizeof(temp), "<!--%s-->", text);
                    APPEND_STRING(temp, len);
                }
            }
        }
        child = leptris_node_get_next_sibling(child);
    }

    /* Close. */
    if (elem_prefix && elem_prefix[0]) {
        len = snprintf(temp, sizeof(temp), "</%s:%s>",
                       elem_prefix, name ? name : "element");
    } else {
        len = snprintf(temp, sizeof(temp), "</%s>", name ? name : "element");
    }
    APPEND_STRING(temp, len);

    emitted->count -= pushed_count;

cleanup:
    return;
}

LEPTRIS_API char* leptris_c14n_canonicalize_ex(
    struct leptris_document* doc,
    int version,
    LeptrisC14NMode mode,
    const char** inclusive_ns_prefixes,
    int with_comments) {
    if (!doc) return NULL;
    leptris_document_ensure_promoted(doc);
    LeptrisElement root = (LeptrisElement)doc->new_dom_root;
    if (!root) return NULL;
    return leptris_c14n_canonicalize_subtree_ex(
        root, version, mode, inclusive_ns_prefixes, with_comments);
}

LEPTRIS_API char* leptris_c14n_canonicalize_subtree_ex(
    LeptrisElement elem,
    int version,
    LeptrisC14NMode mode,
    const char** inclusive_ns_prefixes,
    int with_comments) {
    (void)version;
    if (!elem) return NULL;
    leptris_document_ensure_promoted(leptris_element_get_document(elem));

    int saved = c14n_include_comments;
    c14n_include_comments = with_comments ? 1 : 0;

    size_t capacity = 4096;
    size_t size = 0;
    char* buf = (char*)malloc(capacity);
    if (!buf) {
        c14n_include_comments = saved;
        return NULL;
    }
    buf[0] = '\0';

    if (mode == LEPTRIS_C14N_MODE_EXCLUSIVE) {
        C14nPrefixStack emitted = {0};
        char* buffer = buf;
        c14n_serialize_element_excl(elem, &buffer, &size, &capacity,
                                     &emitted, inclusive_ns_prefixes);
        buf = buffer;
    } else {
        char* buffer = buf;
        c14n_serialize_element(elem, &buffer, &size, &capacity);
        buf = buffer;
        if (!with_comments) {
            c14n_strip_comments(buf, &size);
        }
    }

    buf[size] = '\0';
    c14n_include_comments = saved;
    return buf;
}

