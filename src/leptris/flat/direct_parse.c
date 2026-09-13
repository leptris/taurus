/* flat/direct_parse.c — Single-pass parse into LeptrisElement tree
 * (TODO 147 Phase A).
 *
 * This is the pugixml-style parser: scan XML once, write LeptrisElement
 * records directly. No FlatDoc intermediate, no promote pass.
 *
 * Key techniques:
 *   1. Upfront bulk allocation: estimate element count, malloc one block
 *   2. Zero-copy names: NUL-terminate in-place in the buffer copy
 *   3. Direct edge wiring: set compact-pointer offsets via pointer
 *      arithmetic (no function call, no overflow table)
 *   4. Lookup tables: single-byte char classification (no branch chains)
 *   5. memchr for text scanning: libc vectorized search
 *
 * The parser handles: elements (open/close/self-closing), attributes
 * (single+double quoted), text, comments, CDATA, PIs, XML declaration,
 * DOCTYPE skipping, BOM. Namespace-aware: splits prefix:local names,
 * moves xmlns declarations to elem->namespaces.
 *
 * For inputs the direct parser can't handle (DTD internal subset with
 * entities), the caller falls back to flat_parse + promote.
 */
#include "direct_parse.h"
#include "../memory/arena.h"
#include "../dom/element.h"
#include "../dom/root_doc_map.h"
#include "../dom/text.h"
#include "../dom/comment.h"
#include "../dom/cdata.h"
#include "../dom/pi.h"
#include "../dom/doctype.h"
#include "../common/string_view.h"
#include "../common/chartype.h"
#include "../common/entities.h"
#include "../common/port.h"
#include "../common/simd_text.h"
#include "../dtd/model.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

extern LEPTRIS_THREAD_LOCAL int g_leptris_strict_mode;
extern void leptris_compact_set_current_document(struct leptris_document* doc);
int leptris_element_add_namespace(struct leptris_element* elem,
                                  struct leptris_namespace* ns);

#define DP_MAX_DEPTH 256

typedef struct {
    char* buf;
    char* pos;
    char* end;
    LeptrisMemoryPool* pool;
    struct leptris_document* doc;
    LeptrisElement open_stack[DP_MAX_DEPTH];
    /* TODO 155 Phase C: per-depth last-child cache. Replaces the
     * last_child_off field on element. last_child_stack[i] holds
     * the most recently wired child of open_stack[i], or NULL when
     * no child has been wired yet at that depth. */
    LeptrisNode* last_child_stack[DP_MAX_DEPTH];
    int depth;
    LeptrisElement root;
    /* Document children (issue #580): [prolog..., root, epilog...]
     * as ONE node chain in document order — the document node's
     * child list. root_chained moves the root element into the
     * chain at the first epilog capture (or at finalize). */
    LeptrisNode* dc_head;
    LeptrisNode* dc_tail;
    int root_chained;
    /* XML declaration fields */
    char* version;
    char* encoding;
    int standalone;
    /* Source line tracking (issue #223). Updated as the scanner
     * crosses '\n' bytes. Frozen into each node at creation. */
    /* Line tracking is LAZY (issue #223 follow-up): parse stores
     * byteOffset+1 in node->base.line and leptris_node_line resolves
     * it against doc->xml_buffer on first query (doc-level newline
     * table, binary search, cached in-place). The per-byte '
'
     * compares this removed were ~30% of text-heavy parse. 0 for
     * documents >= 2 GiB (offsets don't fit; line reads return
     * unknown, matching the pre-#223 behavior). */
    int line_offsets_ok;
    int had_declaration;
    /* Bulk-allocated attribute block. Pre-allocated from pool so the
     * common case is a bump-pointer off the block — no per-attr
     * pool_alloc, no name interning, no value pool_strdup. Overflow
     * falls back to per-attr pool_alloc. */
    struct leptris_attribute* attr_block;
    size_t attr_idx;
    size_t attr_capacity;
    /* Attr-diet fields: a running cursor replaces the per-attr
     * `&block[idx++]` multiply (48-byte stride), and the parser-local
     * counter replaces the per-attr elem->attr_count load-add-store
     * (the element cache line only needs touching twice per tag). */
    struct leptris_attribute* attr_cursor;
    struct leptris_attribute* attr_end;
    /* Bulk text-node block (round 8): leptris_text_create_borrowed
     * is an out-of-line call per text node — pool_alloc alone costs
     * more than the field stores. Same carve pattern as the elem and
     * attr blocks. Overflow falls back to the pool path. */
    struct leptris_text_node* text_cursor;
    struct leptris_text_node* text_end;
    /* Comment/CDATA/PI bulk block (#450 endgame): carved from the
     * same combined allocation as elem/text so sibling and parent
     * edges stay intra-block (raw int32, < 2 GB). The per-node
     * pool allocations these types previously used land in a
     * different malloc REGION than the arena on macOS — distances
     * beyond ±2 GB that no compact edge can hold raw. Overflow
     * falls back to the pool path whose setters route far links
     * through the overflow table. */
    char* cpi_cursor;
    char* cpi_end;
    size_t cpi_stride;
    unsigned cur_attr_count;
    /* 1 when the parser owns an over-allocated copy: probe windows
     * may read up to 48 bytes past p->end because the 64-byte zeroed
     * slack after the sentinel stops them (NUL matches neither quote
     * nor '&', and the bounded memchr fallback re-checks). 0 for
     * in-place parses on caller buffers — clamped windows only. */
    int probe_slack;
    /* LEPTRIS_PARSE_DROP_WS_TEXT: skip creating nodes for
     * whitespace-only text runs (pugixml-default semantics). */
    int drop_ws_text;
    /* TODO 159 Phase G: parser-local last-attr cache for the element
     * currently being parsed. Eliminates the O(N) walk-to-find-tail
     * in dp_add_attr_inline — O(1) wiring per attr instead of O(N),
     * removing the O(N²) per-element-attrs cost. Reset to NULL at
     * the start of each dp_parse_attrs call.
     *
     * Also caches the last xmlns ns-node for the same reason — the
     * old code walked the ns list to find the tail on every xmlns. */
    struct leptris_attribute* current_elem_last_attr;
    struct leptris_namespace* current_elem_last_ns;
    int saw_namespace;   /* any xmlns seen → doc->has_namespaces */
    /* Raw-view bulk block (issue #635 perf, pugixml race): raw-attr
     * entries carved from 128-entry pool chunks — one bump per attr,
     * no per-attr pool_alloc, no tail walk. raw_last is the current
     * element's chain tail; reset at dp_parse_attrs start like
     * current_elem_last_attr. */
    struct leptris_raw_attr* raw_cursor;
    struct leptris_raw_attr* raw_end;
    struct leptris_raw_attr* raw_last;
    struct leptris_ns_cache* nsc_cursor;
    struct leptris_ns_cache* nsc_end;
    /* DTD parsed from the DOCTYPE internal subset. NULL when the
     * document has no DTD (or only an external subset). When non-NULL,
     * text/attr entity expansion routes through
     * leptris_decode_entities_view_with_dtd so custom entities
     * (&foo; where foo is declared in the DTD) resolve correctly. */
    LeptrisDTD* dtd;
} DParser;

static inline void dp_skip_ws(DParser* p) {
    /* Sentinel-terminated (parse endgame): the parse buffer always
     * carries a NUL at buf[len] (copy and in-place entries both
     * write it), and NUL classifies as no chartype — the bounds
     * check per byte was the pugixml delta this loop can drop. */
    while (IS_WS(*p->pos)) p->pos++;
}

/* Phase B (TODO 166): name-char scan helper.
 *
 * Originally prototyped a 4-byte ASCII fast path
 * (`(w & 0x80808080u) == 0` guard + 4 IS_NAME_CHAR checks per
 * iteration). On the many-attrs benchmark (avg attr-name length
 * 7), the fast path added MORE work per 4 chars than the byte
 * loop saved: the memcpy + mask check + 4 byte extractions cost
 * more than 4 byte loads, while modern branch predictors already
 * make the byte loop nearly free. Measured ~25% regression at
 * K=50 attrs/element. Reverted to the plain byte loop.
 *
 * Kept as a thin LEPTRIS_ALWAYS_INLINE wrapper — preserves the
 * call-site DRY (six name-scan loops collapse to one helper)
 * and guarantees no call-site regression vs the original
 * inline loop. */
#if defined(__GNUC__)
#  define DP_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#  define DP_UNLIKELY(x) (x)
#endif
/* Sentinel-terminated + 4x-unrolled byte loads (parse endgame):
 * pugixml's SCANWHILE_UNROLL shape, never tested here — the earlier
 * failed fast path was SWAR (mask setup dominated short names);
 * this one keeps plain byte loads and only amortizes the loop
 * counter and exits via unlikely hints. NUL stops the scan. */
static LEPTRIS_ALWAYS_INLINE void dp_scan_name(DParser* p) {
    char* s = p->pos;
    for (;;) {
        char c = s[0];
        if (DP_UNLIKELY(!IS_NAME_CHAR(c))) break;
        c = s[1];
        if (DP_UNLIKELY(!IS_NAME_CHAR(c))) { s += 1; break; }
        c = s[2];
        if (DP_UNLIKELY(!IS_NAME_CHAR(c))) { s += 2; break; }
        c = s[3];
        if (DP_UNLIKELY(!IS_NAME_CHAR(c))) { s += 3; break; }
        s += 4;
    }
    p->pos = s;
}

/* Compile-time offset tables for next_sibling and parent_off
 * across all node types. Eliminates the type-dispatched switch in
 * dp_wire_child — one array lookup + one store replaces 5-way branch.
 * TODO 158: branchless tree wiring.
 *
 * (#450) ALL node types use unscaled int32 sibling edges — every
 * compact-encoding range assumption broke at document scale; the
 * uniform encoding removes the overflow table from the parse path. */
static const size_t dp_ns_off_int32[5] = {
    offsetof(struct leptris_element,  next_sibling_off),
    offsetof(LeptrisTextNode,        next_sibling_off),
    offsetof(LeptrisCommentNode,     next_sibling_off),
    offsetof(LeptrisCDATANode,       next_sibling_off),
    offsetof(LeptrisPINode,          next_sibling_off),
};
static const size_t dp_par_off[5] = {
    offsetof(struct leptris_element,  parent_off),
    offsetof(LeptrisTextNode,        parent_off),
    offsetof(LeptrisCommentNode,     parent_off),
    offsetof(LeptrisCDATANode,       parent_off),
    offsetof(LeptrisPINode,          parent_off),
};

/* Wire child into parent's child chain. Uses compile-time offset
 * tables for branchless type dispatch — no switch, no branch predict. */
static inline void dp_wire_child(DParser* p, LeptrisElement parent,
                                  LeptrisNode* child) {
    unsigned t = (unsigned)child->type;
    if (t < 5) {
        int32_t* par_field =
            (int32_t*)((char*)child + dp_par_off[t]);
        *par_field = leptris_compact_int32_encode(
            child, parent, (const int32_t*)par_field);
    }

    LeptrisNode* prev_last = p->last_child_stack[p->depth - 1];
    if (prev_last) {
        unsigned pt = (unsigned)prev_last->type;
        if (pt <= LEPTRIS_NODE_TYPE_PI) {
            /* All sibling edges: unscaled int32 byte offsets (#450 —
             * cp16 ranges cannot hold cross-block sibling links on
             * large documents). Routed through the encoder so a
             * delta that overflows int32 (or collides with the
             * sentinel) lands in the overflow table instead of
             * truncating (issue #478). */
            int32_t* sib_field =
                (int32_t*)((char*)prev_last + dp_ns_off_int32[pt]);
            *sib_field = leptris_compact_int32_encode(
                prev_last, child, (const int32_t*)sib_field);
        }
    } else {
        parent->first_child_off = leptris_compact_int32_encode(
            parent, child, &parent->first_child_off);
    }
    p->last_child_stack[p->depth - 1] = child;

    if (t == LEPTRIS_NODE_TYPE_ELEMENT) {
        parent->child_count++;
    }
}

/* Append a node to the document-child chain (issue #580). The first
 * epilog capture moves the root element into the chain between the
 * prolog nodes and everything that follows. */
static inline void dp_doc_child(DParser* p, LeptrisNode* n) {
    /* #612: document linkage — the #526 setters resolve the owning
     * document through owner_doc; parse-created doc-level nodes
     * must carry it exactly like leptris_pi_node_create ones. */
    if (n && n->type == LEPTRIS_NODE_TYPE_PI)
        ((LeptrisPINode*)n)->owner_doc = p->doc;
    else if (n && n->type == LEPTRIS_NODE_TYPE_COMMENT)
        ((LeptrisCommentNode*)n)->owner_doc = p->doc;
    if (!p->root_chained && p->root) {
        LeptrisNode* rn = (LeptrisNode*)p->root;
        if (p->dc_tail) leptris_node_set_next_sibling(p->dc_tail, rn);
        else p->dc_head = rn;
        p->dc_tail = rn;
        p->root_chained = 1;
    }
    if (p->dc_tail) leptris_node_set_next_sibling(p->dc_tail, n);
    else p->dc_head = n;
    p->dc_tail = n;
}

/* Inline attribute allocation. Takes the next slot from the
 * pre-allocated attr_block (bump pointer) when capacity remains,
 * else falls back to a per-attr pool_alloc. Both name and value
 * are zero-copy pointers into the buffer copy (already NUL-
 * terminated in-place by the caller). Skips name interning and
 * value pool_strdup entirely — direct_parse path excludes entity
 * inputs, so has_entities is always 0. */
static inline int dp_add_attr_inline(DParser* p, LeptrisElement elem,
                                      char* name, size_t name_len,
                                      char* val, size_t val_len,
                                      int has_amp, int has_ws,
                                      const char* name_colon) {
    /* Attribute-value normalization (XML 1.0 §3.3.3, issue #576):
     * each literal tab/LF/CR in a CDATA attribute becomes a single
     * space. Character references (&#9;) are ASCII text here —
     * untouched by the replacement, so they expand later as literal
     * whitespace, exactly as the spec requires. Clean values keep
     * the zero-copy view; only affected values pay the pool copy. */
    if (has_ws) {
        char* norm = (char*)leptris_pool_alloc(p->pool, val_len + 1);
        if (!norm) return -1;
        for (size_t i = 0; i < val_len; i++) {
            char ch = val[i];
            norm[i] = (ch == '\t' || ch == '\n' || ch == '\r')
                          ? ' ' : ch;
        }
        norm[val_len] = '\0';
        val = norm;
    }
    struct leptris_attribute* attr = p->attr_cursor;
    if (DP_UNLIKELY(attr >= p->attr_end)) {
        attr = (struct leptris_attribute*)leptris_pool_alloc(
            p->pool, sizeof(struct leptris_attribute));
        if (!attr) return -1;
    } else {
        p->attr_cursor = attr + 1;
    }

    attr->name_view = leptris_sv_from_ptr(name, name_len);
    leptris_attr_value_set_heap(attr, leptris_sv_from_ptr(val, val_len));
    /* Round 19 packed tail: single name_hash store carries both the
     * lazy-hash sentinel (0) and the entity flag (bit 15). Entity
     * routing (has_amp from the caller's fused scan):
     * - DTD present: eagerly expand; the owned pool copy REPLACES
     *   the view, entities=0.
     * - No DTD: view stays raw, entities=1 — accessors expand
     *   predefined entities lazily on first read.
     * - No '&': nothing to do. */
    unsigned ent = 0;
    if (val_len > 0 && has_amp) {
        if (p->dtd) {
            LeptrisStringView dsv = leptris_sv_from_ptr(val, val_len);
            char* expanded = leptris_decode_entities_view_with_dtd(
                &dsv, p->dtd, p->pool);
            if (expanded) {
                leptris_attr_value_set_heap(attr, leptris_sv_from_cstr(expanded));
            } else {
                ent = 1;
            }
        } else {
            ent = 1;
        }
    }
    attr->name_hash = (uint16_t)(ent << 15);
    attr->ns_cache_off = 0;  /* TODO 173: side cache allocated on demand */
    /* #542 fused into the parse loop (was the post-loop
     * dp_stamp_attr_owners pass): a prefixed attr name gets its
     * ns side-cache HERE — owner stamped, prefix materialized from
     * the just-scanned name. Per-attr independent; order within the
     * element is irrelevant. Unprefixed names pay one NULL check. */
    if (name_colon) {
        struct leptris_attr_ns_cache* c =
            (struct leptris_attr_ns_cache*)leptris_pool_alloc(
                p->pool, sizeof(*c));
        if (c) {
            memset(c, 0, sizeof(*c));
            size_t pl = (size_t)(name_colon - name);
            char* pfx = (char*)leptris_pool_alloc(p->pool, pl + 1);
            if (pfx) {
                memcpy(pfx, name, pl);
                pfx[pl] = '\0';
                c->prefix = pfx;
            }
            c->owner_elem = elem;
            attr_set_ns_cache(attr, c);
        }
    }
    /* TODO 183 Phase 5 (TODO 181 Phase D): cp16 sibling edge. Attrs of
     * one element are adjacent attr_block slots (distance ≤ K × 40 B,
     * far inside cp16 range) — direct store, no encoder call on the
     * hot path. The defensive walk below uses the decoder. */
    attr->next_cp = 0;

    /* Wire attr into elem's attr list. TODO 159 Phase G: use the
     * parser-local last-attr cache for O(1) wiring instead of walking
     * to find the tail (was O(N) per attr, O(N²) per element).
     *
     * Careful: first_attribute_off == 0 means empty list. We can't decode
     * the pointer and check for NULL — at offset 0 the decoded pointer
     * is `elem` itself, which is non-NULL. Check the offset field. */
    if (elem->first_attribute_off != 0) {
        /* Cache should always be valid mid-parse. Fall back to walk
         * only if cache is NULL (defensive — shouldn't happen). */
        struct leptris_attribute* tail = p->current_elem_last_attr;
        if (tail) {
            /* Same-element attrs are adjacent block slots — direct
             * cp16 store, no encoder call (TODO 183 Phase 5). */
            tail->next_cp = (int16_t)(((char*)attr - (char*)tail) >> 3);
        } else {
            tail = (struct leptris_attribute*)((char*)elem + elem->first_attribute_off);
            while (leptris_attr_next(tail)) tail = leptris_attr_next(tail);
            leptris_attr_set_next(tail, attr);
        }
    } else {
        /* First attr of this element: the only place the elem->attr
         * offset is ever needed. */
        elem->first_attribute_off =
            (int32_t)((char*)attr - (char*)elem);
    }
    p->current_elem_last_attr = attr;
    p->cur_attr_count++;
    return 0;
}

/* Handle `<!DOCTYPE ...>` markup. Extracted as a cold path
 * (TODO 166 Phase A): runs 0–1 times per document, but the body is
 * ~140 lines including PUBLIC/SYSTEM re-scan, internal-subset
 * extraction, and DTD parsing. Keeping it inline bloats the hot
 * parse loop's i-cache footprint for no benefit.
 *
 * Returns:
 *   0  — handled, caller should `continue` the parse loop.
 *  -1  — parse error, caller should fail. */
static LEPTRIS_NOINLINE int dp_parse_doctype(DParser* p) {
    p->pos += 2; /* skip "<!" */
    /* Match "DOCTYPE" keyword (case-sensitive per XML spec). */
    if (p->end - p->pos < 7 ||
        memcmp(p->pos, "DOCTYPE", 7) != 0) {
        while (p->pos < p->end && *p->pos != '>') p->pos++;
        if (p->pos < p->end) p->pos++;
        return 0;
    }
    p->pos += 7;
    while (p->pos < p->end && IS_WS(*p->pos)) p->pos++;
    /* Scan DOCTYPE name. */
    char* dt_name_start = p->pos;
    dp_scan_name(p);
    size_t dt_name_len = p->pos - dt_name_start;

    /* Skip to '[' (internal subset) or '>' (no subset). */
    char* subset_start = NULL;
    char* subset_end = NULL;
    while (p->pos < p->end && *p->pos != '>' && *p->pos != '[') {
        p->pos++;
    }
    if (p->pos < p->end && *p->pos == '[') {
        subset_start = ++p->pos;
        /* Find matching ']'. Nested brackets aren't legal
         * in DTD internal subsets, so no depth tracking. */
        while (p->pos < p->end && *p->pos != ']') p->pos++;
        subset_end = p->pos;
        if (p->pos < p->end) p->pos++; /* skip ']' */
        /* Skip to '>'. */
        while (p->pos < p->end && *p->pos != '>') p->pos++;
        if (p->pos < p->end) p->pos++; /* skip '>' */
    } else {
        /* No internal subset, skip to '>'. */
        while (p->pos < p->end && *p->pos != '>') p->pos++;
        if (p->pos < p->end) p->pos++; /* skip '>' */
    }

    /* Create DOCTYPE node with the extracted name. */
    LeptrisDoctypeNode* dt = NULL;
    if (dt_name_len > 0) {
        dt = leptris_doctype_create(
            dt_name_start, dt_name_len, p->pool);
        if (dt) {
            p->doc->doctype = dt;
        }
    }

    /* Parse PUBLIC/SYSTEM identifiers (issue #253).
     * DOCTYPE grammar (XML 1.0 §2.8):
     *   <!DOCTYPE name (SYSTEM quoted | PUBLIC quoted quoted)? subset?>
     * The keywords and quoted strings sit between the name
     * and the '[' or '>'. We need to re-scan that region
     * because the initial skip above skipped past them
     * without extracting values. */
    /* Re-scan from after the name to find PUBLIC/SYSTEM. */
    char* scan = dt_name_start + dt_name_len;
    /* Skip whitespace after name. */
    while (scan < p->end && IS_WS(*scan)) scan++;
    char* public_id = NULL;
    size_t public_id_len = 0;
    char* system_id = NULL;
    size_t system_id_len = 0;
    if (scan + 6 <= p->end && memcmp(scan, "SYSTEM", 6) == 0) {
        scan += 6;
        while (scan < p->end && IS_WS(*scan)) scan++;
        if (scan < p->end && (*scan == '"' || *scan == '\'')) {
            char q = *scan++;
            system_id = scan;
            while (scan < p->end && *scan != q) scan++;
            system_id_len = scan - system_id;
            if (scan < p->end) scan++; /* skip closing quote */
        }
    } else if (scan + 6 <= p->end && memcmp(scan, "PUBLIC", 6) == 0) {
        scan += 6;
        while (scan < p->end && IS_WS(*scan)) scan++;
        /* Public ID (quoted) */
        if (scan < p->end && (*scan == '"' || *scan == '\'')) {
            char q = *scan++;
            public_id = scan;
            while (scan < p->end && *scan != q) scan++;
            public_id_len = scan - public_id;
            if (scan < p->end) scan++; /* skip closing quote */
        }
        /* System ID (quoted, after whitespace) */
        while (scan < p->end && IS_WS(*scan)) scan++;
        if (scan < p->end && (*scan == '"' || *scan == '\'')) {
            char q = *scan++;
            system_id = scan;
            while (scan < p->end && *scan != q) scan++;
            system_id_len = scan - system_id;
            if (scan < p->end) scan++; /* skip closing quote */
        }
    }
    /* NUL-terminate and set on the DOCTYPE node. The
     * values point into the mutable buffer copy. */
    if (dt) {
        if (public_id && public_id_len > 0) {
            public_id[public_id_len] = '\0';
            leptris_doctype_set_public_id(dt, public_id, p->pool);
        }
        if (system_id && system_id_len > 0) {
            system_id[system_id_len] = '\0';
            leptris_doctype_set_system_id(dt, system_id, p->pool);
        }
    }

    /* Parse internal subset if non-empty — builds the
     * entity table for custom entity expansion, and
     * stores the raw text on the DOCTYPE node so the
     * public API (leptris_doctype_get_internal_subset)
     * returns it (#253). */
    if (subset_start && subset_end > subset_start) {
        /* NUL-terminate the subset text in the buffer. */
        *subset_end = '\0';
        if (dt) {
            leptris_doctype_set_internal_subset(
                dt, subset_start, p->pool);
        }
        LeptrisDTD* dtd = leptris_dtd_parse_internal_subset(
            subset_start, (size_t)(subset_end - subset_start),
            p->pool);
        if (dtd) {
            p->dtd = dtd;
        }
    }
    return 0;
}

/* Fused prefix split + 16-bit FNV-1a of the local name (TODO 159
 * hash), one bounded walk instead of strchr + a second walk to NUL.
 * ':' resets the hash so it covers exactly the local part.
 *
 * Deliberately noinline: inlining this into the element branch of
 * direct_parse_internal grew the hot attr loop's neighborhood and
 * regressed K=100 by ~1.5% (code layout, not work). Called once
 * per element; the call cost is ~1ns against 51us at K=5. */
static LEPTRIS_ALWAYS_INLINE void dp_split_hash_name(LeptrisElement elem, char* name_start,
                               size_t name_len, LeptrisMemoryPool* pool) {
    char* colon = NULL;
    uint16_t h = 0x811C;
    /* A colon needs prefix (>=1) + ':' + local (>=1) = 3 bytes; 1-2
     * byte names skip the colon test in the walk entirely. */
    const int colon_possible = name_len >= 3;
    for (const char* c = name_start; c < name_start + name_len; c++) {
        if (colon_possible && DP_UNLIKELY(*c == ':')) {
            colon = (char*)c;
            h = 0x811C; /* restart on the local part */
            continue;
        }
        h ^= (unsigned char)*c;
        h *= 0x0193;
    }
    elem->name_hash = h;
    if (colon) {
        *colon = '\0';
        leptris_elem_set_prefix(elem, name_start, pool);
        elem->name = colon + 1;
        size_t local_len = name_len - (size_t)(colon + 1 - name_start);
        elem->name_len = (local_len > 254) ? 0xFF : (uint8_t)local_len;
    } else {
        elem->name = name_start;
        elem->name_len = (name_len > 254) ? 0xFF : (uint8_t)name_len;
    }
}

/* Parse attributes for an element. Writes attr structs into the
 * pre-allocated attr_block (zero-copy name/value, no interning).
 * Names are NUL-terminated in-place AFTER '=' is consumed; values
 * are NUL-terminated in-place at the closing quote. */
static int dp_raw_attr(DParser* p, LeptrisElement elem,
                       const char* qname, const char* value);
static struct leptris_ns_cache* dp_ensure_cache(DParser* p,
                                                LeptrisElement elem);

static int dp_parse_attrs(DParser* p, LeptrisElement elem) {
    /* Reset the per-element caches so dp_add_attr_inline and the
     * xmlns wiring below can both run in O(1) per attr. */
    p->current_elem_last_attr = NULL;
    p->current_elem_last_ns = NULL;
    p->raw_last = NULL;
    p->cur_attr_count = 0;
    /* Sentinel-terminated (parse endgame, third application): buf[len]
     * is NUL and NUL fails every classification below — not '>', not
     * '/', not '=' , not a quote, not IS_NAME_START. Each former
     * bounds check returned -1 on overrun; the NUL now reaches the
     * same return -1 through the character checks, so the compares
     * were pure overhead (3-4 per attribute). */
    for (;;) {
        dp_skip_ws(p);
        char c = *p->pos;
        if (c == '>' || (c == '/' && p->pos[1] == '>')) {
            int self_close = (c == '/');
            p->pos += self_close ? 2 : 1;
            elem->attr_count = (uint8_t)p->cur_attr_count;
            /* Issue #542: prefixed attrs got their owner-stamped
             * side-cache inline in dp_add_attr_inline. */
            return self_close ? 1 : 0;
        }
        if (c == '/') return -1;  /* '/' not followed by '>' */

        /* Attribute name — scan as (pointer, length), no NUL-term. */
        char* name_start = p->pos;
        if (!IS_NAME_START(*p->pos)) return -1;
        p->pos++;
        dp_scan_name(p);
        char* name_end = p->pos;
        size_t name_len = name_end - name_start;
        /* Prefixed-name probe (fused here; was the post-loop
         * dp_stamp_attr_owners pass): a colon needs prefix + ':'
         * + local = >= 3 bytes, so 1-2 byte names skip entirely.
         * Short names inline (a memchr call costs more than the
         * 2-4 iterations); long names keep memchr. */
        const char* attr_colon = NULL;
        if (name_len >= 3) {
            if (name_len <= 16) {
                for (const char* c9 = name_start; c9 < name_end; c9++) {
                    if (*c9 == ':') { attr_colon = c9; break; }
                }
            } else {
                attr_colon =
                    (const char*)memchr(name_start, ':', name_len);
            }
        }
        /* Defer NUL-termination until after '=' is consumed — the
         * delimiter byte (whitespace or '=') is needed for the scan. */

        /* Skip = and whitespace. NUL sentinel fails the '=' test. */
        dp_skip_ws(p);
        if (*p->pos != '=') return -1;
        p->pos++;
        /* '=' consumed — the delimiter byte at name_end is no longer
         * needed. Safe to NUL-terminate the name in-place now. */
        *name_end = '\0';
        dp_skip_ws(p);

        /* Quoted value — FUSED scan (TODO 184): one pass finds the
         * closing quote AND flags '&' for entity routing. Replaces
         * quote-memchr + separate amp check (two passes; memchr pays
         * ~10ns setup even on 6-byte values — the TODO 174 finding,
         * applied to the quote scan too).
         *
         * First 48 bytes inline (byte loop beats memchr setup — the
         * common case: values are 5-15 bytes). Longer values fall
         * back to SIMD: memchr for the quote, leptris_text_contains
         * for '&' over the scanned prefix. pugixml's single-table
         * ct_parse_attr loop is the model; this is its shape with a
         * SIMD tail. */
        char quote = *p->pos;
        if (quote != '"' && quote != '\'') return -1; /* NUL sentinel fails */
        p->pos++;
        char* val_start = p->pos;
        int has_amp = 0;
        int has_ws = 0;   /* literal \t/\n/\r — §3.3.3 (#576) */
        char* val_end = NULL;
        {
            const char* q = p->pos;
            const char* probe_end = p->probe_slack
                ? q + 48
                : ((p->end - q > 48) ? q + 48 : p->end);
            while (q < probe_end) {
                char vch = *q;
                if (vch == quote) { val_end = (char*)q; goto value_done; }
                if (vch == '&') has_amp = 1;
                else if (vch == '\t' || vch == '\n' || vch == '\r')
                    has_ws = 1;
                q++;
            }
            if (q >= p->end) return -1; /* unterminated */
            val_end = (char*)memchr(q, quote, p->end - q);
            if (!val_end) return -1;
            if (q < val_end) {
                if (!has_amp &&
                    leptris_text_contains(q, (size_t)(val_end - q), '&'))
                    has_amp = 1;
                if (!has_ws) {
                    for (const char* w = q; w < val_end; w++) {
                        if (*w == '\t' || *w == '\n' || *w == '\r') {
                            has_ws = 1;
                            break;
                        }
                    }
                }
            }
        }
    value_done:
        p->pos = val_end;
        *p->pos = '\0'; /* Safe: overwrites closing quote, not a delimiter */
        size_t val_len = p->pos - val_start;
        p->pos++; /* skip the NUL */

        /* Check for xmlns. */
        if (name_len >= 5 && name_start[0] == 'x' &&
            name_start[1] == 'm' && name_start[2] == 'l' &&
            name_start[3] == 'n' && name_start[4] == 's') {
            /* xmlns or xmlns:prefix. Name was NUL-terminated at
             * name_end above; value at val_start (closing quote
             * replaced with NUL). For "xmlns:foo" the prefix
             * portion is name_start+6 .. name_end; pool-alloc +
             * memcpy because we can't split the name in-place. */
            const char* ns_prefix = NULL;
            if (name_len > 6) {
                size_t plen = name_len - 6;
                ns_prefix = (char*)leptris_pool_alloc(p->pool, plen + 1);
                if (!ns_prefix) return -1;
                memcpy((void*)ns_prefix, name_start + 6, plen);
                ((char*)ns_prefix)[plen] = '\0';
            }
            struct leptris_namespace* ns =
                (struct leptris_namespace*)leptris_pool_alloc(
                    p->pool, sizeof(struct leptris_namespace));
            if (!ns) return -1;
            ns->prefix = (char*)ns_prefix;
            ns->uri = val_start; /* already NUL-terminated */
            /* Entity-bearing xmlns values (bug-140: xmlns="&svgns;")
             * expand through the DTD — the raw view must never
             * become a namespace URI. */
            if (has_amp && p->dtd) {
                LeptrisStringView nsv =
                    leptris_sv_from_ptr(val_start, val_len);
                char* nuri = leptris_decode_entities_view_with_dtd(
                    &nsv, p->dtd, p->pool);
                if (nuri) ns->uri = nuri;
            }
            ns->next = NULL;
            /* TODO 155 Phase A: parser has the pool directly; use it
             * to allocate ns_cache without going through the
             * (not-yet-registered) document. */
            struct leptris_ns_cache* ecache = dp_ensure_cache(p, elem);
            if (!ecache) return -1;
            struct leptris_namespace** head_ptr = &ecache->declarations;
            if (!head_ptr) return -1;
            if (!*head_ptr) {
                *head_ptr = ns;
            } else {
                /* TODO 159 Phase G: use cached last-ns pointer for
                 * O(1) wiring instead of walking to the tail. */
                struct leptris_namespace* tail = p->current_elem_last_ns;
                if (tail) {
                    tail->next = ns;
                } else {
                    tail = *head_ptr;
                    while (tail->next) tail = tail->next;
                    tail->next = ns;
                }
            }
            p->current_elem_last_ns = ns;
            p->saw_namespace = 1;
            if (dp_raw_attr(p, elem, name_start, ns->uri) != 0) return -1;
            continue;
        }

        /* Regular attribute — zero-copy name/value, bulk-allocated struct. */
        if (dp_add_attr_inline(p, elem, name_start, name_len,
                                val_start, val_len, has_amp, has_ws,
                                attr_colon) != 0)
            return -1;
        if (dp_raw_attr(p, elem, name_start, val_start) != 0) return -1;
    }
    /* for (;;): every exit is a return inside the loop (0 = tag
     * closed, 1 = self-closing, -1 = error) — there is no
     * fallthrough path, so no trailing statement. */
}
static struct leptris_ns_cache* dp_ensure_cache(DParser* p,
                                                LeptrisElement elem) {
    if (elem->ns_cache) return elem->ns_cache;
    struct leptris_ns_cache* c;
    if (p->nsc_cursor < p->nsc_end) {
        c = p->nsc_cursor++;
    } else {
        c = (struct leptris_ns_cache*)leptris_pool_alloc(
            p->pool, 64 * sizeof(struct leptris_ns_cache));
        if (!c) return NULL;
        p->nsc_end = c + 64;
        p->nsc_cursor = c + 1;
    }
    c->prefix = NULL;
    c->namespace_uri = NULL;
    c->declarations = NULL;
    c->raw_attrs = NULL;
    c->doc_next = NULL;
    c->prefix_heap = 0;
    c->uri_heap = 0;
    elem->ns_cache = c;
    return c;
}

/* Issue #635: append one raw attribute-view entry (attrs AND xmlns
 * declarations, source order) to the element's cache chain. Entries
 * carve from 128-entry pool chunks (the attr/text block pattern) —
 * one bump per attr, O(1) tail via p->raw_last; the old per-attr
 * pool_alloc + tail walk was the largest single cost of attr-heavy
 * parses (39% of attr-heavy-5k). */
static int dp_raw_attr(DParser* p, LeptrisElement elem,
                       const char* qname, const char* value) {
    struct leptris_ns_cache* cache = dp_ensure_cache(p, elem);
    if (!cache) return 0;
    struct leptris_raw_attr* ra;
    if (p->raw_cursor < p->raw_end) {
        ra = p->raw_cursor++;
    } else {
        ra = (struct leptris_raw_attr*)leptris_pool_alloc(
            p->pool, 128 * sizeof(struct leptris_raw_attr));
        if (!ra) return -1;
        p->raw_end = ra + 128;
        p->raw_cursor = ra + 1;
    }
    ra->qname = qname;
    ra->value = value;
    ra->next = NULL;
    if (p->raw_last) {
        p->raw_last->next = ra;
    } else {
        cache->raw_attrs = ra;
    }
    p->raw_last = ra;
    return 0;
}


/* Inline borrowed-text creation from the bulk block (round 8): the
 * same stores leptris_text_create_borrowed performs, minus the
 * out-of-line pool_alloc call per node. */
/* Carve a comment/CDATA/PI node from the bulk block. Returns a
 * zeroed node with base.type set; callers stamp line/frozen and the
 * content pointers (zero-copy into the document buffer). Returns
 * NULL on exhaustion — callers fall back to the pool constructors. */
static inline void* dp_cpi_carve(DParser* p, int node_type) {
    if (p->cpi_cursor >= p->cpi_end) return NULL;
    void* n = (void*)p->cpi_cursor;
    p->cpi_cursor += p->cpi_stride;
    memset(n, 0, p->cpi_stride);
    ((LeptrisNode*)n)->type = (unsigned char)node_type;
    return n;
}

static inline LeptrisTextNode* dp_text_create(DParser* p,
                                             const char* content,
                                             size_t content_len) {
    LeptrisTextNode* tn = p->text_cursor;
    if (DP_UNLIKELY(tn >= p->text_end)) {
        /* The lt_count bound under-counts when comments/PIs/CDATA
         * interleave with text (each can split a run, adding one).
         * Grow by arena chunks instead of falling to the out-of-line
         * pool path per node — one bump per 128 nodes. */
        tn = (LeptrisTextNode*)leptris_pool_alloc(
            p->pool, 128 * sizeof(LeptrisTextNode));
        if (!tn) {
            return leptris_text_create_borrowed(content, content_len,
                                               p->pool);
        }
        p->text_end = tn + 128;
        /* cursor == tn: carve below */
    }
    p->text_cursor = tn + 1;
    tn->base.type = LEPTRIS_NODE_TYPE_TEXT;
    tn->base.frozen = 1;   /* parse-created: set here, not after */
    tn->base.version = 0;
    /* #815: the carve skips the memset dp_cpi_carve pays — a dirty
     * recycled pool page left raw set on ~5-7% of churn-parses and
     * serialize_text_internal then emitted text VERBATIM (raw < in
     * the output, invalid XML). */
    tn->base.raw = 0;
    tn->base.binding_wrapper = NULL;
    tn->base.line = 0;     /* caller stamps the offset */
    tn->content = (char*)content;
    tn->content_len = content_len;
    tn->pool = p->pool;
    tn->borrowed = 1;
    tn->parent_off = 0;
    tn->next_sibling_off = 0;
    return tn;
}

/* Internal: parse from a writable, NUL-terminated buffer.
 * owns_buffer: 1 = document frees buf on leptris_document_free,
 *              0 = caller owns buf (in-place mode). */
/* owns_buffer values:
 *   0 — caller owns the buf (in-place parse). Don't free.
 *   1 — direct_parse_internal owns the buf via separate malloc.
 *       Free on failure; doc->xml_buffer owns it on success.
 *   2 — the input `buf` is const; the parser mallocs its own copy,
 *       FUSED with the count3 sizing pre-scan into one pass
 *       (TODO 188), then behaves as owns_buffer = 1. The copy
 *       lives outside the arena; the document frees it via
 *       doc->xml_buffer_needs_free.
 */
/* §3.3.2 defaults pass: walk the finished tree; for every default-
 * bearing declaration whose element matches, materialize the value
 * when the element does not carry the attribute. */
static struct leptris_document* direct_parse_internal(char* buf, size_t len,
                                                     int owns_buffer,
                                                     int drop_ws_text,
                                                     int apply_dtd_attrs) {
    /* Set when the owns_buffer==2 path made our slack-backed copy. */
    int buf_is_owned_copy = 0;

    /* 2. Create arena-backed pool (TODO 183 Phase 3).
     *
     * Content-derived sizing: one cheap pre-scan counts '<' (an upper
     * bound on tags — close tags and '<' in text only over-count, which
     * over-allocates in the safe direction) and quote characters (each
     * attribute value contributes exactly 2, single- or double-quoted).
     * This sizes the element and attribute blocks to the document's
     * actual shape instead of a fixed len/10 heuristic — the K=100
     * many-attrs case (884 bytes/element) no longer over-allocates
     * attrs 5×, and text-heavy docs reserve room the old est missed.
     *
     * One arena replaces the old "single 4 MB-capped first pool page"
     * trick (#261): there is no page cap, so the bulk elem+attr block
     * and every text/string/mutation allocation share one contiguous
     * span. Exhaustion is a hard NULL → parse fails cleanly (pugixml
     * semantics) instead of scattering a second page megabytes away.
     *
     * Slack: len covers all text/name/value copies (they're substrings
     * of the document), plus len/2 mutation headroom (post-parse
     * append/set calls allocate from the same arena). */
    /* TODO 188: fused copy+count. The owns-copy path used to stream
     * the input TWICE — count3 for arena sizing, then the memcpy
     * into the pool's buffer copy. Now one kernel copies into a
     * separately-malloc'd buffer AND produces the three counters
     * from the same load. The arena no longer carries the copy
     * (buf_extra = 0 below) and the document owns the malloc via
     * the owns_buffer = 1 free paths + doc->xml_buffer. In-place
     * callers (owns_buffer = 0) have no copy to fuse and keep the
     * plain pre-scan. */
    size_t lt_count, dq_count, sq_count;
    if (owns_buffer == 2) {
        /* Retained buffer: inputs >256 KB would otherwise be
         * munmapped on free and re-faulted on the next parse (the
         * arena free-list rationale — see arena.c). */
        /* +64 zeroed slack past the sentinel: probe windows on the
         * owned copy may read up to 48 bytes past p->end (probe_slack)
         * — zeros stop them, NUL matching neither quote nor '&'. */
        char* own = leptris_arena_buffer_alloc(len + 1 + 64);
        if (!own) return NULL;
        leptris_copy_count3(own, buf, len, '<', '"', '\'',
                           &lt_count, &dq_count, &sq_count);
        own[len] = '\0';
        memset(own + len + 1, 0, 64);
        buf = own;
        buf_is_owned_copy = 1;
        owns_buffer = 1;  /* free-on-failure below; doc owns on success */
    } else {
        leptris_text_count3(buf, len, '<', '"', '\'',
                           &lt_count, &dq_count, &sq_count);
    }
    size_t quote_count = dq_count + sq_count;
    size_t est_elems = lt_count + 8;
    size_t elem_bytes = est_elems * sizeof(struct leptris_element);
    size_t attr_bytes = (quote_count / 2 + 64) * sizeof(struct leptris_attribute);
    /* len bounds all text/name/value content copies (substrings of the
     * document), but each text/comment NODE costs ~48-56 B of struct
     * overhead regardless of content length — one per element on
     * mixed-content docs, ~2x the per-item markup. Bound it per
     * element (CI catch: a 134 KB item-heavy doc exhausted the arena
     * at item 4144/5000 with 38 bytes left when this term was
     * missing). */
    size_t node_overhead = est_elems * 64;
    size_t text_room = len + node_overhead;
    size_t slack = len / 2 + 64 * 1024;  /* mutation headroom + floor */
    size_t arena_size = elem_bytes + attr_bytes + text_room + slack;
    LeptrisArena* arena = leptris_arena_create(arena_size);
    if (!arena) {
        if (owns_buffer == 1)
            leptris_arena_buffer_release(buf, len + 1 + 64);
        return NULL;
    }
    LeptrisMemoryPool* pool = leptris_pool_create_arena_backed(arena, 1);
    if (!pool) {
        leptris_arena_destroy(arena);
        if (owns_buffer == 1)
            leptris_arena_buffer_release(buf, len + 1 + 64);
        return NULL;
    }
    if (len >= 256) {
        pool->string_cache = leptris_hash_table_create(pool, 128);
    }

    /* owns_buffer == 2 was resolved above (TODO 188): the input is
     * already copied — fused with the count3 pre-scan into the
     * separately-malloc'd buffer `buf` — and owns_buffer is now 1.
     * There is no second copy here. */

    /* 3. Bulk-allocate element + attribute blocks as ONE contiguous
     * arena bump. Layout: [ elem_block | attr_block ]
     *
     * attr capacity follows the quote-derived estimate (each attr
     * value contributes exactly 2 quote chars). Overflow falls back
     * to per-attr pool alloc — which now bumps the same arena. */
    size_t attr_capacity = quote_count / 2 + 64;
    /* Text-node upper bound: every text node is followed by a '<'
     * (lt_count), so the count can never exceed it. Attr-heavy docs
     * over-reserve (~56 B per tag) — bounded, and the retained arena
     * absorbs it after the first parse of each size. */
    size_t text_bytes = lt_count * sizeof(struct leptris_text_node);
    size_t cpi_stride =
        sizeof(LeptrisPINode) > sizeof(LeptrisCommentNode)
            ? sizeof(LeptrisPINode) : sizeof(LeptrisCommentNode);
    if (cpi_stride < sizeof(LeptrisCDATANode)) cpi_stride = sizeof(LeptrisCDATANode);
    cpi_stride = (cpi_stride + 7u) & ~(size_t)7u;
    size_t cpi_bytes = lt_count * cpi_stride;  /* each markup node starts with '<' */
    char* combined = (char*)leptris_pool_alloc(
        pool, elem_bytes + attr_bytes + text_bytes + cpi_bytes);
    if (!combined) {
        leptris_pool_destroy(pool);
        if (owns_buffer == 1)
            leptris_arena_buffer_release(buf, len + 1 + 64);
        return NULL;
    }
    LeptrisElement elem_block = (LeptrisElement)combined;
    memset(elem_block, 0, elem_bytes);
    struct leptris_attribute* attr_block =
        (struct leptris_attribute*)(combined + elem_bytes);
    struct leptris_text_node* text_block =
        (struct leptris_text_node*)(combined + elem_bytes + attr_bytes);
    char* cpi_block = combined + elem_bytes + attr_bytes + text_bytes;
    /* No memset on attr_block — dp_add_attr_inline initializes every
     * field of each attr it uses. */
    size_t elem_idx = 0;

    /* 4. Create document. Pool-allocated to save one malloc per
     * parse (TODO 154 Phase B). The doc_pool_allocated flag tells
     * leptris_document_free to skip the LEPTRIS_FREE(doc) — pool
     * destroy reclaims it. */
    struct leptris_document* doc = (struct leptris_document*)
        leptris_pool_alloc(pool, sizeof(struct leptris_document));
    if (!doc) {
        leptris_pool_destroy(pool);
        if (owns_buffer == 1)
            leptris_arena_buffer_release(buf, len + 1 + 64);
        return NULL;
    }
    memset(doc, 0, sizeof(*doc));
    doc->doc_pool_allocated = 1;
    doc->strict_mode = g_leptris_strict_mode;
    doc->pool = pool;
    doc->page_base = leptris_pool_get_base(pool);
    doc->ref_count = 1;
    doc->xml_buffer = buf;
    doc->xml_buffer_len = len;
    doc->xml_buffer_needs_free = owns_buffer;
    doc->xml_buffer_slack = buf_is_owned_copy ? 64u : 0u;
    /* No leptris_compact_set_current_document — direct_parse is
     * overflow-table-free. All compact pointer edges use direct
     * offset arithmetic, never touching the shared thread-local
     * overflow hash table. This eliminates cross-document
     * contamination under high document counts (#261). */

    /* 5. Parse. */
    DParser p;
    p.buf = buf;
    p.pos = buf;
    p.end = buf + len;
    p.pool = pool;
    p.doc = doc;
    p.depth = 0;
    p.root = NULL;
    p.dc_head = NULL;
    p.dc_tail = NULL;
    p.root_chained = 0;
    p.version = NULL;
    p.encoding = NULL;
    p.standalone = -1;
    p.had_declaration = 0;
    p.attr_block = attr_block;
    p.attr_idx = 0;
    p.attr_capacity = attr_capacity;
    p.attr_cursor = attr_block;
    p.attr_end = attr_block + attr_capacity;
    p.text_cursor = text_block;
    p.text_end = text_block + lt_count;
    p.cpi_cursor = cpi_block;
    p.cpi_end = cpi_block + cpi_bytes;
    p.raw_cursor = NULL;
    p.raw_end = NULL;
    p.raw_last = NULL;
    p.nsc_cursor = NULL;
    p.nsc_end = NULL;
    p.cpi_stride = cpi_stride;
    /* owns_buffer==2 was converted to 1 above; only the parser's own
     * copy carries the zeroed slack. */
    p.probe_slack = owns_buffer == 1 && buf_is_owned_copy;
    p.drop_ws_text = drop_ws_text;
    p.dtd = NULL;
    /* Only ever set to 1 in the loop; without the zero here the
     * field rides the stack (MSVC C4701) and doc->has_namespaces
     * gets garbage on namespace-less documents. */
    p.saw_namespace = 0;
    p.line_offsets_ok = len < 0x7FFFFFFFu;

    /* Skip BOM. */
    if (len >= 3 && (unsigned char)buf[0] == 0xEF &&
        (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) {
        p.pos += 3;
    }

    while (p.pos < p.end) {
        /* Top-level whitespace: a TEXT child of the document node —
         * chained like prolog comments/PIs so /node() counts it and
         * identity transforms copy it (#580 chain completion,
         * bug-195). */
        if (p.depth == 0) {
            char* ws_start = p.pos;
            dp_skip_ws(&p);
            size_t wl = (size_t)(p.pos - ws_start);
            /* libxml2 keeps document-level ws ONLY after a comment
             * or after the root element (bug-195 vs bug-196: ws
             * after a prolog PI is dropped); leading ws is prolog
             * Misc; trailing ws is trimmed after the loop. */
            if (wl > 0 && p.dc_tail &&
                (p.dc_tail->type == LEPTRIS_NODE_TYPE_COMMENT ||
                 (p.root_chained &&
                  p.dc_tail == (LeptrisNode*)p.root))) {
                LeptrisTextNode* wtn =
                    dp_text_create(&p, ws_start, wl);
                if (!wtn) goto fail;
                wtn->base.frozen = 1;
                dp_doc_child(&p, (LeptrisNode*)wtn);
            }
            if (p.pos >= p.end) break;
        }

        if (*p.pos != '<') {
            /* DROP_WS_TEXT (issue #677): drop whitespace-ONLY runs —
             * the boundary whitespace of NON-blank text is data
             * (libxml2 XML_PARSE_NOBLANKS parity). The run is
             * dropped only when every byte up to the next markup
             * delimiter is whitespace; otherwise the text path sees
             * the leading whitespace untouched. */
            if (p.drop_ws_text &&
                (*p.pos == ' ' || *p.pos == '\t' || *p.pos == '\n' ||
                 *p.pos == '\r')) {
                char* q = p.pos + 1;
                while (q < p.end && *q != '<') {
                    if (*q != ' ' && *q != '\t' && *q != '\n' &&
                        *q != '\r')
                        break;
                    q++;
                }
                if (q >= p.end) break;
                if (*q == '<') { p.pos = q; continue; }
            }
            /* Text content — FUSED scan (TODO 184): one inline loop
             * finds '<' AND counts '\n' (issue #223 line tracking),
             * replacing the memchr + dp_advance_line re-walk (two
             * passes; memchr pays ~10ns setup on short text — the
             * same finding as the attr-value scan). First 48 bytes
             * inline, SIMD fallback for longer spans. */
            char* text_start = p.pos;
            uint32_t text_off =
                p.line_offsets_ok
                    ? (uint32_t)(text_start - p.buf) + 1u : 0u;
            char* lt = NULL;
            {
                const char* q = p.pos;
                const char* probe_end = p.probe_slack
                    ? q + 48
                    : ((p.end - q > 48) ? q + 48 : p.end);
                while (q < probe_end) {
                    if (*q == '<') { lt = (char*)q; goto text_done; }
                    q++;
                }
                if (q >= p.end) goto text_done;
                lt = (char*)memchr(q, '<', p.end - q);
                if (!lt) lt = p.end;
            }
        text_done:
            p.pos = lt ? lt : p.end;
            size_t tlen = p.pos - text_start;

            if (tlen == 0) continue;
            /* (ws handling lives at the text-branch ENTRY — see the
             * drop_ws_text fast path before text_start.) */
            if (p.depth == 0) {
                /* Whitespace-only between root and PIs. */
                for (char* c = text_start; c < p.pos; c++) {
                    if (!IS_WS(*c)) goto fail;
                }
                continue;
            }
            /* Text node. When a DTD is present (custom entity
             * declarations), eagerly expand entities into a pool-
             * allocated string — borrowed storage can't resolve
             * &foo; without the DTD at access time. Without a DTD,
             * stay borrowed and let leptris_text_get_content expand
             * predefined entities lazily. */
            LeptrisTextNode* tn;
            if (p.dtd && tlen > 0 &&
                memchr(text_start, '&', tlen) != NULL) {
                LeptrisStringView sv = leptris_sv_from_ptr(text_start, tlen);
                char* expanded = leptris_decode_entities_view_with_dtd(
                    &sv, p.dtd, pool);
                if (expanded) {
                    tn = leptris_text_create(expanded, strlen(expanded), pool);
                } else {
                    tn = dp_text_create(&p, text_start, tlen);
                }
            } else {
                tn = dp_text_create(&p, text_start, tlen);
            }
            if (!tn) goto fail;
            tn->base.line = text_off;
            if (!tn->base.frozen) tn->base.frozen = 1;
            dp_wire_child(&p, p.open_stack[p.depth - 1], (LeptrisNode*)tn);
            continue;
        }

        /* '<' — dispatch. */
        if (p.end - p.pos < 2) goto fail;
        char next = p.pos[1];

        if (IS_NAME_START(next)) {
            /* Element. Snapshot line BEFORE scanning the open tag so
             * the element reports the line where '<' appeared. */
            uint32_t elem_off = p.line_offsets_ok
                ? (uint32_t)(p.pos - p.buf) + 1u : 0u;
            LeptrisElement elem;
            if (elem_idx < est_elems) {
                elem = &elem_block[elem_idx++];
            } else {
                /* Bulk block exhausted — fall back to pool_alloc.
                 * Pool pages are contiguous, so compact-pointer
                 * offsets remain valid as long as the pool's first
                 * page is large enough to hold both the bulk block
                 * and these overflow elements. direct_parse sizes
                 * the first page for exactly this (see page_size
                 * calculation above). */
                elem = (LeptrisElement)leptris_pool_alloc(
                    pool, sizeof(struct leptris_element));
                if (!elem) goto fail;
                memset(elem, 0, sizeof(struct leptris_element));
            }
            elem->base.type = LEPTRIS_NODE_TYPE_ELEMENT;
            elem->base.line = elem_off;
            /* Parse-built trees are frozen (immutable until a
             * mutation copies) — set at creation instead of the old
             * post-parse tree walk, which cost ~11% of parse at
             * K=5 in the sample profile (TODO 187). */
            elem->base.frozen = 1;
            /* TODO 155 Phase A: document field removed; root registered
             * below via leptris_root_doc_register. */

            /* Scan name (zero-copy). DON'T NUL-terminate yet — the
             * byte after the name is '>' or whitespace, which
             * dp_parse_attrs needs to see intact. We NUL-terminate
             * AFTER dp_parse_attrs returns. */
            p.pos++; /* skip '<' */
            char* name_start = p.pos;
            dp_scan_name(&p);
            size_t name_len = p.pos - name_start;

            /* Parse attributes (scans from the delimiter position). */
            int self_closing = dp_parse_attrs(&p, elem);
            if (self_closing < 0) goto fail;

            /* NOW safe to NUL-terminate the name — dp_parse_attrs
             * has finished scanning the open tag. */
            name_start[name_len] = '\0';

            dp_split_hash_name(elem, name_start, name_len, p.pool);

            /* Wire into parent. */
            if (p.depth > 0) {
                dp_wire_child(&p, p.open_stack[p.depth - 1], (LeptrisNode*)elem);
            } else {
                if (p.root) goto fail; /* multiple top-level elements */
                p.root = elem;
            }

            if (!self_closing) {
                /* Respect custom depth limit (g_leptris_max_depth)
                 * when set by the caller; fall back to the
                 * compile-time default otherwise. */
                extern LEPTRIS_THREAD_LOCAL int g_leptris_max_depth;
                int max_depth = g_leptris_max_depth > 0
                    ? g_leptris_max_depth : DP_MAX_DEPTH;
                if (p.depth >= max_depth) goto fail;
                p.open_stack[p.depth++] = elem;
                /* Initialize last_child_stack for the new depth. */
                p.last_child_stack[p.depth - 1] = NULL;
            }
        }
        else if (next == '/') {
            /* Close tag. */
            p.pos += 2;
            /* Scan name (includes prefix:local). */
            char* close_start = p.pos;
            dp_scan_name(&p);
            size_t close_len = p.pos - close_start;
            dp_skip_ws(&p);
            if (p.pos >= p.end || *p.pos != '>') goto fail;
            p.pos++;
            if (p.depth == 0) goto fail;
            /* Verify close tag matches open element. The open
             * element's name is the LOCAL part (prefix stripped).
             * Compare the local part of the close tag, which is
             * everything after ':' (or the whole name if no ':'). */
            LeptrisElement open = p.open_stack[p.depth - 1];
            const char* open_name = open->name;
            size_t open_len = (open->name_len != 0xFF)
                ? (size_t)open->name_len : strlen(open_name);
            const char* close_local = close_start;
            size_t close_local_len = close_len;
            /* Strip the close tag's prefix ONLY when the open element
             * was prefixed. Exact, allocation-independent test: the
             * open tag's colon was NUL-terminated in-place, so
             * open->name[-1] is that NUL for prefixed names and '<'
             * for unprefixed ones (element names directly follow '<').
             * An unprefixed open closed by a prefixed name already
             * fails the length compare below — same result as the old
             * unconditional scan, without the per-element memchr call
             * (a libc call for a 1-6 byte span — the TODO 174 law). */
            const char* colon = NULL;
            if (open->name[-1] == '\0') {
                if (close_len < 16) {
                    for (const char* c = close_start;
                         c < close_start + close_len; c++) {
                        if (*c == ':') { colon = c; break; }
                    }
                } else {
                    colon = (const char*)memchr(close_start, ':', close_len);
                }
            }
            if (colon) {
                close_local = colon + 1;
                close_local_len = close_len - (colon + 1 - close_start);
            }
            if (open_len != close_local_len) goto fail;
            /* Masked 64-bit compare for short names — one load
             * each side vs a libc memcmp call (the TODO 174 law:
             * setup dominates sub-8-byte compares; ~9% of the
             * element-heavy corpus profile). The 8-byte load may
             * read past the name: safe outright on the owned copy
             * (zeroed slack), and safe on caller buffers whenever
             * the load stays inside [buf, end] (the sentinel at
             * buf[len] is the +1) — only names within 8 bytes of the
             * document end fall back to memcmp (issue #561: gating
             * the whole fast path on probe_slack cost inplace
             * parsing a memcmp per element). */
            if (close_local_len <= 8 &&
                (p.probe_slack ||
                 close_local + 8 <= p.end + 1)) {
                uint64_t mask = (close_local_len == 8)
                    ? ~(uint64_t)0
                    : ((1ull << (close_local_len * 8)) - 1);
                uint64_t a, b;
                memcpy(&a, open_name, 8);
                memcpy(&b, close_local, 8);
                if ((a & mask) != (b & mask)) goto fail;
            } else if (memcmp(open_name, close_local,
                              close_local_len) != 0) {
                goto fail;
            }
            p.depth--;
            /* Clear the closed level's tail cache (AFTER the
             * decrement: at max depth the pre-decrement index would
             * write past the array): a stale prev_last here wires
             * cross-level siblings (issue #478). */
            p.last_child_stack[p.depth] = NULL;
        }
        else if (next == '!') {
            /* Comment, CDATA, or DOCTYPE. */
            uint32_t markup_off = p.line_offsets_ok
                ? (uint32_t)(p.pos - p.buf) + 1u : 0u;
            if (p.end - p.pos >= 4 && p.pos[2] == '-' && p.pos[3] == '-') {
                /* Comment: <!-- ... --> */
                p.pos += 4;
                char* start = p.pos;
                /* TODO 177: SIMD find3 for "-->" (NEON/AVX2 when
                 * available, scalar memchr-anchor fallback). Replaces
                 * the per-candidate dash verify loop; dash-run-heavy
                 * comment bodies no longer re-scan candidates. */
                ptrdiff_t hit = leptris_text_find3(
                    p.pos, (size_t)(p.end - p.pos), '-', '-', '>');
                if (hit < 0) goto fail;
                p.pos += hit;
                *p.pos = '\0'; /* NUL-terminate content */
                LeptrisCommentNode* cn = (LeptrisCommentNode*)
                    dp_cpi_carve(&p, LEPTRIS_NODE_TYPE_COMMENT);
                if (cn) {
                    cn->content = start;  /* zero-copy, buf-lifetime */
                } else {
                    cn = leptris_comment_create(start, p.pos - start, pool);
                    if (!cn) goto fail;
                }
                cn->base.line = markup_off;
                cn->base.frozen = 1;  /* at creation — TODO 187 */
                if (p.depth > 0) {
                    dp_wire_child(&p, p.open_stack[p.depth - 1], (LeptrisNode*)cn);
                } else {
                    /* Doc-level comment: a tree child of the document
                     * node (#580) — the carved node chains directly,
                     * no heap twin. */
                    dp_doc_child(&p, (LeptrisNode*)cn);
                }
                p.pos += 3;
            } else if (p.end - p.pos >= 9 &&
                       memcmp(p.pos + 2, "[CDATA[", 7) == 0) {
                /* CDATA: <![CDATA[ ... ]]> */
                p.pos += 9;
                char* start = p.pos;
                /* TODO 177: SIMD find3 for "]]>". */
                ptrdiff_t hit = leptris_text_find3(
                    p.pos, (size_t)(p.end - p.pos), ']', ']', '>');
                if (hit < 0) goto fail;
                p.pos += hit;
                *p.pos = '\0';
                LeptrisCDATANode* cd = (LeptrisCDATANode*)
                    dp_cpi_carve(&p, LEPTRIS_NODE_TYPE_CDATA);
                if (cd) {
                    cd->content = start;  /* zero-copy, buf-lifetime */
                } else {
                    cd = leptris_cdata_create(start, p.pos - start, pool);
                    if (!cd) goto fail;
                }
                cd->base.line = markup_off;
                cd->base.frozen = 1;  /* at creation — TODO 187 */
                if (p.depth > 0) {
                    dp_wire_child(&p, p.open_stack[p.depth - 1], (LeptrisNode*)cd);
                }
                p.pos += 3;
            } else {
                /* DOCTYPE: extract name + internal subset. Cold path
                 * extracted to dp_parse_doctype (TODO 166 Phase A). */
                if (dp_parse_doctype(&p) != 0) goto fail;
                continue;
            }
        }
        else if (next == '?') {
            /* PI: <?target data?> or XML declaration. */
            uint32_t pi_off = p.line_offsets_ok
                ? (uint32_t)(p.pos - p.buf) + 1u : 0u;
            p.pos += 2;
            char* target_start = p.pos;
            if (!IS_NAME_START(*p.pos)) goto fail;
            p.pos++;
            dp_scan_name(&p);
            /* NUL-terminate the target by consuming ONE separator
             * byte — but only when there is one. A dataless PI
             * (`<?pi?>`, issue #577) ends the target at the CLOSING
             * '?'; clobbering it would leave nothing for the data
             * scan to find and fail the whole parse. */
            char* data_start = p.pos;
            if (*p.pos != '?') {
                *p.pos = '\0';
                p.pos++;
                data_start = p.pos;
            }
            /* memchr for '?' — fast-skip PI data bodies. */
            {
                char* q = (char*)memchr(p.pos, '?', p.end - p.pos);
                if (!q) goto fail;
                p.pos = q;
            }
            if (p.pos + 1 >= p.end || p.pos[1] != '>') goto fail;
            *p.pos = '\0'; /* NUL-terminate data */
            size_t data_len = p.pos - data_start;
            p.pos += 2;

            /* XML declaration handling. */
            if (strcmp(target_start, "xml") == 0) {
                p.had_declaration = 1;
                /* Parse version/encoding/standalone from data. Values
                 * are NUL-terminated in-place by overwriting the
                 * closing quote; the final heap-strdup is required
                 * because leptris_document_free calls LEPTRIS_FREE on
                 * these fields. */
                char* scan = data_start;
                while (*scan) {
                    while (IS_WS(*scan)) scan++;
                    if (!*scan) break;
                    char* pname = scan;
                    while (*scan && !IS_WS(*scan) && *scan != '=') scan++;
                    size_t pname_len = scan - pname;
                    while (IS_WS(*scan)) scan++;
                    if (*scan != '=') break;
                    scan++;
                    while (IS_WS(*scan)) scan++;
                    if (*scan != '"' && *scan != '\'') break;
                    char q = *scan++;
                    char* pval = scan;
                    while (*scan && *scan != q) scan++;
                    if (*scan != q) break;
                    *scan = '\0';
                    scan++;

                    if (pname_len == 7 && memcmp(pname, "version", 7) == 0) {
                        p.version = pval;
                    } else if (pname_len == 8 && memcmp(pname, "encoding", 8) == 0) {
                        p.encoding = pval;
                    } else if (pname_len == 10 && memcmp(pname, "standalone", 10) == 0) {
                        if (strcmp(pval, "yes") == 0) p.standalone = 1;
                        else if (strcmp(pval, "no") == 0) p.standalone = 0;
                    }
                }
                continue;
            }

            LeptrisPINode* pi = (LeptrisPINode*)
                dp_cpi_carve(&p, LEPTRIS_NODE_TYPE_PI);
            if (pi) {
                pi->target = target_start;  /* zero-copy, buf-lifetime */
                pi->data = data_start;
                (void)data_len;
            } else {
                pi = leptris_pi_create(
                    target_start, strlen(target_start),
                    data_start, data_len, pool);
                if (!pi) goto fail;
            }
            pi->base.line = pi_off;
            pi->base.frozen = 1;  /* at creation — TODO 187 */
            if (p.depth > 0) {
                dp_wire_child(&p, p.open_stack[p.depth - 1], (LeptrisNode*)pi);
            } else {
                /* Doc-level PI: a tree child of the document node
                 * (#580) — the carved node chains directly. */
                dp_doc_child(&p, (LeptrisNode*)pi);
            }
        }
        else {
            goto fail;
        }
    }

    if (p.depth != 0) goto fail;
    if (!p.root) goto fail;

    /* 6. Commit to doc. */
    doc->new_dom_root = p.root;
    /* TODO 155 Phase A: register root→doc mapping in the thread-local
     * hash so non-root elements can reach the doc via walk + lookup. */
    leptris_root_doc_register(p.root, doc);
    /* Tree is eagerly built — no FlatDoc, no lazy promote. */
    doc->doc_children_head = p.dc_head;
    doc->doc_children_tail = p.dc_tail;
    if (!p.root_chained && p.root) {
        LeptrisNode* rn = (LeptrisNode*)p.root;
        if (doc->doc_children_tail)
            leptris_node_set_next_sibling(
                (LeptrisNode*)doc->doc_children_tail, rn);
        else
            doc->doc_children_head = rn;
        doc->doc_children_tail = rn;
    }
    /* Trim trailing whitespace-only TEXT off the chain (AFTER the
     * root splice): libxml2 drops document-tail whitespace (bug-83
     * regression guard); inter-markup nodes keep their position
     * (bug-195). */
    {
        LeptrisNode* last_mark = NULL;
        for (LeptrisNode* c = doc->doc_children_head; c;
             c = leptris_node_get_next_sibling(c)) {
            if (c->type != LEPTRIS_NODE_TYPE_TEXT) last_mark = c;
        }
        if (last_mark) {
            leptris_node_set_next_sibling(last_mark, NULL);
            doc->doc_children_tail = last_mark;
        } else {
            doc->doc_children_head = NULL;
            doc->doc_children_tail = NULL;
        }
    }
    doc->has_namespaces = p.saw_namespace;
    doc->dtd = p.dtd;  /* NULL when no DOCTYPE internal subset */

    /* XML 1.0 §3.3.2: ATTLIST default values materialize on elements
     * that don't carry the attribute (a post-parse pass — the hot
     * parse loop stays untouched; DTD-less documents never enter).
     * First-declaration-wins keeps re-declared attributes stable.
     * OPT-IN via LEPTRIS_PARSE_DTDATTR (#606): the libxml2
     * XML_PARSE_DTDATTR default — without it a non-validating parse
     * leaves defaulted attributes out (W3C C14N ex 3.3 parity). */
    if (apply_dtd_attrs)
        leptris_dtd_apply_attribute_defaults(doc);
    doc->had_declaration = p.had_declaration;
    doc->standalone = p.standalone;
    /* encoding and xml_version are borrowed pointers into doc->xml_buffer.
     * leptris_document_free calls LEPTRIS_FREE on them, so we must heap-strdup. */
    if (p.encoding) {
        doc->encoding = strdup(p.encoding);
        if (!doc->encoding) goto fail;
    }
    if (p.version) {
        doc->xml_version = strdup(p.version);
        if (!doc->xml_version) goto fail;
    }

    /* 7. Freeze tree (match legacy parser behavior).
     *
     * TODO 187: the tree walk is GONE — every node is created with
     * frozen=1 above (elements, text, comments, CDATA, PIs). The
     * walk cost ~11% of parse on element-heavy documents (sample
     * profile: leptris_node_freeze + sibling/offset decoders). The
     * public leptris_document_freeze API still walks on demand. */

    {
        static LEPTRIS_THREAD_LOCAL int dbg_ok = -1;
        if (dbg_ok < 0) dbg_ok = getenv("LEPTRIS_DEBUG_PARSE") != NULL;
        if (dbg_ok) {
            size_t dbg_attr_idx =
                (size_t)(p.attr_cursor - p.attr_block);
            fprintf(stderr, "dp ok: arena_used=%zu arena_cap=%zu elem_idx=%zu attr_idx=%zu\n",
                    arena->used, arena->size, elem_idx, dbg_attr_idx);
        }
    }

    return doc;

fail:
    /* TODO.concurrency/01: record the failure position in the
     * thread-local slot so bindings can read leptris_last_error()
     * when leptris_parse_string returns NULL. */
    {
        char msg[80];
        long off = (long)(p.pos ? p.pos - p.buf : 0);
        snprintf(msg, sizeof(msg), "XML parse error at byte %ld: malformed input", off);
        leptris_set_error(LEPTRIS_ERROR_PARSE_FAILED, msg);
        /* 1-based line/column of the failure (issue #510). */
        int line = 1, column = 1;
        for (const char* q = p.buf; q < p.pos; q++) {
            if (*q == '\n') { line++; column = 1; }
            else column++;
        }
        leptris_set_error_position(line, column);
    }
    {
        /* Cached env lookup — getenv is a linear environ scan, too
         * costly to repeat on every parse. Benign init race. */
        static LEPTRIS_THREAD_LOCAL int dbg = -1;
        if (dbg < 0) dbg = getenv("LEPTRIS_DEBUG_PARSE") != NULL;
        if (dbg) {
            fprintf(stderr, "dp fail: pos=%ld off=%ld depth=%d arena_used=%zu arena_cap=%zu elem_idx=%zu attr_idx=%zu lt=%zu q=%zu est=%zu attrcap=%zu\n",
                    (long)(p.pos - p.buf), (long)(p.end - p.pos), p.depth,
                    arena ? arena->used : 0, arena ? arena->size : 0,
                    elem_idx, (size_t)(p.attr_cursor - p.attr_block),
                    lt_count, quote_count, est_elems,
                    attr_capacity);
        }
    }
    leptris_pool_destroy(pool);
    /* elem_block AND doc are pool-allocated — both freed by
     * pool_destroy above. Don't LEPTRIS_FREE(doc) (TODO 154). */
    if (owns_buffer == 1)
            leptris_arena_buffer_release(buf, len + 1 + 64);  /* Only free our own copy, not caller's */
    return NULL;
}

/* Public: copy the input then parse (standard path).
 *
 * The copy and the arena-sizing pre-scan are ONE fused SIMD pass
 * into a separately-malloc'd buffer (TODO 188) — the old design
 * streamed the input twice (count3 pre-scan + pool memcpy). The
 * document owns the malloc via doc->xml_buffer_needs_free; the
 * arena holds only nodes and strings. */
struct leptris_document* direct_parse(const char* xml, size_t len) {
    if (!xml || len == 0) return NULL;
    return direct_parse_internal((char*)xml, len, 2, 0, 0);
}

/* Public flagged entry (LEPTRIS_PARSE_DROP_WS_TEXT et al.). */
struct leptris_document* direct_parse_flags(const char* xml, size_t len,
                                           unsigned parse_flags) {
    if (!xml || len == 0) return NULL;
    return direct_parse_internal((char*)xml, len, 2,
                                 (parse_flags & LEPTRIS_PARSE_DROP_WS_TEXT)
                                     ? 1 : 0,
                                 (parse_flags & LEPTRIS_PARSE_DTDATTR)
                                     ? 1 : 0);
}

/* Public: parse a caller-owned writable buffer without copying. */
struct leptris_document* direct_parse_inplace(char* buf, size_t len) {
    if (!buf || len == 0) return NULL;
    buf[len] = '\0';  /* Ensure NUL termination */
    return direct_parse_internal(buf, len, 0, 0, 0);
}
