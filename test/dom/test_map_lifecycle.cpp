#include <gtest/gtest.h>
#include <leptris.h>
#include <cstring>
#include <string>

extern "C" struct leptris_document* leptris_root_doc_lookup(
    LeptrisElement root);

/* #1038: root-doc map entries must die with their document. The
 * pool-fallback create path (names > 254 bytes, carve failure)
 * registers the DETACHED element, and adopted-child frees null
 * new_dom_root before recursing — pre-fix those entries outlived
 * the doc, and a malloc-recycled element address later resolved
 * the FREED doc through the stale entry: roaming heap corruption
 * in every downstream binding suite (~5% of runs, v1.9.151-155). */
TEST(RootDocMapLifecycle, FallbackEntryDiesWithDocument) {
    const std::string long_name(300, 'n');  /* > 254: pool fallback */
    LeptrisDocument d = leptris_document_create();
    ASSERT_TRUE(d != NULL);
    LeptrisElement e = leptris_element_create(d, long_name.c_str());
    ASSERT_TRUE(e != NULL);
    /* Registered (fallback) and resolvable while the doc lives. */
    EXPECT_TRUE(leptris_root_doc_lookup(e) != NULL);
    leptris_document_free(d);
    /* The entry must not survive the document: a recycled element
     * address here resolves a freed doc (lookup only compares
     * pointers, so this assertion itself is well-defined). */
    EXPECT_EQ(leptris_root_doc_lookup(e), nullptr);
}

TEST(RootDocMapLifecycle, RecycleLoopSurvives) {
    const std::string long_name(300, 'm');
    for (int rep = 0; rep < 4000; rep++) {
        LeptrisDocument d = leptris_document_create();
        if (!d) break;
        LeptrisElement root = leptris_element_create(d, "r");
        leptris_document_set_root(d, root);
        for (int k = 0; k < 4; k++) {
            /* Detached fallback-registrations: leaked pre-fix. */
            (void)leptris_element_create(d, long_name.c_str());
        }
        char* s = leptris_document_serialize(d, NULL);
        if (s) leptris_free_string(s);
        leptris_document_free(d);
    }
    SUCCEED();
}
