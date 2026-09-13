/* lib/src/dom/element.h - Element node type
 * Copyright (c) 2024, Ribose Inc.
 *
 * Element nodes contain:
 * - Name and namespace information
 * - Attributes
 * - Child nodes (elements, text, comments, CDATA, PIs)
 *
 * COMPACT ARCHITECTURE:
 * Uses compressed pointer encoding for minimal memory footprint.
 * - ~96 bytes per element (vs 192 bytes in legacy design = 2x reduction!)
 * - 1-2 byte compressed pointers instead of 8-byte pointers
 * - Better cache locality and faster tree traversal
 */

#ifndef LEPTRIS_DOM_ELEMENT_H
#define LEPTRIS_DOM_ELEMENT_H

/* These typedefs match the public leptris.h / leptris/types.h.  Guarded
 * so internal headers can be included in any order without triggering
 * C99's typedef-redefinition warning.  See TODO 12. */
#ifndef LEPTRIS_INTERNAL_TYPES_DEFINED
#define LEPTRIS_INTERNAL_TYPES_DEFINED
typedef struct leptris_node*            LeptrisNodeRef;
typedef struct leptris_document*        LeptrisDocument;
typedef struct leptris_element*         LeptrisElement;
typedef struct leptris_attribute*       LeptrisAttribute;
typedef struct leptris_doctype*         LeptrisDoctype;
typedef const char*                    LeptrisNamespace;
typedef struct leptris_xpath_result*    LeptrisXPathResult;
typedef struct leptris_relaxng* LeptrisRelaxNG;
#endif

#include "node.h"
#include "../common/string_view.h"
#include "compact.h"  /* Compressed pointer types */

/* Forward decl: LeptrisMemoryPool is defined in memory/pool.h. */
struct leptris_memory_pool;

/* Attribute structure - inline storage for minimal memory footprint
 *
 * Instead of storing pointers to attribute structures, we store attributes
 * as a linked list with inline StringView storage. This eliminates pointer
 * indirection and improves cache locality.
 *
 * Size: 72 bytes (was 112 before TODO 173).
 *
 * TODO 173: prefix / namespace_uri (views + pointers) moved into a side
 * cache struct (leptris_attr_ns_cache) allocated only when actually needed.
 * The common case (no namespace activity) has ns_cache == NULL — saves
 * 48 bytes per attribute. For 100,000 attrs that's 4.8 MB less memory
 * pressure and better cache locality. */
struct leptris_attr_ns_cache {
    LeptrisStringView prefix_view;
    LeptrisStringView namespace_uri_view;
    char* prefix;                /* NULL until first access */
    char* namespace_uri;         /* NULL until first access */
    /* Issue #542: owning element for standalone expanded-name
     * accessors — stamped at parse / set time. The URI itself stays
     * UNRESOLVED (resolved per read through the owner's in-scope
     * declarations, so namespace mutation stays correct). */
    struct leptris_element* owner_elem;
};

struct leptris_attribute {
    /* SINGLE string representation (TODO 184 round 4): the views.
     * - Parse path: zero-copy into the document buffer (NUL-
     *   terminated in place by the parser; document-lifetime).
     * - Mutation API / entity expansion: view.data is an OWNED
     *   pool/heap copy, NUL-terminated, length set. Owned copies
     *   REPLACE the view because callers' strings may be temporary
     *   and the raw entity text is never needed post-expansion.
     * 40 bytes: 16 + 16 + 8-byte packed tail (round 19). */

    LeptrisStringView name_view;
    /* #1038 lane18 S1: values <= 15 bytes store INLINE in this
     * 16-byte slot (zero allocation on overwrite); longer values
     * keep the heap view. The union field name (value, not
     * value_view) is deliberate: the compiler finds every reader.
     * Flag = top bit of heap.length; inline bytes are NUL-
     * terminated at [len]. */
    union {
        LeptrisStringView heap;
        char inline_value[16];
    } value;

    /* Side cache for namespace activity. 0 when the attr has no
     * prefix and no namespace_uri (the common case). Points via
     * self-relative offset at a pool-allocated struct; use
     * attr_get_ns_cache()/attr_set_ns_cache(). TODO 173.
     * Round 19: was a raw pointer — now an int32 offset so the
     * struct fits 40 bytes (compact.c overflow-table fallback
     * covers >2GB spans). */
    

int32_t ns_cache_off;

    /* Next attribute in linked list. TODO 183 Phase 5 (TODO 181
     * Phase D): cp16 compact pointer — the attr `next` edge only
     * connects attrs of the SAME element, which are allocated as
     * adjacent slots in the parser's attr_block (contiguous by
     * construction since TODO 183 Phase 3; distance ≤ K × sizeof,
     * ~3 KB at K=100 — always within cp16's ±256 KB). Mutation-
     * created attrs (different region) go through the encoder's
     * overflow-table path. 0 = NULL end-of-list. */
    int16_t next_cp;

    /* Bits 0-14: 15-bit FNV-1a of the name, LAZY (0 = not yet
     * computed; the first read via attr_name_hash() computes and
     * caches; a real hash is never stored as 0 — the compute maps
     * 0 to 1). Bit 15: has_entities (1 if value_view contains '&',
     * 0 otherwise; set during parsing, cleared after eager
     * expansion). The parse path skips the hash entirely (saves
     * ~5ns per attr on attr-heavy inputs); query paths pay it on
     * first access, then cached. The hash is a pre-filter only —
     * every hash hit is confirmed by memcmp — so 15 bits suffice.
     * TODO 172.
     *
     * Layout note: ns_cache_off + name_hash + next_cp pack into one
     * 8-byte tail — sizeof is 40 (was 48, 64 with separate char*
     * fields, 72 with a raw next pointer). */
    uint16_t name_hash;

    /* MEASURED LAYOUT DECISION (TODO 186, revises TODO 184 round 4;
     * round 19 revises again): sizeof was 48 through v0.26.x. 40 B
     * (ns_cache as int32 offset + 15-bit hash + entity flag in the
     * tail) reaches pugixml attr density (1.6/line vs 1.33). The
     * 56 B point measured dead and the 32 B split-stream upper
     * bound dead — 40 was the last unmeasured point on the axis. */
};
#define LEPTRIS_ATTR_VALUE_INLINE_BIT ((size_t)1 << 63)
#define LEPTRIS_ATTR_VALUE_MAX_INLINE 15

static inline LeptrisStringView leptris_attr_value_sv(
    const struct leptris_attribute* a) {
    if (a->value.heap.length & LEPTRIS_ATTR_VALUE_INLINE_BIT) {
        LeptrisStringView sv;
        sv.data = a->value.inline_value;
        sv.length = a->value.heap.length & ~LEPTRIS_ATTR_VALUE_INLINE_BIT;
        return sv;
    }
    return a->value.heap;
}

static inline void leptris_attr_value_set_heap(
    struct leptris_attribute* a, LeptrisStringView sv) {
    sv.length &= ~LEPTRIS_ATTR_VALUE_INLINE_BIT;
    a->value.heap = sv;
}

static inline void leptris_attr_value_set_inline(
    struct leptris_attribute* a, const char* s, size_t len) {
    if (len > LEPTRIS_ATTR_VALUE_MAX_INLINE) len = LEPTRIS_ATTR_VALUE_MAX_INLINE;
    memcpy(a->value.inline_value, s, len);
    a->value.inline_value[len] = '\0';
    a->value.heap.length = LEPTRIS_ATTR_VALUE_INLINE_BIT | len;
}

static inline int leptris_attr_value_is_inline(
    const struct leptris_attribute* a) {
    return (a->value.heap.length & LEPTRIS_ATTR_VALUE_INLINE_BIT) != 0;
}


/* Size pin lives with the LEPTRIS_STATIC_ASSERT macro below (C++-
 * compatible), next to the element size pin. */

/* C-string accessors (TODO 184 rounds 3–4). The views are the
 * single representation; their data is always NUL-terminated
 * (parse path: in the document buffer; mutation/expansion: owned
 * copy). attr_cvalue does NOT expand entities — callers needing
 * expansion use leptris_element_attribute(), which materializes
 * lazily. */
static inline const char* attr_cname(const struct leptris_attribute* a) {
    return a->name_view.data ? a->name_view.data : "";
}

static inline const char* attr_cvalue(const struct leptris_attribute* a) {
    return a ? (leptris_attr_value_sv(a).data ? leptris_attr_value_sv(a).data
                                              : "")
             : "";
}

/* Attr list-edge accessors (TODO 183 Phase 5). next_cp stores the
 * byte offset to the next attr scaled by 8 (align_log2=3); attrs are
 * 8-byte aligned pool/arena residents. The encoder falls back to the
 * per-field overflow table when the pair spans regions (mutation-
 * created attrs). */
static inline struct leptris_attribute* leptris_attr_next(
    const struct leptris_attribute* a) {
    return (a)
        ? (struct leptris_attribute*)leptris_compact_ptr16_decode(
              (const void*)a, a->next_cp, 3, &a->next_cp)
        : NULL;
}

static inline void leptris_attr_set_next(struct leptris_attribute* a,
                                         struct leptris_attribute* next) {
    if (!a) return;
    a->next_cp = leptris_compact_ptr16_encode(a, next, 3, &a->next_cp);
}

/* 15-bit FNV-1a of a name (round 19). Single source of truth for
 * both the lazy attr accessor and query-side pre-filters — both
 * sides must truncate identically or hash compares silently
 * mismatch. Truncation: keep bits 16-30 of the 32-bit FNV (the
 * low bits sit under the multiply's carry chain; the upper half
 * mixes better). Returns nonzero: 0 maps to 1 so the "uncomputed"
 * sentinel stays unambiguous. */
static inline uint16_t attr_hash15(const char* s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    /* Finalizer (round 20): raw FNV truncation (h>>16) mapped
     * names differing in their last digit to hashes exactly 256
     * apart — arithmetic progressions that collide onto one open-
     * addressing chain at every cap ≤ 256. A murmur-style avalanche
     * step breaks the progression before the 15-bit cut. */
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    uint16_t h15 = (uint16_t)(h & 0x7FFFu);
    return h15 ? h15 : 1;
}

/* Entity flag (round 19): name_hash bit 15. */
static inline int attr_has_entities(const struct leptris_attribute* a) {
    return (a->name_hash & 0x8000u) != 0;
}
static inline void attr_set_entities(struct leptris_attribute* a, int v) {
    if (v) a->name_hash |= 0x8000u;
    else   a->name_hash &= 0x7FFFu;
}

/* Lazy 15-bit hash accessor (TODO 172). Computes and caches on first
 * call, preserving the entity flag in bit 15. Thread-unsafe in the
 * strict sense (racy writes), but the worst case is two threads
 * both writing the same value (idempotent). Documents are
 * single-threaded by contract. */
static inline uint16_t attr_name_hash(struct leptris_attribute* a) {
    if ((a->name_hash & 0x7FFFu) == 0) {
        a->name_hash = (uint16_t)((a->name_hash & 0x8000u) |
            attr_hash15(a->name_view.data, a->name_view.length));
    }
    return (uint16_t)(a->name_hash & 0x7FFFu);
}

/* Attribute namespace-cache accessors (TODO 173). The prefix and
 * namespace_uri (both view and cstr form) live in a side cache struct
 * that's only allocated when one of them is set. The common case (attr
 * without prefix, no namespace_uri) has ns_cache_off == 0.
 *
 * Readers use these helpers — they return NULL / empty when no cache.
 * Round 19: the cache is reached via a self-relative int32 offset
 * (0 = none; overflow-table fallback for >2GB spans). */
static inline struct leptris_attr_ns_cache* attr_get_ns_cache(
    struct leptris_attribute* a) {
    return (struct leptris_attr_ns_cache*)leptris_compact_int32_decode(
        a, a->ns_cache_off, &a->ns_cache_off);
}
static inline void attr_set_ns_cache(struct leptris_attribute* a,
                                     struct leptris_attr_ns_cache* ns) {
    a->ns_cache_off = leptris_compact_int32_encode(a, ns, &a->ns_cache_off);
}
static inline const char* attr_get_prefix(struct leptris_attribute* a) {
    struct leptris_attr_ns_cache* c = attr_get_ns_cache(a);
    return c ? c->prefix : NULL;
}
static inline const char* attr_get_namespace_uri(struct leptris_attribute* a) {
    struct leptris_attr_ns_cache* c = attr_get_ns_cache(a);
    return c ? c->namespace_uri : NULL;
}
static inline LeptrisStringView attr_get_prefix_view(struct leptris_attribute* a) {
    struct leptris_attr_ns_cache* c = attr_get_ns_cache(a);
    return c ? c->prefix_view : leptris_sv_empty();
}
static inline LeptrisStringView attr_get_namespace_uri_view(struct leptris_attribute* a) {
    struct leptris_attr_ns_cache* c = attr_get_ns_cache(a);
    return c ? c->namespace_uri_view : leptris_sv_empty();
}

/* Element node - compact architecture
 *
 * Uses compressed pointers and inline strings for minimal memory footprint.
 * This enables better cache locality and faster tree traversal.
 *
 * Size: ~96 bytes (vs 192 bytes in legacy design = 2x reduction!)
 *
 * Key features:
 * - 1-byte compressed pointers for child/sibling/attribute (±504 bytes)
 * - 2-byte compressed pointer for parent (±262KB)
 * - StringView storage for zero-copy parsing
 * - uint8_t counts instead of size_t (1 byte vs 8 bytes)
 * - 4-byte base node (vs 32 bytes in legacy) - no redundant pointers
 * - Falls back to hash table for large offsets
 */
/* Phase 2e-B (TODO 150): merged prefix + namespace_uri into a single
 * nullable ns_cache pointer. Most elements have no prefix and no
 * namespace_uri → ns_cache is NULL, zero overhead. Only elements that
 * have a prefix (qualified name like foo:bar) or resolved namespace_uri
 * pay the 16-byte pool allocation for the cache struct.
 *
 * Saves 8 bytes per element (88 → 80). */
/* Phase 2e-B + TODO 155 Phase B: ns_cache holds BOTH the element's
 * own prefix/URI AND the linked list of xmlns:* declarations parsed
 * on this element. Merging the parallel `namespaces` head pointer
 * into ns_cache saves 8 bytes per element (88 → 80).
 *
 * Most elements have no namespace activity → ns_cache is NULL,
 * zero overhead. Elements that declare namespaces OR have a prefix
 * pay one 16-byte pool allocation for the cache struct. */
/* One raw attribute view entry (issue #635): qname + value as
 * written, xmlns declarations included, source order preserved. */
struct leptris_raw_attr {
    const char* qname;
    const char* value;
    struct leptris_raw_attr* next;
};

struct leptris_ns_cache {
    char* prefix;                       /* This element's prefix (from `<p:loc>`) */
    char* namespace_uri;                /* Resolved URI for this element's prefix */
    struct leptris_namespace* declarations;  /* xmlns:* declared on this element */
    /* Raw attribute view (issue #635): every attr AND xmlns decl in
     * SOURCE order — the mixed qname-ordered list the streaming
     * transports deliver. Lazily built at parse; pool-allocated
     * entries with borrowed (parse zero-copy) strings. */
    struct leptris_raw_attr* raw_attrs;
    /* Document-owned chain (issue: LSan leak): HEAP strings on a
     * POOL-allocated cache need an explicit document_free walk. The
     * flags mark exactly which pointers are heap-owned — the cache
     * also carries pool-allocated and borrowed (parse zero-copy)
     * strings the pool/buffer own. */
    struct leptris_ns_cache* doc_next;
    unsigned char prefix_heap;
    unsigned char uri_heap;
};

struct leptris_element {
    /* Compact base node (12 bytes) - MUST be first for casting. */
    LeptrisNode base;

    /* Packed header + counts + name_hash (7 bytes, fills the 8-byte
     * tail of base's alignment slot). The name_hash is a 16-bit FNV-1a
     * of the element's local name, computed at parse time. Used by
     * XPath child-axis lookups and close-tag comparison to reject
     * non-matching names via 2-byte hash compare before strcmp.
     * Phase 2a of TODO 90 + TODO 159 fast-child-lookup. */
    LeptrisCompactHeader header;        /* 2 bytes */
    uint16_t name_hash;               /* 2 bytes — FNV-1a of local name */
    uint8_t attr_count;                /* 1 byte */
    /* Local-name byte length, 0xFF = name longer than 254 bytes
     * (callers fall back to strlen). Fills the last padding byte
     * of this packed header — sizeof stays 64. Kills the per-element
     * strlen in the serializer (8% of text-heavy serialize) and the
     * close-tag comparison in the parser. */
    uint8_t name_len;                  /* 1 byte */
    uint16_t child_count;              /* 2 bytes */

    /* Cached NULL-terminated strings (16 bytes).
     * Phase 2e-B: prefix + namespace_uri merged into ns_cache (8 bytes
     * instead of 16). name stays inline — it's accessed on every
     * serialize/XPath hit. */
    char* name;                        /* NULL until first access */
    struct leptris_ns_cache* ns_cache;  /* NULL for non-namespaced elements */

    /* Tree edges (12 bytes; was 16 before TODO 155 Phase C).
     * last_child_off is GONE — append operations walk the child list
     * (O(child_count)) or use the parser-local cache during parse.
     * Saves 4 bytes per element. */
    int32_t parent_off;
    int32_t first_child_off;
    int32_t next_sibling_off;

    /* Attribute list (4 bytes; was 8 before TODO 155 Phase C).
     * last_attribute_off is GONE — same reasoning. Append walks the
     * list. Saves 4 bytes per element. */
    int32_t first_attribute_off;

    /* TODO 155 Phase A: `document` field is GONE — element now fits
     * one 64-byte cache line. Non-root elements reach their document
     * via leptris_element_get_document() in dom/root_doc_map.h. */
};

struct leptris_document* leptris_element_get_document(LeptrisElement elem);
LeptrisMemoryPool* leptris_element_get_pool(LeptrisElement elem);

/* Round 21: header.flags bit 6 — "elem->name is preceded by 8 bytes
 * holding the owning document pointer" (mutation name-block carve
 * layout). Lets get_document resolve UNATTACHED mutation elements
 * with zero registration: the map is only consulted for roots and
 * map-registered (parse/copy) elements; a stateless backpointer
 * replaces the register-on-create / unregister-on-attach pair that
 * cost ~11ns per append. */
#define LEPTRIS_NAMEBP_FLAG 0x40u

/* Lane 18: header.flags bit 5 — "this element's attributes are
 * doc-index-served". elem->attr_count is uint8_t and wraps past
 * 255; the walk-vs-index threshold must not oscillate with the
 * wrapped count or high-attr elements periodically walk the whole
 * chain (O(N) per set). Once set, never cleared: removals leave
 * tombstones in the index and lookups stay correct. */
#define LEPTRIS_ATTR_INDEXED_FLAG 0x20u

static inline int leptris_elem_has_namebp(const LeptrisElement e) {
    return (e->header.flags & LEPTRIS_NAMEBP_FLAG) != 0;
}

static inline struct leptris_document* leptris_elem_namebp_doc(
    const LeptrisElement e) {
    /* name points at slot+8; slot start holds the doc pointer. */
    return ((struct leptris_document* const*)e->name)[-1];
}

/* Compute 16-bit FNV-1a hash of an element name string. Used
 * together with elem->name_hash for fast pre-filtering in child-
 * axis lookups. TODO 159: fast child-name matching. */
static inline uint16_t leptris_name_hash_compute(const char* name) {
    uint16_t h = 0x811C;
    for (const char* c = name; *c; c++) {
        h ^= (unsigned char)*c;
        h *= 0x0193;
    }
    return h;
}

/* Fast element-name equality check: compare 2-byte hash first,
 * fall back to strcmp only on hash match. Rejects non-matching
 * names in ~1ns vs ~5ns for strcmp on short names. */
static inline int leptris_elem_name_is(LeptrisElement e, const char* name,
                                       uint16_t target_hash) {
    if (!e || !e->name || e->name_hash != target_hash) return 0;
    return strcmp(e->name, name) == 0;
}

/* Inline accessors — use these instead of direct field access. */
static inline char* leptris_elem_prefix(const LeptrisElement e) {
    return e && e->ns_cache ? e->ns_cache->prefix : NULL;
}
static inline char* leptris_elem_ns_uri(const LeptrisElement e) {
    return e && e->ns_cache ? e->ns_cache->namespace_uri : NULL;
}

/* Allocate ns_cache if needed, then set the prefix. Pool required
 * for the one-time allocation. The cache struct is zeroed so all
 * fields (prefix, namespace_uri, declarations) start as NULL. */
static inline void leptris_elem_set_prefix(LeptrisElement e, char* prefix,
                                           LeptrisMemoryPool* pool) {
    if (!e) return;
    if (!e->ns_cache) {
        if (!pool) return;
        e->ns_cache = (struct leptris_ns_cache*)
            leptris_pool_alloc(pool, sizeof(struct leptris_ns_cache));
        if (!e->ns_cache) return;
        e->ns_cache->prefix = NULL;
        e->ns_cache->namespace_uri = NULL;
        e->ns_cache->declarations = NULL;
    }
    e->ns_cache->prefix = prefix;
}

/* Allocate ns_cache if needed, then set the namespace URI. */
static inline void leptris_elem_set_ns_uri(LeptrisElement e, char* uri,
                                            LeptrisMemoryPool* pool) {
    if (!e) return;
    if (!e->ns_cache) {
        if (!pool) return;
        e->ns_cache = (struct leptris_ns_cache*)
            leptris_pool_alloc(pool, sizeof(struct leptris_ns_cache));
        if (!e->ns_cache) return;
        e->ns_cache->prefix = NULL;
        e->ns_cache->namespace_uri = NULL;
        e->ns_cache->declarations = NULL;
    }
    e->ns_cache->namespace_uri = uri;
}

/* #846: split a just-stored QName copy ("p:local") into prefix +
 * local, giving constructed elements the same shape the parser
 * produces (dp_split_hash_name): name points at the local part,
 * ns_cache->prefix at the pre-colon span, hash/len recomputed for
 * the local part. The name storage must be owned and writable
 * (pool or mut-block copy) — the split NULs the colon in place.
 * No-op for colon-free names. */
static inline void leptris_elem_split_qname(LeptrisElement e,
                                            LeptrisMemoryPool* pool) {
    if (!e || !e->name) return;
    char* colon = strchr(e->name, ':');
    if (!colon) return;
    /* Lane 18 P1: read the doc backpointer BEFORE the split moves
     * the name — then re-stamp it in a fresh slot so namebp
     * resolution survives (public creates no longer register in
     * the map; the re-stamp keeps prefixed elements resolvable
     * while detached). */
    struct leptris_document* bp_doc =
        leptris_elem_has_namebp(e) ? leptris_elem_namebp_doc(e) : NULL;
    *colon = '\0';
    leptris_elem_set_prefix(e, e->name, pool);
    e->name = colon + 1;
    if (bp_doc && pool) {
        size_t keep_len = strlen(e->name);
        char* slot = (char*)leptris_pool_alloc(
            pool, sizeof(struct leptris_document*) + keep_len + 1);
        if (slot) {
            *(struct leptris_document**)slot = bp_doc;
            memcpy(slot + sizeof(struct leptris_document*),
                   e->name, keep_len);
            slot[sizeof(struct leptris_document*) + keep_len] = '\0';
            e->name = slot + sizeof(struct leptris_document*);
        } else {
            e->header.flags &=
                (uint8_t)(~LEPTRIS_NAMEBP_FLAG & 0xFFu);
        }
    } else {
        /* The name moved into the name bytes: the Round-21
         * backpointer slot at name[-1] is no longer the doc — the
         * flag must not outlive the slot. */
        e->header.flags &= (uint8_t)(~LEPTRIS_NAMEBP_FLAG & 0xFFu);
    }
    size_t local_len = strlen(e->name);
    e->name_len = (local_len > 254) ? 0xFF : (uint8_t)local_len;
    e->name_hash = leptris_name_hash_compute(e->name);
}

/* Get the linked list of xmlns:* declarations on this element.
 * Returns NULL when the element has no namespace declarations
 * (the overwhelmingly common case). TODO 155 Phase B. */
static inline struct leptris_namespace* leptris_elem_namespaces(const LeptrisElement e) {
    return e && e->ns_cache ? e->ns_cache->declarations : NULL;
}

/* Ensure ns_cache exists (allocating if needed) and return a
 * writable pointer to the declarations head. Used by mutation
 * paths that append xmlns:* declarations. */
static inline struct leptris_ns_cache** leptris_elem_cache_ptr(
    LeptrisElement e, LeptrisMemoryPool* pool) {
    if (!e) return NULL;
    if (!e->ns_cache) {
        if (!pool) return NULL;
        e->ns_cache = (struct leptris_ns_cache*)
            leptris_pool_alloc(pool, sizeof(struct leptris_ns_cache));
        if (!e->ns_cache) return NULL;
        e->ns_cache->prefix = NULL;
        e->ns_cache->namespace_uri = NULL;
        e->ns_cache->declarations = NULL;
        e->ns_cache->raw_attrs = NULL;
    }
    return &e->ns_cache;
}

static inline struct leptris_namespace** leptris_elem_namespaces_ptr(
    LeptrisElement e, LeptrisMemoryPool* pool) {
    struct leptris_ns_cache** c = leptris_elem_cache_ptr(e, pool);
    return c ? &(*c)->declarations : NULL;
}


/* Compile-time element-size tracker (TODO 90).
 *
 * Current layout (Phase 1 + 2a + 2b + 2d complete, 88 bytes):
 *   - 12-byte base (LeptrisNode + uint32_t line for #223)
 *     + 2-byte header + 1-byte attr_count + 2-byte child_count
 *     packed into the 4-byte tail of base's alignment slot
 *     (5 bytes used, 3 pad)
 *   - 24 bytes of cached name/prefix/namespace_uri char* pointers
 *   - 16 bytes of int32_t tree-edge offsets (parent, first/last/next)
 *   - 8 bytes of int32_t attribute-list offsets (first, last)
 *   - 16 bytes of document context (namespaces, document)
 *   - 8 bytes binding_wrapper (LeptrisNode base, #262 FFI cache)
 *
 * pugixml compact node: 44 bytes. The binding_wrapper field adds
 * ~10% memory but eliminates per-node FFI call overhead for
 * language bindings (7× nodeset XPath speedup at Ruby level). */
#ifndef LEPTRIS_STATIC_ASSERT
#  ifdef __cplusplus
#    define LEPTRIS_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#  elif defined(_MSC_VER)
#    define LEPTRIS_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#  else
#    define LEPTRIS_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#  endif
#endif

LEPTRIS_STATIC_ASSERT(sizeof(struct leptris_element) == 64,
    "leptris_element must fit one cache line (TODO 155 Phase A)");

LEPTRIS_STATIC_ASSERT(sizeof(struct leptris_attribute) == 40,
    "round 19 attr layout: 16+16+4+2+2 = 40");

/* ============================================================================
 * Compact tree-edge accessors (Phase 2b of TODO 90)
 *
 * Tree edges (parent, first_child, last_child, next_sibling) are stored
 * as int32_t byte-offsets relative to the element's own address, not as
 * raw pointers. This cuts the element struct from 104 → 88 bytes (better
 * cache locality, fewer pool pages on large documents).
 *
 * Offset 0 encodes NULL (no element has zero offset to itself because
 * pool-allocated elements are 8-byte aligned). Read via these inline
 * accessors; never read the `_off` fields directly.
 *
 * Type aliases preserve the original types: parent is always an element;
 * the child/sibling edges may point to any node type (element, text,
 * comment, cdata, pi) so they return LeptrisNode*.
 * ============================================================================ */

static inline LeptrisElement leptris_elem_parent(const LeptrisElement e) {
    return (e)
        ? (LeptrisElement)leptris_compact_int32_decode((void*)e, e->parent_off, &e->parent_off)
        : NULL;
}

static inline LeptrisNode* leptris_elem_first_child(const LeptrisElement e) {
    return (e)
        ? (LeptrisNode*)leptris_compact_int32_decode((void*)e, e->first_child_off, &e->first_child_off)
        : NULL;
}

static inline LeptrisNode* leptris_elem_last_child(const LeptrisElement e) {
    /* TODO 155 Phase C: last_child_off removed. Walk the child list
     * to find the tail. O(child_count); the typical element has few
     * children, and cold paths (mutation, traversal) tolerable.
     * Parse path uses a parser-local cache (DParser.last_child_of_depth)
     * to avoid this walk during the hot parse loop. */
    if (!e) return NULL;
    LeptrisNode* c = leptris_elem_first_child(e);
    if (!c) return NULL;
    while (1) {
        LeptrisNode* next = leptris_node_get_next_sibling(c);
        if (!next) return c;
        c = next;
    }
}

static inline LeptrisNode* leptris_elem_next_sibling(const LeptrisElement e) {
    return (e)
        ? (LeptrisNode*)leptris_compact_int32_decode((void*)e, e->next_sibling_off, &e->next_sibling_off)
        : NULL;
}

/* Setters. NULL target resets the offset to 0.  TODO 121: on int32
 * overflow (macOS ASLR can place pool-resident nodes > 2GB apart),
 * fall back to the global overflow table keyed on the field address
 * instead of silently dropping the edge. */
static inline void leptris_elem_set_parent(LeptrisElement e, LeptrisElement parent) {
    if (!e) return;
    e->parent_off = leptris_compact_int32_encode(e, parent, &e->parent_off);
}

static inline void leptris_elem_set_first_child(LeptrisElement e, LeptrisNode* child) {
    if (!e) return;
    e->first_child_off = leptris_compact_int32_encode(e, child, &e->first_child_off);
}

static inline void leptris_elem_set_last_child(LeptrisElement e, LeptrisNode* child) {
    /* TODO 155 Phase C: last_child_off removed. This setter is now a
     * no-op retained for ABI compatibility. Callers that need to
     * append a child use leptris_element_append_child_internal, which
     * walks the child list (or uses the parser-local cache). */
    (void)e; (void)child;
}

static inline void leptris_elem_set_next_sibling(LeptrisElement e, LeptrisNode* sibling) {
    if (!e) return;
    e->next_sibling_off = leptris_compact_int32_encode(e, sibling, &e->next_sibling_off);
}

/* Compact attribute-list accessors (Phase 2d of TODO 90).
 * first_attribute and last_attribute are int32_t byte-offsets to
 * leptris_attribute records in the document's pool; 0 = empty list.
 * Pool-allocated, same safety argument as the tree edges. */
static inline struct leptris_attribute* leptris_elem_first_attribute(const LeptrisElement e) {
    return (e)
        ? (struct leptris_attribute*)leptris_compact_int32_decode((void*)e, e->first_attribute_off, &e->first_attribute_off)
        : NULL;
}

static inline struct leptris_attribute* leptris_elem_last_attribute(const LeptrisElement e) {
    /* TODO 155 Phase C: last_attribute_off removed. Walk the attr
     * list to find the tail. O(attr_count) — typically ≤ 10. */
    if (!e) return NULL;
    struct leptris_attribute* a = leptris_elem_first_attribute(e);
    if (!a) return NULL;
    while (leptris_attr_next(a)) a = leptris_attr_next(a);
    return a;
}

static inline void leptris_elem_set_first_attribute(LeptrisElement e, struct leptris_attribute* attr) {
    if (!e) return;
    e->first_attribute_off = leptris_compact_int32_encode(e, attr, &e->first_attribute_off);
}

static inline void leptris_elem_set_last_attribute(LeptrisElement e, struct leptris_attribute* attr) {
    /* TODO 155 Phase C: last_attribute_off removed. No-op retained
     * for ABI compatibility. Callers should use the append helpers
     * (leptris_elem_last_attribute walks the list to find the tail). */
    (void)e; (void)attr;
}

/* LeptrisElement typedef comes from the public include/leptris/types.h
 * (re-exported via leptris.h).  No local redefinition — see TODO 12. */

/* ============================================================================
 * Element Creation
 * ============================================================================ */

/* Create element with StringView (true zero-copy!) */
LeptrisElement leptris_element_create_with_view(
    LeptrisStringView name_view,
    LeptrisMemoryPool* pool
);

/* Create element with in-place string (zero-copy) */
LeptrisElement leptris_element_create_pooled_inplace(char* name, LeptrisMemoryPool* pool);

/* Create element skeleton for the deferred-NUL zero-copy parser path
 * (TODO 113 Phase 5). Element name is left NULL — parser fills it in
 * after consuming the opening tag. See leptris_element_create_zero_copy. */
LeptrisElement leptris_element_create_zero_copy(LeptrisMemoryPool* pool);

/* Create element using memory pool (O(1) bump allocation).
 * Single pool-routed entry point — TODO 26 removed the _fast wrapper. */
LeptrisElement leptris_element_create_pooled(const char* name, LeptrisMemoryPool* pool);

void leptris_element_free(LeptrisElement elem);

/* ============================================================================
 * Hot Accessor Functions (static inline for performance)
 * ============================================================================ */

/* These are static inline to eliminate function call overhead.
 * The compiler can inline these across translation units for maximum performance.
 * All are simple field accesses with NULL checks - O(1) operations. */

static inline LeptrisElement leptris_element_get_parent(LeptrisElement elem) {
    return leptris_elem_parent(elem);
}

static inline LeptrisElement leptris_element_get_first_child(LeptrisElement elem) {
    if (!elem) return NULL;

    /* Fast path: if first child is an element, return immediately
     * This covers the common case where elements are directly nested */
    LeptrisNode* node = leptris_elem_first_child(elem);
    if (node && node->type == LEPTRIS_NODE_TYPE_ELEMENT) {
        return (LeptrisElement)node;
    }

    /* Slow path: skip non-element nodes (text, comments, etc.) */
    while (node) {
        if (node->type == LEPTRIS_NODE_TYPE_ELEMENT) {
            return (LeptrisElement)node;
        }
        node = leptris_node_get_next_sibling(node);
    }
    return NULL;
}

static inline LeptrisElement leptris_element_get_last_child(LeptrisElement elem) {
    if (!elem) return NULL;

    /* last_child might point to a non-element node (text, comment, etc.)
     * We need to scan from first_child to find the last element child */
    LeptrisNode* node = leptris_elem_first_child(elem);
    LeptrisElement last_elem = NULL;

    while (node) {
        if (node->type == LEPTRIS_NODE_TYPE_ELEMENT) {
            last_elem = (LeptrisElement)node;
        }
        node = leptris_node_get_next_sibling(node);
    }
    return last_elem;
}

static inline LeptrisElement leptris_element_get_next_sibling(LeptrisElement elem) {
    if (!elem) return NULL;
    /* Get next sibling and skip non-element nodes
     * This implements the XPath semantics where sibling axis only returns elements */
    LeptrisNode* next = leptris_elem_next_sibling(elem);

    /* Fast path: if next sibling is an element, return immediately
     * This covers the common case where elements are directly nested */
    if (next && next->type == LEPTRIS_NODE_TYPE_ELEMENT) {
        return (LeptrisElement)next;
    }

    /* Slow path: skip non-element nodes (text, comments, etc.) */
    while (next && next->type != LEPTRIS_NODE_TYPE_ELEMENT) {
        next = leptris_node_get_next_sibling(next);
    }

    return (LeptrisElement)next;
}

/* ============================================================================
 * Compressed Pointer Access Functions (deprecated/removed - use inline above)
 * ============================================================================ */
LeptrisElement leptris_element_get_parent(LeptrisElement elem);
LeptrisElement leptris_element_get_first_child(LeptrisElement elem);
LeptrisElement leptris_element_get_last_child(LeptrisElement elem);
LeptrisElement leptris_element_get_next_sibling(LeptrisElement elem);

/* Set encoded pointers in element */
void leptris_element_set_parent(LeptrisElement elem, LeptrisElement parent);
void leptris_element_set_first_child(LeptrisElement elem, LeptrisElement child);
void leptris_element_set_last_child(LeptrisElement elem, LeptrisElement child);
void leptris_element_set_next_sibling(LeptrisElement elem, LeptrisElement sibling);

/* Cache invalidation hook: no-op since children_array was removed in
 * TODO 90 Phase 1. Retained as a single mutation-site chokepoint so a
 * future compact-storage cache (e.g. pugixml-style compact pointer
 * table) can plug in without touching every mutation call site. */
static inline void leptris_element_invalidate_child_cache(LeptrisElement elem) {
    (void)elem;
}

/* ============================================================================
 * Attribute Access Functions
 * ============================================================================ */

/* Get first attribute from element */
struct leptris_attribute* leptris_element_get_first_attribute(LeptrisElement elem);

/* Set first attribute in element */
void leptris_element_set_first_attribute(LeptrisElement elem, struct leptris_attribute* attr);

/* Get attribute count (size_t for public API compatibility — TODO 138) */
LEPTRIS_API size_t leptris_element_attribute_count(LeptrisElement elem);

/* Get attribute by index */
struct leptris_attribute* leptris_element_get_attribute_by_index(LeptrisElement elem, uint8_t index);

/* Get attribute by name */
struct leptris_attribute* leptris_element_get_attribute_by_name(LeptrisElement elem, const char* name);

/* Get attribute by name (StringView version - internal, faster) */
struct leptris_attribute* leptris_element_get_attribute_by_name_view(LeptrisElement elem, LeptrisStringView name);

/* Add attribute to element */
int leptris_element_add_attribute(LeptrisElement elem,
                                LeptrisStringView name_view,
                                LeptrisStringView value_view,
                                LeptrisMemoryPool* pool);

/* Add attribute in deferred-NUL mode (TODO 113 Phase 5).
 * Leaves attr->name and attr->value NULL; parser finalizes them
 * after consuming the opening tag. */
int leptris_element_add_attribute_zero_copy(LeptrisElement elem,
                                           LeptrisStringView name_view,
                                           LeptrisStringView value_view,
                                           LeptrisMemoryPool* pool);

/* ============================================================================
 * Name and Namespace Access
 * ============================================================================ */

/* Get element name (lazy conversion from StringView to C string) */
const char* leptris_element_get_name(LeptrisElement elem);

/* Set prefix using StringView (zero-copy!) */

/* Set namespace URI from StringView (eager pool-strdup — TODO 90).
 * The namespace_uri_view field was removed from the struct; this
 * setter does the conversion eagerly so the cached char* is always
 * available. */
void leptris_element_set_namespace_uri_view(LeptrisElement elem, LeptrisStringView uri_view);

/* Get element prefix */
const char* leptris_element_get_prefix(LeptrisElement elem);

/* Get element namespace URI */
const char* leptris_element_get_namespace_uri(LeptrisElement elem);

/* Legacy functions for C string input */
void leptris_element_set_prefix(LeptrisElement elem, const char* prefix);
void leptris_element_set_namespace_uri(LeptrisElement elem, const char* uri);

/* ============================================================================
 * Internal StringView Accessors (for performance-critical internal code)
 * ============================================================================ */

/* Get element name as StringView (derived from cached char* — TODO 90). */
static inline LeptrisStringView leptris_element_name_view(LeptrisElement elem) {
    return elem && elem->name
        ? leptris_sv_from_ptr(elem->name, strlen(elem->name))
        : leptris_sv_empty();
}

/* Get element prefix as StringView (NO conversion, O(1) access) */
static inline LeptrisStringView leptris_element_prefix_view(LeptrisElement elem) {
    char* p = leptris_elem_prefix(elem);
    return p ? leptris_sv_from_ptr(p, strlen(p)) : leptris_sv_empty();
}

/* Get element namespace URI as StringView (derived from cached char*). */
static inline LeptrisStringView leptris_element_namespace_view(LeptrisElement elem) {
    char* u = leptris_elem_ns_uri(elem);
    return u ? leptris_sv_from_ptr(u, strlen(u)) : leptris_sv_empty();
}

/* Get attribute name as StringView (NO conversion, O(1) access) */
static inline LeptrisStringView leptris_attribute_name_view(const struct leptris_attribute* attr) {
    return attr ? attr->name_view : leptris_sv_empty();
}

/* Get attribute value as StringView (NO conversion, O(1) access) */
static inline LeptrisStringView leptris_attribute_value_view(const struct leptris_attribute* attr) {
    return attr ? leptris_attr_value_sv(attr) : leptris_sv_empty();
}

/* Fast name comparison helpers (for hot paths like traversal) */
static inline int leptris_element_name_equals(LeptrisElement elem, LeptrisStringView name) {
    if (!elem || !elem->name) return 0;
    return name.length == strlen(elem->name) &&
           memcmp(elem->name, name.data, name.length) == 0;
}

static inline int leptris_element_name_equals_lit(LeptrisElement elem, const char* lit) {
    if (!elem || !lit || !elem->name) return 0;
    return strcmp(elem->name, lit) == 0;
}

/* ============================================================================
 * Child Access Functions
 * ============================================================================ */

/* Get child count */
LEPTRIS_API size_t leptris_element_child_count(LeptrisElement elem);

/* Get child by index */
LeptrisElement leptris_element_get_child(LeptrisElement elem, uint16_t index);

/* ============================================================================
 * Legacy Public API Functions (compatibility wrappers)
 * ============================================================================ */

/* Add attribute (legacy C string API) */
void leptris_element_add_attribute_legacy(LeptrisElement elem,
                                   const char* name,
                                   const char* value);

/* Add attribute using memory pool (fast) */
void leptris_element_add_attribute_pooled(LeptrisElement elem,
                                   const char* name,
                                   const char* value,
                                   LeptrisMemoryPool* pool);

/* Add attribute with in-place strings (zero-copy) */
void leptris_element_add_attribute_pooled_inplace(LeptrisElement elem,
                                                   char* name,
                                                   char* value,
                                                   LeptrisMemoryPool* pool);

/* Get attribute value by name (legacy API) */
const char* leptris_element_get_attribute_legacy(LeptrisElement elem, const char* name);

/* NOTE: Use leptris_namespace_new() + leptris_element_add_namespace() from leptris_memory.h
 * for proper namespace management. This function is deprecated. */
void leptris_element_add_namespace_deprecated(LeptrisElement elem,
                                             const char* prefix,
                                             const char* uri);

/* Add namespace with in-place strings (zero-copy) */
void leptris_element_add_namespace_inplace(LeptrisElement elem,
                                           char* prefix,
                                           char* uri,
                                           LeptrisMemoryPool* pool);

/* Create namespace structure using pool allocation (recommended for parsing)
 * Namespace and strings are automatically freed when pool is destroyed.
 * This supports multiple documents being parsed simultaneously. */
struct leptris_namespace* leptris_namespace_new_pooled(
    const char* prefix,
    const char* uri,
    LeptrisMemoryPool* pool);

/* Lookup namespace URI by prefix */
const char* leptris_element_lookup_namespace(LeptrisElement elem,
                                             const char* prefix);

/* Children manipulation */
void leptris_element_append_child_internal(LeptrisElement elem, LeptrisNode* child);
void leptris_element_append_child_internal_doc(LeptrisElement elem, LeptrisNode* child,
                                              struct leptris_document* doc);
void leptris_element_prepend_child_internal(LeptrisElement elem, LeptrisNode* child);

/* Bulk allocation for subtree copy (10-15% faster for large subtrees) */
LeptrisElement leptris_element_append_copy_bulk(LeptrisElement parent, LeptrisElement source);

/* Text content extraction (concatenates all text nodes) */
char* leptris_element_get_text_content(LeptrisElement elem);

/* Document tree operations */
void leptris_element_set_document_tree(LeptrisElement elem, struct leptris_document* doc);

/* ============================================================================
 * Subtree Analysis (for bulk allocation planning)
 * ============================================================================ */

/* Structure to hold subtree statistics for bulk allocation */
typedef struct leptris_subtree_stats {
    uint32_t element_count;    /* Total elements in subtree */
    uint32_t attribute_count;  /* Total attributes in subtree */
    uint32_t text_count;       /* Total text nodes in subtree */
    uint32_t comment_count;    /* Total comment nodes in subtree */
    uint32_t cdata_count;     /* Total CDATA sections in subtree */
    uint32_t pi_count;         /* Total PIs in subtree */
} LeptrisSubtreeStats;

/* Count all nodes in subtree (for bulk allocation planning)
 * Recursively traverses element tree and counts all nodes by type.
 * Used to pre-calculate allocation size for bulk operations.
 *
 * @param elem Root element to count
 * @param stats Output structure to fill with counts
 */
void leptris_element_count_subtree(LeptrisElement elem, LeptrisSubtreeStats* stats);

/* ============================================================================
 * Casting Helpers
 * ============================================================================ */

#define LEPTRIS_NODE_AS_ELEMENT(node) \
    (LEPTRIS_NODE_IS_ELEMENT(node) ? (LeptrisElement)(node) : NULL)

#define LEPTRIS_ELEMENT_AS_NODE(elem) \
    ((LeptrisNode*)(elem))

#endif /* LEPTRIS_DOM_ELEMENT_H */
