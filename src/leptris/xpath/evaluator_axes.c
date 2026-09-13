/* evaluator_axes.c - XPath axis implementations
 * Copyright (c) 2024, Ribose Inc.
 *
 * All 13 XPath 1.0 axes per specification
 */

#include "evaluator_internal.h"
#include "../dom/document_node.h"
#include "../leptris_internal.h"
#include "../dom/element.h"  /* For LeptrisElement structure */
#include "../common/entities.h"
#include "../memory/pool.h"
#include <string.h>
#include <stdio.h>

/* Debug logging - Set to 0 to disable */
#define XPATH_DEBUG 0

#if XPATH_DEBUG
#define DEBUG_LOG(fmt, ...) fprintf(stderr, "[XPath DEBUG] " fmt "\n", ##__VA_ARGS__)
#else
#define DEBUG_LOG(fmt, ...) do {} while(0)
#endif

/* ============================================================================
 * Axis Implementations (All 13 XPath Axes)
 * ============================================================================ */

/* Helper: Collect descendants recursively (TODO 109: walks all node
 * types, not just elements). */
static void collect_descendants(XPathContext* ctx,
                               LeptrisElement node,
                               XPathNodeSet* result,
                               XPathASTNode* node_test) {
    if (!node) return;

    /* Walk the raw child list — any node type (text, comment, cdata, pi). */
    LeptrisNode* child = leptris_elem_first_child(node);
    while (child) {
        if (matches_node_test(ctx, child, node_test)) {
            xpath_nodeset_add(result, child);
        }
        /* Recurse into element children only (other nodes have no children). */
        if (child->type == LEPTRIS_NODE_TYPE_ELEMENT) {
            collect_descendants(ctx, (LeptrisElement)child, result, node_test);
        }
        child = leptris_node_get_next_sibling(child);
    }
}

/* Helper: Collect descendants or self (TODO 109: walks all node types). */
static void collect_descendants_or_self(XPathContext* ctx,
                                       LeptrisElement node,
                                       XPathNodeSet* result,
                                       XPathASTNode* node_test) {
    if (!node) return;

    if (leptris_node_get_type((LeptrisNodeRef)node) ==
        LEPTRIS_NODE_TYPE_DOCUMENT) {
        struct leptris_document* d =
            ((LeptrisDocumentNode*)node)->doc;
        LeptrisElement root = (LeptrisElement)d->new_dom_root;
        if (!root) root = d->root;
        collect_descendants_or_self(ctx, root, result, node_test);
        return;
    }

    if (matches_node_test(ctx, (LeptrisNode*)node, node_test)) {
        xpath_nodeset_add(result, node);
    }

    /* Walk the raw child list — any node type. */
    LeptrisNode* child = leptris_elem_first_child(node);
    while (child) {
        if (child->type == LEPTRIS_NODE_TYPE_ELEMENT) {
            collect_descendants_or_self(ctx, (LeptrisElement)child, result, node_test);
        } else {
            /* Non-element child: test it directly (no recursion). */
            if (matches_node_test(ctx, child, node_test)) {
                xpath_nodeset_add(result, child);
            }
        }
        child = leptris_node_get_next_sibling(child);
    }
}

/* Helper: Create attribute node from leptris_attribute */
static LeptrisAttributeNode* create_attribute_node(struct leptris_attribute* attr,
                                                   LeptrisElement owner) {
    if (!attr) return NULL;

    LeptrisAttributeNode* attr_node = LEPTRIS_ALLOC(LeptrisAttributeNode);
    if (!attr_node) return NULL;

    attr_node->node_type = LEPTRIS_NODE_ATTRIBUTE;

    /* Handle name - copy from the single-representation view */
    if (!leptris_sv_is_empty(&attr->name_view)) {
        size_t len = attr->name_view.length;
        char* name_copy = LEPTRIS_ALLOC_N(char, len + 1);
        if (name_copy) {
            memcpy(name_copy, attr->name_view.data, len);
            name_copy[len] = '\0';
            attr_node->name = name_copy;
        } else {
            attr_node->name = NULL;
        }
    } else {
        attr_node->name = NULL;
    }

    /* Handle value - copy from the single-representation view.
     * Entity-bearing views decode first — the axis must return the
     * same string the accessor returns (raw &#38; bytes
     * double-escaped downstream, libxslt bug-59). */
    LeptrisStringView axv = leptris_attr_value_sv(attr);
    if (!leptris_sv_is_empty(&axv)) {
        size_t len = axv.length;
        char* value_copy = NULL;
        if (attr_has_entities(attr)) {
            LeptrisMemoryPool* pool = leptris_element_get_pool(owner);
            if (pool) {
                char* dec = leptris_decode_entities_view(&axv, pool);
                if (dec) value_copy = leptris_strdup(dec);
            }
        }
        if (!value_copy) {
            value_copy = LEPTRIS_ALLOC_N(char, len + 1);
            if (value_copy) {
                memcpy(value_copy, leptris_attr_value_sv(attr).data, len);
                value_copy[len] = '\0';
            }
        }
        attr_node->value = value_copy;
    } else {
        attr_node->value = NULL;
    }

    {
        const char* src_ns = attr_get_namespace_uri(attr);
        attr_node->namespace_uri = src_ns ? leptris_strdup(src_ns) : NULL;
    }
    attr_node->owner = owner;

    return attr_node;
}

/* child:: axis (TODO 109: walks all node types, not just elements).
 * Note: only elements have children. Text/comment/CDATA/PI never have
 * children — return an empty nodeset for non-element context. */
static XPathNodeSet* axis_child(XPathContext* ctx, LeptrisNode* node,
                               XPathASTNode* test) {
    XPathNodeSet* result = xpath_nodeset_new();
    if (!result || !node) return result;

    /* Document node: children = the document's root element (and,
     * in this engine, nothing else — top-level comments/PIs live in
     * the document's side lists, outside the XPath tree). */
    if (node->type == LEPTRIS_NODE_TYPE_DOCUMENT) {
        struct leptris_document* d =
            ((LeptrisDocumentNode*)node)->doc;
        LeptrisNode* root = (LeptrisNode*)d->new_dom_root;
        if (!root) root = (LeptrisNode*)d->root;
        /* Fragment documents chain multiple top-level elements as
         * the root's siblings — ALL are children of the document. */
        for (LeptrisNode* c = root; c;
             c = leptris_node_get_next_sibling(c))
            if (matches_node_test(ctx, c, test))
                xpath_nodeset_add(result, c);
        return result;
    }

    if (node->type != LEPTRIS_NODE_TYPE_ELEMENT) return result;

    /* Walk the raw child list — any node type. */
    LeptrisNode* child = leptris_elem_first_child((LeptrisElement)node);
    while (child) {
        if (matches_node_test(ctx, child, test)) {
            xpath_nodeset_add(result, child);
        }
        child = leptris_node_get_next_sibling(child);
    }

    return result;
}

/* descendant:: axis */
static XPathNodeSet* axis_descendant(XPathContext* ctx, LeptrisElement node,
                                    XPathASTNode* test) {
    XPathNodeSet* result = xpath_nodeset_new();
    if (!result || !node) return result;
    collect_descendants(ctx, node, result, test);
    return result;
}

/* descendant-or-self:: axis */
static XPathNodeSet* axis_descendant_or_self(XPathContext* ctx,
                                            LeptrisElement node,
                                            XPathASTNode* test) {
    XPathNodeSet* result = xpath_nodeset_new();
    if (!result || !node) return result;
    collect_descendants_or_self(ctx, node, result, test);
    return result;
}

/* parent:: axis */
static XPathNodeSet* axis_parent(XPathContext* ctx, LeptrisElement node,
                                XPathASTNode* test) {
    XPathNodeSet* result = xpath_nodeset_new();
    if (!result || !node) return result;

    /* Get parent element using compact accessor */
    LeptrisElement parent = leptris_element_get_parent(node);

    if (!parent) return result;

    if (matches_node_test(ctx, (LeptrisNode*)parent, test)) {
        xpath_nodeset_add(result, parent);
    }

    return result;
}

/* ancestor:: axis */
static XPathNodeSet* axis_ancestor(XPathContext* ctx, LeptrisElement node,
                                  XPathASTNode* test) {
    XPathNodeSet* result = xpath_nodeset_new();
    if (!result || !node) return result;

    LeptrisElement current = leptris_element_get_parent(node);
    while (current) {
        if (matches_node_test(ctx, (LeptrisNode*)current, test)) {
            xpath_nodeset_add(result, current);
        }
        current = leptris_element_get_parent(current);
    }

    return result;
}

/* ancestor-or-self:: axis */
static XPathNodeSet* axis_ancestor_or_self(XPathContext* ctx,
                                          LeptrisElement node,
                                          XPathASTNode* test) {
    XPathNodeSet* result = xpath_nodeset_new();
    if (!result || !node) return result;

    if (matches_node_test(ctx, (LeptrisNode*)node, test)) {
        xpath_nodeset_add(result, node);
    }

    LeptrisElement current = leptris_element_get_parent(node);
    while (current) {
        if (matches_node_test(ctx, (LeptrisNode*)current, test)) {
            xpath_nodeset_add(result, current);
        }
        current = leptris_element_get_parent(current);
    }

    /* XPath §2.2: reverse axes yield REVERSE document order —
     * outermost ancestor first. */
    if (result->count > 1) {
        for (size_t i = 0, j = result->count - 1; i < j; i++, j--) {
            void* t = result->nodes[i];
            result->nodes[i] = result->nodes[j];
            result->nodes[j] = t;
        }
    }
    return result;
}

/* self:: axis */
static XPathNodeSet* axis_self(XPathContext* ctx, LeptrisElement node,
                              XPathASTNode* test) {
    XPathNodeSet* result = xpath_nodeset_new();
    if (!result || !node) return result;

    if (matches_node_test(ctx, (LeptrisNode*)node, test)) {
        xpath_nodeset_add(result, node);
    }

    return result;
}

/* following-sibling:: axis */
/* Parentless elements still have siblings: a top-level child of a
 * document (result-fragment roots chained as the root's siblings;
 * #580 doc-level nodes on the doc_children chain) derives its
 * sibling chain from its OWN document. Returns the chain head when
 * node is on it, NULL otherwise. */
static LeptrisNodeRef top_level_chain_head(LeptrisNodeRef node) {
    if (leptris_node_get_type(node) != LEPTRIS_NODE_TYPE_ELEMENT)
        return NULL;
    struct leptris_document* d =
        (struct leptris_document*)leptris_element_get_document(
            (LeptrisElement)node);
    if (!d) return NULL;
    LeptrisNodeRef head = (LeptrisNodeRef)d->doc_children_head;
    if (!head) head = (LeptrisNodeRef)d->new_dom_root;
    if (!head) head = (LeptrisNodeRef)d->root;
    if (!head) return NULL;
    for (LeptrisNodeRef c = head; c; c = leptris_node_get_next_sibling(c))
        if (c == node) return head;
    return NULL;
}

static XPathNodeSet* axis_following_sibling(XPathContext* ctx,
                                           LeptrisElement node,
                                           XPathASTNode* test) {
    XPathNodeSet* result = xpath_nodeset_new();
    if (!result || !node) return result;

    /* Any-kind links (text/comment contexts position correctly —
     * bug-133). */
    LeptrisElement parent =
        (LeptrisElement)leptris_node_parent((LeptrisNodeRef)node);
    LeptrisNodeRef start;
    if (parent) {
        start = leptris_node_first_child(leptris_element_as_node(parent));
    } else {
        start = top_level_chain_head((LeptrisNodeRef)node);
    }
    if (!start) return result;

    int found = 0;

    LeptrisNodeRef sibling = start;
    while (sibling) {
        if (sibling == (LeptrisNodeRef)node) {
            found = 1;
        } else if (found) {
            if (matches_node_test(ctx, (LeptrisNode*)sibling, test)) {
                xpath_nodeset_add(result, sibling);
            }
        }
        sibling = leptris_node_next_sibling(sibling);
    }

    return result;
}

/* preceding-sibling:: axis */
static XPathNodeSet* axis_preceding_sibling(XPathContext* ctx,
                                           LeptrisElement node,
                                           XPathASTNode* test) {
    XPathNodeSet* result = xpath_nodeset_new();
    if (!result || !node) return result;

    /* Any-kind parent + child links: the context may be a
     * text/comment node (key use expressions, bug-133) whose
     * position among the parent's children the element-typed
     * accessors cannot see. */
    LeptrisElement parent =
        (LeptrisElement)leptris_node_parent((LeptrisNodeRef)node);
    LeptrisNodeRef start;
    if (parent) {
        start = leptris_node_first_child(leptris_element_as_node(parent));
    } else {
        start = top_level_chain_head((LeptrisNodeRef)node);
    }
    if (!start) return result;

    LeptrisNodeRef sibling = start;
    while (sibling && sibling != (LeptrisNodeRef)node) {
        if (matches_node_test(ctx, (LeptrisNode*)sibling, test)) {
            xpath_nodeset_add(result, sibling);
        }
        sibling = leptris_node_next_sibling(sibling);
    }

    /* Reverse the nodeset to match XPath spec (reverse document order) */
    if (result->count > 1) {
        for (size_t i = 0; i < result->count / 2; i++) {
            void* temp = result->nodes[i];
            result->nodes[i] = result->nodes[result->count - 1 - i];
            result->nodes[result->count - 1 - i] = temp;
        }
    }

    return result;
}

/* following:: axis - all nodes after context in document order */
static XPathNodeSet* axis_following(XPathContext* ctx, LeptrisElement node,
                                   XPathASTNode* test) {
    XPathNodeSet* result = xpath_nodeset_new();
    if (!result || !node) return result;

    /* Get following siblings and their descendants */
    LeptrisElement parent = leptris_element_get_parent(node);
    if (!parent) return result;

    int found = 0;

    /* Iterate through siblings using compact accessor functions */
    LeptrisElement sibling = leptris_element_get_first_child(parent);
    while (sibling) {
        if (sibling == node) {
            found = 1;
        } else if (found) {
            if (matches_node_test(ctx, (LeptrisNode*)sibling, test)) {
                xpath_nodeset_add(result, sibling);
            }
            collect_descendants(ctx, sibling, result, test);
        }
        sibling = leptris_element_get_next_sibling(sibling);
    }

    /* Recursively get parent's following nodes */
    XPathNodeSet* parent_following = axis_following(ctx, parent, test);
    if (parent_following) {
        for (size_t i = 0; i < xpath_nodeset_count(parent_following); i++) {
            xpath_nodeset_add(result, xpath_nodeset_get(parent_following, i));
        }
        xpath_nodeset_free(parent_following);
    }

    return result;
}

/* preceding:: axis - all nodes before context in document order */
static XPathNodeSet* axis_preceding(XPathContext* ctx, LeptrisElement node,
                                   XPathASTNode* test) {
    XPathNodeSet* result = xpath_nodeset_new();
    if (!result || !node) return result;

    /* Get preceding siblings and their descendants */
    LeptrisElement parent = leptris_element_get_parent(node);
    if (!parent) return result;

    /* Iterate through siblings using compact accessor functions */
    LeptrisElement sibling = leptris_element_get_first_child(parent);
    while (sibling && sibling != node) {
        if (matches_node_test(ctx, (LeptrisNode*)sibling, test)) {
            xpath_nodeset_add(result, sibling);
        }
        collect_descendants(ctx, sibling, result, test);
        sibling = leptris_element_get_next_sibling(sibling);
    }

    /* Recursively get parent's preceding nodes */
    XPathNodeSet* parent_preceding = axis_preceding(ctx, parent, test);
    if (parent_preceding) {
        for (size_t i = 0; i < xpath_nodeset_count(parent_preceding); i++) {
            xpath_nodeset_add(result, xpath_nodeset_get(parent_preceding, i));
        }
        xpath_nodeset_free(parent_preceding);
    }

    return result;
}

/* attribute:: axis
 *
 * Returns attributes as proper LeptrisAttributeNode structures.
 * These nodes have type LEPTRIS_NODE_ATTRIBUTE and work with all
 * type conversion functions properly.
 */
static XPathNodeSet* axis_attribute(XPathContext* ctx, LeptrisElement node,
                                   XPathASTNode* test) {
    DEBUG_LOG("        === axis_attribute START ===");
    DEBUG_LOG("        node=%p, name=%s", (void*)node, node ? leptris_element_get_name(node) : "(null)");
    DEBUG_LOG("        attr_count=%zu", node ? (size_t)leptris_element_attribute_count(node) : 0);

    XPathNodeSet* result = xpath_nodeset_new();
    if (!result || !node) {
        DEBUG_LOG("        EARLY RETURN: result=%p, node=%p", (void*)result, (void*)node);
        return result;
    }

    /* Iterate through element's attributes using compact accessor functions */
    size_t attr_count = leptris_element_attribute_count(node);
    for (size_t i = 0; i < attr_count; i++) {
        /* Walk the attribute linked list to get the i-th attribute */
        struct leptris_attribute* attr = leptris_element_get_first_attribute(node);
        for (size_t j = 0; j < i && attr; j++) {
            attr = leptris_attr_next(attr);
        }

        /* Validate attribute pointer before accessing */
        if (!attr) {
            DEBUG_LOG("        [%zu] SKIPPED: attr is NULL", i);
            continue;
        }

        /* Sanity check: attribute should point to valid memory */
        if ((uintptr_t)attr < 0x1000) {
            DEBUG_LOG("        [%zu] SKIPPED: attr has invalid pointer %p", i, (void*)attr);
            continue;
        }

        DEBUG_LOG("        [%zu] attr=%p", i, (void*)attr);
        DEBUG_LOG("        [%zu] attr->name=%p, attr->value=%p",
                 i, attr->name, attr->value);

        /* Check if attribute matches node test */
        int matches = 0;
        if (test && test->type == XPATH_AST_NODE_TEST_NAME) {
            /* Specific attribute name test - handle both StringView and cached name */
            int name_matches = 0;
            const char* tv = test->value;
            const char* tcolon = tv ? strchr(tv, ':') : NULL;
            if (tcolon && ctx->ns_set) {
                /* Prefixed test: resolve through the binding set and
                 * match (URI, local) — the attribute's own spelling
                 * may use a different prefix for the same namespace
                 * (libxslt bug-97). Unbound prefix: fall through to
                 * the literal compare (legacy behavior). */
                const char* uri = leptris_xpath_ns_lookup(
                    (const struct leptris_xpath_ns_map*)ctx->ns_set,
                    tv, (size_t)(tcolon - tv));
                if (uri && !leptris_sv_is_empty(&attr->name_view)) {
                    const char* a = attr->name_view.data;
                    size_t al = attr->name_view.length;
                    const char* ac =
                        (const char*)memchr(a, ':', al);
                    const char* ans = leptris_attribute_namespace_uri(
                        (LeptrisAttribute)attr);
                    if (ac && ans) {
                        size_t ll = al - (size_t)(ac - a) - 1;
                        size_t tl = strlen(tcolon + 1);
                        name_matches =
                            ll == tl &&
                            strncmp(ac + 1, tcolon + 1, tl) == 0 &&
                            strcmp(ans, uri) == 0;
                    }
                }
            }
            /* Literal fallback also serves PREFIXED tests whose prefix
             * is not in the binding set (xml: is prebound by the spec
             * but absent from ns maps) — vm_apply_axis_attribute
             * parity; without it [@xml:lang] matched nothing
             * (libxslt bug-142). */
            if (!name_matches &&
                !leptris_sv_is_empty(&attr->name_view)) {
                name_matches = leptris_sv_equals_cstr(&attr->name_view, test->value);
            }
            matches = name_matches;
            DEBUG_LOG("        [%zu] NAME test: looking for '%s', matches=%d",
                     i, test->value ? test->value : "(null)", matches);
        } else if (test && test->type == XPATH_AST_NODE_TEST_ALL) {
            /* Wildcard - matches all attributes */
            matches = 1;
            DEBUG_LOG("        [%zu] WILDCARD test: matches=%d", i, matches);
        } else if (!test) {
            /* No test means match all */
            matches = 1;
            DEBUG_LOG("        [%zu] NO test: matches=%d", i, matches);
        }

        if (matches) {
            /* Create proper attribute node */
            DEBUG_LOG("        [%zu] Creating attribute node...", i);
            LeptrisAttributeNode* attr_node = create_attribute_node(attr, node);
            DEBUG_LOG("        [%zu] attr_node=%p", i, (void*)attr_node);
            if (attr_node) {
                DEBUG_LOG("        [%zu] attr_node->node_type=%d (should be 1)",
                         i, (int)attr_node->node_type);
                DEBUG_LOG("        [%zu] attr_node->name=%s",
                         i, attr_node->name ? attr_node->name : "(null)");
                DEBUG_LOG("        [%zu] attr_node->value=%s",
                         i, attr_node->value ? attr_node->value : "(null)");
                DEBUG_LOG("        [%zu] Adding to nodeset...", i);
                xpath_nodeset_add(result, (void*)attr_node);
                DEBUG_LOG("        [%zu] Added. Nodeset count now: %zu",
                         i, xpath_nodeset_count(result));
            } else {
                DEBUG_LOG("        [%zu] FAILED to create attr_node!", i);
            }
        }
    }

    DEBUG_LOG("        Final nodeset count: %zu", xpath_nodeset_count(result));
    DEBUG_LOG("        === axis_attribute END ===");
    return result;
}

/* Helper: Check if prefix has already been seen */
static int is_prefix_seen(char** seen_prefixes, size_t seen_count, const char* prefix) {
    for (size_t i = 0; i < seen_count; i++) {
        /* Both NULL = default namespace, already seen */
        if (!prefix && !seen_prefixes[i]) return 1;
        /* One NULL, one not = different */
        if (!prefix || !seen_prefixes[i]) continue;
        /* Both non-NULL, compare strings */
        if (strcmp(prefix, seen_prefixes[i]) == 0) return 1;
    }
    return 0;
}

/* namespace:: axis */
static int ns_axis_match(XPathASTNode* test, const char* prefix) {
    if (!test || test->type == XPATH_AST_NODE_TEST_ALL) return 1;
    if (test->type == XPATH_AST_NODE_TEST_NAME)
        return test->value && prefix &&
               strcmp(test->value, prefix) == 0;
    return 0;
}

/* Push one in-scope namespace (dedup by prefix, innermost first). */
static void ns_axis_push(XPathNodeSet* result,
                         char*** seen_prefixes, size_t* seen_count,
                         size_t* seen_capacity,
                         const char* prefix, const char* uri,
                         LeptrisElement owner, XPathASTNode* test) {
    if (!uri) return;
    if (is_prefix_seen(*seen_prefixes, *seen_count, prefix)) return;
    if (*seen_count >= *seen_capacity) {
        size_t cap = *seen_capacity == 0 ? 4 : *seen_capacity * 2;
        char** grown = (char**)realloc(*seen_prefixes,
                                       cap * sizeof(char*));
        if (!grown) return;
        *seen_prefixes = grown;
        *seen_capacity = cap;
    }
    (*seen_prefixes)[(*seen_count)++] =
        prefix ? leptris_strdup(prefix) : NULL;
    if (!ns_axis_match(test, prefix)) return;
    LeptrisNamespaceNode* ns_node = LEPTRIS_ALLOC(LeptrisNamespaceNode);
    if (!ns_node) return;
    ns_node->node_type = LEPTRIS_NODE_NAMESPACE;
    ns_node->prefix = prefix ? leptris_strdup(prefix) : NULL;
    ns_node->uri = leptris_strdup(uri);
    ns_node->owner = owner;
    xpath_nodeset_add(result, (void*)ns_node);
}

/* In-scope namespaces of `node`: the DECLARATIONS on each ancestor,
 * innermost-first (a closer declaration overrides an outer one bound
 * to the same prefix), plus the always-in-scope xml prefix. The old
 * walk only saw each element's OWN prefix, missing sibling
 * declarations such as xmlns:c beside a default xmlns. */
static XPathNodeSet* axis_namespace(XPathContext* ctx, LeptrisElement node,
                                    XPathASTNode* test) {
    (void)ctx;
    XPathNodeSet* result = xpath_nodeset_new();
    if (!result || !node) return result;

    char** seen_prefixes = NULL;
    size_t seen_count = 0;
    size_t seen_capacity = 0;

    for (LeptrisElement current = node; current;
         current = leptris_element_get_parent(current)) {
        for (struct leptris_namespace* decl =
                 leptris_elem_namespaces(current);
             decl; decl = decl->next)
            ns_axis_push(result, &seen_prefixes, &seen_count,
                         &seen_capacity, decl->prefix, decl->uri,
                         current, test);
    }
    ns_axis_push(result, &seen_prefixes, &seen_count, &seen_capacity,
                 "xml", "http://www.w3.org/XML/1998/namespace", node,
                 test);

    for (size_t i = 0; i < seen_count; i++)
        if (seen_prefixes[i]) free(seen_prefixes[i]);
    free(seen_prefixes);

    /* libxslt axis order: REVERSED (last declaration first) — match
     * it so copied declarations serialize in the recorded order. */
    if (result->count > 1) {
        for (size_t i = 0, j = result->count - 1; i < j; i++, j--) {
            void* t = result->nodes[i];
            result->nodes[i] = result->nodes[j];
            result->nodes[j] = t;
        }
    }

    result->owns_namespaces = 1;
    return result;
}

/* Map an axis name string to its enum (TODO 113 Phase 1 perf).
 * Called once at parse time so apply_axis can switch on the enum
 * instead of strcmp'ing the name on every axis dispatch. */
XPathAxisType xpath_axis_from_name(const char* name) {
    if (!name || !*name) return XPATH_AXIS_CHILD;
    /* First-character dispatch: most axes have a unique first char,
     * collapsing 13 strcmp calls into a single switch + one compare.
     * axis_id is set once at parse time, so this runs ~13× less
     * often than apply_axis itself (TODO 113 Phase 1). */
    switch (name[0]) {
        case 'a':
            if (strcmp(name, "ancestor") == 0) return XPATH_AXIS_ANCESTOR;
            if (strcmp(name, "ancestor-or-self") == 0) return XPATH_AXIS_ANCESTOR_OR_SELF;
            if (strcmp(name, "attribute") == 0) return XPATH_AXIS_ATTRIBUTE;
            break;
        case 'c':
            if (strcmp(name, "child") == 0) return XPATH_AXIS_CHILD;
            break;
        case 'd':
            if (strcmp(name, "descendant") == 0) return XPATH_AXIS_DESCENDANT;
            if (strcmp(name, "descendant-or-self") == 0) return XPATH_AXIS_DESCENDANT_OR_SELF;
            break;
        case 'f':
            if (strcmp(name, "following") == 0) return XPATH_AXIS_FOLLOWING;
            if (strcmp(name, "following-sibling") == 0) return XPATH_AXIS_FOLLOWING_SIBLING;
            break;
        case 'n':
            if (strcmp(name, "namespace") == 0) return XPATH_AXIS_NAMESPACE;
            break;
        case 'p':
            if (strcmp(name, "parent") == 0) return XPATH_AXIS_PARENT;
            if (strcmp(name, "preceding") == 0) return XPATH_AXIS_PRECEDING;
            if (strcmp(name, "preceding-sibling") == 0) return XPATH_AXIS_PRECEDING_SIBLING;
            break;
        case 's':
            if (strcmp(name, "self") == 0) return XPATH_AXIS_SELF;
            break;
    }
    return XPATH_AXIS_CHILD;
}

/* Apply axis dispatcher.
 * TODO 109: accepts LeptrisNode* so the descendant-or-self expansion
 * of // can pass non-element nodes through. Most axes only make
 * sense on element context (child, descendant, sibling, etc.); the
 * dispatcher returns an empty nodeset for those if the context is
 * not an element.
 *
 * TODO 113 Phase 1: switches on the pre-computed axis_id from the
 * AST node, avoiding the strcmp chain. Falls back to from_name if
 * axis_id wasn't set (defensive — should always be set by parser). */
XPathNodeSet* apply_axis(XPathContext* ctx, LeptrisNode* node,
                         const char* axis_name, XPathASTNode* test) {
    DEBUG_LOG("      === apply_axis: %s ===", axis_name ? axis_name : "(null/child)");

    XPathAxisType axis_id = test ? test->axis_id : XPATH_AXIS_CHILD;
    if (axis_name && *axis_name && axis_id == XPATH_AXIS_CHILD &&
        strcmp(axis_name, "child") != 0) {
        /* Defensive fallback: re-derive from name if axis_id wasn't
         * set to the right value but the name is non-default. */
        axis_id = xpath_axis_from_name(axis_name);
    }

    int element_only = 0;  /* set per-axis below */

    /* Fast path: child is the default axis. Skip the element_only
     * check and dispatch directly. */
    if (axis_id == XPATH_AXIS_CHILD) {
        return axis_child(ctx, node, test);
    }

    /* Descendant/following/preceding axes need an element context;
     * the SIBLING axes also accept text/comment/PI contexts (their
     * walkers use the any-kind node links — key use expressions
     * evaluated from text nodes, bug-133). */
    element_only = (axis_id == XPATH_AXIS_DESCENDANT ||
                    axis_id == XPATH_AXIS_DESCENDANT_OR_SELF ||
                    axis_id == XPATH_AXIS_FOLLOWING ||
                    axis_id == XPATH_AXIS_PRECEDING);

    if (element_only && node && node->type != LEPTRIS_NODE_TYPE_ELEMENT) {
        return xpath_nodeset_new();
    }

    LeptrisElement elem = (LeptrisElement)node;
    switch (axis_id) {
        case XPATH_AXIS_DESCENDANT:
            return axis_descendant(ctx, elem, test);
        case XPATH_AXIS_DESCENDANT_OR_SELF:
            return axis_descendant_or_self(ctx, elem, test);
        case XPATH_AXIS_PARENT:
            return axis_parent(ctx, elem, test);
        case XPATH_AXIS_ANCESTOR:
            return axis_ancestor(ctx, elem, test);
        case XPATH_AXIS_ANCESTOR_OR_SELF:
            return axis_ancestor_or_self(ctx, elem, test);
        case XPATH_AXIS_SELF:
            return axis_self(ctx, elem, test);
        case XPATH_AXIS_FOLLOWING_SIBLING:
            return axis_following_sibling(ctx, elem, test);
        case XPATH_AXIS_PRECEDING_SIBLING:
            return axis_preceding_sibling(ctx, elem, test);
        case XPATH_AXIS_FOLLOWING:
            return axis_following(ctx, elem, test);
        case XPATH_AXIS_PRECEDING:
            return axis_preceding(ctx, elem, test);
        case XPATH_AXIS_ATTRIBUTE:
            return axis_attribute(ctx, elem, test);
        case XPATH_AXIS_NAMESPACE:
            return axis_namespace(ctx, elem, test);
        default:
            return xpath_nodeset_new();  /* Unknown axis */
    }
}