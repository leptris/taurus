// test/dom/test_compact.cpp — compact allocator / overflow table specs (TODO 39).
//
// The compact allocator is the 4-byte-pointer optimization used by the
// DOM.  When a pointer can't be encoded into 4 bytes (e.g., the
// address is far from the pool's base), an overflow entry is added to
// a thread-local table.  These specs exercise the table's lifecycle
// and verify cleanup happens per-document.

#include <gtest/gtest.h>

#include "leptris.h"

extern "C" {
#include "compact.h"
#include "element.h"
}

#include <cstring>
#include <vector>
#include <cstdlib>
#include <cstdint>

namespace {

TEST(CompactAllocator, ParsesMultipleDocumentsWithoutLeak) {
    /* The overflow table is reused across documents within a thread.
     * Each document free must clean up its own entries. */
    const char xml[] = "<r><a/><b/><c/></r>";

    for (int i = 0; i < 5; i++) {
        LeptrisStatus st;
        LeptrisDocument doc = leptris_parse_string(xml, std::strlen(xml), &st);
        ASSERT_NE(doc, nullptr) << "iter " << i;
        leptris_document_free(doc);
    }
    /* Under leaks --atExit --, zero bytes leaked is the contract. */
}

TEST(CompactAllocator, ExplicitCleanupDoesNotCrash) {
    /* leptris_explicit_cleanup tears down the thread-local overflow
     * table.  Calling it when no documents are active must be safe. */
    leptris_explicit_cleanup();
    leptris_explicit_cleanup();  /* idempotent */
    SUCCEED();
}

TEST(CompactAllocator, LargeDocumentDoesNotLeakOverflowEntries) {
    /* A document large enough to trigger many overflow entries must
     * release them all on free. */
    std::string xml = "<r>";
    for (int i = 0; i < 1000; i++) {
        xml += "<a>text</a>";
    }
    xml += "</r>";

    LeptrisStatus st;
    LeptrisDocument doc = leptris_parse_string(xml.data(), xml.size(), &st);
    ASSERT_NE(doc, nullptr);

    leptris_document_free(doc);
    /* Zero leaks under valgrind/leaks. */
}

/* ---- TODO 178: 1-byte and 2-byte compact pointer round-trip specs ---- */

TEST(CompactPtr8, NullRoundTrips) {
    /* base + non-null field address. Encode NULL → 0. Decode 0 → NULL. */
    int dummy_base;
    int8_t field = 42;
    int8_t encoded = leptris_compact_ptr8_encode(&dummy_base, nullptr,
                                                 3 /* align_log2 */,
                                                 &field);
    EXPECT_EQ(encoded, 0);
    EXPECT_EQ(leptris_compact_ptr8_decode(&dummy_base, 0, 3, &field), nullptr);
}

TEST(CompactPtr8, EncodesPositiveOffsetWithinRange) {
    /* Allocate two 8-byte-aligned buffers far enough apart that the
     * offset fits in 1 byte (scaled by 8). */
    char buf[2048];
    char* base = buf;
    char* target = buf + 256;  /* 256 / 8 = 32, fits in int8_t */
    int8_t field = 0;
    int8_t encoded = leptris_compact_ptr8_encode(base, target, 3, &field);
    ASSERT_NE(encoded, 0);
    ASSERT_NE(encoded, LEPTRIS_COMPACT_PTR8_OVERFLOW);
    EXPECT_EQ(leptris_compact_ptr8_decode(base, encoded, 3, &field), target);
}

TEST(CompactPtr8, EncodesNegativeOffsetWithinRange) {
    char buf[2048];
    char* base = buf + 1024;  /* middle of buffer */
    char* target = buf + 512; /* 512 bytes before base; scaled -64 fits in int8_t */
    int8_t field = 0;
    int8_t encoded = leptris_compact_ptr8_encode(base, target, 3, &field);
    ASSERT_NE(encoded, 0);
    ASSERT_NE(encoded, LEPTRIS_COMPACT_PTR8_OVERFLOW);
    EXPECT_EQ(leptris_compact_ptr8_decode(base, encoded, 3, &field), target);
}

TEST(CompactPtr8, OverflowsBeyond1ByteRange) {
    /* Offset too large for 1 byte even with alignment scaling. */
    size_t sz = 64 * 1024;
    void* big = std::malloc(sz);
    ASSERT_NE(big, nullptr);
    char* base = (char*)big;
    char* target = (char*)big + sz - 8;  /* far beyond 1KB reach */
    /* Round target down to 8-byte alignment. */
    target = (char*)((uintptr_t)target & ~(uintptr_t)7);

    int8_t field = 0;
    int8_t encoded = leptris_compact_ptr8_encode(base, target, 3, &field);
    EXPECT_EQ(encoded, LEPTRIS_COMPACT_PTR8_OVERFLOW);
    EXPECT_EQ(leptris_compact_ptr8_decode(base, encoded, 3, &field), target);

    /* Cleanup so we don't leak the overflow entry. */
    leptris_explicit_cleanup();
    std::free(big);
}

TEST(CompactPtr8, MisalignedTargetOverflows) {
    /* Offset between base and target isn't divisible by alignment. */
    char buf[256];
    char* base = buf;
    char* target = buf + 5;  /* not 8-byte aligned */
    int8_t field = 0;
    int8_t encoded = leptris_compact_ptr8_encode(base, target, 3, &field);
    EXPECT_EQ(encoded, LEPTRIS_COMPACT_PTR8_OVERFLOW);
    EXPECT_EQ(leptris_compact_ptr8_decode(base, encoded, 3, &field), target);
    leptris_explicit_cleanup();
}

TEST(CompactPtr8, DistinctFieldsOnSameBaseDoNotCollide) {
    /* Two fields on the same struct each get their own overflow entry. */
    size_t sz = 64 * 1024;
    void* big = std::malloc(sz);
    ASSERT_NE(big, nullptr);
    char* base = (char*)big;
    char* t1 = (char*)big + sz - 8;
    char* t2 = (char*)big + sz - 16;
    t1 = (char*)((uintptr_t)t1 & ~(uintptr_t)7);
    t2 = (char*)((uintptr_t)t2 & ~(uintptr_t)7);

    int8_t f1 = 0, f2 = 0;
    int8_t e1 = leptris_compact_ptr8_encode(base, t1, 3, &f1);
    int8_t e2 = leptris_compact_ptr8_encode(base, t2, 3, &f2);
    EXPECT_EQ(e1, LEPTRIS_COMPACT_PTR8_OVERFLOW);
    EXPECT_EQ(e2, LEPTRIS_COMPACT_PTR8_OVERFLOW);
    EXPECT_EQ(leptris_compact_ptr8_decode(base, e1, 3, &f1), t1);
    EXPECT_EQ(leptris_compact_ptr8_decode(base, e2, 3, &f2), t2);

    leptris_explicit_cleanup();
    std::free(big);
}

/* ---- 2-byte compact pointer ---- */

TEST(CompactPtr16, NullRoundTrips) {
    int dummy_base;
    int16_t field = 42;
    int16_t encoded = leptris_compact_ptr16_encode(&dummy_base, nullptr,
                                                   3, &field);
    EXPECT_EQ(encoded, 0);
    EXPECT_EQ(leptris_compact_ptr16_decode(&dummy_base, 0, 3, &field), nullptr);
}

TEST(CompactPtr16, EncodesLargeOffsetWithinRange) {
    /* 2-byte covers ±256 KB at align_log2=3 — enough for any document. */
    size_t sz = 100 * 1024;
    void* big = std::malloc(sz);
    ASSERT_NE(big, nullptr);
    char* base = (char*)big;
    char* target = (char*)big + 65536;  /* 64 KB away, fits in int16_t */
    int16_t field = 0;
    int16_t encoded = leptris_compact_ptr16_encode(base, target, 3, &field);
    ASSERT_NE(encoded, 0);
    ASSERT_NE(encoded, LEPTRIS_COMPACT_PTR16_OVERFLOW);
    EXPECT_EQ(leptris_compact_ptr16_decode(base, encoded, 3, &field), target);
    std::free(big);
}

TEST(CompactPtr16, OverflowsBeyond256KB) {
    /* Beyond int16 range even with alignment — needs overflow table. */
    size_t sz = 1024 * 1024;
    void* big = std::malloc(sz);
    ASSERT_NE(big, nullptr);
    char* base = (char*)big;
    char* target = (char*)big + sz - 8;
    target = (char*)((uintptr_t)target & ~(uintptr_t)7);

    int16_t field = 0;
    int16_t encoded = leptris_compact_ptr16_encode(base, target, 3, &field);
    EXPECT_EQ(encoded, LEPTRIS_COMPACT_PTR16_OVERFLOW);
    EXPECT_EQ(leptris_compact_ptr16_decode(base, encoded, 3, &field), target);

    leptris_explicit_cleanup();
    std::free(big);
}

TEST(CompactPtr8And16, ShareOverflowTableWithoutInterference) {
    /* Both encoders use the same table; field addresses are unique
     * keys, so 1-byte and 2-byte entries coexist safely. */
    size_t sz = 1024 * 1024;
    void* big = std::malloc(sz);
    ASSERT_NE(big, nullptr);
    char* base = (char*)big;
    char* target = (char*)big + sz - 8;
    target = (char*)((uintptr_t)target & ~(uintptr_t)7);

    int8_t f8 = 0;
    int16_t f16 = 0;
    int8_t e8 = leptris_compact_ptr8_encode(base, target, 3, &f8);
    int16_t e16 = leptris_compact_ptr16_encode(base, target, 3, &f16);
    EXPECT_EQ(e8, LEPTRIS_COMPACT_PTR8_OVERFLOW);
    EXPECT_EQ(e16, LEPTRIS_COMPACT_PTR16_OVERFLOW);
    EXPECT_EQ(leptris_compact_ptr8_decode(base, e8, 3, &f8), target);
    EXPECT_EQ(leptris_compact_ptr16_decode(base, e16, 3, &f16), target);

    leptris_explicit_cleanup();
    std::free(big);
}


/* Overflow-table growth (round 17): the table historically kept a
 * fixed 256-bucket chained hash; >256 entries degraded to linear
 * chain walks (the rising mutation-append cost). Load-factor growth
 * must preserve every entry across rehashes. */
TEST(LeptrisCompact, OverflowTableGrowsCorrectly) {
    LeptrisCompactOverflowTable* t = leptris_compact_overflow_table_create(16);
    ASSERT_NE(t, nullptr);

    /* 5000 distinct keys — forces several doublings past the
     * initial 16 buckets. Values are the key address itself. */
    static void* keys[5000];
    for (int i = 0; i < 5000; i++) {
        keys[i] = (void*)(uintptr_t)(0x100000 + (size_t)i * 24);
        ASSERT_EQ(leptris_compact_overflow_set(t, keys[i], keys[i], nullptr), 0);
    }
    for (int i = 0; i < 5000; i++) {
        EXPECT_EQ(leptris_compact_overflow_get(t, keys[i]), keys[i])
            << "entry " << i << " lost across growth rehash";
    }
    /* Overwrite one entry; the update must win over the old value. */
    void* sentinel = (void*)(uintptr_t)0xDEAD;
    ASSERT_EQ(leptris_compact_overflow_set(t, keys[7], sentinel, nullptr), 0);
    EXPECT_EQ(leptris_compact_overflow_get(t, keys[7]), sentinel);

    leptris_compact_overflow_table_destroy(t);
}

/* Lane 18 P1 invariant: leptris_elem_split_qname RE-STAMPS the
 * namebp backpointer into a fresh [doc][local] slot when it splits
 * a prefixed name (public creates no longer register in the root
 * map — the backpointer is the resolution path, so it must survive
 * the split, not drop with it). */
TEST(MutNameBackpointer, PrefixedSplitRestampsBackpointer) {
    LeptrisStatus st = LEPTRIS_OK;
    LeptrisDocument doc = leptris_document_create();
    ASSERT_NE(doc, nullptr);

    LeptrisElement plain = leptris_element_create(doc, "local");
    ASSERT_NE(plain, nullptr);
    /* Colon-free: backpointer stays valid. */
    EXPECT_TRUE(leptris_elem_has_namebp(plain));
    EXPECT_EQ(leptris_elem_namebp_doc(plain), doc);

    LeptrisElement prefixed = leptris_element_create(doc, "p:local");
    ASSERT_NE(prefixed, nullptr);
    /* Split moved name into a FRESH stamped slot — the flag stays
     * and name[-1] is the doc pointer again. */
    EXPECT_TRUE(leptris_elem_has_namebp(prefixed));
    EXPECT_EQ(leptris_elem_namebp_doc(prefixed), doc);
    /* And document resolution must still be correct. */
    EXPECT_EQ(leptris_element_get_document(prefixed), doc);

    leptris_document_free(doc);
}

}  // namespace
