/* dom/element_index.c — Per-document element index (TODO 132).
 *
 * Build a flat preorder array of all elements + per-name buckets.
 * The index lives on LeptrisDocument and is built lazily on first
 * descendant-axis access.
 *
 * Build walks the tree once via the same parent / first_child /
 * next_sibling links the iterative descendant_walk uses. */
#include "element_index.h"
#include "element.h"
#include <stdlib.h>
#include <string.h>

/* Grow a generic pointer array. */
static int grow_ptr_array(void** arr, size_t* cap, size_t elem_size) {
    size_t new_cap = (*cap == 0) ? 8 : (*cap * 2);
    void* grown = realloc(*arr, new_cap * elem_size);
    if (!grown) return -1;
    *arr = grown;
    *cap = new_cap;
    return 0;
}

/* ---- Build-side bucket lookup hash -------------------------------
 * The bucket lookups during index_walk used to be linear scans over
 * the distinct names / attribute names / attribute values. Documents
 * with many distinct values (e.g. 20k unique id attributes) made the
 * build O(distinct^2) — a 300 ms spike on a 20k-element document.
 * Keyed on (name bytes, optional value bytes); id is the bucket the
 * key resolved to. Key strings are the buckets' own malloc'd
 * copies, stable for the build's lifetime. */
typedef struct {
    const char* s1; size_t l1;   /* element/attr name */
    const char* s2; size_t l2;   /* attr value; l2==0 = name-only key */
    size_t id;
} IndexKey;

typedef struct {
    IndexKey* entries;           /* NULL slot = empty */
    size_t mask;                 /* capacity - 1 */
    size_t count;
} IndexKeyTable;

static size_t index_key_hash(const char* s1, size_t l1,
                             const char* s2, size_t l2) {
    size_t h = 1469598103934665603ull;
    for (size_t i = 0; i < l1; i++) { h ^= (unsigned char)s1[i]; h *= 1099511628211ull; }
    for (size_t i = 0; i < l2; i++) { h ^= (unsigned char)s2[i]; h *= 1099511628211ull; }
    h ^= l1; h *= 1099511628211ull;
    return h;
}

static void index_key_table_free(IndexKeyTable* t) {
    free(t->entries);
}

/* Returns the id stored for the key, or SIZE_MAX when absent. */
static size_t index_key_table_get(const IndexKeyTable* t,
                                  const char* s1, size_t l1,
                                  const char* s2, size_t l2) {
    if (!t->entries) return (size_t)-1;
    size_t i = index_key_hash(s1, l1, s2, l2) & t->mask;
    while (t->entries[i].s1) {
        const IndexKey* e = &t->entries[i];
        if (e->l1 == l1 && e->l2 == l2 &&
            memcmp(e->s1, s1, l1) == 0 &&
            (l2 == 0 || memcmp(e->s2, s2, l2) == 0)) {
            return e->id;
        }
        i = (i + 1) & t->mask;
    }
    return (size_t)-1;
}

static int index_key_table_put(IndexKeyTable* t, const char* s1, size_t l1,
                               const char* s2, size_t l2, size_t id) {
    if (!t->entries || (t->count + 1) * 10 >= (t->mask + 1) * 7) {
        IndexKey* old_entries = t->entries;
        size_t old_mask = t->mask;
        size_t new_cap = old_entries ? (old_mask + 1) << 1 : 256;
        IndexKey* grown = (IndexKey*)calloc(new_cap, sizeof(IndexKey));
        if (!grown) return -1;
        size_t new_mask = new_cap - 1;
        for (size_t j = 0; old_entries && j <= old_mask; j++) {
            if (!old_entries[j].s1) continue;
            size_t k = index_key_hash(old_entries[j].s1, old_entries[j].l1,
                                      old_entries[j].s2, old_entries[j].l2) & new_mask;
            while (grown[k].s1) k = (k + 1) & new_mask;
            grown[k] = old_entries[j];
        }
        free(old_entries);
        t->entries = grown;
        t->mask = new_mask;
    }
    size_t i = index_key_hash(s1, l1, s2, l2) & t->mask;
    while (t->entries[i].s1) {
        if (t->entries[i].l1 == l1 && t->entries[i].l2 == l2 &&
            memcmp(t->entries[i].s1, s1, l1) == 0 &&
            (l2 == 0 || memcmp(t->entries[i].s2, s2, l2) == 0)) {
            return 0;  /* already present */
        }
        i = (i + 1) & t->mask;
    }
    t->entries[i].s1 = s1;
    t->entries[i].l1 = l1;
    t->entries[i].s2 = s2;
    t->entries[i].l2 = l2;
    t->entries[i].id = id;
    t->count++;
    return 0;
}

/* Recursive preorder walk. Appends each element to all_elements and
 * to the appropriate name bucket. */
static void index_walk(LeptrisElementIndex* idx, IndexKeyTable* name_keys,
                       IndexKeyTable* pair_keys, LeptrisElement elem) {
    if (!elem) return;

    size_t me = idx->all_count;
    idx->all_elements[idx->all_count++] = elem;

    /* Name bucket. */
    const char* name = leptris_element_get_name(elem);
    if (name) {
        LeptrisElementIndexBucket* bucket = NULL;
        size_t name_len = strlen(name);
        size_t bi = index_key_table_get(name_keys, name, name_len, NULL, 0);
        if (bi != (size_t)-1) bucket = &idx->buckets[bi];
        if (!bucket) {
            if (idx->bucket_count >= idx->bucket_capacity) {
                if (grow_ptr_array((void**)&idx->buckets,
                                    &idx->bucket_capacity,
                                    sizeof(LeptrisElementIndexBucket)) != 0) return;
            }
            bucket = &idx->buckets[idx->bucket_count];
            bucket->name = strdup(name);
            bucket->name_len = name_len;
            if (bucket->name) {
                index_key_table_put(name_keys, bucket->name, name_len,
                                    NULL, 0, idx->bucket_count);
            }
            idx->bucket_count++;
            bucket->matches = NULL;
            bucket->count = 0;
            bucket->capacity = 0;
            bucket->match_positions = NULL;
        }
        if (bucket->count >= bucket->capacity) {
            /* Grow matches and match_positions in lockstep — one
             * capacity, two arrays (TODO 192). */
            size_t new_cap = (bucket->capacity == 0) ? 8 : bucket->capacity * 2;
            LeptrisElement* gm = (LeptrisElement*)realloc(
                bucket->matches, new_cap * sizeof(LeptrisElement));
            size_t* gp = (size_t*)realloc(
                bucket->match_positions, new_cap * sizeof(size_t));
            if (!gm || !gp) {
                if (gm) bucket->matches = gm;
                if (gp) bucket->match_positions = gp;
                return;
            }
            bucket->matches = gm;
            bucket->match_positions = gp;
            bucket->capacity = new_cap;
        }
        bucket->matches[bucket->count] = elem;
        bucket->match_positions[bucket->count] = me;
        bucket->count++;
    }

    /* Attribute buckets (TODO 133). */
    struct leptris_attribute* attr = leptris_element_get_first_attribute(elem);
    while (attr) {
        LeptrisStringView nv = attr->name_view;
        LeptrisStringView vv = leptris_attr_value_sv(attr);
        if (nv.length > 0 && nv.data) {
            LeptrisElementIndexAttrBucket* abucket = NULL;
            size_t ai = index_key_table_get(pair_keys, nv.data, nv.length,
                                            NULL, 0);
            if (ai != (size_t)-1) abucket = &idx->attr_buckets[ai];
            if (!abucket) {
                if (idx->attr_bucket_count >= idx->attr_bucket_capacity) {
                    if (grow_ptr_array((void**)&idx->attr_buckets,
                                        &idx->attr_bucket_capacity,
                                        sizeof(LeptrisElementIndexAttrBucket)) != 0) {
                        attr = leptris_attr_next(attr); continue;
                    }
                }
                abucket = &idx->attr_buckets[idx->attr_bucket_count];
                abucket->attr_name = (char*)malloc(nv.length + 1);
                if (abucket->attr_name) {
                    memcpy(abucket->attr_name, nv.data, nv.length);
                    abucket->attr_name[nv.length] = '\0';
                }
                abucket->attr_name_len = nv.length;
                if (abucket->attr_name) {
                    index_key_table_put(pair_keys, abucket->attr_name,
                                        nv.length, NULL, 0,
                                        idx->attr_bucket_count);
                }
                idx->attr_bucket_count++;
                abucket->matches = NULL;
                abucket->count = 0;
                abucket->capacity = 0;
                abucket->match_positions = NULL;
                abucket->values = NULL;
                abucket->value_count = 0;
                abucket->value_capacity = 0;
            }
            if (abucket->count >= abucket->capacity) {
                /* Lockstep growth: one capacity, two arrays. */
                size_t new_cap = (abucket->capacity == 0) ? 8 : abucket->capacity * 2;
                LeptrisElement* gm = (LeptrisElement*)realloc(
                    abucket->matches, new_cap * sizeof(LeptrisElement));
                size_t* gp = (size_t*)realloc(
                    abucket->match_positions, new_cap * sizeof(size_t));
                if (!gm || !gp) {
                    if (gm) abucket->matches = gm;
                    if (gp) abucket->match_positions = gp;
                    attr = leptris_attr_next(attr); continue;
                }
                abucket->matches = gm;
                abucket->match_positions = gp;
                abucket->capacity = new_cap;
            }
            abucket->matches[abucket->count] = elem;
            abucket->match_positions[abucket->count] = me;
            abucket->count++;

            if (vv.length > 0 && vv.data) {
                LeptrisElementIndexAttrValue* vbucket = NULL;
                size_t vi = index_key_table_get(pair_keys,
                                                abucket->attr_name, nv.length,
                                                vv.data, vv.length);
                if (vi != (size_t)-1) vbucket = &abucket->values[vi];
                if (!vbucket) {
                    if (abucket->value_count >= abucket->value_capacity) {
                        if (grow_ptr_array((void**)&abucket->values,
                                            &abucket->value_capacity,
                                            sizeof(LeptrisElementIndexAttrValue)) != 0) {
                            attr = leptris_attr_next(attr); continue;
                        }
                    }
                    vbucket = &abucket->values[abucket->value_count];
                    vbucket->value = (char*)malloc(vv.length + 1);
                    if (vbucket->value) {
                        memcpy(vbucket->value, vv.data, vv.length);
                        vbucket->value[vv.length] = '\0';
                    }
                    vbucket->value_len = vv.length;
                    if (abucket->attr_name && vbucket->value) {
                        index_key_table_put(pair_keys, abucket->attr_name,
                                            nv.length, vbucket->value,
                                            vv.length, abucket->value_count);
                    }
                    abucket->value_count++;
                    vbucket->matches = NULL;
                    vbucket->count = 0;
                    vbucket->capacity = 0;
                    vbucket->match_positions = NULL;
                }
                if (vbucket->count >= vbucket->capacity) {
                    /* Lockstep growth: one capacity, two arrays. */
                    size_t new_cap = (vbucket->capacity == 0) ? 8 : vbucket->capacity * 2;
                    LeptrisElement* gm = (LeptrisElement*)realloc(
                        vbucket->matches, new_cap * sizeof(LeptrisElement));
                    size_t* gp = (size_t*)realloc(
                        vbucket->match_positions, new_cap * sizeof(size_t));
                    if (!gm || !gp) {
                        if (gm) vbucket->matches = gm;
                        if (gp) vbucket->match_positions = gp;
                        attr = leptris_attr_next(attr); continue;
                    }
                    vbucket->matches = gm;
                    vbucket->match_positions = gp;
                    vbucket->capacity = new_cap;
                }
                vbucket->matches[vbucket->count] = elem;
                vbucket->match_positions[vbucket->count] = me;
                vbucket->count++;
            }
        }
        attr = leptris_attr_next(attr);
    }

    LeptrisElement child = leptris_element_get_first_child(elem);
    while (child) {
        index_walk(idx, name_keys, pair_keys, child);
        child = leptris_element_get_next_sibling(child);
    }

    /* Subtree interval close (TODO 192): all children walked, so
     * the subtree covers [me, all_count - 1] in preorder. */
    idx->subtree_end[me] = idx->all_count - 1;
}

/* Count elements in the tree (for initial allocation). */
static size_t count_elements(LeptrisElement elem) {
    if (!elem) return 0;
    size_t n = 1;
    LeptrisElement child = leptris_element_get_first_child(elem);
    while (child) {
        n += count_elements(child);
        child = leptris_element_get_next_sibling(child);
    }
    return n;
}

LeptrisElementIndex* leptris_element_index_build(struct leptris_document* doc) {
    if (!doc) return NULL;
    /* TODO 139 Phase D: lazy promote. */
    leptris_document_ensure_promoted(doc);
    if (!doc->new_dom_root) return NULL;

    LeptrisElement root = (LeptrisElement)doc->new_dom_root;
    size_t total = count_elements(root);

    LeptrisElementIndex* idx = (LeptrisElementIndex*)calloc(1, sizeof(*idx));
    if (!idx) return NULL;

    idx->all_elements = (LeptrisElement*)malloc(total * sizeof(LeptrisElement));
    if (!idx->all_elements) {
        free(idx);
        return NULL;
    }
    idx->subtree_end = (size_t*)malloc(total * sizeof(size_t));
    if (!idx->subtree_end) {
        free(idx->all_elements);
        free(idx);
        return NULL;
    }
    idx->all_count = 0;

    IndexKeyTable name_keys = {0};
    IndexKeyTable pair_keys = {0};
    index_walk(idx, &name_keys, &pair_keys, root);
    index_key_table_free(&name_keys);
    index_key_table_free(&pair_keys);
    return idx;
}

void leptris_element_index_free(LeptrisElementIndex* idx) {
    if (!idx) return;
    free(idx->all_elements);
    free(idx->subtree_end);
    for (size_t i = 0; i < idx->bucket_count; i++) {
        free(idx->buckets[i].name);
        free(idx->buckets[i].matches);
        free(idx->buckets[i].match_positions);
    }
    free(idx->buckets);
    for (size_t i = 0; i < idx->attr_bucket_count; i++) {
        free(idx->attr_buckets[i].attr_name);
        free(idx->attr_buckets[i].matches);
        free(idx->attr_buckets[i].match_positions);
        for (size_t j = 0; j < idx->attr_buckets[i].value_count; j++) {
            free(idx->attr_buckets[i].values[j].value);
            free(idx->attr_buckets[i].values[j].matches);
            free(idx->attr_buckets[i].values[j].match_positions);
        }
        free(idx->attr_buckets[i].values);
    }
    free(idx->attr_buckets);
    free(idx);
}

void leptris_element_index_invalidate(struct leptris_document* doc) {
    if (!doc) return;
    /* Always bump: the document-order rank cache (issue #485) keys
     * on this even when no element index was ever built. */
    doc->mutation_version++;
    if (!doc->element_index) return;
    leptris_element_index_free(doc->element_index);
    doc->element_index = NULL;
}

const LeptrisElementIndexBucket* leptris_element_index_lookup(
    const LeptrisElementIndex* idx, const char* name) {
    if (!idx || !name) return NULL;
    size_t name_len = strlen(name);
    for (size_t i = 0; i < idx->bucket_count; i++) {
        if (idx->buckets[i].name_len == name_len &&
            memcmp(idx->buckets[i].name, name, name_len) == 0) {
            return &idx->buckets[i];
        }
    }
    return NULL;
}

int leptris_element_index_subtree_interval(
    const LeptrisElementIndex* idx, LeptrisElement ctx,
    size_t* out_lo, size_t* out_hi) {
    if (!idx || !ctx || !idx->subtree_end || idx->all_count == 0) return 0;

    /* Binary search by pointer. For parse-produced documents the
     * arena order is preorder, so all_elements is pointer-sorted and
     * the search hits. For mutated documents the order may be
     * disturbed — a miss just means "fall back to a walk"; an exact
     * pointer hit is always the element's true preorder position. */
    size_t lo = 0, hi = idx->all_count;
    size_t pos = (size_t)-1;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if ((const void*)idx->all_elements[mid] == (const void*)ctx) {
            pos = mid;
            break;
        }
        if ((const void*)idx->all_elements[mid] < (const void*)ctx) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (pos == (size_t)-1) return 0;

    *out_lo = pos;
    *out_hi = idx->subtree_end[pos];
    return 1;
}

const LeptrisElementIndexAttrBucket* leptris_element_index_lookup_attr(
    const LeptrisElementIndex* idx, const char* attr_name) {
    if (!idx || !attr_name) return NULL;
    size_t name_len = strlen(attr_name);
    for (size_t i = 0; i < idx->attr_bucket_count; i++) {
        if (idx->attr_buckets[i].attr_name_len == name_len &&
            memcmp(idx->attr_buckets[i].attr_name, attr_name, name_len) == 0) {
            return &idx->attr_buckets[i];
        }
    }
    return NULL;
}

const LeptrisElementIndexAttrValue* leptris_element_index_attr_lookup_value(
    const LeptrisElementIndexAttrBucket* bucket, const char* value) {
    if (!bucket || !value) return NULL;
    size_t value_len = strlen(value);
    for (size_t i = 0; i < bucket->value_count; i++) {
        if (bucket->values[i].value_len == value_len &&
            memcmp(bucket->values[i].value, value, value_len) == 0) {
            return &bucket->values[i];
        }
    }
    return NULL;
}
