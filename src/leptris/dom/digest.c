/* dom/digest.c — on-demand subtree structural digest (#869).
 *
 * Content-defined Merkle hash: no pointers or addresses
 * participate, so the value is stable across processes. Equality
 * implies subtree equivalence under the flag semantics (the
 * comparator's soundness contract); inequality implies nothing.
 * Zero cost when never called — nothing here runs on any parse,
 * serialize, or eval path.
 */

#include "../include/leptris.h"
#include "../leptris_internal.h"
#include "element.h"
#include "node.h"
#include "text.h"
#include "comment.h"
#include "cdata.h"
#include "pi.h"
#include <stdlib.h>
#include <string.h>

#define DIGEST_FNV_OFFSET 1469598103934665603ULL
#define DIGEST_FNV_PRIME  1099511628211ULL

/* Length-prefixed byte mix: the length prefix keeps field
 * boundaries unambiguous ("ab"+"c" never collides with "a"+"bc"). */
static uint64_t digest_mix_bytes(uint64_t h, const char* s, size_t len) {
    h ^= (uint64_t)len + 1;
    h *= DIGEST_FNV_PRIME;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= DIGEST_FNV_PRIME;
    }
    return h;
}

static uint64_t digest_mix_str(uint64_t h, const char* s) {
    /* NULL and "" both mix as empty — an absent namespace and an
     * empty namespace URI are the same no-namespace. */
    return s ? digest_mix_bytes(h, s, strlen(s))
             : digest_mix_bytes(h, "", 0);
}

static uint64_t digest_mix_u64(uint64_t h, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        h ^= (unsigned char)(v >> (i * 8));
        h *= DIGEST_FNV_PRIME;
    }
    return h;
}

static int digest_is_ws_only(const char* s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return 0;
    }
    return 1;
}

typedef struct {
    const char* uri;      /* resolved namespace URI ("" = none) */
    const char* local;    /* local part (after the colon) */
    const char* value;
    size_t local_len;
    size_t value_len;
} DigestAttr;

static int digest_attr_cmp(const void* a, const void* b) {
    const DigestAttr* da = (const DigestAttr*)a;
    const DigestAttr* db = (const DigestAttr*)b;
    const char* ua = da->uri ? da->uri : "";
    const char* ub = db->uri ? db->uri : "";
    int c = strcmp(ua, ub);
    if (c != 0) return c;
    size_t ml = da->local_len < db->local_len ? da->local_len : db->local_len;
    c = memcmp(da->local, db->local, ml);
    if (c != 0) return c;
    if (da->local_len < db->local_len) return -1;
    if (da->local_len > db->local_len) return 1;
    return 0;
}

static uint64_t digest_element(LeptrisElement e, LeptrisDigestFlags flags);

static uint64_t digest_element(LeptrisElement e, LeptrisDigestFlags flags) {
    uint64_t h = DIGEST_FNV_OFFSET;
    h = digest_mix_bytes(h, "E", 1);
    char* pfx = leptris_elem_prefix(e);
    h = digest_mix_str(h, pfx);
    h = digest_mix_str(h, leptris_element_get_namespace_uri(e));
    const char* name = leptris_element_get_name(e);
    h = digest_mix_str(h, name);

    /* Namespace declarations ON this element (prefix->URI pairs,
     * sorted like attrs so declaration order doesn't false-split;
     * a changed or re-prefixed declaration changes the subtree —
     * the x:root case resolved through the element URI, but an
     * unused redeclaration only shows here). */
    {
        size_t ndecl = 0;
        for (struct leptris_namespace* n2 = leptris_elem_namespaces(e);
             n2; n2 = n2->next)
            ndecl++;
        if (ndecl > 0) {
            struct leptris_namespace** order =
                (struct leptris_namespace**)malloc(
                    ndecl * sizeof(*order));
            if (order) {
                size_t k = 0;
                for (struct leptris_namespace* n2 =
                         leptris_elem_namespaces(e);
                     n2; n2 = n2->next)
                    order[k++] = n2;
                for (size_t i = 0; i < ndecl; i++)
                    for (size_t j = i + 1; j < ndecl; j++) {
                        const char* pi = order[i]->prefix
                                            ? order[i]->prefix : "";
                        const char* pj = order[j]->prefix
                                            ? order[j]->prefix : "";
                        if (strcmp(pi, pj) > 0) {
                            struct leptris_namespace* t = order[i];
                            order[i] = order[j];
                            order[j] = t;
                        }
                    }
                h = digest_mix_u64(h, (uint64_t)ndecl);
                for (size_t i = 0; i < ndecl; i++) {
                    h = digest_mix_str(h, order[i]->prefix);
                    h = digest_mix_str(h, order[i]->uri);
                }
                free(order);
            }
        }
    }

    /* Attributes: resolved (URI, local, value) triples, sorted,
     * deduplicated first-wins (document order). */
    uint8_t acount = leptris_element_attribute_count(e);
    if (acount > 0) {
        DigestAttr stack_attrs[32];
        DigestAttr* attrs = stack_attrs;
        DigestAttr* heap_attrs = NULL;
        if (acount > 32) {
            heap_attrs = (DigestAttr*)malloc(acount * sizeof(DigestAttr));
            attrs = heap_attrs;
        }
        size_t n = 0;
        if (attrs) {
            for (struct leptris_attribute* a =
                     leptris_element_get_first_attribute(e);
                 a; a = leptris_attr_next(a)) {
                const char* cname = attr_cname(a);
                const char* colon = cname ? strchr(cname, ':') : NULL;
                const char* local =
                    colon ? colon + 1 : (cname ? cname : "");
                size_t local_len =
                    a->name_view.length -
                    (colon ? (size_t)(colon - cname) + 1 : 0);
                const char* uri = NULL;
                if (colon) {
                    uri = leptris_attribute_namespace_uri(a);
                    if (!uri) continue;   /* undeclared prefix: skip */
                }
                /* First-wins dedup on the resolved key. */
                int dup = 0;
                for (size_t i = 0; i < n; i++) {
                    const char* u2 = attrs[i].uri ? attrs[i].uri : "";
                    const char* u1 = uri ? uri : "";
                    if (strcmp(u1, u2) == 0 &&
                        attrs[i].local_len == local_len &&
                        memcmp(attrs[i].local, local, local_len) == 0) {
                        dup = 1;
                        break;
                    }
                }
                if (dup) continue;
                attrs[n].uri = uri;
                attrs[n].local = local;
                attrs[n].local_len = local_len;
                attrs[n].value = leptris_attr_value_sv(a).data ? leptris_attr_value_sv(a).data : "";
                attrs[n].value_len = leptris_attr_value_sv(a).length;
                n++;
            }
            qsort(attrs, n, sizeof(DigestAttr), digest_attr_cmp);
        }
        h = digest_mix_u64(h, n);
        for (size_t i = 0; i < n; i++) {
            h = digest_mix_str(h, attrs[i].uri);
            h = digest_mix_bytes(h, attrs[i].local, attrs[i].local_len);
            h = digest_mix_bytes(h, attrs[i].value, attrs[i].value_len);
        }
        free(heap_attrs);
    }

    /* Children in document order, each hashed recursively (Merkle
     * combine — a child's full digest feeds the parent). */
    size_t n_children = 0;
    for (LeptrisNode* c = leptris_node_first_child_internal((LeptrisNode*)e);
         c; c = leptris_node_get_next_sibling(c)) {
        if ((flags & LEPTRIS_DIGEST_DROP_WS_TEXT) &&
            c->type == LEPTRIS_NODE_TYPE_TEXT) {
            const char* t = leptris_text_get_content((LeptrisTextNode*)c);
            if (digest_is_ws_only(t ? t : "", t ? strlen(t) : 0)) continue;
        }
        n_children++;
    }
    h = digest_mix_u64(h, n_children);
    for (LeptrisNode* c = leptris_node_first_child_internal((LeptrisNode*)e);
         c; c = leptris_node_get_next_sibling(c)) {
        if ((flags & LEPTRIS_DIGEST_DROP_WS_TEXT) &&
            c->type == LEPTRIS_NODE_TYPE_TEXT) {
            const char* t = leptris_text_get_content((LeptrisTextNode*)c);
            if (digest_is_ws_only(t ? t : "", t ? strlen(t) : 0)) continue;
        }
        h = digest_mix_u64(h, leptris_node_digest((LeptrisNodeRef)c, flags));
    }
    return h;
}

LEPTRIS_API uint64_t leptris_node_digest(LeptrisNodeRef node,
                                         LeptrisDigestFlags flags) {
    if (!node) return 0;
    LeptrisNode* n = (LeptrisNode*)node;
    switch (n->type) {
        case LEPTRIS_NODE_TYPE_ELEMENT:
            return digest_element((LeptrisElement)n, flags);
        case LEPTRIS_NODE_TYPE_TEXT: {
            uint64_t h = DIGEST_FNV_OFFSET;
            h = digest_mix_bytes(h, "T", 1);
            const char* t = leptris_text_get_content((LeptrisTextNode*)n);
            return digest_mix_str(h, t);
        }
        case LEPTRIS_NODE_TYPE_CDATA: {
            uint64_t h = DIGEST_FNV_OFFSET;
            h = digest_mix_bytes(h, "C", 1);
            const char* t = leptris_cdata_get_content((LeptrisCDATANode*)n);
            return digest_mix_str(h, t);
        }
        case LEPTRIS_NODE_TYPE_COMMENT: {
            uint64_t h = DIGEST_FNV_OFFSET;
            h = digest_mix_bytes(h, "M", 1);
            const char* t = leptris_comment_get_content((LeptrisCommentNode*)n);
            return digest_mix_str(h, t);
        }
        case LEPTRIS_NODE_TYPE_PI: {
            uint64_t h = DIGEST_FNV_OFFSET;
            h = digest_mix_bytes(h, "P", 1);
            LeptrisPINode* pi = (LeptrisPINode*)n;
            h = digest_mix_str(h, leptris_pi_get_target(pi));
            return digest_mix_str(h, leptris_pi_get_data(pi));
        }
        default:
            return 0;
    }
}
