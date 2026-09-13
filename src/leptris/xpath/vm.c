/* lib/src/xpath/vm.c — XPath bytecode VM interpreter (TODO 120 Phase F)
 *
 * Stack-based interpreter for the bytecode emitted by compiler.c.
 * The VM holds `struct leptris_xpath_result*` values on its stack.
 *
 * Inline handlers (Phase F):
 *   BC_LITERAL_NUMBER / STRING / BOOL
 *     → push a fresh XPathResult.
 *   BC_PATH_ABSOLUTE
 *     → push document root as single-node nodeset.
 *   BC_PATH_RELATIVE
 *     → push context node as single-node nodeset.
 *   BC_AXIS_STEP
 *     → pop input nodeset, call evaluate_step(ctx, ast, input),
 *       push result nodeset. Reuses the existing axis + node-test
 *       + predicate machinery.
 *   BC_BINARY_OP
 *     → pop two operands, apply arithmetic / comparison / boolean
 *       logic inline. Avoids the AST-node walking that
 *       evaluate_operator would do.
 *   BC_FUNC_CALL
 *     → call evaluate_function_call(ctx, ast) directly. Skips the
 *       evaluate_expr AST-type switch.
 *   BC_FALLBACK_EVAL
 *     → call evaluate_expr(ctx, ast). Used for variable refs and
 *       any AST shape the compiler doesn't specifically lower.
 *   BC_RETURN
 *     → pop and return as final result.
 *
 * The VM is COMPLETE: every XPath expression compiles to a sequence
 * of these opcodes and evaluates correctly. The inline handlers
 * exist to reduce dispatch overhead; correctness is identical to
 * direct AST evaluation because the inline handlers call into the
 * same evaluator helpers.
 */
#include "bytecode.h"
#include "xpath_variables.h"
#include "evaluator_internal.h"
#include "../dom/document_node.h"
#include "functions.h"
#include "../leptris_internal.h"
#include "../dom/element.h"
#include "../common/entities.h"
#include "../dom/element_index.h"
#include "../dom/text.h"
#include "../dom/node.h"
#include "../dom/pi.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

typedef struct {
    struct leptris_xpath_result** stack;
    size_t sp;
    size_t cap;
    int error;
} XPathVM;

static int vm_push(XPathVM* vm, struct leptris_xpath_result* v) {
    if (vm->sp >= vm->cap) {
        size_t new_cap = vm->cap ? vm->cap * 2 : 16;
        struct leptris_xpath_result** grown =
            (struct leptris_xpath_result**)realloc(vm->stack,
                new_cap * sizeof(struct leptris_xpath_result*));
        if (!grown) { vm->error = 1; return -1; }
        vm->stack = grown;
        vm->cap = new_cap;
    }
    vm->stack[vm->sp++] = v;
    return 0;
}

static struct leptris_xpath_result* vm_pop(XPathVM* vm) {
    if (vm->sp == 0) { vm->error = 1; return NULL; }
    return vm->stack[--vm->sp];
}

static uint8_t read_u8(const unsigned char** pc) {
    uint8_t v = *pc[0];
    *pc += 1;
    return v;
}

static uint16_t read_u16(const unsigned char** pc) {
    uint16_t v = ((uint16_t)(*pc)[0] << 8) | (*pc)[1];
    *pc += 2;
    return v;
}

static double read_double(const unsigned char** pc) {
    double v;
    memcpy(&v, *pc, sizeof(double));
    *pc += sizeof(double);
    return v;
}

/* Build a single-node nodeset from a context element. Caller owns
 * the returned result. */
static struct leptris_xpath_result* make_singleton_nodeset(void* node) {
    struct leptris_xpath_result* r =
        (struct leptris_xpath_result*)calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->type = XPATH_RESULT_NODESET;
    r->value.nodeset_value = xpath_nodeset_new();
    if (!r->value.nodeset_value) {
        free(r);
        return NULL;
    }
    if (node) xpath_nodeset_add(r->value.nodeset_value, node);
    return r;
}

/* ----------------------------------------------------------------------- *
 * Specialized axis handlers (TODO 126).
 *
 * Each handler pops the input nodeset from the VM stack, walks the
 * tree inline (bypassing evaluate_step / apply_axis / matches_node_test),
 * and returns a freshly-allocated result.
 *
 * `name` is NULL for wildcards; otherwise a NUL-terminated C string
 * to match against element names. The compiler only emits these
 * opcodes for the no-namespace-prefix case (no ':' in test value).
 * ----------------------------------------------------------------------- */

/* Common preamble: pop input nodeset, detach it from the result
 * wrapper, free the wrapper. Returns the detached nodeset or NULL
 * on error (vm->error is set). */
static XPathNodeSet* vm_detach_input_nodeset(XPathVM* vm) {
    struct leptris_xpath_result* input = vm_pop(vm);
    if (!input || input->type != XPATH_RESULT_NODESET) {
        if (input) xpath_result_free(input);
        vm->error = 1;
        return NULL;
    }
    XPathNodeSet* ns = input->value.nodeset_value;
    input->value.nodeset_value = NULL;
    xpath_result_free(input);
    return ns;
}

/* Helper: is this node an element (vs attribute / text / etc.)?
 * The VM opcodes only accept element input; non-element inputs are
 * skipped (matches the existing axis behavior for non-element context). */
static int node_is_element(void* node) {
    if (!node) return 0;
    /* Unified tag space (issue #477): every node — real DOM node or
     * synthetic attribute/namespace/text node — is classified by its
     * first int; 0 is the public element tag, nothing else uses it. */
    return *(LeptrisNodeType*)node == LEPTRIS_NODE_ELEMENT;
}

struct leptris_xpath_result* vm_apply_axis_child(XPathContext* ctx, XPathVM* vm,
                                                 const char* name, int wild);
struct leptris_xpath_result* vm_apply_axis_attribute(XPathContext* ctx, XPathVM* vm,
                                                     const char* name, int wild);
struct leptris_xpath_result* vm_apply_axis_self(XPathContext* ctx, XPathVM* vm,
                                                const char* name, int wild);
struct leptris_xpath_result* vm_apply_axis_parent(XPathContext* ctx, XPathVM* vm,
                                                  const char* name, int wild);
struct leptris_xpath_result* vm_apply_axis_descendant(XPathContext* ctx, XPathVM* vm,
                                                      const char* name, int wild,
                                                      int include_self);

/* Forward decl for fused handler (TODO 134). */
static struct leptris_xpath_result* vm_apply_axis_descendant_pred_attr(
    XPathContext* ctx, XPathVM* vm,
    const char* attr_name, const char* attr_value,
    int value_match, int include_self);

/* Forward decl: descendant_walk is defined below, but vm_apply_absolute
 * uses it. */
static void descendant_walk(XPathContext* ctx, XPathNodeSet* out, LeptrisElement elem,
                              const char* name, int wild, int include_self);

/* Absolute-path handlers (TODO 129). Like vm_apply_axis_descendant
 * but starts from the document root element directly, with the
 * document-root semantics: `/foo` matches root itself (not its
 * children), `//foo` walks the entire tree.
 *
 * `mode` controls behavior:
 *   0 = root-match only: push [root] if root matches name/wild.
 *       Used for BC_ABSOLUTE_ROOT_MATCH (child axis from document).
 *   1 = descendant: push all descendants of root matching name.
 *       Used for BC_ABSOLUTE_DESCENDANT (descendant axis).
 *   2 = descendant-or-self: push root if matches + descendants
 *       matching. Used for BC_ABSOLUTE_DESCENDANT_OR_SELF. */
/* Absolute `//type-test` (issue #485): one pre-order walk of the
 * root subtree emitting matching nodes directly in document order.
 * This replaces the generic two-step form (descendant-or-self::node()
 * + child::type()) whose per-context merge needs a doc-order sort.
 * `want`: 0=node(), 1=text(), 2=comment(), 3=processing-instruction().
 * pi_target is only consulted for want==3 (NULL = any target). */
static void vm_absolute_type_walk(XPathNodeSet* out, LeptrisElement elem,
                                  int want, const char* pi_target) {
    LeptrisNode* child = leptris_elem_first_child(elem);
    while (child) {
        int match = 0;
        switch (want) {
            case 0:  /* node() */
                match = 1;
                break;
            case 1:  /* text() — CDATA sections count as text */
                match = (child->type == LEPTRIS_NODE_TYPE_TEXT ||
                         child->type == LEPTRIS_NODE_TYPE_CDATA);
                break;
            case 2:  /* comment() */
                match = (child->type == LEPTRIS_NODE_TYPE_COMMENT);
                break;
            case 3:  /* processing-instruction([target]) */
                if (child->type == LEPTRIS_NODE_TYPE_PI) {
                    if (pi_target) {
                        LeptrisPINode* pi = (LeptrisPINode*)child;
                        match = pi->target &&
                                strcmp(pi->target, pi_target) == 0;
                    } else {
                        match = 1;
                    }
                }
                break;
            default:
                break;
        }
        if (match) xpath_nodeset_add_fast(out, child);
        if (child->type == LEPTRIS_NODE_TYPE_ELEMENT) {
            vm_absolute_type_walk(out, (LeptrisElement)child, want, pi_target);
        }
        child = leptris_node_get_next_sibling(child);
    }
}

/* Issue #525 (XPath 1.0 §2.3): an UNPREFIXED name test matches only
 * elements in NO namespace — a default-xmlns element is in a
 * namespace even though it has no prefix. Documents without any
 * namespace declaration skip the check (zero-cost common case). */
static struct leptris_document* xpath_context_document(
        XPathContext* ctx) {
    if (!ctx || !ctx->context_node) return ctx ? ctx->document : NULL;
    LeptrisNode* cn = (LeptrisNode*)ctx->context_node;
    /* Non-element contexts (pattern matching on text()/comment()/PI)
     * walk any-kind parent links up to the nearest ELEMENT first -
     * leptris_element_get_document would misread their layout. */
    if (cn->type != LEPTRIS_NODE_TYPE_ELEMENT) {
        LeptrisElement e = leptris_node_parent((LeptrisNodeRef)cn);
        if (!e) return ctx->document;
        cn = (LeptrisNode*)e;
    }
    struct leptris_document* nd =
        leptris_element_get_document((LeptrisElement)cn);
    return nd ? nd : ctx->document;
}

static inline int vm_unprefixed_name_matches(XPathContext* ctx,
                                             LeptrisElement e,
                                             const char* name) {
    /* Prefixed QNames (issue #564): resolve through the context's
     * namespace bindings — the same semantics as the interpreter's
     * matches_node_test. A bound prefix matches by namespace URI
     * (any element prefix or the default ns); an unbound prefix
     * falls back to the literal prefix comparison. */
    const char* colon = name ? strchr(name, ':') : NULL;
    if (colon) {
        size_t plen = (size_t)(colon - name);
        const char* local = colon + 1;
        const char* en = leptris_element_get_name(e);
        if (!en || strcmp(en, local) != 0) return 0;
        const char* test_uri = ctx
            ? leptris_xpath_ns_lookup(
                  (const struct leptris_xpath_ns_map*)ctx->ns_set,
                  name, plen)
            : NULL;
        if (test_uri) {
            const char* node_uri = leptris_element_get_namespace_uri(e);
            return node_uri && strcmp(node_uri, test_uri) == 0;
        }
        const char* node_prefix = leptris_element_get_prefix(e);
        return node_prefix &&
               strlen(node_prefix) == plen &&
               strncmp(node_prefix, name, plen) == 0;
    }
    const char* en = leptris_element_get_name(e);
    if (!en || strcmp(en, name) != 0) return 0;
    if (!ctx || !ctx->document || !ctx->document->has_namespaces) return 1;
    const char* uri = leptris_element_get_namespace_uri(e);
    return !uri || !uri[0];
}

static struct leptris_xpath_result* vm_apply_absolute_type(
        XPathContext* ctx, const char* type_name, const char* pi_target) {
    if (!ctx || !ctx->document) return NULL;
    LeptrisElement root = (LeptrisElement)ctx->document->new_dom_root;
    if (!root) return NULL;

    int want = -1;
    if (strcmp(type_name, "node") == 0) want = 0;
    else if (strcmp(type_name, "text") == 0) want = 1;
    else if (strcmp(type_name, "comment") == 0) want = 2;
    else if (strcmp(type_name, "processing-instruction") == 0) want = 3;
    if (want < 0) return NULL;

    XPathNodeSet* out = xpath_nodeset_new();
    if (!out) return NULL;
    /* `//node()` per XPath 1.0 selects every node in the document
     * except the document node itself — the root element included
     * (it is a child of the document node). The walk covers the
     * root's descendants, so node() needs the root itself added
     * first. The other type tests never match an element, so the
     * descendant walk already is their complete result.
     * #580: document-level comments/PIs are descendants of the
     * document too — the chain walks in document order (prolog
     * nodes, root, subtree, epilog nodes). */
    {
        LeptrisNode* start = (LeptrisNode*)ctx->document->doc_children_head;
        if (!start) start = (LeptrisNode*)root;
        for (LeptrisNode* c = start; c;
             c = leptris_node_get_next_sibling(c)) {
            if (c->type == LEPTRIS_NODE_TYPE_ELEMENT) {
                if (want == 0) xpath_nodeset_add_fast(out, root);
                vm_absolute_type_walk(out, root, want, pi_target);
                continue;
            }
            int match = (want == 0);   /* node() matches all kinds */
            if (want == 1) {
                match = (c->type == LEPTRIS_NODE_TYPE_TEXT ||
                         c->type == LEPTRIS_NODE_TYPE_CDATA);
            } else if (want == 2) {
                match = (c->type == LEPTRIS_NODE_TYPE_COMMENT);
            } else if (want == 3 && c->type == LEPTRIS_NODE_TYPE_PI) {
                if (pi_target) {
                    LeptrisPINode* pi = (LeptrisPINode*)c;
                    match = pi->target && strcmp(pi->target, pi_target) == 0;
                } else {
                    match = 1;
                }
            }
            if (match) xpath_nodeset_add_fast(out, c);
        }
        if (!ctx->document->doc_children_head) {
            /* No chain (no doc-level nodes): plain subtree walk. */
            if (want == 0) xpath_nodeset_add_fast(out, root);
            vm_absolute_type_walk(out, root, want, pi_target);
        }
    }

    struct leptris_xpath_result* r = xpath_result_new(XPATH_RESULT_NODESET);
    if (!r) { xpath_nodeset_free(out); return NULL; }
    r->value.nodeset_value = out;
    return r;
}

/* Prefixed-QName bucket path (issue #564): the index keys elements
 * by LOCAL name, so a prefixed test resolves through the local
 * bucket and verifies each (small) candidate with the ns-aware
 * matcher — instead of walking the whole tree. */
static void vm_add_prefixed_bucket(XPathContext* ctx,
                                   struct leptris_element_index* idx,
                                   const char* qname,
                                   XPathNodeSet* out) {
    const char* colon = strchr(qname, ':');
    if (!colon) return;
    const LeptrisElementIndexBucket* bucket =
        leptris_element_index_lookup(idx, colon + 1);
    if (!bucket) return;
    for (size_t i = 0; i < bucket->count; i++) {
        LeptrisElement m = (LeptrisElement)bucket->matches[i];
        if (vm_unprefixed_name_matches(ctx, m, qname))
            xpath_nodeset_add_fast(out, m);
    }
}

static struct leptris_xpath_result* vm_apply_absolute(XPathContext* ctx,
                                                      const char* name,
                                                      int wild,
                                                      int mode) {
    if (!ctx || !ctx->document) return NULL;
    /* Absolute paths root at the CONTEXT NODE'S document (libxslt
     * semantics): document() output and RTF fragments must not see
     * the transform source tree. */
    struct leptris_document* ctx_doc = xpath_context_document(ctx);
    LeptrisElement root = (LeptrisElement)ctx_doc->new_dom_root;
    if (!root) return NULL;

    /* Mode 0 (root match only) — short-circuit, no descendant walk. */
    if (mode == 0) {
        XPathNodeSet* out = xpath_nodeset_new();
        if (!out) return NULL;
        if (wild) {
            xpath_nodeset_add_fast(out, root);
        } else {
            if (vm_unprefixed_name_matches(ctx, root, name)) {
                xpath_nodeset_add_fast(out, root);
            }
        }
        struct leptris_xpath_result* r = xpath_result_new(XPATH_RESULT_NODESET);
        if (!r) { xpath_nodeset_free(out); return NULL; }
        r->value.nodeset_value = out;
        return r;
    }

    /* Mode 2 (whole tree incl. the root): use the element index with
     * a memcpy fast path (TODO 137). Replaces the per-element
     * fast_add loop with a single malloc+memcpy of the relevant
     * index slice. ~10x faster for the loop portion of descendant::*.
     * (Mode 1 — element-relative descendant, root excluded — was
     * removed: every absolute descendant form takes mode 2 because
     * its context is the document node.)
     *
     * Layout invariants:
     *   - all_elements[0] = root (preorder starts at root)
     *   - name_bucket.matches: all elements with that name in preorder
     */
    struct leptris_element_index* idx = ctx_doc->element_index;
    /* TODO 190: build on the SECOND axis query. The build costs
     * two tree walks + per-name bucket allocations; a document
     * that sees one query pays less walking directly. */
    if (!idx && ++ctx_doc->axis_query_count >= 2) {
        idx = leptris_element_index_build(ctx_doc);
        ctx_doc->element_index = idx;
    }

    XPathNodeSet* out = xpath_nodeset_new();
    if (!out) return NULL;

    if (idx) {
        if (wild) {
            /* Result = all_elements (the root included). */
            void** src = (void**)idx->all_elements;
            size_t n = idx->all_count;
            if (n > 0) {
                void** arr = (void**)malloc(n * sizeof(void*));
                if (arr) {
                    memcpy(arr, src, n * sizeof(void*));
                    out->nodes = arr;
                    out->count = n;
                    out->capacity = n;
                }
            }
        } else if (strchr(name, ':')) {
            /* Prefixed test: local-name bucket + ns verification. */
            vm_add_prefixed_bucket(ctx, idx, name, out);
        } else {
            const LeptrisElementIndexBucket* bucket =
                leptris_element_index_lookup(idx, name);
            if (bucket) {
                /* #525 gate: on namespace-bearing documents the
                 * bucket keys by LOCAL name — verify each entry
                 * (unprefixed tests match no-namespace elements
                 * only). The plain fast memcpy stays for
                 * namespace-free documents. */
                if (ctx && ctx->document && ctx->document->has_namespaces) {
                    for (size_t i = 0; i < bucket->count; i++) {
                        LeptrisElement m = (LeptrisElement)bucket->matches[i];
                        if (vm_unprefixed_name_matches(ctx, m, name))
                            xpath_nodeset_add_fast(out, m);
                    }
                    struct leptris_xpath_result* rg =
                        xpath_result_new(XPATH_RESULT_NODESET);
                    if (!rg) { xpath_nodeset_free(out); return NULL; }
                    rg->value.nodeset_value = out;
                    return rg;
                }
                void** src = (void**)bucket->matches;
                size_t n = bucket->count;
                if (n > 0) {
                    /* Copy all matches (root included — document
                     * context). */
                    void** arr = (void**)malloc(n * sizeof(void*));
                    if (arr) {
                        memcpy(arr, src, n * sizeof(void*));
                        out->nodes = arr;
                        out->count = n;
                        out->capacity = n;
                    }
                }
            }
        }
    } else {
        /* Index build failed — fall back to walk. */
        /* descendant-or-self::name matches the root only when the
         * root's name matches (or it's a wildcard). The index path
         * applies the same filter via the name bucket; this walk
         * path ran only on index-build failure until TODO 190 made
         * first queries walk directly — the unconditional root add
         * was a latent off-by-one for named // queries (exposed by
         * count(//book) returning root+books). */
        if (wild || vm_unprefixed_name_matches(ctx, root, name)) {
            xpath_nodeset_add_fast(out, root);
        }
        descendant_walk(ctx, out, root, name, wild, 0);
    }

    struct leptris_xpath_result* r = xpath_result_new(XPATH_RESULT_NODESET);
    if (!r) { xpath_nodeset_free(out); return NULL; }
    r->value.nodeset_value = out;
    return r;
}

struct leptris_xpath_result* vm_apply_axis_child(XPathContext* ctx, XPathVM* vm,
                                                 const char* name, int wild) {
    XPathNodeSet* input = vm_detach_input_nodeset(vm);
    if (!input) return NULL;

    XPathNodeSet* out = xpath_nodeset_new();
    if (!out) { xpath_nodeset_free(input); return NULL; }

    for (size_t i = 0; i < input->count; i++) {
        LeptrisNode* nd = input->nodes[i];
        /* Document node: child axis = the document child chain —
         * prolog nodes, the root element, epilog nodes (#580). */
        if (nd && nd->type == LEPTRIS_NODE_TYPE_DOCUMENT) {
            struct leptris_document* d = ((LeptrisDocumentNode*)nd)->doc;
            LeptrisNode* start = (LeptrisNode*)d->doc_children_head;
            if (!start) {
                start = (LeptrisNode*)d->new_dom_root;
                if (!start) start = (LeptrisNode*)d->root;
            }
            for (LeptrisNode* c = start; c;
                 c = leptris_node_get_next_sibling(c)) {
                if (wild ||
                    (c->type == LEPTRIS_NODE_TYPE_ELEMENT &&
                     vm_unprefixed_name_matches(ctx, (LeptrisElement)c,
                                                name)))
                    xpath_nodeset_add_fast(out, c);
            }
            continue;
        }
        if (!node_is_element(nd)) continue;
        LeptrisElement elem = (LeptrisElement)nd;
        LeptrisElement child = leptris_element_get_first_child(elem);
        while (child) {
            if (wild) {
                xpath_nodeset_add_fast(out, child);
            } else {
                if (vm_unprefixed_name_matches(ctx, child, name)) {
                    xpath_nodeset_add_fast(out, child);
                }
            }
            child = leptris_element_get_next_sibling(child);
        }
    }

    xpath_nodeset_free(input);

    struct leptris_xpath_result* r = xpath_result_new(XPATH_RESULT_NODESET);
    if (!r) { xpath_nodeset_free(out); return NULL; }
    r->value.nodeset_value = out;
    return r;
}

/* Fused child::NAME[k] (issue #645): the k-th child matching NAME
 * of EACH input context node. A forward axis's step position is
 * per-context by definition — count matching children and stop at
 * k, no intermediate nodeset, no predicate frame. Document-node
 * inputs walk the doc child chain like vm_apply_axis_child. */
static struct leptris_xpath_result* vm_apply_axis_child_name_pos(
        XPathContext* ctx, XPathVM* vm, const char* name, uint16_t pos) {
    XPathNodeSet* input = vm_detach_input_nodeset(vm);
    if (!input) return NULL;

    XPathNodeSet* out = xpath_nodeset_new();
    if (!out) { xpath_nodeset_free(input); return NULL; }

    for (size_t i = 0; i < input->count; i++) {
        LeptrisNode* nd = input->nodes[i];
        if (nd && nd->type == LEPTRIS_NODE_TYPE_DOCUMENT) {
            struct leptris_document* d = ((LeptrisDocumentNode*)nd)->doc;
            LeptrisNode* start = (LeptrisNode*)d->doc_children_head;
            if (!start) {
                start = (LeptrisNode*)d->new_dom_root;
                if (!start) start = (LeptrisNode*)d->root;
            }
            unsigned count = 0;
            for (LeptrisNode* c = start; c;
                 c = leptris_node_get_next_sibling(c)) {
                if (c->type == LEPTRIS_NODE_TYPE_ELEMENT &&
                    vm_unprefixed_name_matches(ctx, (LeptrisElement)c,
                                               name) &&
                    ++count == pos) {
                    xpath_nodeset_add_fast(out, c);
                    break;
                }
            }
            continue;
        }
        if (!node_is_element(nd)) continue;
        LeptrisElement elem = (LeptrisElement)nd;
        LeptrisElement child = leptris_element_get_first_child(elem);
        unsigned count = 0;
        while (child) {
            if (vm_unprefixed_name_matches(ctx, child, name) &&
                ++count == pos) {
                xpath_nodeset_add_fast(out, child);
                break;
            }
            child = leptris_element_get_next_sibling(child);
        }
    }

    xpath_nodeset_free(input);
    struct leptris_xpath_result* r = xpath_result_new(XPATH_RESULT_NODESET);
    if (!r) { xpath_nodeset_free(out); return NULL; }
    r->value.nodeset_value = out;
    return r;
}

struct leptris_xpath_result* vm_apply_axis_attribute(XPathContext* ctx, XPathVM* vm,
                                                     const char* name, int wild) {
    XPathNodeSet* input = vm_detach_input_nodeset(vm);
    if (!input) return NULL;

    XPathNodeSet* out = xpath_nodeset_new();
    if (!out) { xpath_nodeset_free(input); return NULL; }

    /* Attribute axis produces LeptrisAttributeNode* entries. The
     * result must own them so they're freed when the result is. */
    out->owns_attributes = 1;

    /* A prefixed name test resolves through the binding set and
     * matches (URI, local) — the attribute's own spelling may use a
     * different prefix for the same namespace (libxslt bug-97). */
    const char* tcolon = name ? strchr(name, ':') : NULL;
    const char* test_uri = NULL;
    if (tcolon && ctx->ns_set) {
        test_uri = leptris_xpath_ns_lookup(
            (const struct leptris_xpath_ns_map*)ctx->ns_set,
            name, (size_t)(tcolon - name));
    }

    for (size_t i = 0; i < input->count; i++) {
        if (!node_is_element(input->nodes[i])) continue;
        LeptrisElement elem = (LeptrisElement)input->nodes[i];
        struct leptris_attribute* attr = leptris_element_get_first_attribute(elem);
        while (attr) {
            LeptrisStringView nv = attr->name_view;
            int matches = 0;
            if (wild) {
                matches = 1;
            } else if (test_uri && nv.length > 0 && nv.data) {
                const char* ac =
                    (const char*)memchr(nv.data, ':', nv.length);
                const char* ans = leptris_attribute_namespace_uri(
                    (LeptrisAttribute)attr);
                if (ac && ans) {
                    size_t ll = nv.length - (size_t)(ac - nv.data) - 1;
                    size_t tl = strlen(tcolon + 1);
                    matches =
                        ll == tl &&
                        memcmp(ac + 1, tcolon + 1, tl) == 0 &&
                        strcmp(ans, test_uri) == 0;
                }
            } else if (name && nv.length > 0 && nv.data) {
                matches = (strlen(name) == nv.length &&
                           memcmp(name, nv.data, nv.length) == 0);
            }

            if (matches) {
                /* Allocate a LeptrisAttributeNode mirroring
                 * create_attribute_node in evaluator_axes.c. */
                LeptrisAttributeNode* an = LEPTRIS_ALLOC(LeptrisAttributeNode);
                if (!an) { attr = leptris_attr_next(attr); continue; }
                an->node_type = LEPTRIS_NODE_ATTRIBUTE;

                /* Copy name (single representation: from the view) */
                if (nv.length > 0 && nv.data) {
                    an->name = (char*)malloc(nv.length + 1);
                    if (an->name) {
                        memcpy(an->name, nv.data, nv.length);
                        an->name[nv.length] = '\0';
                    }
                } else {
                    an->name = NULL;
                }

                /* Copy value (single representation: from the view).
                 * Entity-bearing views decode first — the axis must
                 * return the same string the accessor returns (the
                 * raw &#38; bytes double-escaped downstream, bug-59). */
                LeptrisStringView vv = leptris_attr_value_sv(attr);
                if (attr_has_entities(attr) && vv.length > 0 && vv.data) {
                    LeptrisMemoryPool* pool =
                        leptris_element_get_pool(elem);
                    char* dec = pool
                        ? leptris_decode_entities_view(&vv, pool)
                        : NULL;
                    if (dec) {
                        an->value = leptris_strdup(dec);
                    } else {
                        an->value = (char*)malloc(vv.length + 1);
                        if (an->value) {
                            memcpy(an->value, vv.data, vv.length);
                            an->value[vv.length] = '\0';
                        }
                    }
                } else if (vv.length > 0 && vv.data) {
                    an->value = (char*)malloc(vv.length + 1);
                    if (an->value) {
                        memcpy(an->value, vv.data, vv.length);
                        an->value[vv.length] = '\0';
                    }
                } else {
                    an->value = NULL;
                }

                an->namespace_uri = NULL;
                an->owner = elem;

                xpath_nodeset_add_fast(out, an);
            }
            attr = leptris_attr_next(attr);
        }
    }

    xpath_nodeset_free(input);

    struct leptris_xpath_result* r = xpath_result_new(XPATH_RESULT_NODESET);
    if (!r) { xpath_nodeset_free(out); return NULL; }
    r->value.nodeset_value = out;
    return r;
}

struct leptris_xpath_result* vm_apply_axis_self(XPathContext* ctx, XPathVM* vm,
                                                const char* name, int wild) {
    (void)ctx;
    XPathNodeSet* input = vm_detach_input_nodeset(vm);
    if (!input) return NULL;

    XPathNodeSet* out = xpath_nodeset_new();
    if (!out) { xpath_nodeset_free(input); return NULL; }

    for (size_t i = 0; i < input->count; i++) {
        LeptrisNode* nd = input->nodes[i];
        if (!node_is_element(nd)) {
            /* self::node() (wild) passes ANY node through — text/
             * comment/PI/attribute/namespace contexts (e.g. `.` in
             * a for-each over namespace::*). Name tests never match
             * non-elements. */
            if (nd && wild) xpath_nodeset_add_fast(out, nd);
            continue;
        }
        LeptrisElement elem = (LeptrisElement)nd;
        if (wild) {
            xpath_nodeset_add_fast(out, elem);
        } else {
            if (vm_unprefixed_name_matches(ctx, elem, name)) {
                xpath_nodeset_add_fast(out, elem);
            }
        }
    }

    xpath_nodeset_free(input);

    struct leptris_xpath_result* r = xpath_result_new(XPATH_RESULT_NODESET);
    if (!r) { xpath_nodeset_free(out); return NULL; }
    r->value.nodeset_value = out;
    return r;
}

struct leptris_xpath_result* vm_apply_axis_parent(XPathContext* ctx, XPathVM* vm,
                                                  const char* name, int wild) {
    (void)ctx;
    XPathNodeSet* input = vm_detach_input_nodeset(vm);
    if (!input) return NULL;

    XPathNodeSet* out = xpath_nodeset_new();
    if (!out) { xpath_nodeset_free(input); return NULL; }

    /* Parent axis can produce duplicates (multiple children → same
     * parent). Inline a pointer-equality dedup. */
    for (size_t i = 0; i < input->count; i++) {
        LeptrisNode* in = input->nodes[i];
        LeptrisElement parent;
        if (in && in->type == LEPTRIS_NODE_NAMESPACE) {
            /* Namespace nodes' parent is the OWNER element (63). */
            parent = ((LeptrisNamespaceNode*)in)->owner;
        } else if (node_is_element(in)) {
            parent = leptris_element_get_parent((LeptrisElement)in);
        } else {
            /* Text/comment/PI contexts resolve through the
             * any-kind parent (key use expressions, bug-133). */
            parent = leptris_node_parent((LeptrisNodeRef)in);
        }
        if (!parent) continue;

        if (!wild) {
            if (!vm_unprefixed_name_matches(ctx, parent, name)) continue;
        }

        /* Dedup */
        int dup = 0;
        for (size_t j = 0; j < out->count; j++) {
            if (out->nodes[j] == parent) { dup = 1; break; }
        }
        if (!dup) xpath_nodeset_add_fast(out, parent);
    }

    xpath_nodeset_free(input);

    struct leptris_xpath_result* r = xpath_result_new(XPATH_RESULT_NODESET);
    if (!r) { xpath_nodeset_free(out); return NULL; }
    r->value.nodeset_value = out;
    return r;
}

/* Recursive subtree walk for descendant / descendant-or-self.
 * Appends matching elements to `out`. No dedup at this layer —
 * the caller decides based on input size. */
/* Process a single element during a subtree walk: add to out if it
 * matches the name filter. Inlined for tight loop performance. */
static inline void descendant_visit(XPathContext* ctx, XPathNodeSet* out,
                                      LeptrisElement elem,
                                      const char* name, int wild) {
    if (wild) {
        xpath_nodeset_add_fast(out, elem);
    } else if (vm_unprefixed_name_matches(ctx, elem, name)) {
        xpath_nodeset_add_fast(out, elem);
    }
}

/* Iterative pre-order subtree walk (TODO 131). Replaces the previous
 * recursive version which paid function-call overhead per element.
 *
 * The walk uses the tree's own parent / first_child / next_sibling
 * links to track position — no explicit stack needed. For an N-element
 * subtree, total work is O(N) with constant per-element overhead.
 *
 * `name` / `wild` filter which elements get added to `out`.
 * `include_self`: if 1, visit `elem` itself before descending; if 0,
 * skip `elem` (it's been handled by the caller, e.g., vm_apply_absolute
 * adds root separately for descendant-or-self). */
static void descendant_walk(XPathContext* ctx, XPathNodeSet* out, LeptrisElement elem,
                              const char* name, int wild, int include_self) {
    if (!elem) return;

    /* Pre-grow the output nodeset to skip the inline→heap transition
     * that would otherwise trigger on the 17th add. For typical
     * medium docs (~50 elements) this avoids 1-2 grow operations. */
    if (out->capacity < 32) {
        void** new_nodes = (void**)malloc(32 * sizeof(void*));
        if (new_nodes) {
            if (out->count > 0) {
                memcpy(new_nodes, out->nodes, out->count * sizeof(void*));
            }
            /* Inline storage is part of the struct; nothing to free. */
            out->nodes = new_nodes;
            out->capacity = 32;
        }
    }

    if (include_self) {
        descendant_visit(ctx, out, elem, name, wild);
    }

    LeptrisElement cur = leptris_element_get_first_child(elem);
    while (cur) {
        descendant_visit(ctx, out, cur, name, wild);

        LeptrisElement next = leptris_element_get_first_child(cur);
        if (next) {
            cur = next;
            continue;
        }

        /* No child — walk up via parent links until we find a sibling
         * or reach `elem` (the subtree root). */
        while (cur && cur != elem) {
            LeptrisElement sib = leptris_element_get_next_sibling(cur);
            if (sib) {
                cur = sib;
                break;
            }
            cur = leptris_element_get_parent(cur);
        }
        if (cur == elem) break;  /* exhausted subtree */
    }
}

/* Pre-order all-nodes walk: n itself, then (for elements) each
 * subtree. Matches descendant_walk's recursion risk profile. */
static void all_nodes_preorder(XPathNodeSet* out, LeptrisNode* n) {
    xpath_nodeset_add_fast(out, n);
    if (n->type == LEPTRIS_NODE_TYPE_ELEMENT) {
        for (LeptrisNode* c = leptris_elem_first_child((LeptrisElement)n); c;
             c = leptris_node_get_next_sibling(c))
            all_nodes_preorder(out, c);
    }
}

struct leptris_xpath_result* vm_apply_axis_descendant(XPathContext* ctx, XPathVM* vm,
                                                      const char* name, int wild,
                                                      int include_self) {
    XPathNodeSet* input = vm_detach_input_nodeset(vm);
    if (!input) return NULL;

    /* The element index keys buckets on LOCAL names; a prefixed test
     * (issue #630: .//ns:x from element context) resolves the local
     * bucket and filters each match namespace-aware — the raw
     * qualified lookup missed every bucket and returned empty. */
    const char* name_colon = (!wild && name) ? strchr(name, ':') : NULL;
    const char* bucket_key = name_colon ? name_colon + 1 : name;

    XPathNodeSet* out = xpath_nodeset_new();
    if (!out) { xpath_nodeset_free(input); return NULL; }

    /* Fast path: single-element input that IS the document root.
     * Use the element index (TODO 132) for O(K) lookup. */
    LeptrisElement doc_root = (ctx && ctx->document)
        ? (LeptrisElement)ctx->document->new_dom_root : NULL;

    if (input->count == 1 && doc_root &&
        input->nodes[0] == doc_root && node_is_element(input->nodes[0])) {
        struct leptris_element_index* idx = ctx->document->element_index;
        /* TODO 190: build on the SECOND axis query. The build costs
         * two tree walks + per-name bucket allocations; a document
         * that sees one query pays less walking directly. */
        if (!idx && ++ctx->document->axis_query_count >= 2) {
            idx = leptris_element_index_build(ctx->document);
            ctx->document->element_index = idx;
        }
        if (idx) {
            /* Memcpy fast path (TODO 137). Single malloc + memcpy
             * instead of per-element add. Root at all_elements[0]. */
            if (wild) {
                if (include_self) {
                    /* root + all descendants = all_elements */
                    if (idx->all_count > 0) {
                        void** arr = (void**)malloc(idx->all_count * sizeof(void*));
                        if (arr) {
                            memcpy(arr, idx->all_elements, idx->all_count * sizeof(void*));
                            out->nodes = arr;
                            out->count = idx->all_count;
                            out->capacity = idx->all_count;
                        }
                    }
                } else {
                    /* all descendants except root = all_elements[1..] */
                    if (idx->all_count > 1) {
                        size_t n = idx->all_count - 1;
                        void** arr = (void**)malloc(n * sizeof(void*));
                        if (arr) {
                            memcpy(arr, idx->all_elements + 1, n * sizeof(void*));
                            out->nodes = arr;
                            out->count = n;
                            out->capacity = n;
                        }
                    }
                }
            } else {
                const LeptrisElementIndexBucket* bucket =
                    leptris_element_index_lookup(idx, bucket_key);
                if (name_colon) {
                    /* Prefixed test: filter the local bucket's matches
                     * namespace-aware (issue #630). */
                    if (bucket) {
                        for (size_t i = 0; i < bucket->count; i++) {
                            LeptrisElement m = bucket->matches[i];
                            if (!include_self && m == doc_root) continue;
                            if (vm_unprefixed_name_matches(ctx, m, name))
                                xpath_nodeset_add_fast(out, m);
                        }
                    }
                } else if (bucket) {
                    if (include_self) {
                        /* all matches including root */
                        if (bucket->count > 0) {
                            void** arr = (void**)malloc(bucket->count * sizeof(void*));
                            if (arr) {
                                memcpy(arr, bucket->matches, bucket->count * sizeof(void*));
                                out->nodes = arr;
                                out->count = bucket->count;
                                out->capacity = bucket->count;
                            }
                        }
                    } else {
                        /* matches except root */
                        size_t skip = (size_t)-1;
                        for (size_t i = 0; i < bucket->count; i++) {
                            if (bucket->matches[i] == doc_root) { skip = i; break; }
                        }
                        if (skip == (size_t)-1) {
                            if (bucket->count > 0) {
                                void** arr = (void**)malloc(bucket->count * sizeof(void*));
                                if (arr) {
                                    memcpy(arr, bucket->matches, bucket->count * sizeof(void*));
                                    out->nodes = arr;
                                    out->count = bucket->count;
                                    out->capacity = bucket->count;
                                }
                            }
                        } else if (bucket->count > 1) {
                            size_t n = bucket->count - 1;
                            void** arr = (void**)malloc(n * sizeof(void*));
                            if (arr) {
                                size_t k = 0;
                                for (size_t i = 0; i < skip; i++) arr[k++] = bucket->matches[i];
                                for (size_t i = skip + 1; i < bucket->count; i++) arr[k++] = bucket->matches[i];
                                out->nodes = arr;
                                out->count = n;
                                out->capacity = n;
                            }
                        }
                    }
                }
            }

            if (out->count > 0 || !include_self) {
                xpath_nodeset_free(input);
                struct leptris_xpath_result* r = xpath_result_new(XPATH_RESULT_NODESET);
                if (!r) { xpath_nodeset_free(out); return NULL; }
                r->value.nodeset_value = out;
                return r;
            }
        }
        /* Index build failed — fall through to walk. */
    }

    /* TODO 192: subtree-interval index for non-root contexts — the
     * fast path above handled the doc-root input; this one serves
     * .//name and chained a//b steps from any context set. Intervals
     * are preorder ranges; name-bucket matches store their preorder
     * positions, so each context costs a binary search plus its own
     * hits instead of a subtree walk. */
    if (!wild && ctx && ctx->document && input->count > 0) {
        struct leptris_element_index* idx = ctx->document->element_index;
        if (!idx && ++ctx->document->axis_query_count >= 2) {
            idx = leptris_element_index_build(ctx->document);
            ctx->document->element_index = idx;
        }
        if (idx && idx->subtree_end) {
            const LeptrisElementIndexBucket* bucket =
                leptris_element_index_lookup(idx, bucket_key);
            /* No bucket / empty bucket: the name does not occur in
             * this document, so every subtree yields empty — but a
             * partially-built index without positions must fall back. */
            if (!bucket || (bucket->count > 0 && bucket->match_positions)) {
                size_t n_ctx = 0;
                for (size_t i = 0; i < input->count; i++) {
                    if (node_is_element(input->nodes[i])) n_ctx++;
                }
                if (n_ctx > 0) {
                    size_t* los = (size_t*)malloc(n_ctx * sizeof(size_t));
                    size_t* his = (size_t*)malloc(n_ctx * sizeof(size_t));
                    int all_found = (los && his);
                    size_t k = 0;
                    for (size_t i = 0; all_found && i < input->count; i++) {
                        if (!node_is_element(input->nodes[i])) continue;
                        all_found = leptris_element_index_subtree_interval(
                            idx, (LeptrisElement)input->nodes[i],
                            &los[k], &his[k]);
                        if (all_found) k++;
                    }
                    if (all_found) {
                        /* Sort intervals by start; subtrees are either
                         * disjoint or nested, so skipping any interval
                         * contained in a previous one removes all overlap
                         * — no dedup pass needed and output stays in
                         * document order (bucket positions ascend). */
                        for (size_t a = 1; a < k; a++) {
                            size_t l = los[a], h = his[a], b = a;
                            while (b > 0 && los[b - 1] > l) {
                                los[b] = los[b - 1];
                                his[b] = his[b - 1];
                                b--;
                            }
                            los[b] = l;
                            his[b] = h;
                        }
                        size_t prev_hi = 0;
                        int have_prev = 0;
                        for (size_t a = 0; a < k; a++) {
                            if (have_prev && los[a] <= prev_hi) continue;
                            if (bucket && bucket->count > 0) {
                                size_t lo2 = los[a], hi2 = his[a];
                                size_t l = 0, r = bucket->count;
                                while (l < r) {
                                    size_t mid = l + (r - l) / 2;
                                    if (bucket->match_positions[mid] < lo2) {
                                        l = mid + 1;
                                    } else {
                                        r = mid;
                                    }
                                }
                                for (size_t j = l;
                                     j < bucket->count &&
                                     bucket->match_positions[j] <= hi2;
                                     j++) {
                                    if (!include_self &&
                                        bucket->match_positions[j] == lo2) {
                                        continue;
                                    }
                                    if (name_colon &&
                                        !vm_unprefixed_name_matches(
                                            ctx,
                                            (LeptrisElement)bucket->matches[j],
                                            name)) {
                                        continue;
                                    }
                                    xpath_nodeset_add_fast(
                                        out, bucket->matches[j]);
                                }
                            }
                            prev_hi = his[a];
                            have_prev = 1;
                        }
                        free(los);
                        free(his);
                        xpath_nodeset_free(input);
                        struct leptris_xpath_result* r =
                            xpath_result_new(XPATH_RESULT_NODESET);
                        if (!r) {
                            xpath_nodeset_free(out);
                            return NULL;
                        }
                        r->value.nodeset_value = out;
                        return r;
                    }
                    free(los);
                    free(his);
                }
            }
        }
    }

    /* General path: walk subtrees from each input element. */
    int need_dedup = (input->count > 1);

    for (size_t i = 0; i < input->count; i++) {
        if (!node_is_element(input->nodes[i])) continue;
        LeptrisElement elem = (LeptrisElement)input->nodes[i];

        size_t mark = out->count;
        descendant_walk(ctx, out, elem, name, wild, include_self);

        if (need_dedup) {
            for (size_t k = mark; k < out->count; k++) {
                int dup = 0;
                for (size_t j = 0; j < mark; j++) {
                    if (out->nodes[j] == out->nodes[k]) {
                        dup = 1;
                        break;
                    }
                }
                if (dup) {
                    out->nodes[k] = out->nodes[out->count - 1];
                    out->count--;
                    k--;
                }
            }
        }
    }

    xpath_nodeset_free(input);

    struct leptris_xpath_result* r = xpath_result_new(XPATH_RESULT_NODESET);
    if (!r) { xpath_nodeset_free(out); return NULL; }
    r->value.nodeset_value = out;
    return r;
}

/* Fused axis+predicate handler (TODO 134). Walks descendant::* and
 * filters by attribute in a single pass — no intermediate nodeset.
 * When input is the document root, uses the attribute index for
 * O(K) lookup where K = match count.
 *
 * `value_match`: if 0, presence check (attr exists). If 1, equality
 * check (attr value == attr_value). */

/* Attribute-predicate matcher — pulled out of the previous macro so
 * the file compiles under MSVC (which rejects GCC statement-expression
 * extension `({ ... })`). Walks the element's attribute list once. */
static int attr_pred_match(LeptrisElement e,
                           const char* attr_name, size_t attr_name_len,
                           int value_match,
                           const char* attr_value, size_t value_len) {
    struct leptris_attribute* a = leptris_element_get_first_attribute(e);
    while (a) {
        LeptrisStringView nv = a->name_view;
        if (attr_name && nv.length == attr_name_len && nv.length > 0 && nv.data &&
            memcmp(attr_name, nv.data, nv.length) == 0) {
            if (!value_match) return 1;
            LeptrisStringView vv = leptris_attr_value_sv(a);
            if (vv.length == value_len && vv.length > 0 && vv.data &&
                memcmp(attr_value, vv.data, vv.length) == 0) {
                return 1;
            }
        }
        a = leptris_attr_next(a);
    }
    return 0;
}

static struct leptris_xpath_result* vm_apply_axis_descendant_pred_attr(
    XPathContext* ctx, XPathVM* vm,
    const char* attr_name, const char* attr_value,
    int value_match, int include_self) {

    XPathNodeSet* input = vm_detach_input_nodeset(vm);
    if (!input) return NULL;

    XPathNodeSet* out = xpath_nodeset_new();
    if (!out) { xpath_nodeset_free(input); return NULL; }

    LeptrisElement doc_root = (ctx && ctx->document)
        ? (LeptrisElement)ctx->document->new_dom_root : NULL;

    /* Index fast path: single-element input that IS the document root.
     * Look up attr_bucket directly. */
    if (input->count == 1 && doc_root &&
        input->nodes[0] == doc_root && node_is_element(input->nodes[0])) {

        struct leptris_element_index* idx = ctx->document->element_index;
        /* TODO 190: build on the SECOND axis query. The build costs
         * two tree walks + per-name bucket allocations; a document
         * that sees one query pays less walking directly. */
        if (!idx && ++ctx->document->axis_query_count >= 2) {
            idx = leptris_element_index_build(ctx->document);
            ctx->document->element_index = idx;
        }
        if (idx) {
            const LeptrisElementIndexAttrBucket* abucket =
                leptris_element_index_lookup_attr(idx, attr_name);
            if (abucket) {
                if (!value_match) {
                    /* [@attr] — return all matches (root is in the
                     * bucket iff it has the attr; same for both
                     * descendant and descendant-or-self). */
                    for (size_t i = 0; i < abucket->count; i++) {
                        if (!include_self && abucket->matches[i] == doc_root) continue;
                        xpath_nodeset_add_fast(out, abucket->matches[i]);
                    }
                } else {
                    const LeptrisElementIndexAttrValue* vbucket =
                        leptris_element_index_attr_lookup_value(abucket, attr_value);
                    if (vbucket) {
                        for (size_t i = 0; i < vbucket->count; i++) {
                            if (!include_self && vbucket->matches[i] == doc_root) continue;
                            xpath_nodeset_add_fast(out, vbucket->matches[i]);
                        }
                    }
                }
            }
            xpath_nodeset_free(input);
            struct leptris_xpath_result* r = xpath_result_new(XPATH_RESULT_NODESET);
            if (!r) { xpath_nodeset_free(out); return NULL; }
            r->value.nodeset_value = out;
            return r;
        }
        /* Index build failed — fall through to walk+filter. */
    }

    /* Walk+filter fallback. Single pass: visit each descendant and
     * check the attribute inline. Avoids the intermediate nodeset
     * that the two-opcode form would allocate. */
    size_t attr_name_len = attr_name ? strlen(attr_name) : 0;
    size_t value_len = attr_value ? strlen(attr_value) : 0;

    /* Helper: check if `e` has the matching attr. */
    #define ATTR_MATCHES(e) \
        attr_pred_match((e), attr_name, attr_name_len, \
                        value_match, attr_value, value_len)

    for (size_t i = 0; i < input->count; i++) {
        if (!node_is_element(input->nodes[i])) continue;
        LeptrisElement elem = (LeptrisElement)input->nodes[i];

        /* include_self: check the input element itself. */
        if (include_self && ATTR_MATCHES(elem)) {
            xpath_nodeset_add_fast(out, elem);
        }

        /* Inline subtree walk + attr filter. */
        LeptrisElement cur = leptris_element_get_first_child(elem);
        while (cur) {
            if (ATTR_MATCHES(cur)) xpath_nodeset_add_fast(out, cur);

            /* Descend or backtrack. */
            LeptrisElement next = leptris_element_get_first_child(cur);
            if (next) {
                cur = next;
                continue;
            }
            while (cur && cur != elem) {
                LeptrisElement sib = leptris_element_get_next_sibling(cur);
                if (sib) { cur = sib; break; }
                cur = leptris_element_get_parent(cur);
            }
            if (cur == elem) break;
        }
    }
    #undef ATTR_MATCHES

    xpath_nodeset_free(input);

    struct leptris_xpath_result* r = xpath_result_new(XPATH_RESULT_NODESET);
    if (!r) { xpath_nodeset_free(out); return NULL; }
    r->value.nodeset_value = out;
    return r;
}

/* Numeric relational comparison for the any-pair loops below. */
static int vm_relational_cmp(XPathOperatorType op, double a, double b) {
    switch (op) {
        case XPATH_OP_LESS:          return a <  b;
        case XPATH_OP_LESS_EQUAL:    return a <= b;
        case XPATH_OP_GREATER:       return a >  b;
        case XPATH_OP_GREATER_EQUAL: return a >= b;
        default:                     return 0;
    }
}

/* Inline binary-operator dispatch. Pops right then left, computes,
 * pushes the result. Returns 0 on success, -1 on error.
 *
 * Short-circuit semantics for AND/OR: this implementation evaluates
 * both operands eagerly. XPath has no side effects, so the only
 * cost is the redundant work, which is rare in real expressions. */
static int vm_apply_binary_op(XPathVM* vm, XPathContext* ctx,
                               XPathOperatorType op) {
    struct leptris_xpath_result* right = vm_pop(vm);
    struct leptris_xpath_result* left  = vm_pop(vm);
    if (!left || !right) {
        if (left) xpath_result_free(left);
        if (right) xpath_result_free(right);
        return -1;
    }

    struct leptris_xpath_result* result = NULL;

    /* Boolean operators. */
    if (op == XPATH_OP_AND) {
        result = xpath_result_new(XPATH_RESULT_BOOLEAN);
        if (result) result->value.boolean_value =
            xpath_to_boolean(left) && xpath_to_boolean(right);
    } else if (op == XPATH_OP_OR) {
        result = xpath_result_new(XPATH_RESULT_BOOLEAN);
        if (result) result->value.boolean_value =
            xpath_to_boolean(left) || xpath_to_boolean(right);
    }
    /* Arithmetic. */
    else if (op == XPATH_OP_PLUS || op == XPATH_OP_MINUS ||
             op == XPATH_OP_MULTIPLY || op == XPATH_OP_DIV ||
             op == XPATH_OP_MOD) {
        double lval = xpath_to_number(left);
        double rval = xpath_to_number(right);
        result = xpath_result_new(XPATH_RESULT_NUMBER);
        if (result) {
            switch (op) {
                case XPATH_OP_PLUS:     result->value.number_value = lval + rval; break;
                case XPATH_OP_MINUS:    result->value.number_value = lval - rval; break;
                case XPATH_OP_MULTIPLY: result->value.number_value = lval * rval; break;
                case XPATH_OP_DIV:      result->value.number_value = lval / rval; break;
                case XPATH_OP_MOD:      result->value.number_value = fmod(lval, rval); break;
                default: break;
            }
        }
    }
    /* Unary negation — right is unused (compiler emits only one operand). */
    else if (op == XPATH_OP_NEGATION) {
        double lval = xpath_to_number(left);
        result = xpath_result_new(XPATH_RESULT_NUMBER);
        if (result) result->value.number_value = -lval;
    }
    /* Union — both must be nodesets; concatenate with dedup.
     * XPath requires no duplicates and document order; we dedup but
     * don't enforce document order (the existing evaluator doesn't
     * either — see compare_document_order note in evaluator_operators.c). */
    else if (op == XPATH_OP_UNION) {
        result = xpath_result_new(XPATH_RESULT_NODESET);
        if (result) {
            result->value.nodeset_value = xpath_nodeset_new();
            if (result->value.nodeset_value) {
                XPathNodeSet* ln = left->type == XPATH_RESULT_NODESET
                                   ? left->value.nodeset_value : NULL;
                XPathNodeSet* rn = right->type == XPATH_RESULT_NODESET
                                   ? right->value.nodeset_value : NULL;
                /* Concatenate both sides; the sort dedups adjacent
                 * duplicates. The old per-candidate linear duplicate
                 * scan was O(n^2) — 99% of //name | //item time on
                 * 20k-element documents. */
                if (ln) for (size_t i = 0; i < ln->count; i++)
                    xpath_nodeset_add(result->value.nodeset_value, ln->nodes[i]);
                if (rn) for (size_t i = 0; i < rn->count; i++)
                    xpath_nodeset_add_fast(result->value.nodeset_value, rn->nodes[i]);
                /* Document order per XPath 1.0 — the merged order is
                 * left-then-right append, not document order (issue
                 * #485). */
                xpath_nodeset_sort_doc_order(
                    ctx, result->value.nodeset_value, 0);

                /* Issue #514: transfer synthetic-node ownership from
                 * the operands — see the twin fix in
                 * evaluator_operators.c. */
                if (ln && rn) {
                    result->value.nodeset_value->owns_attributes =
                        ln->owns_attributes || rn->owns_attributes;
                    result->value.nodeset_value->owns_namespaces =
                        ln->owns_namespaces || rn->owns_namespaces;
                    result->value.nodeset_value->owns_synthetic_text =
                        ln->owns_synthetic_text ||
                        rn->owns_synthetic_text;
                    ln->owns_attributes = 0;
                    ln->owns_namespaces = 0;
                    ln->owns_synthetic_text = 0;
                    rn->owns_attributes = 0;
                    rn->owns_namespaces = 0;
                    rn->owns_synthetic_text = 0;
                }
            }
        }
    }
    /* Comparisons. */
    else if (op == XPATH_OP_EQUAL || op == XPATH_OP_NOT_EQUAL ||
             op == XPATH_OP_LESS || op == XPATH_OP_LESS_EQUAL ||
             op == XPATH_OP_GREATER || op == XPATH_OP_GREATER_EQUAL) {
        /* XPath comparison semantics depend on operand types:
         *   - nodeset vs nodeset: any-pair match
         *   - nodeset vs number/string/boolean: any-node match
         *   - otherwise: convert both to number and compare
         * A nodeset operand needs the §3.4 any-node loop — the old
         * first-node shortcut silently broke variable nodesets on
         * the VM path (libxslt bug-76: union of $var nodesets). */
        int matches = 0;
        int l_is_ns = (left->type == XPATH_RESULT_NODESET);
        int r_is_ns = (right->type == XPATH_RESULT_NODESET);
        if (l_is_ns || r_is_ns) {
            int relational = (op != XPATH_OP_EQUAL && op != XPATH_OP_NOT_EQUAL);
            int negate = (op == XPATH_OP_NOT_EQUAL);
            int boolean_side = (l_is_ns ? right->type : left->type)
                                   == XPATH_RESULT_BOOLEAN;
            XPathNodeSet* ns = l_is_ns ? left->value.nodeset_value
                                       : right->value.nodeset_value;
            if (boolean_side && !relational) {
                /* boolean vs nodeset: boolean(nodeset) OP boolean. */
                int lb = xpath_to_boolean(left);
                int rb = xpath_to_boolean(right);
                matches = negate ? (lb != rb) : (lb == rb);
            } else if (l_is_ns && r_is_ns) {
                XPathNodeSet* other = right->value.nodeset_value;
                for (size_t i = 0; !matches && ns && i < ns->count; i++) {
                    char* a = get_node_text(ns->nodes[i]);
                    if (!a) continue;
                    for (size_t j = 0; !matches && other && j < other->count; j++) {
                        char* b = get_node_text(other->nodes[j]);
                        if (!b) continue;
                        if (relational) {
                            double av = atof(a);
                            double bv = atof(b);
                            matches = vm_relational_cmp(op, av, bv);
                        } else {
                            matches = negate ? (strcmp(a, b) != 0)
                                             : (strcmp(a, b) == 0);
                        }
                        free(b);
                    }
                    free(a);
                }
            } else {
                /* nodeset vs scalar: per-node string (equality) or
                 * number (relational) compare against the scalar. */
                if (relational) {
                    /* Operand order is part of the semantics (#965):
                     * nodeset on the RIGHT compares scalar op node. */
                    double scalar = xpath_to_number(
                        l_is_ns ? right : left);
                    for (size_t i = 0; !matches && ns && i < ns->count; i++) {
                        char* a = get_node_text(ns->nodes[i]);
                        if (!a) continue;
                        matches = l_is_ns
                            ? vm_relational_cmp(op, atof(a), scalar)
                            : vm_relational_cmp(op, scalar, atof(a));
                        free(a);
                    }
                } else {
                    char* scalar = xpath_to_string(l_is_ns ? right : left);
                    for (size_t i = 0; !matches && ns && scalar &&
                             i < ns->count; i++) {
                        char* a = get_node_text(ns->nodes[i]);
                        if (!a) continue;
                        matches = negate ? (strcmp(a, scalar) != 0)
                                         : (strcmp(a, scalar) == 0);
                        free(a);
                    }
                    if (scalar) free(scalar);
                }
            }
        } else {
        double lnum = xpath_to_number(left);
        double rnum = xpath_to_number(right);
        char* lstr_owned = xpath_to_string(left);
        char* rstr_owned = xpath_to_string(right);
        int eq_str = (lstr_owned && rstr_owned &&
                      strcmp(lstr_owned, rstr_owned) == 0);
        int eq_num = (lnum == rnum);
        int eq = eq_str || eq_num;
        if (lstr_owned) free(lstr_owned);
        if (rstr_owned) free(rstr_owned);

        switch (op) {
            case XPATH_OP_EQUAL:         matches = eq; break;
            case XPATH_OP_NOT_EQUAL:     matches = !eq; break;
            case XPATH_OP_LESS:          matches = (lnum <  rnum); break;
            case XPATH_OP_LESS_EQUAL:    matches = (lnum <= rnum); break;
            case XPATH_OP_GREATER:       matches = (lnum >  rnum); break;
            case XPATH_OP_GREATER_EQUAL: matches = (lnum >= rnum); break;
            default: break;
        }
        }
        result = xpath_result_new(XPATH_RESULT_BOOLEAN);
        if (result) result->value.boolean_value = matches;
        (void)ctx;
    }

    xpath_result_free(left);
    xpath_result_free(right);

    if (!result) { vm->error = 1; return -1; }
    vm_push(vm, result);
    return 0;
}



static struct leptris_xpath_result* vm_run(LeptrisXPathBytecode* bc,
                                           XPathContext* ctx) {
    XPathVM vm = {0};
    const unsigned char* pc = bc->code;
    const unsigned char* end = bc->code + bc->code_len;

    while (pc < end && !vm.error) {
        XPathOpcode op = (XPathOpcode)*pc++;
        switch (op) {
            case XPATH_BC_NOP:
                break;

            case XPATH_BC_LITERAL_NUMBER: {
                uint16_t idx = read_u16(&pc);
                if (idx >= bc->const_count) { vm.error = 1; break; }
                struct leptris_xpath_result* r =
                    xpath_result_new(XPATH_RESULT_NUMBER);
                if (!r) { vm.error = 1; break; }
                r->value.number_value = bc->constants[idx].v.number;
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_LITERAL_STRING: {
                uint16_t idx = read_u16(&pc);
                if (idx >= bc->const_count) { vm.error = 1; break; }
                struct leptris_xpath_result* r =
                    xpath_result_new(XPATH_RESULT_STRING);
                if (!r) { vm.error = 1; break; }
                r->value.string_value =
                    leptris_strdup(bc->constants[idx].v.string);
                if (!r->value.string_value) {
                    xpath_result_free(r);
                    vm.error = 1;
                    break;
                }
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_LITERAL_BOOL: {
                uint8_t b = read_u8(&pc);
                struct leptris_xpath_result* r =
                    xpath_result_new(XPATH_RESULT_BOOLEAN);
                if (!r) { vm.error = 1; break; }
                r->value.boolean_value = b ? 1 : 0;
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_PATH_ABSOLUTE: {
                LeptrisElement root =
                    (LeptrisElement)ctx->document->new_dom_root;
                struct leptris_xpath_result* r = make_singleton_nodeset(root);
                if (!r) { vm.error = 1; break; }
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_PATH_RELATIVE: {
                struct leptris_xpath_result* r =
                    make_singleton_nodeset(ctx->context_node);
                if (!r) { vm.error = 1; break; }
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_AXIS_STEP: {
                uint16_t idx = read_u16(&pc);
                if (idx >= bc->const_count ||
                    bc->constants[idx].type != XPATH_CONST_AST_NODE) {
                    vm.error = 1;
                    break;
                }
                XPathASTNode* step_ast = bc->constants[idx].v.ast;

                struct leptris_xpath_result* input = vm_pop(&vm);
                if (!input || input->type != XPATH_RESULT_NODESET) {
                    if (input) xpath_result_free(input);
                    vm.error = 1;
                    break;
                }

                XPathNodeSet* input_ns = input->value.nodeset_value;
                input->value.nodeset_value = NULL;
                xpath_result_free(input);

                struct leptris_xpath_result* step_result =
                    evaluate_step(ctx, step_ast, input_ns);
                xpath_nodeset_free(input_ns);
                if (!step_result) { vm.error = 1; break; }
                vm_push(&vm, step_result);
                break;
            }

            /* Specialized axis handlers (TODO 126). Each is a tight
             * loop over the input nodeset that bypasses the
             * evaluate_step scaffolding. The handlers cover the
             * common shapes: single name test or wildcard, no
             * namespace prefix, no predicates. Anything else
             * stays on BC_AXIS_STEP. */

            case XPATH_BC_AXIS_CHILD_NAME: {
                uint16_t idx = read_u16(&pc);
                const char* name = (idx < bc->const_count &&
                                    bc->constants[idx].type == XPATH_CONST_STRING)
                                   ? bc->constants[idx].v.string : NULL;
                struct leptris_xpath_result* r =
                    vm_apply_axis_child(ctx, &vm, name, 0);
                if (!r) { vm.error = 1; break; }
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_AXIS_CHILD_WILD: {
                struct leptris_xpath_result* r =
                    vm_apply_axis_child(ctx, &vm, NULL, 1);
                if (!r) { vm.error = 1; break; }
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_AXIS_CHILD_NAME_POS: {
                uint16_t name_idx = read_u16(&pc);
                uint16_t pos = read_u16(&pc);
                const char* name =
                    (name_idx < bc->const_count &&
                     bc->constants[name_idx].type == XPATH_CONST_STRING)
                        ? bc->constants[name_idx].v.string : NULL;
                struct leptris_xpath_result* r =
                    vm_apply_axis_child_name_pos(ctx, &vm, name, pos);
                if (!r) { vm.error = 1; break; }
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_AXIS_ATTRIBUTE_NAME: {
                uint16_t idx = read_u16(&pc);
                const char* name = (idx < bc->const_count &&
                                    bc->constants[idx].type == XPATH_CONST_STRING)
                                   ? bc->constants[idx].v.string : NULL;
                struct leptris_xpath_result* r =
                    vm_apply_axis_attribute(ctx, &vm, name, 0);
                if (!r) { vm.error = 1; break; }
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_AXIS_ATTRIBUTE_WILD: {
                struct leptris_xpath_result* r =
                    vm_apply_axis_attribute(ctx, &vm, NULL, 1);
                if (!r) { vm.error = 1; break; }
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_AXIS_SELF_NAME: {
                uint16_t idx = read_u16(&pc);
                const char* name = (idx < bc->const_count &&
                                    bc->constants[idx].type == XPATH_CONST_STRING)
                                   ? bc->constants[idx].v.string : NULL;
                struct leptris_xpath_result* r =
                    vm_apply_axis_self(ctx, &vm, name, 0);
                if (!r) { vm.error = 1; break; }
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_AXIS_SELF_WILD: {
                struct leptris_xpath_result* r =
                    vm_apply_axis_self(ctx, &vm, NULL, 1);
                if (!r) { vm.error = 1; break; }
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_AXIS_PARENT_NAME: {
                uint16_t idx = read_u16(&pc);
                const char* name = (idx < bc->const_count &&
                                    bc->constants[idx].type == XPATH_CONST_STRING)
                                   ? bc->constants[idx].v.string : NULL;
                struct leptris_xpath_result* r =
                    vm_apply_axis_parent(ctx, &vm, name, 0);
                if (!r) { vm.error = 1; break; }
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_AXIS_PARENT_WILD: {
                struct leptris_xpath_result* r =
                    vm_apply_axis_parent(ctx, &vm, NULL, 1);
                if (!r) { vm.error = 1; break; }
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_AXIS_DESCENDANT_NAME: {
                uint16_t idx = read_u16(&pc);
                const char* name = (idx < bc->const_count &&
                                    bc->constants[idx].type == XPATH_CONST_STRING)
                                   ? bc->constants[idx].v.string : NULL;
                struct leptris_xpath_result* r =
                    vm_apply_axis_descendant(ctx, &vm, name, 0, 0);
                if (!r) { vm.error = 1; break; }
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_AXIS_DESCENDANT_WILD: {
                struct leptris_xpath_result* r =
                    vm_apply_axis_descendant(ctx, &vm, NULL, 1, 0);
                if (!r) { vm.error = 1; break; }
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_AXIS_DESCENDANT_NAME_ATTREQ: {
                /* TODO 192c: relative descendant::name[@attr='value']
                 * served from the index — each context element's
                 * subtree interval windows the attr-VALUE bucket's
                 * preorder positions. O(log B + hits) per context. */
                uint16_t name_idx = read_u16(&pc);
                uint16_t attr_idx = read_u16(&pc);
                uint16_t value_idx = read_u16(&pc);
                const char* name = (name_idx < bc->const_count &&
                                    bc->constants[name_idx].type == XPATH_CONST_STRING)
                                   ? bc->constants[name_idx].v.string : NULL;
                const char* attr_name = (attr_idx < bc->const_count &&
                                         bc->constants[attr_idx].type == XPATH_CONST_STRING)
                                        ? bc->constants[attr_idx].v.string : NULL;
                /* 0xFFFF = no value operand: attr-EXISTS predicate. */
                const char* value = (value_idx != 0xFFFF &&
                                     value_idx < bc->const_count &&
                                     bc->constants[value_idx].type == XPATH_CONST_STRING)
                                    ? bc->constants[value_idx].v.string : NULL;
                if (!name || !attr_name || (value_idx != 0xFFFF && !value)) {
                    vm.error = 1;
                    break;
                }

                XPathNodeSet* input = vm_detach_input_nodeset(&vm);
                if (!input) { vm.error = 1; break; }
                XPathNodeSet* out = xpath_nodeset_new();
                if (!out) { xpath_nodeset_free(input); vm.error = 1; break; }

                struct leptris_element_index* index =
                    (ctx && ctx->document) ? ctx->document->element_index : NULL;
                if (!index && ctx && ctx->document &&
                    ++ctx->document->axis_query_count >= 2) {
                    index = leptris_element_index_build(ctx->document);
                    ctx->document->element_index = index;
                }

                int served = 0;
                if (index && index->subtree_end) {
                    const LeptrisElementIndexAttrBucket* abucket =
                        leptris_element_index_lookup_attr(index, attr_name);
                    /* Equality reads the value bucket; existence reads
                     * the any-value bucket (TODO 192e). No bucket for
                     * this attribute anywhere: empty answer. A bucket
                     * without positions must fall back. */
                    const LeptrisElement* bucket_matches = NULL;
                    const size_t* bucket_positions = NULL;
                    size_t bucket_count = 0;
                    int bucket_valid = 0;
                    if (value) {
                        const LeptrisElementIndexAttrValue* vbucket =
                            abucket ? leptris_element_index_attr_lookup_value(abucket, value)
                                    : NULL;
                        if (!vbucket ||
                            (vbucket->count > 0 && vbucket->match_positions)) {
                            bucket_valid = 1;
                            if (vbucket) {
                                bucket_matches = vbucket->matches;
                                bucket_positions = vbucket->match_positions;
                                bucket_count = vbucket->count;
                            }
                        }
                    } else if (!abucket ||
                               (abucket->count > 0 && abucket->match_positions)) {
                        bucket_valid = 1;
                        if (abucket) {
                            bucket_matches = abucket->matches;
                            bucket_positions = abucket->match_positions;
                            bucket_count = abucket->count;
                        }
                    }
                    if (bucket_valid) {
                        size_t n_ctx = 0;
                        for (size_t i = 0; i < input->count; i++) {
                            if (node_is_element(input->nodes[i])) n_ctx++;
                        }
                        size_t* los = (n_ctx > 0)
                            ? (size_t*)malloc(n_ctx * sizeof(size_t)) : NULL;
                        size_t* his = (n_ctx > 0)
                            ? (size_t*)malloc(n_ctx * sizeof(size_t)) : NULL;
                        int all_found = (n_ctx == 0) || (los && his);
                        size_t k = 0;
                        for (size_t i = 0; all_found && i < input->count; i++) {
                            if (!node_is_element(input->nodes[i])) continue;
                            all_found = leptris_element_index_subtree_interval(
                                index, (LeptrisElement)input->nodes[i],
                                &los[k], &his[k]);
                            if (all_found) k++;
                        }
                        if (all_found) {
                            /* Same ordering argument as the name path:
                             * bucket positions ascend, subtrees are
                             * disjoint-or-nested, skip contained ones. */
                            for (size_t a = 1; a < k; a++) {
                                size_t l = los[a], h = his[a], b = a;
                                while (b > 0 && los[b - 1] > l) {
                                    los[b] = los[b - 1];
                                    his[b] = his[b - 1];
                                    b--;
                                }
                                los[b] = l;
                                his[b] = h;
                            }
                            size_t prev_hi = 0;
                            int have_prev = 0;
                            for (size_t a = 0; a < k; a++) {
                                if (have_prev && los[a] <= prev_hi) continue;
                                if (bucket_count > 0) {
                                    size_t lo2 = los[a], hi2 = his[a];
                                    size_t l = 0, r = bucket_count;
                                    while (l < r) {
                                        size_t mid = l + (r - l) / 2;
                                        if (bucket_positions[mid] < lo2) {
                                            l = mid + 1;
                                        } else {
                                            r = mid;
                                        }
                                    }
                                    for (size_t j = l;
                                         j < bucket_count &&
                                         bucket_positions[j] <= hi2;
                                         j++) {
                                        if (vm_unprefixed_name_matches(
                                                ctx, bucket_matches[j], name)) {
                                            xpath_nodeset_add_fast(
                                                out, bucket_matches[j]);
                                        }
                                    }
                                }
                                prev_hi = his[a];
                                have_prev = 1;
                            }
                            served = 1;
                        }
                        free(los);
                        free(his);
                    }
                }

                if (!served) {
                    /* Fallback: subtree walk + inline attr filter
                     * (hash-prefiltered walk, the PRED_ATTR_EXISTS
                     * handler's pattern). */
                    uint16_t attr_hash =
                        attr_hash15(attr_name, strlen(attr_name));
                    for (size_t i = 0; i < input->count; i++) {
                        if (!node_is_element(input->nodes[i])) continue;
                        LeptrisElement elem = (LeptrisElement)input->nodes[i];
                        size_t mark = out->count;
                        descendant_walk(ctx, out, elem, name, 0, 0);
                        for (size_t w = mark; w < out->count;) {
                            LeptrisElement e2 = (LeptrisElement)out->nodes[w];
                            const char* v = NULL;
                            for (struct leptris_attribute* a =
                                     leptris_element_get_first_attribute(e2);
                                 a; a = leptris_attr_next(a)) {
                                if (attr_name_hash(a) == attr_hash &&
                                    attr_cname(a) &&
                                    strcmp(attr_cname(a), attr_name) == 0) {
                                    v = attr_cvalue(a);
                                    break;
                                }
                            }
                            if (value ? (v && strcmp(v, value) == 0)
                                      : (v != NULL)) {
                                w++;
                            } else {
                                out->nodes[w] = out->nodes[out->count - 1];
                                out->count--;
                            }
                        }
                    }
                }

                xpath_nodeset_free(input);
                struct leptris_xpath_result* r =
                    xpath_result_new(XPATH_RESULT_NODESET);
                if (!r) { xpath_nodeset_free(out); vm.error = 1; break; }
                r->value.nodeset_value = out;
                vm_push(&vm, r);
                break;
            }

            /* Fused axis+predicate (TODO 134). */
            case XPATH_BC_AXIS_DESCENDANT_WILD_PRED_ATTR_EXISTS: {
                uint16_t idx = read_u16(&pc);
                const char* attr_name = (idx < bc->const_count &&
                                          bc->constants[idx].type == XPATH_CONST_STRING)
                                         ? bc->constants[idx].v.string : NULL;
                struct leptris_xpath_result* r =
                    vm_apply_axis_descendant_pred_attr(ctx, &vm, attr_name, NULL, 0, 0);
                if (!r) { vm.error = 1; break; }
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_AXIS_DESCENDANT_WILD_PRED_ATTR_EQ_STRING: {
                uint16_t name_idx = read_u16(&pc);
                uint16_t value_idx = read_u16(&pc);
                const char* attr_name = (name_idx < bc->const_count &&
                                          bc->constants[name_idx].type == XPATH_CONST_STRING)
                                         ? bc->constants[name_idx].v.string : NULL;
                const char* attr_value = (value_idx < bc->const_count &&
                                           bc->constants[value_idx].type == XPATH_CONST_STRING)
                                          ? bc->constants[value_idx].v.string : NULL;
                struct leptris_xpath_result* r =
                    vm_apply_axis_descendant_pred_attr(ctx, &vm, attr_name, attr_value, 1, 0);
                if (!r) { vm.error = 1; break; }
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_AXIS_DESCENDANT_OR_SELF_WILD_PRED_ATTR_EXISTS: {
                uint16_t idx = read_u16(&pc);
                const char* attr_name = (idx < bc->const_count &&
                                          bc->constants[idx].type == XPATH_CONST_STRING)
                                         ? bc->constants[idx].v.string : NULL;
                struct leptris_xpath_result* r =
                    vm_apply_axis_descendant_pred_attr(ctx, &vm, attr_name, NULL, 0, 1);
                if (!r) { vm.error = 1; break; }
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_AXIS_DESCENDANT_OR_SELF_WILD_PRED_ATTR_EQ_STRING: {
                uint16_t name_idx = read_u16(&pc);
                uint16_t value_idx = read_u16(&pc);
                const char* attr_name = (name_idx < bc->const_count &&
                                          bc->constants[name_idx].type == XPATH_CONST_STRING)
                                         ? bc->constants[name_idx].v.string : NULL;
                const char* attr_value = (value_idx < bc->const_count &&
                                           bc->constants[value_idx].type == XPATH_CONST_STRING)
                                          ? bc->constants[value_idx].v.string : NULL;
                struct leptris_xpath_result* r =
                    vm_apply_axis_descendant_pred_attr(ctx, &vm, attr_name, attr_value, 1, 1);
                if (!r) { vm.error = 1; break; }
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_AXIS_DESCENDANT_OR_SELF_NAME: {
                uint16_t idx = read_u16(&pc);
                const char* name = (idx < bc->const_count &&
                                    bc->constants[idx].type == XPATH_CONST_STRING)
                                   ? bc->constants[idx].v.string : NULL;
                struct leptris_xpath_result* r =
                    vm_apply_axis_descendant(ctx, &vm, name, 0, 1);
                if (!r) { vm.error = 1; break; }
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_AXIS_DESCENDANT_OR_SELF_WILD: {
                struct leptris_xpath_result* r =
                    vm_apply_axis_descendant(ctx, &vm, NULL, 1, 1);
                if (!r) { vm.error = 1; break; }
                vm_push(&vm, r);
                break;
            }

            /* Absolute-path first-step opcodes (TODO 129). Each
             * pushes a nodeset directly — no pop. These start a
             * new path evaluation from the document root. */
            case XPATH_BC_ABSOLUTE_ROOT_MATCH_NAME: {
                uint16_t idx = read_u16(&pc);
                const char* name = (idx < bc->const_count &&
                                    bc->constants[idx].type == XPATH_CONST_STRING)
                                   ? bc->constants[idx].v.string : NULL;
                /* `/foo` = root if root.name == foo. No descendants. */
                struct leptris_xpath_result* r =
                    vm_apply_absolute(ctx, name, 0, 0 /* root-match only */);
                if (!r) { vm.error = 1; break; }
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_ABSOLUTE_ROOT_MATCH_WILD: {
                /* Absolute root with wildcard match = root node. */
                struct leptris_xpath_result* r =
                    vm_apply_absolute(ctx, NULL, 1, 0);
                if (!r) { vm.error = 1; break; }
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_ABSOLUTE_DESCENDANT_NAME: {
                uint16_t idx = read_u16(&pc);
                const char* name = (idx < bc->const_count &&
                                    bc->constants[idx].type == XPATH_CONST_STRING)
                                   ? bc->constants[idx].v.string : NULL;
                /* `/descendant::foo`: the context is the DOCUMENT node,
                 * so the root element is itself a descendant — mode 2
                 * includes it (mode 1's root-skip implemented element-
                 * relative semantics; NsAbsolutePaths regression). */
                struct leptris_xpath_result* r =
                    vm_apply_absolute(ctx, name, 0, 2);
                if (!r) { vm.error = 1; break; }
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_ABSOLUTE_DESCENDANT_TYPE: {
                uint16_t idx = read_u16(&pc);
                uint16_t tgt = read_u16(&pc);
                const char* type_name = (idx < bc->const_count &&
                                         bc->constants[idx].type == XPATH_CONST_STRING)
                                        ? bc->constants[idx].v.string : NULL;
                const char* pi_target = (tgt != 0xFFFF && tgt < bc->const_count &&
                                         bc->constants[tgt].type == XPATH_CONST_STRING)
                                        ? bc->constants[tgt].v.string : NULL;
                struct leptris_xpath_result* r =
                    vm_apply_absolute_type(ctx, type_name, pi_target);
                if (!r) { vm.error = 1; break; }
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_ABSOLUTE_DESCENDANT_WILD: {
                /* Same document-context semantics as the NAME variant:
                 * /descendant::* includes the root element. */
                struct leptris_xpath_result* r =
                    vm_apply_absolute(ctx, NULL, 1, 2);
                if (!r) { vm.error = 1; break; }
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_ABSOLUTE_DESCENDANT_OR_SELF_NAME: {
                uint16_t idx = read_u16(&pc);
                const char* name = (idx < bc->const_count &&
                                    bc->constants[idx].type == XPATH_CONST_STRING)
                                   ? bc->constants[idx].v.string : NULL;
                /* `//foo` = root if matches + all matching descendants. */
                struct leptris_xpath_result* r =
                    vm_apply_absolute(ctx, name, 0, 2);
                if (!r) { vm.error = 1; break; }
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_ABSOLUTE_DESCENDANT_NAME_ATTREQ: {
                /* TODO 192d: `//name[@attr='value']` served from the
                 * index. The value bucket holds exactly the elements
                 * carrying attr=value (root included when it matches),
                 * so one name-filtered scan IS the answer — no name
                 * bucket materialization, no per-element attr walks. */
                uint16_t name_idx = read_u16(&pc);
                uint16_t attr_idx = read_u16(&pc);
                uint16_t value_idx = read_u16(&pc);
                const char* name = (name_idx < bc->const_count &&
                                    bc->constants[name_idx].type == XPATH_CONST_STRING)
                                   ? bc->constants[name_idx].v.string : NULL;
                const char* attr_name = (attr_idx < bc->const_count &&
                                         bc->constants[attr_idx].type == XPATH_CONST_STRING)
                                        ? bc->constants[attr_idx].v.string : NULL;
                /* 0xFFFF = no value operand: attr-EXISTS predicate. */
                const char* value = (value_idx != 0xFFFF &&
                                     value_idx < bc->const_count &&
                                     bc->constants[value_idx].type == XPATH_CONST_STRING)
                                    ? bc->constants[value_idx].v.string : NULL;
                if (!name || !attr_name || (value_idx != 0xFFFF && !value)) {
                    vm.error = 1;
                    break;
                }

                struct leptris_element_index* index =
                    (ctx && ctx->document) ? ctx->document->element_index : NULL;
                if (!index && ctx && ctx->document &&
                    ++ctx->document->axis_query_count >= 2) {
                    index = leptris_element_index_build(ctx->document);
                    ctx->document->element_index = index;
                }

                XPathNodeSet* out = xpath_nodeset_new();
                if (!out) { vm.error = 1; break; }

                int served = 0;
                if (index && index->subtree_end) {
                    const LeptrisElementIndexAttrBucket* abucket =
                        leptris_element_index_lookup_attr(index, attr_name);
                    /* Equality reads the value bucket; existence reads
                     * the any-value bucket (TODO 192e). */
                    const LeptrisElement* bucket_matches = NULL;
                    size_t bucket_count = 0;
                    int bucket_valid = 0;
                    if (value) {
                        const LeptrisElementIndexAttrValue* vbucket =
                            abucket ? leptris_element_index_attr_lookup_value(abucket, value)
                                    : NULL;
                        if (!vbucket ||
                            (vbucket->count > 0 && vbucket->match_positions)) {
                            bucket_valid = 1;
                            if (vbucket) {
                                bucket_matches = vbucket->matches;
                                bucket_count = vbucket->count;
                            }
                        }
                    } else if (!abucket ||
                               (abucket->count > 0 && abucket->match_positions)) {
                        bucket_valid = 1;
                        if (abucket) {
                            bucket_matches = abucket->matches;
                            bucket_count = abucket->count;
                        }
                    }
                    if (bucket_valid) {
                        for (size_t j = 0; j < bucket_count; j++) {
                            if (vm_unprefixed_name_matches(
                                    ctx, bucket_matches[j], name)) {
                                xpath_nodeset_add_fast(out, bucket_matches[j]);
                            }
                        }
                        served = 1;
                    }
                }

                if (!served) {
                    /* Fallback: full walk from root with inline
                     * hash-prefiltered attr filter. */
                    LeptrisElement root = (ctx && ctx->document)
                        ? (LeptrisElement)ctx->document->new_dom_root : NULL;
                    if (root) {
                        uint16_t attr_hash =
                            attr_hash15(attr_name, strlen(attr_name));
                        size_t mark = out->count;
                        descendant_walk(ctx, out, root, name, 0, 1);
                        for (size_t w = mark; w < out->count;) {
                            LeptrisElement e2 = (LeptrisElement)out->nodes[w];
                            const char* v = NULL;
                            for (struct leptris_attribute* a =
                                     leptris_element_get_first_attribute(e2);
                                 a; a = leptris_attr_next(a)) {
                                if (attr_name_hash(a) == attr_hash &&
                                    attr_cname(a) &&
                                    strcmp(attr_cname(a), attr_name) == 0) {
                                    v = attr_cvalue(a);
                                    break;
                                }
                            }
                            if (value ? (v && strcmp(v, value) == 0)
                                      : (v != NULL)) {
                                w++;
                            } else {
                                out->nodes[w] = out->nodes[out->count - 1];
                                out->count--;
                            }
                        }
                    }
                }

                struct leptris_xpath_result* r =
                    xpath_result_new(XPATH_RESULT_NODESET);
                if (!r) { xpath_nodeset_free(out); vm.error = 1; break; }
                r->value.nodeset_value = out;
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_ABSOLUTE_DESCENDANT_NAME_ATTREQ_VAR: {
                /* `//name[@attr=$var]` (issue #565): RHS resolved at
                 * run time from the context's variable set, then the
                 * attr-VALUE bucket + name check — one variable read
                 * per query, no per-element XPath evaluation. */
                uint16_t name_idx = read_u16(&pc);
                uint16_t attr_idx = read_u16(&pc);
                uint16_t var_idx = read_u16(&pc);
                const char* name = (name_idx < bc->const_count &&
                                    bc->constants[name_idx].type == XPATH_CONST_STRING)
                                   ? bc->constants[name_idx].v.string : NULL;
                const char* attr_name = (attr_idx < bc->const_count &&
                                         bc->constants[attr_idx].type == XPATH_CONST_STRING)
                                        ? bc->constants[attr_idx].v.string : NULL;
                const char* var_name = (var_idx < bc->const_count &&
                                        bc->constants[var_idx].type == XPATH_CONST_STRING)
                                       ? bc->constants[var_idx].v.string : NULL;
                if (!name || !attr_name || !var_name) {
                    vm.error = 1; break;
                }
                if (!ctx->variable_set) {
                    snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                            "Variable '%s' not found (no variable set provided)",
                            var_name);
                    vm.error = 1; break;
                }
                const XPathVariable* var = xpath_variable_set_get_const(
                    (XPathVariableSet*)ctx->variable_set, var_name);
                if (!var) {
                    snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                            "Undefined variable: %s", var_name);
                    vm.error = 1; break;
                }
                /* string($var) via the shared converter — borrows the
                 * variable's storage; only the produced string is ours. */
                struct leptris_xpath_result tmp;
                memset(&tmp, 0, sizeof(tmp));
                switch (var->value.type) {
                    case XPATH_VAR_TYPE_BOOLEAN:
                        tmp.type = XPATH_RESULT_BOOLEAN;
                        tmp.value.boolean_value = var->value.v.boolean_value;
                        break;
                    case XPATH_VAR_TYPE_NUMBER:
                        tmp.type = XPATH_RESULT_NUMBER;
                        tmp.value.number_value = var->value.v.number_value;
                        break;
                    case XPATH_VAR_TYPE_NODE_SET:
                        tmp.type = XPATH_RESULT_NODESET;
                        tmp.value.nodeset_value = var->value.v.nodeset_value;
                        break;
                    default:
                        tmp.type = XPATH_RESULT_STRING;
                        tmp.value.string_value = var->value.v.string_value
                            ? var->value.v.string_value : (char*)"";
                        break;
                }
                char* value = xpath_to_string(&tmp);
                if (!value) { vm.error = 1; break; }

                struct leptris_element_index* index =
                    (ctx && ctx->document) ? ctx->document->element_index : NULL;
                if (!index && ctx && ctx->document &&
                    ++ctx->document->axis_query_count >= 2) {
                    index = leptris_element_index_build(ctx->document);
                    ctx->document->element_index = index;
                }

                XPathNodeSet* out = xpath_nodeset_new();
                if (!out) { free(value); vm.error = 1; break; }

                /* Same serve order as the literal ATTREQ opcode: the
                 * attr-VALUE bucket + name filter when the index has
                 * it, otherwise one hash-prefiltered walk. */
                int served = 0;
                if (index && index->subtree_end) {
                    const LeptrisElementIndexAttrBucket* abucket =
                        leptris_element_index_lookup_attr(index, attr_name);
                    const LeptrisElementIndexAttrValue* vbucket = abucket
                        ? leptris_element_index_attr_lookup_value(abucket, value)
                        : NULL;
                    if (!vbucket ||
                        (vbucket->count > 0 && vbucket->match_positions)) {
                        served = 1;
                        if (vbucket) {
                            for (size_t j = 0; j < vbucket->count; j++) {
                                if (vm_unprefixed_name_matches(
                                        ctx, vbucket->matches[j], name))
                                    xpath_nodeset_add_fast(out,
                                                           vbucket->matches[j]);
                            }
                        }
                    }
                }

                if (!served) {
                    LeptrisElement root = (ctx && ctx->document)
                        ? (LeptrisElement)ctx->document->new_dom_root : NULL;
                    if (root) {
                        uint16_t attr_hash =
                            attr_hash15(attr_name, strlen(attr_name));
                        size_t mark = out->count;
                        descendant_walk(ctx, out, root, name, 0, 1);
                        for (size_t w = mark; w < out->count;) {
                            LeptrisElement e2 = (LeptrisElement)out->nodes[w];
                            const char* v = NULL;
                            for (struct leptris_attribute* a =
                                     leptris_element_get_first_attribute(e2);
                                 a; a = leptris_attr_next(a)) {
                                if (attr_name_hash(a) == attr_hash &&
                                    attr_cname(a) &&
                                    strcmp(attr_cname(a), attr_name) == 0) {
                                    v = attr_cvalue(a);
                                    break;
                                }
                            }
                            if (v && strcmp(v, value) == 0) {
                                w++;
                            } else {
                                out->nodes[w] = out->nodes[out->count - 1];
                                out->count--;
                            }
                        }
                    }
                }
                free(value);

                struct leptris_xpath_result* r =
                    xpath_result_new(XPATH_RESULT_NODESET);
                if (!r) { xpath_nodeset_free(out); vm.error = 1; break; }
                r->value.nodeset_value = out;
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_ABSOLUTE_DESCENDANT_OR_SELF_WILD: {
                struct leptris_xpath_result* r =
                    vm_apply_absolute(ctx, NULL, 1, 2);
                if (!r) { vm.error = 1; break; }
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_ABSOLUTE_ELEMENTS_AND_DOC: {
                /* Doc node + every element (root first) — the exact
                 * context set for //NAME[...] (#645). */
                struct leptris_xpath_result* els =
                    vm_apply_absolute(ctx, NULL, 1, 2);
                if (!els || !els->value.nodeset_value) {
                    if (els) leptris_xpath_result_free(els);
                    vm.error = 1;
                    break;
                }
                XPathNodeSet* src = els->value.nodeset_value;
                XPathNodeSet* out = xpath_nodeset_new_with_capacity(
                    src->count + 1);
                if (!out) {
                    leptris_xpath_result_free(els);
                    vm.error = 1;
                    break;
                }
                LeptrisNodeRef dn = (LeptrisNodeRef)
                    leptris_document_get_node(ctx->document);
                if (dn) xpath_nodeset_add_fast(out, (LeptrisNode*)dn);
                for (size_t i = 0; i < src->count; i++)
                    xpath_nodeset_add_fast(out, src->nodes[i]);
                leptris_xpath_result_free(els);
                struct leptris_xpath_result* r =
                    xpath_result_new(XPATH_RESULT_NODESET);
                if (!r) { xpath_nodeset_free(out); vm.error = 1; break; }
                r->value.nodeset_value = out;
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_ABSOLUTE_DOS_ALL_NODES: {
                /* Exact /descendant-or-self::node(): the doc node,
                 * then every node in document order (#645). */
                XPathNodeSet* out = xpath_nodeset_new();
                if (!out) { vm.error = 1; break; }
                LeptrisNodeRef dn = (LeptrisNodeRef)
                    leptris_document_get_node(ctx->document);
                if (dn) {
                    xpath_nodeset_add_fast(out, (LeptrisNode*)dn);
                    /* Doc-node children = the document child chain
                     * (#580), root included. */
                    struct leptris_document* d = ctx->document;
                    LeptrisNode* start = (LeptrisNode*)d->doc_children_head;
                    if (!start) start = (LeptrisNode*)d->new_dom_root;
                    for (LeptrisNode* c = start; c;
                         c = leptris_node_get_next_sibling(c))
                        all_nodes_preorder(out, c);
                } else if (ctx->document->new_dom_root) {
                    all_nodes_preorder(
                        out, (LeptrisNode*)ctx->document->new_dom_root);
                }
                struct leptris_xpath_result* r =
                    xpath_result_new(XPATH_RESULT_NODESET);
                if (!r) { xpath_nodeset_free(out); vm.error = 1; break; }
                r->value.nodeset_value = out;
                vm_push(&vm, r);
                break;
            }

            /* Simple predicate handlers (TODO 128). Each pops the
             * input nodeset, filters inline, pushes the filtered
             * result. The compiler emits these after a specialized
             * axis opcode for the common predicate shapes
             * ([@attr], [@attr = 'lit'], [N]). */

            case XPATH_BC_PRED_ATTR_EXISTS: {
                uint16_t idx = read_u16(&pc);
                const char* attr_name = (idx < bc->const_count &&
                                          bc->constants[idx].type == XPATH_CONST_STRING)
                                         ? bc->constants[idx].v.string : NULL;
                XPathNodeSet* input = vm_detach_input_nodeset(&vm);
                if (!input) { vm.error = 1; break; }

                /* TODO 159 Phase E: hoist strlen + hash out of the
                 * inner attribute-walk loop. Previous code called
                 * strlen(attr_name) inside the loop body, redundant
                 * per-attr. */
                size_t name_len = attr_name ? strlen(attr_name) : 0;
                uint16_t name_hash = attr_name ? attr_hash15(attr_name, name_len) : 0;

                size_t write = 0;
                for (size_t read = 0; read < input->count; read++) {
                    void* node = input->nodes[read];
                    if (!node_is_element(node)) continue;
                    LeptrisElement elem = (LeptrisElement)node;
                    struct leptris_attribute* a =
                        leptris_element_get_first_attribute(elem);
                    int found = 0;
                    while (a) {
                        /* Hash pre-filter first; only fall through to
                         * length + memcmp on hash match. Lazy compute
                         * on first read (TODO 172). */
                        if (attr_name && attr_name_hash(a) == name_hash &&
                            a->name_view.length == name_len &&
                            a->name_view.data &&
                            memcmp(attr_name, a->name_view.data, name_len) == 0) {
                            found = 1;
                            break;
                        }
                        a = leptris_attr_next(a);
                    }
                    if (found) {
                        if (write != read) input->nodes[write] = node;
                        write++;
                    }
                }
                input->count = write;

                struct leptris_xpath_result* r = xpath_result_new(XPATH_RESULT_NODESET);
                if (!r) { xpath_nodeset_free(input); vm.error = 1; break; }
                r->value.nodeset_value = input;
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_PRED_ATTR_EQ_STRING: {
                /* Two u16 operands: attr name idx, value idx. */
                uint16_t name_idx = read_u16(&pc);
                uint16_t value_idx = read_u16(&pc);
                const char* attr_name = (name_idx < bc->const_count &&
                                          bc->constants[name_idx].type == XPATH_CONST_STRING)
                                         ? bc->constants[name_idx].v.string : NULL;
                const char* expected = (value_idx < bc->const_count &&
                                         bc->constants[value_idx].type == XPATH_CONST_STRING)
                                        ? bc->constants[value_idx].v.string : NULL;
                XPathNodeSet* input = vm_detach_input_nodeset(&vm);
                if (!input) { vm.error = 1; break; }

                /* TODO 159 Phase E: hoist strlen's out of the inner
                 * loop. Previous code called strlen(attr_name) and
                 * strlen(expected) once per attribute per element. */
                size_t name_len = attr_name ? strlen(attr_name) : 0;
                size_t value_len = expected ? strlen(expected) : 0;
                uint16_t name_hash = attr_name ? attr_hash15(attr_name, name_len) : 0;

                size_t write = 0;
                for (size_t read = 0; read < input->count; read++) {
                    void* node = input->nodes[read];
                    if (!node_is_element(node)) continue;
                    LeptrisElement elem = (LeptrisElement)node;
                    struct leptris_attribute* a =
                        leptris_element_get_first_attribute(elem);
                    int match = 0;
                    while (a) {
                        /* Hash + length pre-filter before memcmp. Lazy hash. */
                        if (attr_name && expected &&
                            attr_name_hash(a) == name_hash &&
                            a->name_view.length == name_len &&
                            leptris_attr_value_sv(a).length == value_len &&
                            a->name_view.data && leptris_attr_value_sv(a).data &&
                            memcmp(attr_name, a->name_view.data, name_len) == 0 &&
                            memcmp(expected, leptris_attr_value_sv(a).data, value_len) == 0) {
                            match = 1;
                            break;
                        }
                        a = leptris_attr_next(a);
                    }
                    if (match) {
                        if (write != read) input->nodes[write] = node;
                        write++;
                    }
                }
                input->count = write;

                struct leptris_xpath_result* r = xpath_result_new(XPATH_RESULT_NODESET);
                if (!r) { xpath_nodeset_free(input); vm.error = 1; break; }
                r->value.nodeset_value = input;
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_PRED_POSITION: {
                uint8_t pos = read_u8(&pc);
                XPathNodeSet* input = vm_detach_input_nodeset(&vm);
                if (!input) { vm.error = 1; break; }

                /* Keep only the pos-th element (1-based). */
                if (pos == 0 || (size_t)pos > input->count) {
                    input->count = 0;
                } else {
                    void* keep = input->nodes[pos - 1];
                    input->nodes[0] = keep;
                    input->count = 1;
                }

                struct leptris_xpath_result* r = xpath_result_new(XPATH_RESULT_NODESET);
                if (!r) { xpath_nodeset_free(input); vm.error = 1; break; }
                r->value.nodeset_value = input;
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_PRED_CHILD_NUM_CMP: {
                /* Fused child-axis numeric comparison (TODO 159).
                 * Inline encoding: u8 op, u16 name idx, f64 RHS. */
                uint8_t op_type = read_u8(&pc);
                uint16_t name_idx = read_u16(&pc);
                double rhs = read_double(&pc);
                const char* child_name = (name_idx < bc->const_count &&
                                          bc->constants[name_idx].type == XPATH_CONST_STRING)
                                         ? bc->constants[name_idx].v.string : NULL;
                XPathNodeSet* input = vm_detach_input_nodeset(&vm);
                if (!input || !child_name) {
                    if (input) xpath_nodeset_free(input);
                    vm.error = 1;
                    break;
                }

                /* Pre-compute the target hash so we can compare 2 bytes
                 * before strcmp on every child. */
                uint16_t target_hash = leptris_name_hash_compute(child_name);
                XPathOperatorType step_op = (XPathOperatorType)op_type;
                size_t child_name_len = strlen(child_name);

                size_t write = 0;
                for (size_t read = 0; read < input->count; read++) {
                    void* node = input->nodes[read];
                    if (!node_is_element(node)) continue;
                    LeptrisElement elem = (LeptrisElement)node;

                    /* Walk this element's children looking for a
                     * child::name match. Hash pre-filter rejects
                     * non-matching children in ~1ns. */
                    LeptrisNode* child = (LeptrisNode*)leptris_elem_first_child(elem);
                    LeptrisElement match = NULL;
                    while (child) {
                        if (child->type == LEPTRIS_NODE_TYPE_ELEMENT) {
                            LeptrisElement ce = (LeptrisElement)child;
                            if (ce->name_hash == target_hash && ce->name &&
                                strlen(ce->name) == child_name_len &&
                                memcmp(ce->name, child_name, child_name_len) == 0) {
                                match = ce;
                                break;
                            }
                        }
                        child = leptris_node_get_next_sibling(child);
                    }
                    if (!match) continue;

                    /* Fast path: single text child is the common shape
                     * for <price>42.50</price>. Read its content view
                     * without allocating. */
                    double lhs;
                    LeptrisNode* tn = (LeptrisNode*)leptris_elem_first_child(match);
                    if (tn && tn->type == LEPTRIS_NODE_TYPE_TEXT &&
                        leptris_elem_next_sibling(match) == NULL) {
                        const char* s =
                            leptris_text_get_content((LeptrisTextNode*)tn);
                        lhs = s ? strtod(s, NULL) : NAN;
                    } else {
                        char* s = leptris_element_get_text_content(match);
                        if (s) {
                            lhs = strtod(s, NULL);
                            leptris_free(s);
                        } else {
                            lhs = NAN;
                        }
                    }

                    int match_flag = 0;
                    switch (step_op) {
                        case XPATH_OP_EQUAL:         match_flag = (lhs == rhs); break;
                        case XPATH_OP_NOT_EQUAL:     match_flag = (lhs != rhs); break;
                        case XPATH_OP_LESS:          match_flag = (lhs <  rhs); break;
                        case XPATH_OP_LESS_EQUAL:    match_flag = (lhs <= rhs); break;
                        case XPATH_OP_GREATER:       match_flag = (lhs >  rhs); break;
                        case XPATH_OP_GREATER_EQUAL: match_flag = (lhs >= rhs); break;
                        default: break;
                    }
                    if (match_flag) {
                        if (write != read) input->nodes[write] = node;
                        write++;
                    }
                }
                input->count = write;

                struct leptris_xpath_result* r = xpath_result_new(XPATH_RESULT_NODESET);
                if (!r) { xpath_nodeset_free(input); vm.error = 1; break; }
                r->value.nodeset_value = input;
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_BINARY_OP: {
                uint8_t op_type = read_u8(&pc);
                if (vm_apply_binary_op(&vm, ctx,
                        (XPathOperatorType)op_type) != 0) {
                    vm.error = 1;
                }
                break;
            }

            case XPATH_BC_FUNC_CALL: {
                uint16_t idx = read_u16(&pc);
                if (idx >= bc->const_count ||
                    bc->constants[idx].type != XPATH_CONST_AST_NODE) {
                    vm.error = 1;
                    break;
                }
                XPathASTNode* fc_ast = bc->constants[idx].v.ast;
                struct leptris_xpath_result* r =
                    evaluate_function_call_inline(ctx, fc_ast);
                if (!r) { vm.error = 1; break; }
                vm_push(&vm, r);
                break;
            }

            /* Inline function handlers (TODO 130). Each pops its
             * args from the stack and pushes the result. The
             * compiler emits <arg bytecode> + BC_FUNC_<NAME>
             * instead of BC_FUNC_CALL for the common XPath
             * functions. The VM evaluates args via normal dispatch
             * (which uses the fast specialized axis opcodes), then
             * applies the function inline. */

            case XPATH_BC_FUNC_COUNT: {
                struct leptris_xpath_result* arg = vm_pop(&vm);
                if (!arg) { vm.error = 1; break; }
                /* atomic = one item (XQuery sequences; the twin
                 * of xpath_func_count's QT3 fix) */
                size_t count = 1;
                if (arg->type == XPATH_RESULT_NODESET) {
                    count = arg->value.nodeset_value
                                ? arg->value.nodeset_value->count
                                : 0;
                }
                xpath_result_free(arg);
                struct leptris_xpath_result* r = xpath_result_new(XPATH_RESULT_NUMBER);
                if (!r) { vm.error = 1; break; }
                r->value.number_value = (double)count;
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_FUNC_SUM: {
                struct leptris_xpath_result* arg = vm_pop(&vm);
                if (!arg) { vm.error = 1; break; }
                double sum = 0.0;
                if (arg->type == XPATH_RESULT_NODESET && arg->value.nodeset_value) {
                    XPathNodeSet* ns = arg->value.nodeset_value;
                    for (size_t i = 0; i < ns->count; i++) {
                        char* txt = get_node_text(ns->nodes[i]);
                        if (txt) {
                            sum += atof(txt);
                            free(txt);
                        }
                    }
                }
                xpath_result_free(arg);
                struct leptris_xpath_result* r = xpath_result_new(XPATH_RESULT_NUMBER);
                if (!r) { vm.error = 1; break; }
                r->value.number_value = sum;
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_FUNC_POSITION: {
                struct leptris_xpath_result* r = xpath_result_new(XPATH_RESULT_NUMBER);
                if (!r) { vm.error = 1; break; }
                r->value.number_value = (double)ctx->context_position;
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_FUNC_LAST: {
                struct leptris_xpath_result* r = xpath_result_new(XPATH_RESULT_NUMBER);
                if (!r) { vm.error = 1; break; }
                r->value.number_value = (double)ctx->context_size;
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_FUNC_TRUE: {
                struct leptris_xpath_result* r = xpath_result_new(XPATH_RESULT_BOOLEAN);
                if (!r) { vm.error = 1; break; }
                r->value.boolean_value = 1;
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_FUNC_FALSE: {
                struct leptris_xpath_result* r = xpath_result_new(XPATH_RESULT_BOOLEAN);
                if (!r) { vm.error = 1; break; }
                r->value.boolean_value = 0;
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_FUNC_NOT: {
                struct leptris_xpath_result* arg = vm_pop(&vm);
                if (!arg) { vm.error = 1; break; }
                int b = xpath_to_boolean(arg);
                xpath_result_free(arg);
                struct leptris_xpath_result* r = xpath_result_new(XPATH_RESULT_BOOLEAN);
                if (!r) { vm.error = 1; break; }
                r->value.boolean_value = !b;
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_FUNC_BOOLEAN: {
                struct leptris_xpath_result* arg = vm_pop(&vm);
                if (!arg) { vm.error = 1; break; }
                int b = xpath_to_boolean(arg);
                xpath_result_free(arg);
                struct leptris_xpath_result* r = xpath_result_new(XPATH_RESULT_BOOLEAN);
                if (!r) { vm.error = 1; break; }
                r->value.boolean_value = b;
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_FUNC_NUMBER: {
                struct leptris_xpath_result* arg = vm_pop(&vm);
                if (!arg) { vm.error = 1; break; }
                double n = xpath_to_number(arg);
                xpath_result_free(arg);
                struct leptris_xpath_result* r = xpath_result_new(XPATH_RESULT_NUMBER);
                if (!r) { vm.error = 1; break; }
                r->value.number_value = n;
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_FUNC_STRING: {
                struct leptris_xpath_result* arg = vm_pop(&vm);
                if (!arg) { vm.error = 1; break; }
                char* s = xpath_to_string(arg);
                xpath_result_free(arg);
                struct leptris_xpath_result* r = xpath_result_new(XPATH_RESULT_STRING);
                if (!r) { free(s); vm.error = 1; break; }
                r->value.string_value = s ? s : leptris_strdup("");
                if (!r->value.string_value) {
                    free(s);
                    xpath_result_free(r);
                    vm.error = 1;
                    break;
                }
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_FUNC_NAME:
            case XPATH_BC_FUNC_LOCAL_NAME:
            case XPATH_BC_FUNC_NAMESPACE_URI: {
                struct leptris_xpath_result* arg = vm_pop(&vm);
                if (!arg) { vm.error = 1; break; }

                const char* str = NULL;
                int free_str = 0;
                if (arg->type == XPATH_RESULT_NODESET &&
                    arg->value.nodeset_value &&
                    arg->value.nodeset_value->count > 0) {
                    void* first = arg->value.nodeset_value->nodes[0];
                    LeptrisNodeType nt = *(LeptrisNodeType*)first;
                    if (nt == LEPTRIS_NODE_ATTRIBUTE) {
                        LeptrisAttributeNode* an = (LeptrisAttributeNode*)first;
                        if (op == XPATH_BC_FUNC_NAME) str = an->name;
                        else if (op == XPATH_BC_FUNC_LOCAL_NAME) {
                            /* Strip prefix from "prefix:local". */
                            const char* colon = an->name ? strchr(an->name, ':') : NULL;
                            str = colon ? colon + 1 : an->name;
                        } else {
                            str = an->namespace_uri;  /* may be NULL */
                        }
                    } else if (nt == LEPTRIS_NODE_ELEMENT) {
                        /* Element. */
                        LeptrisElement e = (LeptrisElement)first;
                        if (op == XPATH_BC_FUNC_NAME) {
                            str = leptris_element_get_name(e);
                        } else if (op == XPATH_BC_FUNC_LOCAL_NAME) {
                            str = leptris_element_get_name(e);
                        } else {
                            str = leptris_element_get_namespace_uri(e);
                        }
                    }
                }

                xpath_result_free(arg);

                struct leptris_xpath_result* r = xpath_result_new(XPATH_RESULT_STRING);
                if (!r) { vm.error = 1; break; }
                r->value.string_value = leptris_strdup(str ? str : "");
                if (!r->value.string_value) {
                    xpath_result_free(r);
                    vm.error = 1;
                    break;
                }
                vm_push(&vm, r);
                (void)free_str;
                break;
            }

            case XPATH_BC_FALLBACK_EVAL: {
                uint16_t idx = read_u16(&pc);
                if (idx >= bc->const_count ||
                    bc->constants[idx].type != XPATH_CONST_AST_NODE) {
                    vm.error = 1;
                    break;
                }
                XPathASTNode* ast_node = bc->constants[idx].v.ast;
                struct leptris_xpath_result* r = evaluate_expr(ctx, ast_node);
                if (!r) { vm.error = 1; break; }
                vm_push(&vm, r);
                break;
            }

            case XPATH_BC_RETURN:
                goto done;

            default:
                vm.error = 1;
                goto done;
        }
    }

done:
    if (vm.error || vm.sp == 0) {
        while (vm.sp > 0) {
            struct leptris_xpath_result* r = vm_pop(&vm);
            if (r) xpath_result_free(r);
        }
        free(vm.stack);
        return NULL;
    }

    struct leptris_xpath_result* result = vm_pop(&vm);

    while (vm.sp > 0) {
        struct leptris_xpath_result* extra = vm_pop(&vm);
        if (extra) xpath_result_free(extra);
    }
    free(vm.stack);
    return result;
}

/* Public entry: compile AST → run VM.  Returns NULL on failure.
 * Caller frees the result via leptris_xpath_result_free.
 *
 * Callers should prefer the cached path
 * (xpath_expr_cache_get_or_compile_bc + vm_run_with_bc) so the
 * compile cost is amortized across many evals of the same
 * expression. This entry point is retained for tests and for
 * one-off evals that bypass the cache. */
struct leptris_xpath_result* leptris_xpath_vm_eval(XPathASTNode* ast,
                                                  XPathContext* ctx) {
    if (!ast || !ctx) return NULL;

    LeptrisXPathBytecode* bc = leptris_xpath_compile_ast(ast);
    if (!bc) return NULL;

    struct leptris_xpath_result* result = vm_run(bc, ctx);

    leptris_xpath_bytecode_free(bc);
    return result;
}

/* Cached entry: run an already-compiled bytecode. Used by
 * leptris_xpath_eval when the expression cache has a compiled bc. */
struct leptris_xpath_result* leptris_xpath_vm_run_bc(LeptrisXPathBytecode* bc,
                                                    XPathContext* ctx) {
    if (!bc || !ctx) return NULL;
    return vm_run(bc, ctx);
}