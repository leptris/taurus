// test/dom/test_document_children.cpp — issue #580: document-level
// PIs/comments are tree children of the document node (libxml2
// model). Navigation from leptris_document_node, XPath §5 visibility
// at the document level, #526 flat accessors over the same store.

#include <gtest/gtest.h>

#include "leptris.h"

#include <cstring>
#include <string>

namespace {

/* <!-- pro --!><?pp d?><r><!-- in --><?ip?></r><!-- epi --> */
constexpr char kDocLevel[] =
    "<!-- pro --><?pp d?><r><!-- in --><?ip?></r><!-- epi --><?ep?>";

TEST(DocumentChildren, ChainIsPrologRootEpilog) {
    LeptrisDocument doc =
        leptris_parse_string(kDocLevel, std::strlen(kDocLevel), nullptr);
    ASSERT_NE(doc, nullptr);

    LeptrisNodeRef n = leptris_document_node(doc);
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(leptris_node_get_type(n), LEPTRIS_NODE_TYPE_DOCUMENT);

    LeptrisNodeRef c = leptris_node_first_child(n);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(leptris_node_get_type(c), LEPTRIS_NODE_TYPE_COMMENT);
    c = leptris_node_next_sibling(c);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(leptris_node_get_type(c), LEPTRIS_NODE_TYPE_PI);
    c = leptris_node_next_sibling(c);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(leptris_node_get_type(c), LEPTRIS_NODE_TYPE_ELEMENT);
    c = leptris_node_next_sibling(c);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(leptris_node_get_type(c), LEPTRIS_NODE_TYPE_COMMENT);
    c = leptris_node_next_sibling(c);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(leptris_node_get_type(c), LEPTRIS_NODE_TYPE_PI);
    EXPECT_EQ(leptris_node_next_sibling(c), nullptr);

    leptris_document_free(doc);
}

TEST(DocumentChildren, FlatAccessorsReadTheSameStore) {
    LeptrisDocument doc =
        leptris_parse_string(kDocLevel, std::strlen(kDocLevel), nullptr);
    ASSERT_NE(doc, nullptr);

    EXPECT_EQ(leptris_document_pi_count(doc), 2u);
    EXPECT_STREQ(leptris_document_pi_target(doc, 0), "pp");
    EXPECT_STREQ(leptris_document_pi_target(doc, 1), "ep");
    EXPECT_EQ(leptris_document_comment_count(doc), 2u);
    EXPECT_STREQ(leptris_document_comment_content(doc, 0), " pro ");
    EXPECT_STREQ(leptris_document_comment_content(doc, 1), " epi ");

    leptris_document_free(doc);
}

TEST(DocumentChildren, XpathSeesDocumentLevelNodes) {
    LeptrisDocument doc =
        leptris_parse_string(kDocLevel, std::strlen(kDocLevel), nullptr);
    ASSERT_NE(doc, nullptr);

    LeptrisXPathResult r =
        leptris_xpath_eval(doc, nullptr, "count(/processing-instruction())");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(leptris_xpath_result_number(r), 2.0);
    leptris_xpath_result_free(r);

    r = leptris_xpath_eval(doc, nullptr, "count(/comment())");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(leptris_xpath_result_number(r), 2.0);
    leptris_xpath_result_free(r);

    /* Target-filtered document-level PI. */
    r = leptris_xpath_eval(doc, nullptr, "count(/processing-instruction('ep'))");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(leptris_xpath_result_number(r), 1.0);
    leptris_xpath_result_free(r);

    /* descendant-or-self from the document covers document-level AND
     * tree-internal nodes. */
    r = leptris_xpath_eval(doc, nullptr, "count(//comment())");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(leptris_xpath_result_number(r), 3.0);
    leptris_xpath_result_free(r);

    r = leptris_xpath_eval(doc, nullptr, "count(//processing-instruction())");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(leptris_xpath_result_number(r), 3.0);
    leptris_xpath_result_free(r);

    leptris_document_free(doc);
}

TEST(DocumentChildren, SerializeKeepsOrderAfterQuery) {
    LeptrisDocument doc =
        leptris_parse_string(kDocLevel, std::strlen(kDocLevel), nullptr);
    ASSERT_NE(doc, nullptr);

    /* Force any lazy structure, then round-trip. */
    LeptrisXPathResult r =
        leptris_xpath_eval(doc, nullptr, "count(//comment())");
    ASSERT_NE(r, nullptr);
    leptris_xpath_result_free(r);

    char* s = leptris_document_serialize(doc, nullptr);
    ASSERT_NE(s, nullptr);
    std::string out(s);
    EXPECT_NE(out.find("<!-- pro --><?pp d?><r>"), std::string::npos)
        << "prolog order must be preserved: " << out;
    EXPECT_NE(out.find("</r><!-- epi --><?ep?>"), std::string::npos)
        << "epilog order must be preserved: " << out;
    leptris_free_string(s);
    leptris_document_free(doc);
}

TEST(DocumentChildren, AddPiIsVisibleInChainAndXpath) {
    LeptrisDocument doc =
        leptris_parse_string("<r/>", 4, nullptr);
    ASSERT_NE(doc, nullptr);
    LeptrisNodeRef added = leptris_document_add_pi(doc, "extra", "x");
    ASSERT_NE(added, nullptr);

    LeptrisXPathResult r =
        leptris_xpath_eval(doc, nullptr, "count(/processing-instruction())");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(leptris_xpath_result_number(r), 1.0);
    leptris_xpath_result_free(r);

    EXPECT_EQ(leptris_document_pi_count(doc), 1u);
    EXPECT_STREQ(leptris_document_pi_target(doc, 0), "extra");
    leptris_document_free(doc);
}

}  // namespace

/* Issue #612: parse-created doc-level PIs carry document linkage
 * (setters work); leptris_document_remove_pi unlinks by target or
 * index; set_root splices the new root into the chain. */
TEST(DocumentChildren, ParseCreatedPiHasDocumentLinkage) {
    const char xml[] = "<?pi x?><root/>";
    LeptrisDocument doc = leptris_parse_string(xml, std::strlen(xml), nullptr);
    ASSERT_NE(doc, nullptr);
    LeptrisNodeRef n = leptris_node_first_child(leptris_document_node(doc));
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(leptris_node_get_type(n), LEPTRIS_NODE_TYPE_PI);
    EXPECT_EQ(leptris_pi_node_set_target(n, "t"), LEPTRIS_OK);
    EXPECT_EQ(leptris_pi_node_set_data(n, "d"), LEPTRIS_OK);
    EXPECT_STREQ(leptris_pi_node_get_target(n), "t");
    leptris_document_free(doc);
}

TEST(DocumentChildren, RemovePiByTargetAndIndex) {
    const char xml[] = "<?a x?><?b y?><r/><?c z?><?a w?>";
    LeptrisDocument doc = leptris_parse_string(xml, std::strlen(xml), nullptr);
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(leptris_document_pi_count(doc), 4u);

    LeptrisNodeRef gone = leptris_document_remove_pi(doc, "b", 0);
    ASSERT_NE(gone, nullptr);
    EXPECT_EQ(leptris_document_pi_count(doc), 3u);
    /* Chain order preserved: a (head), root, c, a. */
    LeptrisNodeRef c = leptris_node_first_child(leptris_document_node(doc));
    EXPECT_EQ(leptris_node_get_type(c), LEPTRIS_NODE_TYPE_PI);

    /* index path targets the SECOND PI now ("c" after b removed? no —
     * remaining PIs: a, c, a → index 1 = c). */
    gone = leptris_document_remove_pi(doc, nullptr, 1);
    ASSERT_NE(gone, nullptr);
    EXPECT_STREQ(leptris_pi_node_get_target(gone), "c");
    EXPECT_EQ(leptris_document_pi_count(doc), 2u);
    leptris_document_free(doc);
}

TEST(DocumentChildren, SetRootSplicesChain) {
    LeptrisDocument doc = leptris_document_create();
    ASSERT_NE(doc, nullptr);
    LeptrisElement built = leptris_element_create(doc, "built");
    ASSERT_NE(built, nullptr);
    ASSERT_EQ(leptris_document_set_root(doc, built), LEPTRIS_OK);
    LeptrisNodeRef n = leptris_node_first_child(leptris_document_node(doc));
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(leptris_node_get_type(n), LEPTRIS_NODE_TYPE_ELEMENT);
    EXPECT_EQ((LeptrisElement)n, built);

    /* Replace: new root takes the old slot between prolog/epilog. */
    leptris_document_add_pi(doc, "pre", "v");
    LeptrisElement second = leptris_element_create(doc, "second");
    ASSERT_EQ(leptris_document_set_root(doc, second), LEPTRIS_OK);
    n = leptris_node_first_child(leptris_document_node(doc));
    EXPECT_EQ(leptris_node_get_type(n), LEPTRIS_NODE_TYPE_PI);  /* prolog */
    n = leptris_node_next_sibling(n);
    ASSERT_EQ((LeptrisElement)n, second);   /* new root at old slot */
    leptris_document_free(doc);
}

/* #1032: document-level comments parse and serialize (#578) but had
 * no writer — the add_pi twin appends at the epilog. */
TEST(DocumentChildren, AddCommentAppendsEpilogAndRoundTrips) {
    LeptrisStatus st = LEPTRIS_OK;
    const char xml[] = "<root/>";
    LeptrisDocument doc = leptris_parse_string(xml, std::strlen(xml), &st);
    ASSERT_NE(doc, nullptr);
    ASSERT_EQ(leptris_document_comment_count(doc), 0u);

    LeptrisNodeRef n = leptris_document_add_comment(doc, " tail ");
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(leptris_node_get_type(n), LEPTRIS_NODE_TYPE_COMMENT);
    EXPECT_EQ(leptris_document_comment_count(doc), 1u);
    EXPECT_STREQ(leptris_document_comment_content(doc, 0), " tail ");

    char* out = leptris_document_serialize(doc, nullptr);
    ASSERT_NE(out, nullptr);
    EXPECT_STREQ(out, "<root/><!-- tail -->") << out;
    leptris_free_string(out);
    leptris_document_free(doc);
}

TEST(DocumentChildren, AddCommentOnRootlessDocument) {
    LeptrisDocument doc = leptris_document_create();
    ASSERT_NE(doc, nullptr);
    LeptrisNodeRef n = leptris_document_add_comment(doc, "note");
    ASSERT_NE(n, nullptr);
    LeptrisNodeRef first = leptris_node_first_child(
        leptris_document_node(doc));
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first, n);
    EXPECT_EQ(leptris_document_comment_count(doc), 1u);

    LeptrisElement root = leptris_element_create(doc, "root");
    ASSERT_EQ(leptris_document_set_root(doc, root), LEPTRIS_OK);
    /* The comment was first in the chain — it stays in the PROLOG
     * slot ahead of the root after the splice. */
    char* out = leptris_document_serialize(doc, nullptr);
    ASSERT_NE(out, nullptr);
    EXPECT_STREQ(out, "<!--note--><root/>") << out;
    leptris_free_string(out);
    leptris_document_free(doc);
}
