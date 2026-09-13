/* dom/root_doc_map.c — Thread-local root-element → document mapping.
 *
 * TODO 155 Phase A: the `document` field was removed from struct
 * leptris_element to fit it in one 64-byte cache line. Non-root
 * elements reach their document by walking parent_off to the root,
 * then looking up the root in this thread-local hash table.
 *
 * TODO 157 (perf): uses a free-list to avoid per-parse malloc/free.
 * After warmup, register and unregister are O(1) with zero heap ops. */
#include "root_doc_map.h"
#include "element.h"
#include "../common/port.h"
#include <stdlib.h>

#define ROOT_DOC_BUCKETS 256

/* Round 20: header.flags bit marking "an entry for this element is
 * (possibly) in the map". flags is otherwise reserved — node types
 * live in base.type. With the bit, register skips the duplicate-
 * check walk for never-registered elements (the mutation hot path:
 * create registers every new element so pre-attach ops can resolve
 * the doc; the walk made sequential appends O(chain) with chains
 * polluted by every element ever created). */
#define ROOTMAP_FLAG 0x80u

static inline int rootmap_marked(LeptrisElement e) {
    return (e->header.flags & ROOTMAP_FLAG) != 0;
}

static inline void rootmap_set(LeptrisElement e, int on) {
    if (on) e->header.flags |= ROOTMAP_FLAG;
    else e->header.flags &= (uint8_t)(~ROOTMAP_FLAG & 0xFFu);
}

typedef struct root_doc_entry {
    LeptrisElement root;
    struct leptris_document* doc;
    struct root_doc_entry* next;
} RootDocEntry;

static LEPTRIS_THREAD_LOCAL RootDocEntry* g_root_doc_buckets[ROOT_DOC_BUCKETS];

/* Free-list: recycled entries from unregistered roots. Eliminates
 * malloc/free churn on the parse→free cycle. */
static LEPTRIS_THREAD_LOCAL RootDocEntry* g_free_list;

/* Entry chunks (lane 18 P0): one TLS bump chunk serves fresh
 * entries so a create-heavy document pays no malloc per element —
 * the free-list only refills after unregister. Entries are still
 * exactly the same map nodes with the same lifetime; only the
 * allocation source changed. */
#define ROOTMAP_CHUNK 128
typedef struct root_doc_chunk {
    struct root_doc_chunk* next;
    RootDocEntry entries[ROOTMAP_CHUNK];
} RootDocChunk;
static LEPTRIS_THREAD_LOCAL RootDocChunk* g_entry_chunks;
static LEPTRIS_THREAD_LOCAL RootDocEntry* g_entry_cursor;
static LEPTRIS_THREAD_LOCAL RootDocEntry* g_entry_end;

static RootDocEntry* entry_take(void) {
    if (g_free_list) {
        RootDocEntry* e = g_free_list;
        g_free_list = e->next;
        return e;
    }
    if (g_entry_cursor < g_entry_end) return g_entry_cursor++;
    RootDocChunk* c = (RootDocChunk*)malloc(sizeof(*c));
    if (!c) return NULL;
    c->next = g_entry_chunks;
    g_entry_chunks = c;
    g_entry_end = c->entries + ROOTMAP_CHUNK;
    RootDocEntry* e = c->entries;
    g_entry_cursor = e + 1;
    return e;
}

static size_t bucket_index(LeptrisElement root) {
    uintptr_t v = (uintptr_t)root;
    v ^= v >> 16;
    v ^= v >> 8;
    return (size_t)(v & (ROOT_DOC_BUCKETS - 1));
}

void leptris_root_doc_register(LeptrisElement root, struct leptris_document* doc) {
    if (!root || !doc) return;
    size_t idx = bucket_index(root);
    if (rootmap_marked(root)) {
        /* Possibly already present: walk to update. */
        for (RootDocEntry* e = g_root_doc_buckets[idx]; e; e = e->next) {
            if (e->root == root) { e->doc = doc; return; }
        }
    } else {
        /* Never registered: prepend directly, no duplicate walk. */
        RootDocEntry* e = entry_take();
        if (!e) return;
        e->root = root; e->doc = doc;
        e->next = g_root_doc_buckets[idx];
        g_root_doc_buckets[idx] = e;
        rootmap_set(root, 1);
        return;
    }
    /* Pop from free-list, or the bump chunk. */
    RootDocEntry* e = entry_take();
    if (!e) return;
    e->root = root; e->doc = doc;
    e->next = g_root_doc_buckets[idx];
    g_root_doc_buckets[idx] = e;
}

/* TODO.concurrency/08: TLS free-list entries outlive their thread
 * (no portable C99 TLS destructor). leptris_thread_cleanup() drains
 * them from each worker thread before exit. */
void leptris_root_doc_drain_thread_caches(void) {
    while (g_free_list) {
        RootDocEntry* next = g_free_list->next;
        g_free_list = next; /* chunk-owned; freed with the chunks */
    }
    while (g_entry_chunks) {
        RootDocChunk* next = g_entry_chunks->next;
        free(g_entry_chunks);
        g_entry_chunks = next;
    }
    g_entry_cursor = NULL;
    g_entry_end = NULL;
}

void leptris_root_doc_unregister(LeptrisElement root) {
    if (!root) return;
    if (!rootmap_marked(root)) return;  /* never registered: O(1) out */
    size_t idx = bucket_index(root);
    RootDocEntry** pp = &g_root_doc_buckets[idx];
    while (*pp) {
        if ((*pp)->root == root) {
            RootDocEntry* freed = *pp;
            *pp = freed->next;
            /* Push to free-list instead of free(). */
            freed->next = g_free_list;
            g_free_list = freed;
            rootmap_set(root, 0);
            return;
        }
        pp = &(*pp)->next;
    }
}

/* #1038: root-doc map entries must die with their document. The
 * pool-fallback create paths register DETACHED elements (long
 * names, carve failure) that never become new_dom_root, and the
 * adopted-child free path nulls new_dom_root before recursing —
 * pre-fix those entries outlived the doc, and a malloc-recycled
 * element address later resolved the FREED doc through the stale
 * entry: roaming heap corruption in downstream binding suites
 * (~5% of runs, v1.9.151-155). document_free sweeps every bucket
 * for this doc; the TLS memo is already invalidated there. */
void leptris_root_doc_unregister_doc(struct leptris_document* doc) {
    if (!doc) return;
    for (size_t b = 0; b < ROOT_DOC_BUCKETS; b++) {
        RootDocEntry** pp = &g_root_doc_buckets[b];
        while (*pp) {
            if ((*pp)->doc == doc) {
                RootDocEntry* freed = *pp;
                *pp = freed->next;
                freed->next = g_free_list;
                g_free_list = freed;
                rootmap_set(freed->root, 0);
                continue;
            }
            pp = &(*pp)->next;
        }
    }
}

struct leptris_document* leptris_root_doc_lookup(LeptrisElement root) {
    if (!root) return NULL;
    size_t idx = bucket_index(root);
    for (RootDocEntry* e = g_root_doc_buckets[idx]; e; e = e->next) {
        if (e->root == root) return e->doc;
    }
    return NULL;
}

/* #682 2x lever: an ambient document hint set by the transform
 * driver. Result-tree elements walk up to a root created in THIS
 * document — when the hint matches, the root climb + TLS hash map
 * are skipped entirely (get_document was 3.5% of the light bench).
 * Hint is thread-local-scoped by construction (one transform per
 * thread), validated by the root-walk fallback when it misses. */
/* #682 2x lever (sound): last-hit (root, doc) memo. The climb to
 * the root is unavoidable without a per-element field, but the TLS
 * bucket-array walk is not — consecutive queries in a transform
 * hit the same root, so one pointer compare replaces the hash
 * bucket chain. One TLS access total (the memo pair), and the
 * 1M-loop sentinel becomes a plain NULL check. */
static LEPTRIS_THREAD_LOCAL LeptrisElement g_memo_root;
static LEPTRIS_THREAD_LOCAL struct leptris_document* g_memo_doc;

/* #904: prime the TLS last-root memo from a driver that already
 * knows the (root, doc) pair — the iterparse yield path hands out
 * a subtree that is released before the next, so the memo misses
 * on every cold read and each pays the climb + bucket walk. The
 * caller must pass a REGISTERED root (register-on-create contract
 * guarantees created subtree roots are); free invalidates. */
void leptris_root_doc_memo_prime(LeptrisElement root,
                                 struct leptris_document* doc) {
    if (!root || !doc) return;
    g_memo_root = root;
    g_memo_doc = doc;
}

void leptris_root_doc_memo_invalidate(const struct leptris_document* doc) {
    if (g_memo_doc == doc) {
        g_memo_root = NULL;
        g_memo_doc = NULL;
    }
}

struct leptris_document* leptris_element_get_document(LeptrisElement elem) {
    if (!elem) return NULL;
    LeptrisElement cur = elem;
    for (;;) {
        if (cur == g_memo_root) return g_memo_doc;
        LeptrisElement parent = leptris_elem_parent(cur);
        if (!parent) break;
        cur = parent;
    }
    for (int i = 0; i < 1000000; i++) {
        LeptrisElement parent = leptris_elem_parent(cur);
        if (!parent) break;
        cur = parent;
    }
    struct leptris_document* d = leptris_root_doc_lookup(cur);
    if (d) {
        g_memo_root = cur;
        g_memo_doc = d;
        return d;
    }
    /* Round 21: unattached mutation elements carry their doc in the
     * name slot backpointer — a stateless fallback that replaced the
     * register-on-create / unregister-on-attach map pair. */
    if (cur->name && leptris_elem_has_namebp(cur)) {
        return leptris_elem_namebp_doc(cur);
    }
    return NULL;
}

LeptrisMemoryPool* leptris_element_get_pool(LeptrisElement elem) {
    struct leptris_document* d = leptris_element_get_document(elem);
    return d ? d->pool : NULL;
}
