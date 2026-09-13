/* evaluator.c - XPath evaluator implementation
 * Copyright (c) 2024, Ribose Inc.
 *
 * Pure C implementation of XPath 1.0 evaluator.
 * Converted from Ruby C extension to pure C.
 */

#include "evaluator.h"
#include "evaluator_internal.h"
#include "functions.h"
#include "xpath_variables.h"
#include "../dom/element.h"  /* For LeptrisElement structure */
#include "../common/port.h"  /* LEPTRIS_THREAD_LOCAL */
#include "lexer.h"
#include "parser.h"
#include "../include/leptris.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <ctype.h>

/* Debug logging - Set to 0 to disable */
#define XPATH_DEBUG 0

#if XPATH_DEBUG
#define DEBUG_LOG(fmt, ...) fprintf(stderr, "[XPath DEBUG] " fmt "\n", ##__VA_ARGS__)
#else
#define DEBUG_LOG(fmt, ...) do {} while(0)
#endif

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

/* The engine's extension-function compatibility contract. libxslt
 * resolves extension calls by NAMESPACE URI + local name, so a
 * stylesheet may bind any of these under ANY prefix (xmlns:z=EXSLT
 * common then z:node-set() must work). The registry keys handlers
 * under canonical prefixes (exslt:node-set); function resolution
 * falls back to the local name when the prefix resolves to one of
 * THESE namespaces — and only these, so a user my:count never
 * silently hits the built-in count(). Adding extension support =
 * adding the URI here plus the handler registration. */
static const char* const k_extension_ns_uris[] = {
    "http://www.w3.org/2005/xpath-functions",  /* fn:* (XQuery
     * pre-binds the default function namespace; QT3 writes
     * fn:substring everywhere) */
    "http://www.w3.org/2005/xpath-functions/math", /* math:* */
    "http://www.w3.org/2001/XMLSchema",   /* xs:* constructors (06) */
    "http://www.w3.org/2005/xpath-functions/map",   /* map:* (08) */
    "http://www.w3.org/2005/xpath-functions/array", /* array:* (08) */
    "http://exslt.org/common",
    "http://exslt.org/sets",
    "http://exslt.org/strings",
    "http://exslt.org/math",
    "http://exslt.org/date",
    "http://exslt.org/regexp",
    "http://exslt.org/dynamic",
    "http://www.w3.org/1999/XSL/Transform",
    /* libxslt's OWN extension namespace: stylesheets bind it under
     * any prefix and call libxslt:node-set() (bug-65). */
    "http://xmlsoft.org/XSLT/namespace",
    NULL };

/* Namespace support functions (v0.8.0) */
static void xpath_context_register_namespace(XPathContext* context,
                                             const char* prefix,
                                             const char* uri);
static void xpath_context_init_from_document(XPathContext* context);

/* Function call evaluation */
static struct leptris_xpath_result* evaluate_function_call_impl(XPathContext* ctx,
                                                                XPathASTNode* ast);

/* Public alias for the VM (TODO 120 Phase F). The VM dispatches
 * BC_FUNC_CALL by calling this directly, skipping the evaluate_expr
 * AST-type switch. */
struct leptris_xpath_result* evaluate_function_call_inline(XPathContext* ctx,
                                                           XPathASTNode* ast) {
    return evaluate_function_call_impl(ctx, ast);
}

/* ============================================================================
 * Context Management
 * ============================================================================ */

XPathContext* xpath_context_new(struct leptris_document* document,
                                LeptrisElement context_node) {
    if (!document || !context_node) return NULL;

    XPathContext* context = LEPTRIS_ALLOC(XPathContext);
    if (!context) return NULL;

    xpath_context_init(context, document, context_node);
    return context;
}

/* Stack-allocatable init/cleanup pair (TODO 163). Lets callers
 * that hold the context for the duration of one eval (the common
 * case — both leptris_xpath_eval and leptris_xpath_eval_with_vars)
 * skip the malloc/free pair. */
void xpath_context_init(XPathContext* context,
                        struct leptris_document* document,
                        LeptrisElement context_node) {
    if (!context || !document || !context_node) return;

    context->document = document;
    context->context_node = context_node;
    context->context_position = 1;
    context->context_size = 1;
    context->error_msg[0] = '\0';
    context->current_predicate_node = NULL;
    context->to_boolean = 0;
    context->max_results = 0;
    context->enable_early_exit = 1;

    context->namespace_mappings = NULL;
    context->namespace_count = 0;
    context->namespace_capacity = 0;
    context->namespaces_collected = 0;

    context->variable_set = NULL;
    context->ns_set = NULL;

    context->input = NULL;
    context->input_len = 0;

    extern XPathFunctionRegistry* leptris_xpath_build_custom_registry(struct leptris_document*);
    context->function_registry = leptris_xpath_build_custom_registry(document);
    if (!context->function_registry) {
        context->function_registry = xpath_function_registry_get_standard();
    } else if (context->function_registry == document->cached_fn_registry) {
        /* The document's cached registry — shared across every eval
         * on this document; the DOC owns it. */
        context->registry_borrowed = 1;
    }

    context->current_fn_user_data = NULL;
    context->error_code[0] = '\0';
    context->owned_docs = NULL;
    context->n_owned_docs = 0;
    context->cap_owned_docs = 0;
}

void xpath_context_free(XPathContext* context) {
    if (!context) return;
    xpath_context_cleanup(context);
    LEPTRIS_FREE(context);
}

/* Release owned resources without freeing the struct (TODO 163).
 * Use after xpath_context_init when the storage is caller-owned
 * (typically stack-allocated). */
void xpath_context_cleanup(XPathContext* context) {
    if (!context) return;

    /* Free namespace mappings (v0.8.0) */
    if (context->namespace_mappings) {
        for (size_t i = 0; i < context->namespace_count; i++) {
            LEPTRIS_FREE(context->namespace_mappings[i].prefix);
            LEPTRIS_FREE(context->namespace_mappings[i].uri);
        }
        LEPTRIS_FREE(context->namespace_mappings);
    }

    /* Function registry: usually the shared singleton (TODO 113
     * perf) — do NOT free it. A per-context registry built for the
     * doc's custom XPath functions (TODO 148 Phase 5) is freed
     * here; a registry BORROWED from the document cache stays with
     * the document. */
    extern XPathFunctionRegistry* xpath_function_registry_get_standard(void);
    if (context->function_registry && !context->registry_borrowed &&
        context->function_registry != xpath_function_registry_get_standard()) {
        xpath_function_registry_free((XPathFunctionRegistry*)context->function_registry);
    }
    context->registry_borrowed = 0;

    /* fn:doc() anchors — the results borrowed their roots. */
    for (size_t i = 0; i < context->n_owned_docs; i++)
        if (context->owned_docs[i])
            leptris_document_free(context->owned_docs[i]);
    free(context->owned_docs);
    context->owned_docs = NULL;
    context->n_owned_docs = 0;
    context->cap_owned_docs = 0;
}

const char* xpath_context_error(XPathContext* context) {
    if (!context) return "Invalid context";
    return context->error_msg[0] ? context->error_msg : NULL;
}

/* ============================================================================
 * Namespace Support (v0.8.0)
 * ============================================================================ */

/**
 * Register a namespace prefix->URI mapping in the context
 *
 * @param context XPath context
 * @param prefix Namespace prefix (NULL for default namespace)
 * @param uri Namespace URI (required)
 */
void xpath_context_register_namespace(XPathContext* context,
                                      const char* prefix,
                                      const char* uri) {
    if (!context || !uri) return;

    /* Check if prefix already registered - update if found */
    for (size_t i = 0; i < context->namespace_count; i++) {
        int prefix_matches = 0;
        if (!prefix && !context->namespace_mappings[i].prefix) {
            prefix_matches = 1; /* Both NULL (default namespace) */
        } else if (prefix && context->namespace_mappings[i].prefix &&
                   strcmp(prefix, context->namespace_mappings[i].prefix) == 0) {
            prefix_matches = 1; /* Both non-NULL and equal */
        }

        if (prefix_matches) {
            /* Update existing mapping */
            LEPTRIS_FREE(context->namespace_mappings[i].uri);
            context->namespace_mappings[i].uri = leptris_strdup(uri);
            return;
        }
    }

    /* Add new mapping - grow array if needed */
    if (context->namespace_count >= context->namespace_capacity) {
        size_t new_capacity = context->namespace_capacity == 0 ?
            4 : context->namespace_capacity * 2;
        XPathNamespaceMapping* new_mappings = LEPTRIS_REALLOC_N(
            context->namespace_mappings,
            XPathNamespaceMapping,
            new_capacity
        );
        if (!new_mappings) return; /* Allocation failed */

        context->namespace_mappings = new_mappings;
        context->namespace_capacity = new_capacity;
    }

    /* Add new mapping */
    context->namespace_mappings[context->namespace_count].prefix =
        prefix ? leptris_strdup(prefix) : NULL;
    context->namespace_mappings[context->namespace_count].uri =
        leptris_strdup(uri);
    context->namespace_count++;
}

/**
 * Resolve namespace prefix to URI (OPTIMIZED with reverse lookup)
 *
 * Strategy: Search from END to START to find most recent registration first.
 * This handles override semantics naturally - child namespace declarations
 * override parent ones because they're registered later.
 *
 * Performance: O(n) worst case, but in practice very fast because:
 * - Most documents have <10 unique namespace prefixes
 * - Recent registrations (local scope) found first
 * - Common prefixes cached by compiler in registers
 *
 * @param context XPath context
 * @param prefix Namespace prefix to resolve (NULL for default namespace)
 * @return Namespace URI, or NULL if not found
 */
const char* xpath_context_resolve_prefix(XPathContext* context,
                                         const char* prefix) {
    if (!context) return NULL;

    /* Lazy namespace collection (TODO 125). Walk the document once,
     * on first prefix lookup, then cache. Expressions that never
     * resolve a prefix (the common case: self::*, count(//book),
     * descendant::*[@id], etc.) never pay this cost. */
    if (!context->namespaces_collected) {
        xpath_context_init_from_document(context);
        context->namespaces_collected = 1;
    }

    if (context->namespace_count == 0) return NULL;

    /* Search BACKWARDS for most recent (local) registration first
     * This implements namespace scope override semantics efficiently */
    for (size_t i = context->namespace_count; i > 0; i--) {
        size_t idx = i - 1;
        XPathNamespaceMapping* mapping = &context->namespace_mappings[idx];

        /* Fast path: Compare prefix pointers first (common case: same string object) */
        if (mapping->prefix == prefix) {
            return mapping->uri;
        }

        /* Both NULL = default namespace match */
        if (!prefix && !mapping->prefix) {
            return mapping->uri;
        }

        /* String comparison only if both non-NULL */
        if (prefix && mapping->prefix && strcmp(prefix, mapping->prefix) == 0) {
            return mapping->uri;
        }
    }

    return NULL; /* Prefix not found */
}

/* Helper: Collect namespaces from element and all descendants recursively */
static void collect_namespaces_recursive(XPathContext* context,
                                         LeptrisElement element) {
    if (!element) return;

    /* Register namespace from this element (inline in compact mode) */
    const char* ns_uri = leptris_element_get_namespace_uri(element);
    const char* ns_prefix = leptris_element_get_prefix(element);
    if (ns_uri) {
        xpath_context_register_namespace(context, ns_prefix, ns_uri);
    }

    /* Also check attributes for xmlns declarations */
    size_t attr_count = leptris_element_attribute_count(element);
    for (size_t i = 0; i < attr_count; i++) {
        /* Walk the attribute linked list */
        struct leptris_attribute* attr = leptris_element_get_first_attribute(element);
        for (size_t j = 0; j < i && attr; j++) {
            attr = leptris_attr_next(attr);
        }
        if (!attr) continue;

        /* Check for xmlns:prefix or xmlns using StringView directly
         * SAFETY: Do NOT modify attribute structures during namespace collection!
         * Modifying attr->name or attr->value here would cause memory leaks because:
         * 1. This code might use leptris_sv_to_cstr() (malloc) instead of pool allocation
         * 2. When the document is freed, the pool is freed (including attr structure)
         * 3. But the malloc-allocated strings would leak and corrupt the heap
         *
         * Solution: Use StringView operations directly without lazy conversion */
        LeptrisStringView attr_name_view = attr->name_view;

        /* Check for xmlns:prefix using StringView prefix match */
        if (attr_name_view.length >= 6 &&
            attr_name_view.data[0] == 'x' &&
            attr_name_view.data[1] == 'm' &&
            attr_name_view.data[2] == 'l' &&
            attr_name_view.data[3] == 'n' &&
            attr_name_view.data[4] == 's' &&
            attr_name_view.data[5] == ':') {
            /* Found xmlns:prefix declaration */
            LeptrisStringView prefix_view = {
                .data = attr_name_view.data + 6,
                .length = attr_name_view.length - 6
            };

            /* Convert prefix StringView to C string for namespace mapping
             * Note: This is stored in XPathContext and freed with context */
            char* prefix = leptris_sv_to_cstr(&prefix_view);
            if (!prefix) continue;

            /* Get value - convert attr value StringView to C string */
            LeptrisStringView uv = leptris_attr_value_sv(attr);
            char* uri = leptris_sv_to_cstr(&uv);
            if (!uri) {
                LEPTRIS_FREE(prefix);
                continue;
            }

            xpath_context_register_namespace(context, prefix, uri);
            LEPTRIS_FREE(prefix);  /* Context makes its own copy */
            LEPTRIS_FREE(uri);     /* Context makes its own copy */
        } else if (attr_name_view.length == 5 &&
                   attr_name_view.data[0] == 'x' &&
                   attr_name_view.data[1] == 'm' &&
                   attr_name_view.data[2] == 'l' &&
                   attr_name_view.data[3] == 'n' &&
                   attr_name_view.data[4] == 's') {
            /* Found xmlns (default namespace) declaration
             *
             * CRITICAL: In XPath 1.0, the default namespace declared in XML does NOT apply
             * to unprefixed element names in XPath expressions! Unprefixed names in XPath
             * are always in NO namespace, even if there's a default xmlns declaration.
             *
             * Therefore, we do NOT register this as a default namespace for XPath context.
             * The default namespace only applies to prefixed names in the XML document.
             *
             * Reference: XPath 1.0 Section 2.3 - "Unprefixed names are not in any namespace"
             * See: https://www.w3.org/TR/xpath/#node-tests
             *
             * For prefixed namespace declarations (xmlns:prefix="uri"), we register the mapping
             * so that prefixed names like prefix:name work in XPath expressions.
             * But for the default namespace, we intentionally do NOT register it here.
             *
             * This means that:
             *   <root xmlns="http://example.com">
             * with XPath "/root" will match the root element (because unprefixed names
             * are in NO namespace in XPath).
             * And XPath "ns1:root" would only match if there's xmlns:ns1="...".
             */
            DEBUG_LOG("  Skipping default namespace declaration (xmlns=...) - not used for unprefixed XPath names");
        }
    }

    /* Recursively collect from children - use compact accessor functions */
    LeptrisElement child_elem = leptris_element_get_first_child(element);
    while (child_elem) {
        collect_namespaces_recursive(context, child_elem);
        child_elem = leptris_element_get_next_sibling(child_elem);
    }
}

/**
 * Initialize XPath context from document
 * Collects all namespace declarations from the document
 */
void xpath_context_init_from_document(XPathContext* context) {
    if (!context || !context->document) return;

    LeptrisElement root = (LeptrisElement)context->document->new_dom_root;
    if (root) {
        collect_namespaces_recursive(context, root);
    }
}

/* ============================================================================
 * Result Management
 * ============================================================================ */

/* ============================================================================
 * NodeSet Management
 * ============================================================================ */

/* Thread-local free-list for XPathNodeSet structs (TODO 159 Phase B).
 * Each leptris_xpath_eval call allocates and frees 2–5 nodesets. The
 * inline_data small-buffer optimisation already eliminates the inner
 * array malloc for small results; this free-list eliminates the struct
 * malloc/free churn too. After warmup, zero heap ops per nodeset.
 *
 * Bounded at NODESET_FREE_LIST_CAP to avoid unbounded growth on
 * pathological queries. The free-list owns XPathNodeSet structs only;
 * spilled nodes arrays are freed before push. */
#define NODESET_FREE_LIST_CAP 64
/* TLS consolidation (#682 lever 2): one thread-local object for
 * all four free-list variables. Each separate __thread variable
 * costs its own tlv_get_addr thunk per access; members of a single
 * struct share one hoisted address per function. */
static LEPTRIS_THREAD_LOCAL struct {
    XPathNodeSet* nodeset_head;
    size_t nodeset_count;
    struct leptris_xpath_result* result_head;
    size_t result_count;
} g_xpath_tls;


/* Thread-local free-list for leptris_xpath_result structs (TODO 162).
 * One result per leptris_xpath_eval call. Pattern matches the nodeset
 * free-list. The struct's value union is reused as the next-pointer
 * slot while on the free-list (smaller than adding a real next field
 * to the struct). */
#define XPATH_RESULT_FREE_LIST_CAP 32

XPathNodeSet* xpath_nodeset_new(void) {
    return xpath_nodeset_new_with_capacity(XPATH_NODESET_INLINE_CAPACITY);
}

/* TODO.concurrency/08: per-thread free lists have no portable TLS
 * destructor in C99 — threads that touched XPath and then exited
 * keep their parked structs forever. leptris_thread_cleanup() calls
 * this from each worker thread before it exits. */
void leptris_xpath_drain_thread_caches(void) {
    while (g_xpath_tls.nodeset_head) {
        XPathNodeSet* next = (XPathNodeSet*)g_xpath_tls.nodeset_head->inline_data[0];
        LEPTRIS_FREE(g_xpath_tls.nodeset_head);
        g_xpath_tls.nodeset_head = next;
    }
    g_xpath_tls.nodeset_count = 0;
    while (g_xpath_tls.result_head) {
        struct leptris_xpath_result* next = (struct leptris_xpath_result*)
            g_xpath_tls.result_head->value.nodeset_value;
        LEPTRIS_FREE(g_xpath_tls.result_head);
        g_xpath_tls.result_head = next;
    }
    g_xpath_tls.result_count = 0;
}

XPathNodeSet* xpath_nodeset_new_with_capacity(size_t capacity) {
    XPathNodeSet* nodeset = NULL;

    /* Fast path: pop from thread-local free-list. The cached structs
     * have count=0 and inline_data already zeroed, so the only setup
     * work is the spill-array allocation when capacity > inline.
     * The next-pointer is stashed in inline_data[0] (a void* slot
     * that is unused while the struct is on the free-list). */
    if (g_xpath_tls.nodeset_head) {
        nodeset = g_xpath_tls.nodeset_head;
        g_xpath_tls.nodeset_head = (XPathNodeSet*)nodeset->inline_data[0];
        g_xpath_tls.nodeset_count--;
        nodeset->inline_data[0] = NULL;
    } else {
        nodeset = LEPTRIS_ALLOC(XPathNodeSet);
        if (!nodeset) return NULL;
        memset(nodeset->inline_data, 0, sizeof(nodeset->inline_data));
    }

    nodeset->count = 0;
    nodeset->owns_attributes = 0;
    nodeset->owns_synthetic_text = 0;
    nodeset->owns_namespaces = 0;
    nodeset->is_sequence = 0;

    /* TODO 113 Phase 2: small-buffer optimization. For capacity ≤ the
     * inline buffer size, point `nodes` at the inline array. This
     * avoids the second heap allocation in the common case where the
     * query result is small (most queries return ≤16 nodes). */
    if (capacity <= XPATH_NODESET_INLINE_CAPACITY) {
        nodeset->nodes = nodeset->inline_data;
        nodeset->capacity = XPATH_NODESET_INLINE_CAPACITY;
    } else if (capacity > 0) {
        nodeset->nodes = LEPTRIS_ALLOC_N(void*, capacity);
        if (!nodeset->nodes) {
            /* On failure, push back to free-list instead of freeing. */
            nodeset->inline_data[0] = (void*)g_xpath_tls.nodeset_head;
            g_xpath_tls.nodeset_head = nodeset;
            g_xpath_tls.nodeset_count++;
            return NULL;
        }
        memset(nodeset->nodes, 0, sizeof(void*) * capacity);
        nodeset->capacity = capacity;
    } else {
        nodeset->nodes = NULL;
        nodeset->capacity = 0;
    }

    return nodeset;
}

/* Dispose ONE heap-owned synthetic node (attribute/namespace/text).
 * Shared by xpath_nodeset_free and the in-place predicate filter,
 * which drops entries without removing the nodeset's ownership
 * (bug: filtered-out namespace nodes leaked — Linux LSan). */
void xpath_nodeset_dispose_node(void* node) {
    if (!node) return;
    if (XPATH_NODE_TYPE(node) == LEPTRIS_NODE_ATTRIBUTE) {
        LeptrisAttributeNode* attr = (LeptrisAttributeNode*)node;
        if (attr->name) LEPTRIS_FREE(attr->name);
        if (attr->value) LEPTRIS_FREE(attr->value);
        if (attr->namespace_uri) LEPTRIS_FREE(attr->namespace_uri);
        LEPTRIS_FREE(attr);
    } else if (XPATH_NODE_TYPE(node) == LEPTRIS_NODE_NAMESPACE) {
        LeptrisNamespaceNode* ns = (LeptrisNamespaceNode*)node;
        if (ns->prefix) LEPTRIS_FREE(ns->prefix);
        if (ns->uri) LEPTRIS_FREE(ns->uri);
        LEPTRIS_FREE(ns);
    } else if (XPATH_NODE_TYPE(node) == LEPTRIS_NODE_TEXT) {
        XPathTextNode* t = (XPathTextNode*)node;
        if (t->content) LEPTRIS_FREE(t->content);
        LEPTRIS_FREE(t);
    }
}

void xpath_nodeset_free(XPathNodeSet* nodeset) {
    if (!nodeset) return;

    /* ONE pass over the nodes, dispatching on kind + ownership —
     * sequential per-flag loops re-read the TYPE of nodes an earlier
     * loop already freed (#720: a sequence holding an owned attribute
     * next to owned synthetic text). */
    if ((nodeset->owns_attributes || nodeset->owns_namespaces ||
         nodeset->owns_synthetic_text) && nodeset->nodes) {
        for (size_t i = 0; i < nodeset->count; i++) {
            void* node = nodeset->nodes[i];
            if (!node) continue;
            int ty = XPATH_NODE_TYPE(node);
            if (ty == LEPTRIS_NODE_ATTRIBUTE &&
                nodeset->owns_attributes) {
                xpath_nodeset_dispose_node(node);
            } else if (ty == LEPTRIS_NODE_NAMESPACE &&
                       nodeset->owns_namespaces) {
                xpath_nodeset_dispose_node(node);
            } else if (ty == LEPTRIS_NODE_TEXT &&
                       nodeset->owns_synthetic_text) {
                xpath_nodeset_dispose_node(node);
            }
        }
    }

    /* Free the spill array unless it's the inline buffer (TODO 113). */
    if (nodeset->nodes && nodeset->nodes != nodeset->inline_data) {
        LEPTRIS_FREE(nodeset->nodes);
    }

    /* Push struct onto thread-local free-list (TODO 159 Phase B).
     * Cap prevents unbounded growth. inline_data[0] is reused as the
     * next-pointer for the singly-linked free-list; it is reset by
     * xpath_nodeset_new_with_capacity on pop. */
    if (g_xpath_tls.nodeset_count < NODESET_FREE_LIST_CAP) {
        nodeset->count = 0;
        nodeset->capacity = 0;
        nodeset->owns_attributes = 0;
        nodeset->owns_namespaces = 0;
        nodeset->is_sequence = 0;
        nodeset->nodes = NULL;
        nodeset->inline_data[0] = (void*)g_xpath_tls.nodeset_head;
        g_xpath_tls.nodeset_head = nodeset;
        g_xpath_tls.nodeset_count++;
    } else {
        LEPTRIS_FREE(nodeset);
    }
}

size_t xpath_nodeset_count(XPathNodeSet* nodeset) {
    return nodeset ? nodeset->count : 0;
}

void* xpath_nodeset_get(XPathNodeSet* nodeset, size_t index) {
    if (!nodeset || index >= nodeset->count) return NULL;
    return nodeset->nodes[index];
}

void xpath_nodeset_add(XPathNodeSet* nodeset, void* node) {
    if (!nodeset || !node) return;

    /* SAFETY: Validate node pointer before adding
     * Stale pointers from previous operations can cause crashes when reallocating */
    if ((uintptr_t)node < 0x1000) {
        /* Clearly invalid pointer - skip */
        return;
    }

    /* SAFETY: Validate nodeset structure before reallocation
     * Corruption in count/capacity can cause heap corruption during realloc */
    if (nodeset->count > nodeset->capacity || nodeset->capacity > 1000000) {
        /* Corrupted structure - skip this addition */
        return;
    }

    /* Grow array if needed */
    if (nodeset->count >= nodeset->capacity) {
        size_t new_capacity = nodeset->capacity == 0 ? 4 : nodeset->capacity * 2;

        /* SAFETY: Check for overflow */
        if (new_capacity > 1000000) {
            return;
        }

        /* TODO 113 Phase 2: if currently using inline_data, switch
         * to a heap allocation and copy. REALLOC can't be used on
         * the inline buffer (it's part of the struct). */
        if (nodeset->nodes == nodeset->inline_data) {
            void** new_nodes = LEPTRIS_ALLOC_N(void*, new_capacity);
            if (!new_nodes) return;
            memcpy(new_nodes, nodeset->inline_data,
                   sizeof(void*) * nodeset->count);
            nodeset->nodes = new_nodes;
            nodeset->capacity = new_capacity;
        } else {
            /* SAFETY: Validate nodes pointer before realloc */
            if (nodeset->nodes && (uintptr_t)nodeset->nodes < 0x1000) {
                return;
            }

            void** new_nodes = LEPTRIS_REALLOC_N(nodeset->nodes, void*, new_capacity);
            if (!new_nodes) return;
            nodeset->nodes = new_nodes;
            nodeset->capacity = new_capacity;
        }
    }

    nodeset->nodes[nodeset->count++] = node;
}

/* Fast inline add (TODO 135). Skips the safety checks that
 * xpath_nodeset_add does. Caller MUST guarantee:
 *   - nodeset is non-NULL and well-formed (count <= capacity)
 *   - node is a valid pointer (not stale, not < 0x1000)
 *   - the element being added is unique within the nodeset (or
 *     the caller is OK with dups; e.g., descendant axis from a
 *     single root can't produce duplicates by tree structure)
 *
 * Used by VM hot paths: vm_apply_axis_descendant, vm_apply_absolute,
 * and the fused axis+predicate handlers. Each call goes from ~30 ns
 * (with checks) to ~5 ns (without).
 *
 * NOT exported — internal use only. Public callers go through
 * xpath_nodeset_add. */
void xpath_nodeset_add_fast(XPathNodeSet* nodeset, void* node) {
    if (nodeset->count >= nodeset->capacity) {
        size_t new_capacity = nodeset->capacity ? nodeset->capacity * 2 : 16;
        if (nodeset->nodes == nodeset->inline_data) {
            void** new_nodes = LEPTRIS_ALLOC_N(void*, new_capacity);
            if (!new_nodes) return;
            memcpy(new_nodes, nodeset->inline_data,
                   sizeof(void*) * nodeset->count);
            nodeset->nodes = new_nodes;
            nodeset->capacity = new_capacity;
        } else {
            void** new_nodes = LEPTRIS_REALLOC_N(nodeset->nodes, void*, new_capacity);
            if (!new_nodes) return;
            nodeset->nodes = new_nodes;
            nodeset->capacity = new_capacity;
        }
    }
    nodeset->nodes[nodeset->count++] = node;
}

struct leptris_xpath_result* xpath_result_new(XPathResultType type) {
    struct leptris_xpath_result* result = NULL;

    /* Fast path: pop from thread-local free-list (TODO 162). The
     * value union is reused as the next-pointer slot while the
     * struct is on the free-list — its lifetime is finished and
     * no live value occupies the union. */
    if (g_xpath_tls.result_head) {
        result = g_xpath_tls.result_head;
        g_xpath_tls.result_head = (struct leptris_xpath_result*)
            result->value.nodeset_value;  /* next pointer */
        g_xpath_tls.result_count--;
    } else {
        result = LEPTRIS_ALLOC(struct leptris_xpath_result);
        if (!result) return NULL;
    }

    result->type = type;
    result->is_int = 0;   /* free-list recycling would leak it */

    /* Initialize union based on type. XPATH_RESULT_CACHED is the
     * free-list sentinel — a fresh result never carries it. */
    switch (type) {
        case XPATH_RESULT_BOOLEAN:
            result->value.boolean_value = 0;
            break;
        case XPATH_RESULT_NUMBER:
            result->value.number_value = 0.0;
            break;
        case XPATH_RESULT_STRING:
            result->value.string_value = NULL;
            break;
        case XPATH_RESULT_NODESET:
            result->value.nodeset_value = NULL;
            break;
        case XPATH_RESULT_CACHED:
            break;
    }

    return result;
}

void xpath_result_free(struct leptris_xpath_result* result) {
    if (!result) return;
    /* Already parked on the free-list: freeing again must be a
     * no-op, never a second payload release. */
    if (result->type == XPATH_RESULT_CACHED) return;

    /* Release the payload through locals: after this block the
     * union slot is dead, and parking the free-list next-pointer
     * into it cannot alias the memory just released. */
    switch (result->type) {
        case XPATH_RESULT_STRING: {
            char* sv = result->value.string_value;
            if (sv) LEPTRIS_FREE(sv);
            break;
        }
        case XPATH_RESULT_NODESET: {
            XPathNodeSet* nv = result->value.nodeset_value;
            if (nv) xpath_nodeset_free(nv);
            break;
        }
        case XPATH_RESULT_NUMBER:
        case XPATH_RESULT_BOOLEAN:
            /* No heap allocation to free */
            break;
        default:
            break;
    }

    /* Push onto thread-local free-list (TODO 162). Cap prevents
     * unbounded growth. The CACHED sentinel parked in type makes a
     * repeat free a no-op (double-free spec). */
    if (g_xpath_tls.result_count < XPATH_RESULT_FREE_LIST_CAP) {
        struct leptris_xpath_result* next = g_xpath_tls.result_head;
        result->type = XPATH_RESULT_CACHED;
        result->value.nodeset_value = (XPathNodeSet*)next;
        g_xpath_tls.result_head = result;
        g_xpath_tls.result_count++;
    } else {
        LEPTRIS_FREE(result);
    }
}

/* ============================================================================
 * Expression Evaluation
 * ============================================================================ */

/* Forward declarations for evaluation functions */
extern struct leptris_xpath_result* evaluate_location_path(XPathContext* ctx,
                                                           XPathASTNode* path);
extern struct leptris_xpath_result* evaluate_operator(XPathContext* ctx,
                                                     XPathASTNode* ast);

/**
 * Evaluate function call AST node
 */
static struct leptris_xpath_result* evaluate_function_call_impl(XPathContext* ctx,
                                                          XPathASTNode* ast) {
    if (!ctx || !ast || ast->type != XPATH_AST_FUNCTION_CALL) {
        if (ctx) {
            snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                    "Invalid function call node");
        }
        return NULL;
    }

    const char* func_name = ast->value;
    if (!func_name) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                "Function call missing name");
        return NULL;
    }

    /* Get function registry */
    if (!ctx->function_registry) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                "Function registry not initialized");
        return NULL;
    }

    XPathFunctionRegistry* registry = (XPathFunctionRegistry*)ctx->function_registry;

    /* Look up function. Extension functions register under their
     * canonical prefix (exslt:node-set); a stylesheet may bind the
     * same namespace under ANY prefix. The local-name fallback runs
     * ONLY when the prefix resolves to a known extension namespace
     * (EXSLT/XSLT) — otherwise a user my:count would silently hit
     * the built-in count(). */
    XPathFunctionDef* func_def =
        xpath_function_registry_get(registry, func_name);
    if (!func_def) {
        const char* colon = strchr(func_name, ':');
        if (colon && colon[1]) {
            const char* uri = leptris_xpath_ns_lookup(
                (const struct leptris_xpath_ns_map*)ctx->ns_set,
                func_name, (size_t)(colon - func_name));
            /* Pre-bound prefixes (XQuery 1.0/3.1 prolog reserves
             * these with no declaration needed): fn, xs, math,
             * map, array, err. Only consulted when the prefix has
             * NO user declaration. */
            if (!uri) {
                static const struct { const char* pfx; const char* u; }
                    k_prebound[] = {
                        { "fn", "http://www.w3.org/2005/xpath-functions" },
                        { "xs", "http://www.w3.org/2001/XMLSchema" },
                        { "math",
                          "http://www.w3.org/2005/xpath-functions/math" },
                        { "map",
                          "http://www.w3.org/2005/xpath-functions/map" },
                        { "array",
                          "http://www.w3.org/2005/xpath-functions/array" },
                        { "err", "http://www.w3.org/2005/xqt-errors" },
                        { NULL, NULL }
                    };
                size_t pl = (size_t)(colon - func_name);
                for (int i = 0; k_prebound[i].pfx; i++)
                    if (strlen(k_prebound[i].pfx) == pl &&
                        strncmp(func_name, k_prebound[i].pfx, pl) == 0) {
                        uri = k_prebound[i].u;
                        break;
                    }
            }
            int ext = 0;
            if (uri) {
                for (int i = 0; k_extension_ns_uris[i]; i++)
                    if (strcmp(uri, k_extension_ns_uris[i]) == 0) {
                        ext = 1;
                        break;
                    }
            }
            if (ext) {
                char local[128];
                snprintf(local, sizeof(local), "%s", colon + 1);
                func_def =
                    xpath_function_registry_get(registry, local);
            }
        }
    }
    if (!func_def) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                "Unknown function: %s", func_name);
        return NULL;
    }

    /* Check argument count */
    size_t arg_count = ast->child_count;
    if ((int)arg_count < func_def->min_args ||
        (func_def->max_args >= 0 && (int)arg_count > func_def->max_args)) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                "Function %s expects %d-%d arguments, got %zu",
                func_name, func_def->min_args, func_def->max_args, arg_count);
        return NULL;
    }

    /* Call function handler. TODO 148 Phase 5: save/restore the
     * per-call user_data slot so the handler can read it via the
     * accessor — supports recursion (nested function calls). */
    void* saved_user_data = ctx->current_fn_user_data;
    ctx->current_fn_user_data = func_def->user_data;
    struct leptris_xpath_result* r = func_def->handler(ctx, ast->children, arg_count);
    ctx->current_fn_user_data = saved_user_data;
    return r;
}

/**
 * Evaluate literal AST node (string or number)
 */
static struct leptris_xpath_result* evaluate_literal(XPathContext* ctx,
                                                    XPathASTNode* ast) {
    if (!ctx || !ast) return NULL;

    switch (ast->type) {
        case XPATH_AST_NUMBER: {
            struct leptris_xpath_result* result = xpath_result_new(XPATH_RESULT_NUMBER);
            if (!result) return NULL;
            result->value.number_value = ast->number_value;
            return result;
        }

        case XPATH_AST_STRING: {
            struct leptris_xpath_result* result = xpath_result_new(XPATH_RESULT_STRING);
            if (!result) return NULL;
            result->value.string_value = ast->value ? leptris_strdup(ast->value) : leptris_strdup("");
            return result;
        }

        default:
            snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                    "Not a literal node");
            return NULL;
    }
}

/**
 * Internal expression evaluator
 * Called by various evaluation functions
 */
struct leptris_xpath_result* evaluate_expr(XPathContext* ctx, XPathASTNode* ast) {
    if (!ctx || !ast) return NULL;

    switch (ast->type) {
        case XPATH_AST_PREDICATE: {
            /* Filter expression: <primary>[pred][pred]... — the
             * parser builds PREDICATE nodes when the primary is not
             * a plain name-test step (node()[1], (@a)[2], (a|b)[1]).
             * Evaluate the primary, then filter in place. */
            if (ast->child_count < 1) return NULL;
            struct leptris_xpath_result* base =
                evaluate_expr(ctx, ast->children[0]);
            if (!base) return NULL;
            if (base->type != XPATH_RESULT_NODESET ||
                !base->value.nodeset_value) {
                xpath_result_free(base);
                return xpath_result_new(XPATH_RESULT_NODESET);
            }
            if (ast->child_count > 1) {
                apply_predicates(ctx, base->value.nodeset_value,
                                 &ast->children[1],
                                 ast->child_count - 1);
            }
            return base;
        }
        case XPATH_AST_STEP:
            /* Bare step (e.g., @id, ., ..) - evaluate as relative path from context node */
            {
                XPathNodeSet* current = xpath_nodeset_new();
                if (!current) return NULL;

                /* Start from context node */
                xpath_nodeset_add(current, ctx->context_node);

                /* Evaluate the step */
                struct leptris_xpath_result* step_result = evaluate_step(ctx, ast, current);
                xpath_nodeset_free(current);

                return step_result;
            }

        case XPATH_AST_PATH_EXPR:
        case XPATH_AST_ABSOLUTE_PATH:
        case XPATH_AST_RELATIVE_PATH:
            return evaluate_location_path(ctx, ast);

        case XPATH_AST_OPERATOR:
            return evaluate_operator(ctx, ast);

        case XPATH_AST_FUNCTION_CALL:
            return evaluate_function_call_impl(ctx, ast);

        case XPATH_AST_VARIABLE_REFERENCE: {
            /* Look up variable in context */
            if (!ctx->variable_set) {
                snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                        "Variable '%s' not found (no variable set provided)", ast->value);
                return NULL;
            }

            /* Get variable from variable set */
            const XPathVariable* var = xpath_variable_set_get_const(
                (XPathVariableSet*)ctx->variable_set, ast->value);

            if (!var) {
                snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                        "Undefined variable: %s", ast->value);
                return NULL;
            }

            /* Create result based on variable type */
            XPathVariableType var_type = xpath_variable_type(var);

            switch (var_type) {
                case XPATH_VAR_TYPE_BOOLEAN: {
                    struct leptris_xpath_result* result = xpath_result_new(XPATH_RESULT_BOOLEAN);
                    if (!result) return NULL;
                    result->value.boolean_value = xpath_variable_get_boolean(var);
                    return result;
                }
                case XPATH_VAR_TYPE_NUMBER: {
                    struct leptris_xpath_result* result = xpath_result_new(XPATH_RESULT_NUMBER);
                    if (!result) return NULL;
                    result->value.number_value = xpath_variable_get_number(var);
                    return result;
                }
                case XPATH_VAR_TYPE_STRING: {
                    struct leptris_xpath_result* result = xpath_result_new(XPATH_RESULT_STRING);
                    if (!result) return NULL;
                    const char* str_val = xpath_variable_get_string(var);
                    if (str_val) {
                        result->value.string_value = leptris_strdup(str_val);
                    } else {
                        result->value.string_value = leptris_strdup("");
                    }
                    return result;
                }
                case XPATH_VAR_TYPE_NODE_SET: {
                    struct leptris_xpath_result* result = xpath_result_new(XPATH_RESULT_NODESET);
                    if (!result) return NULL;
                    /* Reference the variable's nodeset by copying the
                     * node pointer array. The nodes themselves are
                     * document-owned (LeptrisElement pointers) so they
                     * remain valid for the document's lifetime; the
                     * result owns the new array. Variable storage must
                     * outlive the result (XPath spec guarantees this
                     * within a single eval call). */
                    XPathNodeSet* var_ns = xpath_variable_get_nodeset(var);
                    if (var_ns && var_ns->count > 0) {
                        /* Synthetic members must be deep-copied: the
                         * variable's storage can be freed while the
                         * result lives (let bindings unwind). */
                        if (var_ns->owns_synthetic_text) {
                            XPathNodeSet* copy =
                                xpath_nodeset_deep_copy(var_ns);
                            if (!copy) return NULL;
                            result->value.nodeset_value = copy;
                            return result;
                        }
                        result->value.nodeset_value = xpath_nodeset_new_with_capacity(var_ns->count);
                        if (!result->value.nodeset_value) return NULL;
                        for (size_t i = 0; i < var_ns->count; i++) {
                            xpath_nodeset_add(result->value.nodeset_value, var_ns->nodes[i]);
                        }
                    } else {
                        result->value.nodeset_value = xpath_nodeset_new();
                    }
                    return result;
                }
                default:
                    snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                            "Unsupported variable type for: %s", ast->value);
                    return NULL;
            }
        }

        case XPATH_AST_NUMBER:
        case XPATH_AST_STRING:
            return evaluate_literal(ctx, ast);

        default:
            snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                    "Unsupported AST node type: %d", ast->type);
            return NULL;
    }
}

/**
 * Main XPath evaluation entry point
 */
struct leptris_xpath_result* xpath_evaluate(XPathContext* context, XPathASTNode* ast) {
    if (!context) return NULL;
    if (!ast) {
        snprintf(context->error_msg, sizeof(context->error_msg),
                "NULL AST node");
        return NULL;
    }

    return evaluate_expr(context, ast);
}
