/* xpath/xpath_public.c — Public XPath API wrappers.
 *
 * Extracted from leptris.c (TODO 42 phase 2). These are the public-facing
 * functions: leptris_xpath_eval, leptris_xpath_result_*, and the
 * leptris_xpath_variable_set_* helpers.
 */

#include "../dom/pi.h"
#include "../include/leptris.h"
#include "../leptris_internal.h"
#include "parser.h"
#include "bytecode.h"  /* TODO 120: LeptrisXPathBytecode + compile */
#include "evaluator.h"
#include "evaluator_internal.h"  /* TODO 120: leptris_xpath_vm_eval */
#include "functions.h"  /* TODO 148 Phase 5 */
#include "xpath_variables.h"
#include "../dom/element.h"
#include "../dom/text.h"
#include "../dom/comment.h"
#include "../dom/cdata.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <ctype.h>


/* Lane 15: versioned eval — LEPTRIS_XPATH_10 gates the 3.x-only
 * SYNTAX (arrow, bang, lookup, let, inline functions, string
 * templates) off with a lexical scan outside literals; 3.1 is the
 * unmodified leptris_xpath_eval. */
static int x10_has_3x_syntax(const char* e) {
    int q = 0;   /* 0 none, '\'' , '"' */
    for (const char* p = e; *p; p++) {
        if (q) {
            if (*p == q) q = 0;
            continue;
        }
        if (*p == '\'' || *p == '"' || *p == '`') {
            q = *p;
            continue;
        }
        if (p[0] == '=' && p[1] == '>') return 1;
        if (*p == '!' || *p == '?' || *p == '`') return 1;
        if ((p == e || !isalnum((unsigned char)p[-1])) &&
            strncmp(p, "let ", 4) == 0) return 1;
        if ((p == e || !isalnum((unsigned char)p[-1])) &&
            strncmp(p, "function", 8) == 0 &&
            (p[8] == '(' || p[8] == ' ')) return 1;
        /* map/array constructors: the keyword at a name boundary
         * followed by optional whitespace and '{' — a '{' can
         * never follow a name in valid 1.0 syntax (the { } forms
         * are 3.x-only; square arrays are caught by their '?'
         * lookups or fail 1.0 predicate parsing anyway). */
        if (p == e || !isalnum((unsigned char)p[-1])) {
            size_t kw = (strncmp(p, "map", 3) == 0) ? 3
                      : (strncmp(p, "array", 5) == 0) ? 5 : 0;
            if (kw) {
                const char* b = p + kw;
                while (*b == ' ' || *b == '\t' || *b == '\n' || *b == '\r')
                    b++;
                if (*b == '{') return 1;
            }
        }
    }
    return q == '`';   /* unterminated template is 3.x anyway */
}

LEPTRIS_API LeptrisXPathResult leptris_xpath_eval_versioned(
    LeptrisDocument doc,
    LeptrisElement context,
    const char* expression,
    LeptrisXPathVersion version,
    LeptrisStatus* status
) {
    if (status) *status = LEPTRIS_OK;
    if (!doc || !expression) {
        if (status) *status = LEPTRIS_ERROR_NULL_ARG;
        return NULL;
    }
    if (version != LEPTRIS_XPATH_10 && version != LEPTRIS_XPATH_31) {
        if (status) *status = LEPTRIS_ERROR_INVALID_ARG;
        return NULL;
    }
    if (version == LEPTRIS_XPATH_10 &&
        x10_has_3x_syntax(expression)) {
        leptris_set_error(
            LEPTRIS_ERROR_INVALID_ARG,
            "XPath 3.x syntax rejected in LEPTRIS_XPATH_10 mode");
        if (status) *status = LEPTRIS_ERROR_INVALID_ARG;
        return NULL;
    }
    return leptris_xpath_eval(doc, context, expression);
}

LEPTRIS_API LeptrisXPathResult leptris_xpath_eval(
    LeptrisDocument doc,
    LeptrisElement context,
    const char* expression
) {
    if (!doc || !expression) return NULL;

    /* FlatDoc fast path removed — direct_parse builds the
     * LeptrisElement tree eagerly. XPath always evaluates against
     * the compact-pointer DOM via the bytecode VM. */

    /* Use context element if provided, otherwise use root */
    LeptrisElement context_elem = context ? context : leptris_document_root(doc);
    if (!context_elem) return NULL;

    /* Call internal implementation with string length */
    size_t expr_len = strlen(expression);

    /* AST + bytecode cache (TODO 113 + TODO 120 Phase F): repeated
     * evaluations of the same expression skip both the parse and the
     * compile phase. 16-entry LRU keyed by FNV-1a hash of the
     * expression string. Single hash + scan via xpath_ast_cache_get
     * (TODO 159 Phase E drive-by: was two separate lookups).
     *
     * Ownership: the cache owns the AST and BC after first parse /
     * compile. Subsequent calls borrow them for the duration of one
     * evaluate; never free. */
    XPathCacheEntry ce;
    int cached = xpath_ast_cache_get(expression, expr_len, &ce);
    XPathASTNode* ast = cached ? ce.ast : NULL;
    LeptrisXPathBytecode* bc = cached ? ce.bc : NULL;

    if (!ast) {
        XPathParser* parser = xpath_parser_new(expression, expr_len);
        if (!parser) return NULL;

        ast = xpath_parse(parser);
        const char* parse_error = xpath_parser_error(parser);

        if (!ast || parse_error) {
            /* TODO.concurrency/01: syntax errors also snapshot into
             * the document slot — same contract as eval failures. */
            if (parse_error && parse_error[0]) {
                strncpy(doc->last_error_message, parse_error,
                        sizeof(doc->last_error_message) - 1);
                doc->last_error_message[sizeof(doc->last_error_message) - 1] = '\0';
            } else {
                strncpy(doc->last_error_message, "XPath syntax error",
                        sizeof(doc->last_error_message) - 1);
            }
            xpath_parser_free(parser);
            return NULL;
        }

        xpath_parser_free(parser);

        /* Hand ownership to the cache. Returns the canonical AST —
         * a racing twin insert may have won the slot, so reassign. */
        ast = xpath_ast_cache_insert(expression, expr_len, ast);
        bc = NULL;  /* not yet compiled */
    }

    /* Create evaluation context. TODO 163: stack-allocate the
     * struct (the storage lives in this function's frame and is
     * released on return). Saves one malloc/free pair per eval. */
    XPathContext ctx_storage;
    XPathContext* xpath_ctx = &ctx_storage;
    xpath_context_init(xpath_ctx, doc, context_elem);
    if (!xpath_ctx->document) {
        /* init refuses to populate when args are invalid. */
        xpath_ast_cache_release(ast);
        return NULL;
    }

    struct leptris_xpath_result* result = NULL;
    if (bc) {
        result = leptris_xpath_vm_run_bc(bc, xpath_ctx);
    } else {
        /* Compile + run + cache. leptris_xpath_vm_eval compiles and
         * runs but does not cache; we cache explicitly here so the
         * next caller hits the fast path above. */
        bc = leptris_xpath_compile_ast(ast);
        if (bc) {
            result = leptris_xpath_vm_run_bc(bc, xpath_ctx);
            xpath_ast_cache_store_bc(expression, expr_len, bc);
            /* cache now owns bc; do not free here. */
        }
    }
    /* If the VM failed for any reason (e.g., unsupported edge case),
     * fall back to direct AST evaluation. */
    if (!result) {
        result = xpath_evaluate(xpath_ctx, ast);
    }

    /* TODO.concurrency/01: evaluation failure snapshots the reason
     * into the document's error slot (thread-local channel may be
     * overwritten by the next call before the binding reads it). */
    if (!result && xpath_ctx->error_msg[0]) {
        strncpy(doc->last_error_message, xpath_ctx->error_msg,
                sizeof(doc->last_error_message) - 1);
        doc->last_error_message[sizeof(doc->last_error_message) - 1] = '\0';
    }

    /* Cleanup. ast and bc are owned by the cache — never free here.
     * xpath_context_cleanup releases the namespace_mappings and the
     * per-call function registry if any; the storage itself is on
     * the stack and goes away when this function returns. */
    xpath_context_cleanup(xpath_ctx);

    /* TODO.concurrency/08: drop the cache pin taken by get/insert
     * before returning (evicted entries wait for this). */
    xpath_ast_cache_release(ast);

    return result;
}

LEPTRIS_API LeptrisXPathResultType leptris_xpath_result_type(LeptrisXPathResult result) {
    if (!result) return LEPTRIS_XPATH_STRING;  /* Default to string for NULL */

    /* Map internal XPathResultType to public LeptrisXPathResultType */
    switch (result->type) {
        case XPATH_RESULT_NODESET:
            /* A function item rides a one-member synthetic nodeset
             * ("\x03FN" closure / "\x03FR" named reference) —
             * classify it here, at the public boundary. Only the
             * synthetic carrier qualifies; a single-member REAL
             * nodeset is an ordinary nodeset. */
            if (result->value.nodeset_value &&
                result->value.nodeset_value->count == 1) {
                void* n = result->value.nodeset_value->nodes[0];
                if (n && XPATH_NODE_TYPE(n) == LEPTRIS_NODE_TEXT) {
                    XPathTextNode* tn = (XPathTextNode*)n;
                    if (tn->content &&
                        (strncmp(tn->content, "\x03" "FN", 3) == 0 ||
                         strncmp(tn->content, "\x03" "FR", 3) == 0))
                        return LEPTRIS_XPATH_FUNCTION;
                }
            }
            return LEPTRIS_XPATH_NODESET;
        case XPATH_RESULT_BOOLEAN:
            return LEPTRIS_XPATH_BOOLEAN;
        case XPATH_RESULT_NUMBER:
            return LEPTRIS_XPATH_NUMBER;
        case XPATH_RESULT_STRING:
            return LEPTRIS_XPATH_STRING;
        default:
            return LEPTRIS_XPATH_STRING;
    }
}

LEPTRIS_API size_t leptris_xpath_result_count(LeptrisXPathResult result) {
    if (!result || result->type != XPATH_RESULT_NODESET) return 0;
    return result->value.nodeset_value ? result->value.nodeset_value->count : 0;
}

LEPTRIS_API LeptrisElement leptris_xpath_result_get(LeptrisXPathResult result, size_t index) {
    if (!result || result->type != XPATH_RESULT_NODESET) return NULL;
    if (!result->value.nodeset_value || index >= result->value.nodeset_value->count) return NULL;

    void* node = result->value.nodeset_value->nodes[index];

    /* SAFETY: Validate node pointer before returning
     * Stale pointers from previous operations can cause crashes */
    if ((uintptr_t)node < 0x1000) {
        return NULL;  /* Clearly invalid pointer */
    }

    /* Elements only (tag 0 — the public element kind). Synthetic
     * attribute/namespace nodes and text/comment/cdata/pi tree nodes
     * must not miscast as LeptrisElement. Mixed results are consumed
     * via node_kind/get_node/node_name/node_value. */
    if (XPATH_NODE_TYPE(node) != LEPTRIS_NODE_ELEMENT) {
        return NULL;
    }

    return (LeptrisElement)node;
}

LEPTRIS_API size_t leptris_xpath_result_get_nodes(
    LeptrisXPathResult result, LeptrisElement* out_nodes, size_t max_count) {
    if (!result || !out_nodes || max_count == 0) return 0;
    if (result->type != XPATH_RESULT_NODESET) return 0;
    if (!result->value.nodeset_value) return 0;

    size_t copied = 0;
    size_t count = result->value.nodeset_value->count;
    for (size_t i = 0; i < count && copied < max_count; i++) {
        void* node = result->value.nodeset_value->nodes[i];
        /* Elements only — same guard as leptris_xpath_result_get;
         * attribute/text result nodes would miscast. */
        if ((uintptr_t)node >= 0x1000 &&
            XPATH_NODE_TYPE(node) == LEPTRIS_NODE_ELEMENT) {
            out_nodes[copied++] = (LeptrisElement)node;
        }
    }
    return copied;
}

LEPTRIS_API size_t leptris_xpath_result_get_nodes_ex(
    LeptrisXPathResult result,
    LeptrisNodeRef* out_nodes,
    LeptrisXPathNodeKind* out_kinds,
    size_t max_count) {
    if (!result || result->type != XPATH_RESULT_NODESET) return 0;
    if (!result->value.nodeset_value) return 0;

    XPathNodeSet* ns = result->value.nodeset_value;
    size_t copied = 0;
    for (size_t i = 0; i < ns->count && copied < max_count; i++) {
        void* node = ns->nodes[i];
        if ((uintptr_t)node < 0x1000) continue;
        if (out_nodes) out_nodes[copied] = (LeptrisNodeRef)node;
        if (out_kinds) out_kinds[copied] = leptris_xpath_result_node_kind(result, i);
        copied++;
    }
    return copied;
}

/* Shared guard for the mixed-nodeset accessors: fetch node i of the
 * result with basic pointer validation, or NULL. */
static void* xp_result_node(LeptrisXPathResult result, size_t index) {
    if (!result || result->type != XPATH_RESULT_NODESET) return NULL;
    if (!result->value.nodeset_value || index >= result->value.nodeset_value->count) return NULL;
    void* node = result->value.nodeset_value->nodes[index];
    if ((uintptr_t)node < 0x1000) return NULL;
    return node;
}

LEPTRIS_API LeptrisXPathNodeKind leptris_xpath_result_node_kind(
    LeptrisXPathResult result, size_t index) {
    void* node = xp_result_node(result, index);
    if (!node) return LEPTRIS_XPATH_NODE_OTHER;
    /* Unified tag space (issue #477): real DOM nodes carry public
     * LeptrisNodeKind values (element=0, text=1, comment=2, cdata=3,
     * pi=4, doctype=5); synthetic attribute/namespace/text nodes
     * carry LEPTRIS_NODE_ATTRIBUTE/NAMESPACE/TEXT (6/7/8). */
    int tag = (int)XPATH_NODE_TYPE(node);
    if (tag == LEPTRIS_NODE_ELEMENT) return LEPTRIS_XPATH_NODE_ELEMENT;
    if (tag == LEPTRIS_NODE_ATTRIBUTE) return LEPTRIS_XPATH_NODE_ATTRIBUTE;
    if (tag == LEPTRIS_NODE_TYPE_TEXT || tag == LEPTRIS_NODE_TYPE_CDATA ||
        tag == LEPTRIS_NODE_TEXT) {
        return LEPTRIS_XPATH_NODE_TEXT;
    }
    return LEPTRIS_XPATH_NODE_OTHER;
}

LEPTRIS_API LeptrisNodeRef leptris_xpath_result_get_node(
    LeptrisXPathResult result, size_t index) {
    return (LeptrisNodeRef)xp_result_node(result, index);
}

LEPTRIS_API const char* leptris_xpath_result_node_name(
    LeptrisXPathResult result, size_t index) {
    void* node = xp_result_node(result, index);
    if (!node) return NULL;
    int tag = (int)XPATH_NODE_TYPE(node);
    if (tag == LEPTRIS_NODE_ATTRIBUTE) {
        LeptrisAttributeNode* attr = (LeptrisAttributeNode*)node;
        return (attr->name && attr->name[0]) ? attr->name : NULL;
    }
    if (tag == LEPTRIS_NODE_NAMESPACE) {
        LeptrisNamespaceNode* ns = (LeptrisNamespaceNode*)node;
        return (ns->prefix && ns->prefix[0]) ? ns->prefix : NULL;
    }
    if (tag == LEPTRIS_NODE_ELEMENT) {
        const char* name = leptris_element_get_name((LeptrisElement)node);
        return (name && name[0]) ? name : NULL;
    }
    return NULL;
}

LEPTRIS_API const char* leptris_xpath_result_node_value(
    LeptrisXPathResult result, size_t index) {
    void* node = xp_result_node(result, index);
    if (!node) return NULL;
    int tag = (int)XPATH_NODE_TYPE(node);
    if (tag == LEPTRIS_NODE_ATTRIBUTE) {
        LeptrisAttributeNode* attr = (LeptrisAttributeNode*)node;
        return attr->value;
    }
    if (tag == LEPTRIS_NODE_NAMESPACE) {
        return ((LeptrisNamespaceNode*)node)->uri;
    }
    if (tag == LEPTRIS_NODE_TYPE_TEXT) {
        return leptris_text_get_content((LeptrisTextNode*)node);
    }
    if (tag == LEPTRIS_NODE_TYPE_CDATA) {
        return leptris_cdata_get_content((LeptrisCDATANode*)node);
    }
    if (tag == LEPTRIS_NODE_TYPE_COMMENT) {
        return leptris_comment_get_content((LeptrisCommentNode*)node);
    }
    if (tag == LEPTRIS_NODE_TYPE_PI) {
        return leptris_pi_get_data((LeptrisPINode*)node);
    }
    if (tag == LEPTRIS_NODE_TEXT) {
        XPathTextNode* text = (XPathTextNode*)node;
        /* Numeric sequence members carry a "\x03N" marker for
         * per-member type checks — never show it to public
         * consumers. */
        if (text->content && text->content[0] == '\x03' &&
            text->content[1] == 'N')
            return text->content + 2;
        return text->content;
    }
    return NULL;
}

LEPTRIS_API int leptris_xpath_result_boolean(LeptrisXPathResult result) {
    if (!result) return 0;

    switch (result->type) {
        case XPATH_RESULT_BOOLEAN:
            return result->value.boolean_value ? 1 : 0;
        case XPATH_RESULT_NUMBER:
            return (result->value.number_value != 0.0 && !isnan(result->value.number_value)) ? 1 : 0;
        case XPATH_RESULT_STRING:
            return (result->value.string_value && result->value.string_value[0] != '\0') ? 1 : 0;
        case XPATH_RESULT_NODESET:
            return (result->value.nodeset_value && result->value.nodeset_value->count > 0) ? 1 : 0;
        default:
            return 0;
    }
}

LEPTRIS_API double leptris_xpath_result_number(LeptrisXPathResult result) {
    if (!result) return NAN;

    switch (result->type) {
        case XPATH_RESULT_NUMBER:
            return result->value.number_value;
        case XPATH_RESULT_BOOLEAN:
            return result->value.boolean_value ? 1.0 : 0.0;
        case XPATH_RESULT_STRING:
            if (!result->value.string_value || result->value.string_value[0] == '\0') {
                return NAN;
            }
            {
                char* endptr;
                double val = strtod(result->value.string_value, &endptr);
                return (endptr == result->value.string_value || *endptr != '\0') ? (NAN) : val;
            }
        case XPATH_RESULT_NODESET:
            if (result->value.nodeset_value && result->value.nodeset_value->count > 0) {
                LeptrisElement elem = (LeptrisElement)result->value.nodeset_value->nodes[0];
                const char* text = leptris_element_text(elem);
                if (text && text[0] != '\0') {
                    char* endptr;
                    double val = strtod(text, &endptr);
                    return (endptr == text || *endptr != '\0') ? (NAN) : val;
                }
            }
            return NAN;
        default:
            return NAN;
    }
}

LEPTRIS_API char* leptris_xpath_result_string(LeptrisXPathResult result) {
    if (!result) return leptris_strdup("");

    switch (result->type) {
        case XPATH_RESULT_STRING:
            return result->value.string_value ? leptris_strdup(result->value.string_value) : leptris_strdup("");
        case XPATH_RESULT_BOOLEAN:
            return leptris_strdup(result->value.boolean_value ? "true" : "false");
        case XPATH_RESULT_NUMBER:
            if (result->is_int) return xpath_int_to_string(result->int_value);
            return xpath_number_to_string(result->value.number_value);
        case XPATH_RESULT_NODESET:
            if (result->value.nodeset_value &&
                result->value.nodeset_value->count > 0) {
                void* first = result->value.nodeset_value->nodes[0];
                /* Document node (RTF variables/params, bug-56): the
                 * string-value is ALL text in the fragment, doc-level
                 * children before the root element's subtree. Shared
                 * walker lives in evaluator_types.c. */
                if ((int)XPATH_NODE_TYPE(first) == LEPTRIS_NODE_TYPE_DOCUMENT) {
                    extern char* get_node_text(void* node);
                    char* dsv = get_node_text(first);
                    return dsv ? dsv : leptris_strdup("");
                }
                /* First-node string-value by KIND (issue #514
                 * fallout): attributes use their value; text/CDATA
                 * and comments their content; elements their
                 * descendant text. */
                LeptrisXPathNodeKind kind =
                    leptris_xpath_result_node_kind(result, 0);
                if (kind == LEPTRIS_XPATH_NODE_ELEMENT) {
                    const char* text =
                        leptris_element_text((LeptrisElement)first);
                    return text ? leptris_strdup(text) : leptris_strdup("");
                }
                const char* v = leptris_xpath_result_node_value(result, 0);
                return v ? leptris_strdup(v) : leptris_strdup("");
            }
            /* fallthrough */
        default:
            return leptris_strdup("");
    }
}

LEPTRIS_API void leptris_xpath_result_free(struct leptris_xpath_result* result) {
    if (!result) return;
    xpath_result_free(result);
}

LEPTRIS_API LeptrisXPathVariableSet leptris_xpath_variable_set_new(void) {
    return (LeptrisXPathVariableSet)xpath_variable_set_new();
}

LEPTRIS_API void leptris_xpath_variable_set_free(LeptrisXPathVariableSet set) {
    xpath_variable_set_free((XPathVariableSet*)set);
}

LEPTRIS_API LeptrisStatus leptris_xpath_variable_set_boolean(LeptrisXPathVariableSet set, const char* name, int value) {
    if (!set || !name) return LEPTRIS_ERROR_NULL_ARG;

    XPathVariable* var = xpath_variable_set_add((XPathVariableSet*)set, name, XPATH_VAR_TYPE_BOOLEAN);
    if (!var) return LEPTRIS_ERROR_MEMORY;

    if (!xpath_variable_set_boolean(var, value)) {
        return LEPTRIS_ERROR_INVALID_ARG;
    }

    return LEPTRIS_OK;
}

LEPTRIS_API LeptrisStatus leptris_xpath_variable_set_number(LeptrisXPathVariableSet set, const char* name, double value) {
    if (!set || !name) return LEPTRIS_ERROR_NULL_ARG;

    XPathVariable* var = xpath_variable_set_add((XPathVariableSet*)set, name, XPATH_VAR_TYPE_NUMBER);
    if (!var) return LEPTRIS_ERROR_MEMORY;

    if (!xpath_variable_set_number(var, value)) {
        return LEPTRIS_ERROR_INVALID_ARG;
    }

    return LEPTRIS_OK;
}

LEPTRIS_API LeptrisStatus leptris_xpath_variable_set_string(LeptrisXPathVariableSet set, const char* name, const char* value) {
    if (!set || !name) return LEPTRIS_ERROR_NULL_ARG;

    XPathVariable* var = xpath_variable_set_add((XPathVariableSet*)set, name, XPATH_VAR_TYPE_STRING);
    if (!var) return LEPTRIS_ERROR_MEMORY;

    if (!xpath_variable_set_string(var, value)) {
        return LEPTRIS_ERROR_MEMORY;
    }

    return LEPTRIS_OK;
}

/* ---- External namespace bindings (v1.2.0) ---------------------------
 * Expression prefix -> URI pairs for namespace-aware name tests;
 * XPointer xmlns() is the canonical producer. */

struct leptris_xpath_ns_map {
    char** prefixes;
    char** uris;
    size_t count;
    size_t capacity;
};

LEPTRIS_API LeptrisXPathNsSet leptris_xpath_ns_set_new(void) {
    struct leptris_xpath_ns_map* m =
        (struct leptris_xpath_ns_map*)calloc(1, sizeof(*m));
    return (LeptrisXPathNsSet)m;
}

LEPTRIS_API void leptris_xpath_ns_set_free(LeptrisXPathNsSet set) {
    struct leptris_xpath_ns_map* m = (struct leptris_xpath_ns_map*)set;
    if (!m) return;
    for (size_t i = 0; i < m->count; i++) {
        free(m->prefixes[i]);
        free(m->uris[i]);
    }
    free(m->prefixes);
    free(m->uris);
    free(m);
}

LEPTRIS_API LeptrisStatus leptris_xpath_ns_set_add(
    LeptrisXPathNsSet set, const char* prefix, const char* uri) {
    struct leptris_xpath_ns_map* m = (struct leptris_xpath_ns_map*)set;
    if (!m || !prefix || !uri || !prefix[0] || !uri[0]) {
        return LEPTRIS_ERROR_NULL_ARG;
    }
    for (size_t i = 0; i < m->count; i++) {
        if (strcmp(m->prefixes[i], prefix) == 0) {
            char* copy = leptris_strdup(uri);
            if (!copy) return LEPTRIS_ERROR_MEMORY;
            free(m->uris[i]);
            m->uris[i] = copy;
            return LEPTRIS_OK;
        }
    }
    if (m->count >= m->capacity) {
        size_t cap = m->capacity ? m->capacity * 2 : 8;
        char** gp = (char**)realloc(m->prefixes, cap * sizeof(char*));
        char** gu = gp ? (char**)realloc(m->uris, cap * sizeof(char*)) : NULL;
        if (!gp || !gu) {
            /* realloc for uris failed but prefixes moved — keep the
             * old (still-valid) arrays by shrinking back is not
             * possible; the map stays consistent with old capacity
             * only if neither shrank. On partial failure free the
             * moved one and report memory. */
            free(gu ? NULL : gp);
            return LEPTRIS_ERROR_MEMORY;
        }
        m->prefixes = gp;
        m->uris = gu;
        m->capacity = cap;
    }
    char* pc = leptris_strdup(prefix);
    char* uc = leptris_strdup(uri);
    if (!pc || !uc) {
        free(pc);
        free(uc);
        return LEPTRIS_ERROR_MEMORY;
    }
    m->prefixes[m->count] = pc;
    m->uris[m->count] = uc;
    m->count++;
    return LEPTRIS_OK;
}

const char* leptris_xpath_ns_lookup(const struct leptris_xpath_ns_map* m,
                                    const char* prefix, size_t prefix_len) {
    if (!m || !prefix || prefix_len == 0) return NULL;
    for (size_t i = 0; i < m->count; i++) {
        if (strlen(m->prefixes[i]) == prefix_len &&
            memcmp(m->prefixes[i], prefix, prefix_len) == 0) {
            return m->uris[i];
        }
    }
    return NULL;
}

LEPTRIS_API LeptrisXPathNsSet leptris_xpath_ns_set_new_from_pairs(
    const char* const* flat,
    size_t pair_count) {
    if (!flat || pair_count == 0) return NULL;
    for (size_t i = 0; i < pair_count * 2; i++) {
        if (!flat[i] || !flat[i][0]) return NULL;
    }
    LeptrisXPathNsSet set = leptris_xpath_ns_set_new();
    if (!set) return NULL;
    for (size_t i = 0; i < pair_count; i++) {
        if (leptris_xpath_ns_set_add(set, flat[i * 2], flat[i * 2 + 1]) !=
            LEPTRIS_OK) {
            leptris_xpath_ns_set_free(set);
            return NULL;
        }
    }
    return set;
}

LEPTRIS_API LeptrisXPathResult leptris_xpath_eval_ns(
    LeptrisDocument doc,
    LeptrisElement context,
    const char* expression,
    LeptrisXPathNsSet ns)
{
    if (!doc || !expression) {
        return NULL;
    }

    /* Compiled-handle route (issue #564): the pinned AST comes from
     * the shared expression cache (no per-call parse) and the
     * compiled evaluator runs the VM first — its name matcher is
     * namespace-aware — with the interpreter as fallback. */
    LeptrisXPathCompiled compiled = leptris_xpath_compile(expression);
    if (!compiled) return NULL;
    struct leptris_xpath_result* result =
        leptris_xpath_compiled_eval_ns(compiled, doc, context, ns);
    leptris_xpath_compiled_free(compiled);
    return result;
}

LEPTRIS_API LeptrisXPathResult leptris_xpath_eval_with_vars(
    LeptrisDocument doc,
    const char* expression,
    LeptrisXPathVariableSet variables)
{
    return leptris_xpath_eval_with_vars_context(doc, NULL, expression, variables);
}

LEPTRIS_API LeptrisXPathResult leptris_xpath_eval_with_vars_context(
    LeptrisDocument doc,
    LeptrisElement context,
    const char* expression,
    LeptrisXPathVariableSet variables)
{
    if (!doc || !expression) {
        return NULL;
    }

    /* Compiled-handle route (issue #565): shared expression cache (no
     * per-call parse), VM first — variable references resolve through
     * BC_FALLBACK_EVAL subtrees and `@attr=$var` predicates run on the
     * fused opcode — interpreter fallback when the VM declines. */
    LeptrisXPathCompiled compiled = leptris_xpath_compile(expression);
    if (!compiled) return NULL;
    struct leptris_xpath_result* result =
        leptris_xpath_compiled_eval_vars(compiled, doc, context, variables);
    leptris_xpath_compiled_free(compiled);
    return result;
}

/* ---- Custom XPath function registration (TODO 148 Phase 5) ----
 *
 * Registered functions live on the document. The evaluator merges
 * them with the standard XPath 1.0 library when building the
 * per-context function registry. Standard functions win name
 * collisions.
 *
 * State is captured via XPathFunctionDef.user_data (set on
 * registration). The evaluator saves/restores that user_data on
 * ctx->current_fn_user_data around each handler invocation, so
 * custom-fn recursion is safe.
 */

/* Per-doc custom function entry. */
struct leptris_custom_xpath_fn {
    char* name;
    LeptrisXPathFn fn;
    void* user_data;
    struct leptris_custom_xpath_fn* next;
};

/* Thunk bridging the internal XPathFunctionHandler signature to
 * the public LeptrisXPathFn. Each arg AST is evaluated, converted
 * to a string, and passed to the user callback. The callback's
 * returned string is wrapped as a string-typed result.
 *
 * The per-call user_data slot (ctx->current_fn_user_data) carries
 * the (LeptrisXPathFn, void* user_data) pair, packed into a small
 * heap struct on registration. */
struct leptris_custom_fn_state {
    LeptrisXPathFn fn;
    void* user_data;
};

static struct leptris_xpath_result* custom_xpath_thunk(
    XPathContext* context,
    XPathASTNode** args,
    size_t arg_count) {
    if (!context) return NULL;
    struct leptris_custom_fn_state* state =
        (struct leptris_custom_fn_state*)context->current_fn_user_data;
    if (!state || !state->fn) return NULL;

    extern struct leptris_xpath_result* evaluate_expr(XPathContext*, XPathASTNode*);

    const char** str_args = NULL;
    char** owned = NULL;
    size_t owned_count = 0;
    if (arg_count > 0) {
        str_args = (const char**)calloc(arg_count, sizeof(char*));
        owned = (char**)calloc(arg_count, sizeof(char*));
        if (!str_args || !owned) {
            free(str_args); free(owned);
            return NULL;
        }
    }

    struct leptris_xpath_result* result = NULL;
    for (size_t i = 0; i < arg_count; i++) {
        struct leptris_xpath_result* r = evaluate_expr(context, args[i]);
        if (!r) goto cleanup;
        char* s = xpath_to_string(r);
        xpath_result_free(r);
        if (!s) s = strdup("");
        owned[owned_count++] = s;
        str_args[i] = s;
    }

    char* result_str = state->fn(str_args, (int)arg_count, state->user_data);
    if (result_str) {
        result = xpath_result_new(XPATH_RESULT_STRING);
        if (result) {
            result->value.string_value = result_str;
        } else {
            free(result_str);
        }
    }

cleanup:
    for (size_t i = 0; i < owned_count; i++) free(owned[i]);
    free(owned);
    free(str_args);
    return result;
}

/* Defined below (registry cache invalidation). */
void leptris_xpath_invalidate_fn_registry(struct leptris_document* doc);

LEPTRIS_API LeptrisStatus leptris_xpath_register_function(
    LeptrisDocument doc,
    const char* name,
    LeptrisXPathFn fn,
    void* user_data) {
    if (!doc || !name || !fn) return LEPTRIS_ERROR_NULL_ARG;

    /* Each registration owns a heap-allocated (fn, user_data) pair
     * that the evaluator packs onto the XPathFunctionDef.user_data
     * slot. The thunk unpacks it via ctx->current_fn_user_data. */
    struct leptris_custom_fn_state* state =
        (struct leptris_custom_fn_state*)calloc(1, sizeof(*state));
    if (!state) return LEPTRIS_ERROR_MEMORY;

    struct leptris_custom_xpath_fn* entry =
        (struct leptris_custom_xpath_fn*)calloc(1, sizeof(*entry));
    if (!entry) { free(state); return LEPTRIS_ERROR_MEMORY; }

    entry->name = strdup(name);
    if (!entry->name) {
        free(entry); free(state);
        return LEPTRIS_ERROR_MEMORY;
    }
    entry->fn = fn;
    entry->user_data = user_data;
    /* Stash the (fn, user_data) state on the entry so the
     * registry builder can pass it through to the thunk via the
     * XPathFunctionDef.user_data slot. Use the `user_data` field
     * of the entry itself — the public API `user_data` arg is
     * what the user wants back, and we pack both into `state`. */
    state->fn = fn;
    state->user_data = user_data;
    /* Override the entry's user_data with the packed state so the
     * registry builder picks it up. */
    entry->user_data = state;
    entry->next = doc->custom_xpath_fns;
    doc->custom_xpath_fns = entry;
    leptris_xpath_invalidate_fn_registry(doc);
    return LEPTRIS_OK;
}

/* Build a per-context function registry that merges standard
 * XPath 1.0 functions with the document's custom fns. Returns
 * NULL if the doc has no custom fns (the context then uses the
 * shared standard singleton). Caller frees via the registry's
 * normal lifecycle. */
LEPTRIS_API LeptrisStatus leptris_exslt_enable(LeptrisDocument doc) {
    if (!doc) return LEPTRIS_ERROR_NULL_ARG;
    struct leptris_document* d = (struct leptris_document*)doc;
    d->exslt_enabled = 1;
    leptris_xpath_invalidate_fn_registry(d);
    return LEPTRIS_OK;
}

XPathFunctionRegistry* leptris_xpath_build_custom_registry(struct leptris_document* doc) {
    if (!doc) return NULL;
    if (!doc->custom_xpath_fns && !doc->exslt_enabled &&
        !doc->xslt_state && !doc->sch_state)
        return NULL;

    /* TODO.transform perf: the merged registry depends only on the
     * three inputs above; while none changes, EVERY evaluation
     * context on this document can share one build (a transform
     * otherwise rebuilds ~45 registrations per expression). */
    if (doc->cached_fn_registry)
        return (XPathFunctionRegistry*)doc->cached_fn_registry;

    XPathFunctionRegistry* reg = xpath_function_registry_new();
    if (!reg) return NULL;
    xpath_function_registry_init_standard(reg);

    /* EXSLT pack (TODO.concurrency/06): native handlers registered
     * alongside (and after) the standard library. */
    if (doc->exslt_enabled) {
        extern void leptris_exslt_register(XPathFunctionRegistry*);
        leptris_exslt_register(reg);
    }

    /* XSLT bridge (TODO.transform 04/05): the board's design
     * contract — "the same path as custom functions (SSOT)". While
     * a transform runs on this document, key()/current()/format-
     * number()/document()/generate-id()/system-property() plus the
     * EXSLT node-set/regexp/date subset resolve through here with
     * the transform state as user_data. The registry's ownership
     * stays with the eval context that built it (init → cleanup),
     * so no lifecycle juggling here. */
    if (doc->xslt_state) {
        extern void xslt_register_bridge_handlers(XPathFunctionRegistry*,
                                                  void*);
        xslt_register_bridge_handlers(reg, doc->xslt_state);
    }

    /* Schematron key bridge (lane 16.5): while a schema validation
     * runs on this document, key() resolves through the schema's
     * xsl:key declarations. Same save/restore + invalidation
     * discipline as the XSLT bridge above. */
    if (doc->sch_state) {
        extern void leptris_sch_register_key_bridge(
            XPathFunctionRegistry*, void*);
        leptris_sch_register_key_bridge(reg, doc->sch_state);
    }

    for (struct leptris_custom_xpath_fn* e = doc->custom_xpath_fns;
         e; e = e->next) {
        /* min=0 max=32: the user callback is responsible for
         * validating its own arity. */
        xpath_function_registry_register(reg, e->name, custom_xpath_thunk, 0, 32);
        /* Patch the just-added entry's user_data. The standard
         * register helper doesn't take user_data; walk to the
         * last entry and set it. */
        if (reg->count > 0) {
            reg->functions[reg->count - 1].user_data = e->user_data;
        }
    }
    doc->cached_fn_registry = reg;
    return reg;
}

/* Drop the cached merged registry — call whenever any input changes
 * (custom fns registered/removed, exslt toggled, xslt_state swapped
 * by transform enter/exit). The next build_custom_registry call
 * rebuilds. Evaluation contexts hold it only for their lifetime, so
 * invalidation between evaluations is safe. */
void leptris_xpath_invalidate_fn_registry(struct leptris_document* doc) {
    if (!doc || !doc->cached_fn_registry) return;
    xpath_function_registry_free((XPathFunctionRegistry*)doc->cached_fn_registry);
    doc->cached_fn_registry = NULL;
}

/* Release the doc's custom-fn list. Called from
 * leptris_document_free. */
void leptris_xpath_free_custom_fns(struct leptris_document* doc) {
    if (!doc) return;
    leptris_xpath_invalidate_fn_registry(doc);
    struct leptris_custom_xpath_fn* e = doc->custom_xpath_fns;
    while (e) {
        struct leptris_custom_xpath_fn* next = e->next;
        free(e->name);
        free(e->user_data);  /* the packed state */
        free(e);
        e = next;
    }
    doc->custom_xpath_fns = NULL;
}

/* ============================================================================
 * Batch-context evaluation (issue #560)
 * ========================================================================= */

/* Shared engine: evaluate `compiled' with each context as the
 * context node, merge every nodeset result into one, then sort into
 * document order — which also compacts duplicates (a node matching
 * from several contexts is kept once; the rank table's stable
 * partition groups identical nodes). Non-nodeset results
 * (booleans/numbers/strings) contribute nothing to the union. */
LEPTRIS_API LeptrisXPathResult leptris_xpath_compiled_eval_nodeset(
        LeptrisDocument doc, LeptrisElement* contexts, size_t count,
        LeptrisXPathCompiled compiled) {
    if (!doc || !contexts || count == 0 || !compiled) return NULL;

    XPathNodeSet* merged = xpath_nodeset_new();
    if (!merged) return NULL;

    for (size_t i = 0; i < count; i++) {
        if (!contexts[i]) continue;
        struct leptris_xpath_result* r =
            leptris_xpath_compiled_eval(compiled, doc, contexts[i]);
        if (!r) continue;
        if (r->type == LEPTRIS_XPATH_NODESET && r->value.nodeset_value) {
            XPathNodeSet* ns = r->value.nodeset_value;
            for (size_t j = 0; j < ns->count; j++)
                xpath_nodeset_add(merged, ns->nodes[j]);
            /* The entries themselves are either real DOM nodes
             * (document-owned) or synthetic attrs the RESULT owned —
             * transfer ownership markers to the merged set before
             * the source frees. */
            if (ns->owns_attributes) merged->owns_attributes = 1;
            if (ns->owns_namespaces) merged->owns_namespaces = 1;
            ns->owns_attributes = 0;
            ns->owns_namespaces = 0;
        }
        leptris_xpath_result_free(r);
    }

    /* Document order + dedup via the shared rank-table sort. */
    XPathContext ctx_storage;
    XPathContext* ctx = &ctx_storage;
    xpath_context_init(ctx, doc, contexts[0]);
    if (merged->count > 1)
        xpath_nodeset_sort_doc_order(ctx, merged, 0);
    xpath_context_cleanup(ctx);

    struct leptris_xpath_result* out =
        xpath_result_new(XPATH_RESULT_NODESET);
    if (!out) { xpath_nodeset_free(merged); return NULL; }
    out->value.nodeset_value = merged;
    return out;
}

LEPTRIS_API LeptrisXPathResult leptris_xpath_eval_nodeset(
        LeptrisDocument doc, LeptrisElement* contexts, size_t count,
        const char* expression) {
    if (!doc || !contexts || count == 0 || !expression) return NULL;
    LeptrisXPathCompiled compiled = leptris_xpath_compile(expression);
    if (!compiled) return NULL;
    struct leptris_xpath_result* r = leptris_xpath_compiled_eval_nodeset(
        doc, contexts, count, compiled);
    leptris_xpath_compiled_free(compiled);
    return r;
}
