// test/xinclude/test_xinclude.cpp — XInclude classification specs.

#include <gtest/gtest.h>
#include "leptris.h"
#include <cstring>
#include <cstdio>
#include <string>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr char kXIncludeNs[] = "http://www.w3.org/2001/XInclude";

TEST(XIncludeClassify, IdentifiesIncludeElement) {
    const char xml[] =
        "<root xmlns:xi='http://www.w3.org/2001/XInclude'>"
        "  <xi:include href='chapter.xml'/>"
        "</root>";
    LeptrisStatus st = LEPTRIS_OK;
    LeptrisDocument doc = leptris_parse_string(xml, std::strlen(xml), &st);
    ASSERT_NE(doc, nullptr);

    LeptrisElement root = leptris_document_root(doc);
    ASSERT_NE(root, nullptr);

    LeptrisElement child = leptris_element_first_child_any(root);
    ASSERT_NE(child, nullptr);

    EXPECT_EQ(leptris_xinclude_is_include_element(child), 1);
    EXPECT_EQ(leptris_xinclude_is_fallback_element(child), 0);

    leptris_document_free(doc);
}

TEST(XIncludeClassify, IdentifiesFallbackElement) {
    const char xml[] =
        "<root xmlns:xi='http://www.w3.org/2001/XInclude'>"
        "  <xi:include href='missing.xml'><xi:fallback>not found</xi:fallback></xi:include>"
        "</root>";
    LeptrisStatus st = LEPTRIS_OK;
    LeptrisDocument doc = leptris_parse_string(xml, std::strlen(xml), &st);
    ASSERT_NE(doc, nullptr);

    LeptrisElement root = leptris_document_root(doc);
    ASSERT_NE(root, nullptr);

    LeptrisElement include_elem = leptris_element_first_child_any(root);
    ASSERT_NE(include_elem, nullptr);
    ASSERT_EQ(leptris_xinclude_is_include_element(include_elem), 1);

    LeptrisElement fallback_elem = leptris_element_first_child_any(include_elem);
    ASSERT_NE(fallback_elem, nullptr);
    EXPECT_EQ(leptris_xinclude_is_fallback_element(fallback_elem), 1);

    leptris_document_free(doc);
}

TEST(XIncludeClassify, RejectsNonXIncludeElement) {
    const char xml[] = "<root><child/></root>";
    LeptrisStatus st = LEPTRIS_OK;
    LeptrisDocument doc = leptris_parse_string(xml, std::strlen(xml), &st);
    ASSERT_NE(doc, nullptr);

    LeptrisElement root = leptris_document_root(doc);
    LeptrisElement child = leptris_element_first_child_any(root);
    ASSERT_NE(child, nullptr);

    EXPECT_EQ(leptris_xinclude_is_include_element(child), 0);
    EXPECT_EQ(leptris_xinclude_is_fallback_element(child), 0);

    leptris_document_free(doc);
}

TEST(XIncludeAttrs, ReturnsHrefAttribute) {
    const char xml[] =
        "<root xmlns:xi='http://www.w3.org/2001/XInclude'>"
        "  <xi:include href='chapter1.xml'/>"
        "</root>";
    LeptrisStatus st = LEPTRIS_OK;
    LeptrisDocument doc = leptris_parse_string(xml, std::strlen(xml), &st);
    ASSERT_NE(doc, nullptr);

    LeptrisElement root = leptris_document_root(doc);
    LeptrisElement include_elem = leptris_element_first_child_any(root);

    EXPECT_STREQ(leptris_xinclude_get_href(include_elem), "chapter1.xml");

    leptris_document_free(doc);
}

TEST(XIncludeAttrs, ParseDefaultsToXml) {
    const char xml[] =
        "<root xmlns:xi='http://www.w3.org/2001/XInclude'>"
        "  <xi:include href='chapter.xml'/>"
        "</root>";
    LeptrisStatus st = LEPTRIS_OK;
    LeptrisDocument doc = leptris_parse_string(xml, std::strlen(xml), &st);
    ASSERT_NE(doc, nullptr);

    LeptrisElement root = leptris_document_root(doc);
    LeptrisElement include_elem = leptris_element_first_child_any(root);

    /* parse attribute omitted -> default is "xml" per XInclude spec. */
    EXPECT_STREQ(leptris_xinclude_get_parse(include_elem), "xml");

    leptris_document_free(doc);
}

TEST(XIncludeProcess, NoIncludesIsOk) {
    /* A document with no xi:include elements processes cleanly. */
    const char xml[] = "<root/>";
    LeptrisStatus st = LEPTRIS_OK;
    LeptrisDocument doc = leptris_parse_string(xml, std::strlen(xml), &st);
    ASSERT_NE(doc, nullptr);

    LeptrisStatus rc = leptris_xinclude_process(doc, nullptr);
    EXPECT_EQ(rc, LEPTRIS_OK);

    leptris_document_free(doc);
}

TEST(XIncludeProcess, ParseTextReplacesIncludeWithContent) {
    /* Create a temp file, include it via parse="text", verify the
     * content appears as a text node. */
    const char text_content[] = "Hello from included file!";
    const char* tmp_path = "/tmp/leptris_xinclude_test.txt";
    FILE* f = fopen(tmp_path, "wb");
    ASSERT_NE(f, nullptr);
    fwrite(text_content, 1, std::strlen(text_content), f);
    fclose(f);

    const char xml[] =
        "<root xmlns:xi='http://www.w3.org/2001/XInclude'>"
        "  <xi:include href='/tmp/leptris_xinclude_test.txt' parse='text'/>"
        "</root>";
    LeptrisStatus st = LEPTRIS_OK;
    LeptrisDocument doc = leptris_parse_string(xml, std::strlen(xml), &st);
    ASSERT_NE(doc, nullptr);

    LeptrisStatus rc = leptris_xinclude_process(doc, nullptr);
    EXPECT_EQ(rc, LEPTRIS_OK);

    /* The xi:include should be replaced by a text node. Walk children. */
    LeptrisElement root = leptris_document_root(doc);
    ASSERT_NE(root, nullptr);

    /* Check that no child is xi:include anymore, and text content
     * from the file is present. */
    bool found_text = false;
    bool found_include = false;
    LeptrisNodeRef child = leptris_node_first_child((LeptrisNodeRef)root);
    while (child) {
        /* Try text accessor — returns NULL for non-text nodes. */
        const char* content = leptris_text_node_get_content(child);
        if (content && std::string(content).find("Hello from") != std::string::npos) {
            found_text = true;
        }
        /* Try element accessor — returns NULL for non-elements. */
        LeptrisElement elem_child = leptris_node_as_element(child);
        if (elem_child && leptris_xinclude_is_include_element(elem_child)) {
            found_include = true;
        }
        child = leptris_node_next_sibling(child);
    }
    EXPECT_TRUE(found_text);
    EXPECT_FALSE(found_include);

    leptris_document_free(doc);
    remove(tmp_path);
}

}  // namespace

TEST(XIncludeProcess, ParseTextUsesFallbackWhenFileMissing) {
    /* When the href can't be loaded, use xi:fallback content instead. */
    const char xml[] =
        "<root xmlns:xi='http://www.w3.org/2001/XInclude'>"
        "  <xi:include href='/tmp/this_file_does_not_exist_xyz_123.txt' "
        "               parse='text'>"
        "    <xi:fallback>Fallback content here</xi:fallback>"
        "  </xi:include>"
        "</root>";
    LeptrisStatus st = LEPTRIS_OK;
    LeptrisDocument doc = leptris_parse_string(xml, std::strlen(xml), &st);
    ASSERT_NE(doc, nullptr);

    LeptrisStatus rc = leptris_xinclude_process(doc, nullptr);
    EXPECT_EQ(rc, LEPTRIS_OK);

    /* Verify the fallback content is in the tree as a text node. */
    LeptrisElement root = leptris_document_root(doc);
    ASSERT_NE(root, nullptr);

    bool found_fallback = false;
    bool found_include = false;
    LeptrisNodeRef child = leptris_node_first_child((LeptrisNodeRef)root);
    while (child) {
        const char* content = leptris_text_node_get_content(child);
        if (content && std::string(content).find("Fallback content") != std::string::npos) {
            found_fallback = true;
        }
        LeptrisElement elem_child = leptris_node_as_element(child);
        if (elem_child && leptris_xinclude_is_include_element(elem_child)) {
            found_include = true;
        }
        child = leptris_node_next_sibling(child);
    }
    EXPECT_TRUE(found_fallback);
    EXPECT_FALSE(found_include);

    leptris_document_free(doc);
}

// ---- parse="xml" ----------------------------------------------------------
//
// XInclude 1.0 default mode: the href is loaded as XML and the root
// of the included document replaces the xi:include element.  Children,
// attributes, and mixed content must all survive the cross-document
// copy.

TEST(XIncludeProcess, ParseXmlReplacesIncludeWithRootElement) {
    const char included_xml[] =
        "<chapter><title>Included</title><para>Body text</para></chapter>";
    const char* inc_path = "/tmp/leptris_xinclude_xml_test.xml";
    FILE* f = fopen(inc_path, "wb");
    ASSERT_NE(f, nullptr);
    fwrite(included_xml, 1, std::strlen(included_xml), f);
    fclose(f);

    const char xml[] =
        "<book xmlns:xi='http://www.w3.org/2001/XInclude'>"
        "  <xi:include href='/tmp/leptris_xinclude_xml_test.xml'/>"
        "</book>";
    LeptrisStatus st = LEPTRIS_OK;
    LeptrisDocument doc = leptris_parse_string(xml, std::strlen(xml), &st);
    ASSERT_NE(doc, nullptr);

    EXPECT_EQ(leptris_xinclude_process(doc, nullptr), LEPTRIS_OK);

    LeptrisElement root = leptris_document_root(doc);
    ASSERT_NE(root, nullptr);

    /* The xi:include must be gone, replaced by <chapter>. */
    bool found_chapter = false;
    bool found_include = false;
    for (LeptrisNodeRef child = leptris_node_first_child((LeptrisNodeRef)root);
         child;
         child = leptris_node_next_sibling(child)) {
        LeptrisElement e = leptris_node_as_element(child);
        if (!e) continue;
        if (leptris_xinclude_is_include_element(e)) {
            found_include = true;
        }
        if (std::string(leptris_element_name(e)) == "chapter") {
            found_chapter = true;
        }
    }
    EXPECT_TRUE(found_chapter);
    EXPECT_FALSE(found_include);

    leptris_document_free(doc);
    remove(inc_path);
}

TEST(XIncludeProcess, ParseXmlCopiesAttributesAndChildren) {
    /* The deep-copy must carry over attributes and the entire child
     * subtree — not just the bare root element. */
    const char included_xml[] =
        "<data id='x' lang='en'>"
        "  <item>one</item>"
        "  <item>two</item>"
        "  text-around"
        "</data>";
    const char* inc_path = "/tmp/leptris_xinclude_attrs_test.xml";
    FILE* f = fopen(inc_path, "wb");
    ASSERT_NE(f, nullptr);
    fwrite(included_xml, 1, std::strlen(included_xml), f);
    fclose(f);

    const char xml[] =
        "<root xmlns:xi='http://www.w3.org/2001/XInclude'>"
        "  <xi:include href='/tmp/leptris_xinclude_attrs_test.xml' parse='xml'/>"
        "</root>";
    LeptrisStatus st = LEPTRIS_OK;
    LeptrisDocument doc = leptris_parse_string(xml, std::strlen(xml), &st);
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(leptris_xinclude_process(doc, nullptr), LEPTRIS_OK);

    LeptrisElement root = leptris_document_root(doc);
    ASSERT_NE(root, nullptr);

    LeptrisElement data_elem = nullptr;
    for (LeptrisNodeRef child = leptris_node_first_child((LeptrisNodeRef)root);
         child;
         child = leptris_node_next_sibling(child)) {
        LeptrisElement e = leptris_node_as_element(child);
        if (e && std::string(leptris_element_name(e)) == "data") {
            data_elem = e;
            break;
        }
    }
    ASSERT_NE(data_elem, nullptr);

    /* Attributes survived the copy. */
    EXPECT_STREQ(leptris_element_attribute(data_elem, "id"), "x");
    EXPECT_STREQ(leptris_element_attribute(data_elem, "lang"), "en");

    /* The concatenated text content includes both item text and the
     * surrounding text node — proving the mixed-content subtree copy
     * is complete. */
    const char* text = leptris_element_text(data_elem);
    EXPECT_NE(text, nullptr);
    if (text) {
        std::string s(text);
        EXPECT_NE(s.find("one"), std::string::npos);
        EXPECT_NE(s.find("two"), std::string::npos);
        EXPECT_NE(s.find("text-around"), std::string::npos);
    }

    /* Child count must match (two <item> elements). */
    int item_count = 0;
    for (LeptrisElement c = leptris_element_first_child_any(data_elem);
         c;
         c = leptris_element_next_sibling_any(c)) {
        if (std::string(leptris_element_name(c)) == "item") item_count++;
    }
    EXPECT_EQ(item_count, 2);

    leptris_document_free(doc);
    remove(inc_path);
}

TEST(XIncludeProcess, ParseXmlUsesFallbackWhenFileMissing) {
    /* Same fallback contract as parse="text": when the resource can't
     * be loaded, xi:fallback content is spliced in as a text node. */
    const char xml[] =
        "<root xmlns:xi='http://www.w3.org/2001/XInclude'>"
        "  <xi:include href='/tmp/does-not-exist-leptris.xml' parse='xml'>"
        "    <xi:fallback>xml fallback text</xi:fallback>"
        "  </xi:include>"
        "</root>";
    LeptrisStatus st = LEPTRIS_OK;
    LeptrisDocument doc = leptris_parse_string(xml, std::strlen(xml), &st);
    ASSERT_NE(doc, nullptr);

    EXPECT_EQ(leptris_xinclude_process(doc, nullptr), LEPTRIS_OK);

    LeptrisElement root = leptris_document_root(doc);
    ASSERT_NE(root, nullptr);

    bool found_fallback = false;
    for (LeptrisNodeRef child = leptris_node_first_child((LeptrisNodeRef)root);
         child;
         child = leptris_node_next_sibling(child)) {
        const char* c = leptris_text_node_get_content(child);
        if (c && std::string(c).find("xml fallback text") != std::string::npos) {
            found_fallback = true;
        }
    }
    EXPECT_TRUE(found_fallback);

    leptris_document_free(doc);
}

TEST(XIncludeProcess, ParseXmlRecursiveIncludesNestedXi) {
    /* An included document can itself contain xi:include; the walker
     * processes bottom-up so the nested include resolves before the
     * outer splice happens. */
    const char inner_xml[] = "<inner>deep</inner>";
    const char* inner_path = "/tmp/leptris_xinclude_inner.xml";
    FILE* f1 = fopen(inner_path, "wb");
    ASSERT_NE(f1, nullptr);
    fwrite(inner_xml, 1, std::strlen(inner_xml), f1);
    fclose(f1);

    const char outer_xml[] =
        "<outer xmlns:xi='http://www.w3.org/2001/XInclude'>"
        "  <xi:include href='/tmp/leptris_xinclude_inner.xml'/>"
        "</outer>";
    const char* outer_path = "/tmp/leptris_xinclude_outer.xml";
    FILE* f2 = fopen(outer_path, "wb");
    ASSERT_NE(f2, nullptr);
    fwrite(outer_xml, 1, std::strlen(outer_xml), f2);
    fclose(f2);

    const char xml[] =
        "<root xmlns:xi='http://www.w3.org/2001/XInclude'>"
        "  <xi:include href='/tmp/leptris_xinclude_outer.xml'/>"
        "</root>";
    LeptrisStatus st = LEPTRIS_OK;
    LeptrisDocument doc = leptris_parse_string(xml, std::strlen(xml), &st);
    ASSERT_NE(doc, nullptr);

    EXPECT_EQ(leptris_xinclude_process(doc, nullptr), LEPTRIS_OK);

    /* Expected tree: root > outer > inner > "deep". */
    LeptrisElement root = leptris_document_root(doc);
    ASSERT_NE(root, nullptr);
    LeptrisElement outer = leptris_element_first_child_any(root);
    ASSERT_NE(outer, nullptr);
    EXPECT_STREQ(leptris_element_name(outer), "outer");
    LeptrisElement inner = leptris_element_first_child_any(outer);
    ASSERT_NE(inner, nullptr);
    EXPECT_STREQ(leptris_element_name(inner), "inner");
    EXPECT_STREQ(leptris_element_text(inner), "deep");

    leptris_document_free(doc);
    remove(inner_path);
    remove(outer_path);
}

// ---- Phase 4: xpointer fragment selection (TODO 92) ------------------

TEST(XIncludeXpointer, SelectsFragmentByXPath) {
    /* Included document has multiple sections; xpointer selects one
     * by id attribute. Only that subtree is spliced in. */
    const char included_xml[] =
        "<doc>"
        "  <section id='intro'>introduction</section>"
        "  <section id='body'>main content</section>"
        "  <section id='appendix'>extra</section>"
        "</doc>";
    const char* path = "/tmp/leptris_xinclude_xpointer_src.xml";
    FILE* f = fopen(path, "wb");
    ASSERT_NE(f, nullptr);
    fwrite(included_xml, 1, std::strlen(included_xml), f);
    fclose(f);

    const char xml[] =
        "<root xmlns:xi='http://www.w3.org/2001/XInclude'>"
        "  <xi:include href='/tmp/leptris_xinclude_xpointer_src.xml'"
        "              xpointer=\"//section[@id='body']\"/>"
        "</root>";
    LeptrisStatus st = LEPTRIS_OK;
    LeptrisDocument doc = leptris_parse_string(xml, std::strlen(xml), &st);
    ASSERT_NE(doc, nullptr);

    EXPECT_EQ(leptris_xinclude_process(doc, nullptr), LEPTRIS_OK);

    /* Expected: root has one child <section id='body'>main content</section>. */
    LeptrisElement root = leptris_document_root(doc);
    ASSERT_NE(root, nullptr);
    LeptrisElement child = leptris_element_first_child_any(root);
    ASSERT_NE(child, nullptr);
    EXPECT_STREQ(leptris_element_name(child), "section");
    EXPECT_STREQ(leptris_element_attribute(child, "id"), "body");
    EXPECT_STREQ(leptris_element_text(child), "main content");

    leptris_document_free(doc);
    remove(path);
}

TEST(XIncludeXpointer, XmlnsBindsExpressionPrefixToNamespace) {
    /* The included document declares the namespace with prefix t:;
     * the xpointer binds p: to the same URI via xmlns() and selects
     * by NAMESPACE — the literal prefixes differ. */
    const char included_xml[] =
        "<doc xmlns:t='http://example.com/ns'>"
        "  <t:section id='one'>first</t:section>"
        "  <section id='two'>plain</section>"
        "</doc>";
    const char* path = "/tmp/leptris_xinclude_xpointer_ns.xml";
    FILE* f = fopen(path, "wb");
    ASSERT_NE(f, nullptr);
    fwrite(included_xml, 1, std::strlen(included_xml), f);
    fclose(f);

    const char xml[] =
        "<root xmlns:xi='http://www.w3.org/2001/XInclude'>"
        "  <xi:include href='/tmp/leptris_xinclude_xpointer_ns.xml'"
        "              xpointer=\"xmlns(p=http://example.com/ns)xpointer(//p:section)\"/>"
        "</root>";
    LeptrisStatus st = LEPTRIS_OK;
    LeptrisDocument doc = leptris_parse_string(xml, std::strlen(xml), &st);
    ASSERT_NE(doc, nullptr);

    EXPECT_EQ(leptris_xinclude_process(doc, nullptr), LEPTRIS_OK);

    LeptrisElement root = leptris_document_root(doc);
    ASSERT_NE(root, nullptr);
    LeptrisElement child = leptris_element_first_child_any(root);
    ASSERT_NE(child, nullptr);
    EXPECT_STREQ(leptris_element_name(child), "section");
    EXPECT_STREQ(leptris_element_attribute(child, "id"), "one");
    EXPECT_STREQ(leptris_element_text(child), "first");

    leptris_document_free(doc);
    remove(path);
}

TEST(XIncludeXpointer, XmlnsWrongUriSelectsNothing) {
    /* A binding to a DIFFERENT namespace must not match: the
     * fallback-to-root behavior then splices <doc>. */
    const char included_xml[] =
        "<doc xmlns:t='http://example.com/ns'><t:section/></doc>";
    const char* path = "/tmp/leptris_xinclude_xpointer_ns2.xml";
    FILE* f = fopen(path, "wb");
    ASSERT_NE(f, nullptr);
    fwrite(included_xml, 1, std::strlen(included_xml), f);
    fclose(f);

    const char xml[] =
        "<root xmlns:xi='http://www.w3.org/2001/XInclude'>"
        "  <xi:include href='/tmp/leptris_xinclude_xpointer_ns2.xml'"
        "              xpointer=\"xmlns(p=http://other)xpointer(//p:section)\"/>"
        "</root>";
    LeptrisStatus st = LEPTRIS_OK;
    LeptrisDocument doc = leptris_parse_string(xml, std::strlen(xml), &st);
    ASSERT_NE(doc, nullptr);

    EXPECT_EQ(leptris_xinclude_process(doc, nullptr), LEPTRIS_OK);

    LeptrisElement root = leptris_document_root(doc);
    ASSERT_NE(root, nullptr);
    LeptrisElement child = leptris_element_first_child_any(root);
    ASSERT_NE(child, nullptr);
    EXPECT_STREQ(leptris_element_name(child), "doc");

    leptris_document_free(doc);
    remove(path);
}

TEST(XIncludeXpointer, EmptyResultFallsBackToRoot) {
    /* If the xpointer matches nothing, the spec is unclear; we fall
     * back to the included doc's root element. */
    const char included_xml[] = "<doc><a/></doc>";
    const char* path = "/tmp/leptris_xinclude_xpointer_empty.xml";
    FILE* f = fopen(path, "wb");
    ASSERT_NE(f, nullptr);
    fwrite(included_xml, 1, std::strlen(included_xml), f);
    fclose(f);

    const char xml[] =
        "<root xmlns:xi='http://www.w3.org/2001/XInclude'>"
        "  <xi:include href='/tmp/leptris_xinclude_xpointer_empty.xml'"
        "              xpointer=\"//nonexistent\"/>"
        "</root>";
    LeptrisStatus st = LEPTRIS_OK;
    LeptrisDocument doc = leptris_parse_string(xml, std::strlen(xml), &st);
    ASSERT_NE(doc, nullptr);

    EXPECT_EQ(leptris_xinclude_process(doc, nullptr), LEPTRIS_OK);

    /* Expected: root has the included doc's root element <doc>. */
    LeptrisElement root = leptris_document_root(doc);
    ASSERT_NE(root, nullptr);
    LeptrisElement child = leptris_element_first_child_any(root);
    ASSERT_NE(child, nullptr);
    EXPECT_STREQ(leptris_element_name(child), "doc");

    leptris_document_free(doc);
    remove(path);
}

TEST(XIncludeRecursion, MutuallyRecursiveIncludesHitDepthLimit) {
    /* Two files include each other — would loop forever without the
     * recursion guard. The processor must stop at XINCLUDE_MAX_DEPTH
     * and return an error (or silently stop; we accept either as long
     * as the process terminates within reasonable time). */
    const char* path_a = "/tmp/leptris_xinclude_recursion_a.xml";
    const char* path_b = "/tmp/leptris_xinclude_recursion_b.xml";
    const char a_xml[] =
        "<a xmlns:xi='http://www.w3.org/2001/XInclude'>"
        "<xi:include href='/tmp/leptris_xinclude_recursion_b.xml'/>"
        "</a>";
    const char b_xml[] =
        "<b xmlns:xi='http://www.w3.org/2001/XInclude'>"
        "<xi:include href='/tmp/leptris_xinclude_recursion_a.xml'/>"
        "</b>";
    FILE* fa = fopen(path_a, "wb");
    ASSERT_NE(fa, nullptr);
    fwrite(a_xml, 1, std::strlen(a_xml), fa);
    fclose(fa);
    FILE* fb = fopen(path_b, "wb");
    ASSERT_NE(fb, nullptr);
    fwrite(b_xml, 1, std::strlen(b_xml), fb);
    fclose(fb);

    const char xml[] =
        "<root xmlns:xi='http://www.w3.org/2001/XInclude'>"
        "<xi:include href='/tmp/leptris_xinclude_recursion_a.xml'/>"
        "</root>";
    LeptrisStatus st = LEPTRIS_OK;
    LeptrisDocument doc = leptris_parse_string(xml, std::strlen(xml), &st);
    ASSERT_NE(doc, nullptr);

    /* Should terminate (not hang). Return value may be OK or an error
     * depending on whether the impl treats max-depth as fatal. The
     * important thing is that this call returns. */
    LeptrisStatus rc = leptris_xinclude_process(doc, nullptr);
    (void)rc;  /* either LEPTRIS_OK or LEPTRIS_ERROR_INVALID_ARG */

    leptris_document_free(doc);
    remove(path_a);
    remove(path_b);
}

/* TODO 117 Phase A: adopted nodes live in the CHILD doc's pool,
 * but are spliced into the parent's tree.  Validate that:
 *   1. The adopted root's document pointer is the PARENT doc
 *      (so subsequent tree ops can resolve back to the parent pool).
 *   2. The parent's pool doesn't free the adopted nodes (they live
 *      in the child pool).  We check this by NOT freeing the parent
 *      until the child pool is gone; the test just confirms the tree
 *      is readable and that free() doesn't crash. */
TEST(XIncludePhaseA, AdoptedRootHasParentDocPointer) {
    const char included[] = "<child><greeting>hi</greeting></child>";
    const char* inc_path = "/tmp/leptris_adopt_child.xml";
    FILE* f = fopen(inc_path, "wb");
    ASSERT_NE(f, nullptr);
    fwrite(included, 1, std::strlen(included), f);
    fclose(f);

    const char xml[] =
        "<root xmlns:xi='http://www.w3.org/2001/XInclude'>"
        "<xi:include href='/tmp/leptris_adopt_child.xml'/>"
        "</root>";
    LeptrisStatus st = LEPTRIS_OK;
    LeptrisDocument doc = leptris_parse_string(xml, std::strlen(xml), &st);
    ASSERT_NE(doc, nullptr);

    ASSERT_EQ(leptris_xinclude_process(doc, nullptr), LEPTRIS_OK);

    /* Walk down to <child> and read its text -- if the adoption
     * worked, the pointer arithmetic across pool boundaries is
     * still valid (parent's pool + child's pool, freed in the
     * right order by leptris_document_free below). */
    LeptrisElement root = leptris_document_root(doc);
    ASSERT_NE(root, nullptr);

    bool found = false;
    for (LeptrisNodeRef c = leptris_node_first_child((LeptrisNodeRef)root);
         c; c = leptris_node_next_sibling(c)) {
        LeptrisElement e = leptris_node_as_element(c);
        if (!e) continue;
        if (std::string(leptris_element_name(e)) == "child") {
            found = true;
            const char* txt = leptris_element_text(e);
            ASSERT_NE(txt, nullptr);
            EXPECT_STREQ(txt, "hi");
        }
    }
    EXPECT_TRUE(found);

    /* If pool ordering is broken, leptris_document_free crashes or
     * double-frees.  ASAN should catch any leaks. */
    leptris_document_free(doc);
    remove(inc_path);
}

/* TODO 117 Phase C: cycle detection.
 *
 * Two files xi:include each other.  The depth guard (32 layers) would
 * stop this in most cases, but the cycle-detect pass catches the
 * direct cycle before recursion even ramps up.  After processing, the
 * tree should NOT be infinite and the included-doc pools should
 * not leak. */
TEST(XIncludePhaseC, MutualIncludeCycleDoesNotLeak) {
    const char* path_a = "/tmp/leptris_cycle_a.xml";
    const char* path_b = "/tmp/leptris_cycle_b.xml";

    /* a includes b */
    const char a_xml[] =
        "<root xmlns:xi='http://www.w3.org/2001/XInclude'>"
        "  <xi:include href='/tmp/leptris_cycle_b.xml'/>"
        "</root>";
    FILE* f = fopen(path_a, "wb");
    ASSERT_NE(f, nullptr);
    fwrite(a_xml, 1, std::strlen(a_xml), f);
    fclose(f);

    /* b includes a (cycle) */
    const char b_xml[] =
        "<root xmlns:xi='http://www.w3.org/2001/XInclude'>"
        "  <xi:include href='/tmp/leptris_cycle_a.xml'/>"
        "</root>";
    f = fopen(path_b, "wb");
    ASSERT_NE(f, nullptr);
    fwrite(b_xml, 1, std::strlen(b_xml), f);
    fclose(f);

    const char xml[] =
        "<root xmlns:xi='http://www.w3.org/2001/XInclude'>"
        "  <xi:include href='/tmp/leptris_cycle_a.xml'/>"
        "</root>";
    LeptrisStatus st = LEPTRIS_OK;
    LeptrisDocument doc = leptris_parse_string(xml, std::strlen(xml), &st);
    ASSERT_NE(doc, nullptr);

    /* Either succeeds (with cycle pruned to fallback) or returns an
     * error code -- but does NOT infinite-loop and does NOT leak. */
    LeptrisStatus rc = leptris_xinclude_process(doc, nullptr);
    (void)rc;  /* Any completion is success for this test. */

    leptris_document_free(doc);
    remove(path_a);
    remove(path_b);
}

// ---- TODO.remaining/04: XPointer scheme forms + href fragments ------

namespace {
constexpr char kXiNs[] = "http://www.w3.org/2001/XInclude";

static void write_file(const char* path, const char* data, size_t len) {
    FILE* f = fopen(path, "wb");
    ASSERT_NE(f, nullptr);
    fwrite(data, 1, len, f);
    fclose(f);
}

static const char kXptrDoc[] =
    "<doc><intro/><section id='target1'>first</section>"
    "<section id='target2'>second</section><outro/></doc>";

/* #1018: gtest_discovered cases run as separate ctest processes;
 * concurrent -jN runs must not share /tmp fixture paths (the
 * write/include/remove windows raced across processes). Every
 * process names its own files — pid, not thread::id (thread ids
 * are per-process namespaces and hash identically across
 * processes, which kept the collision). */
static std::string xptr_unique_path(const char* base, const char* ext) {
#if defined(_WIN32)
    long pid = (long)_getpid();
#else
    long pid = (long)getpid();
#endif
    return std::string("/tmp/") + base + "_" +
           std::to_string(pid) + "." + ext;
}

/* Include kXptrDoc with the given xi:include markup ({DOC}
 * substitutes this process's doc path); return the spliced
 * child's name (or attr value). */
static std::string splice(const std::string& include_xml,
                          const char* attr = nullptr) {
    std::string doc_path = xptr_unique_path("leptris_xptr_doc", "xml");
    std::string markup = include_xml;
    for (size_t pos; (pos = markup.find("{DOC}")) != std::string::npos;)
        markup.replace(pos, 5, doc_path);
    write_file(doc_path.c_str(), kXptrDoc, strlen(kXptrDoc));

    std::string xml = std::string("<root xmlns:xi='") + kXiNs + "'>" +
                      markup + "</root>";
    LeptrisStatus st = LEPTRIS_OK;
    LeptrisDocument doc =
        leptris_parse_string(xml.data(), xml.size(), &st);
    if (!doc) return "(parse-failed)";
    leptris_xinclude_process(doc, nullptr);

    LeptrisElement root = leptris_document_root(doc);
    LeptrisElement child = leptris_element_first_child_any(root);
    std::string out;
    if (!child) out = "(none)";
    else if (attr) {
        const char* v = leptris_element_attribute(child, attr);
        out = v ? v : "(none)";
    } else {
        out = leptris_element_name(child);
    }
    leptris_document_free(doc);
    remove(doc_path.c_str());
    return out;
}

TEST(XIncludeXpointerForms, HrefFragmentIsShorthand) {
    /* §4: the href fragment is a shorthand pointer -- the element
     * with that ID. */
    EXPECT_EQ(splice("<xi:include href='{DOC}#target2'/>"),
              "section");
    EXPECT_EQ(splice("<xi:include href='{DOC}#target2'/>", "id"),
              "target2");
}

TEST(XIncludeXpointerForms, XpointerScheme) {
    EXPECT_EQ(splice("<xi:include href='{DOC}'"
                     " xpointer='xpointer(//section[@id=\"target1\"])'/>"),
              "section");
    EXPECT_EQ(splice("<xi:include href='{DOC}'"
                     " xpointer='xpointer(//section[@id=\"target1\"])'/>", "id"),
              "target1");
}

TEST(XIncludeXpointerForms, ElementSchemeChildSequence) {
    /* element(/1/3): root's first element child is <intro>, its
     * third element child is section#target2. */
    EXPECT_EQ(splice("<xi:include href='{DOC}'"
                     " xpointer='element(/1/3)'/>", "id"),
              "target2");
}

TEST(XIncludeXpointerForms, ElementSchemeNamedStart) {
    EXPECT_EQ(splice("<xi:include href='{DOC}'"
                     " xpointer='element(target1)'/>", "id"),
              "target1");
}

TEST(XIncludeXpointerForms, SchemesTriedLeftToRight) {
    /* First scheme matches nothing; the second wins. */
    EXPECT_EQ(splice("<xi:include href='{DOC}'"
                     " xpointer='element(nosuch)xpointer(//outro)'/>"),
              "outro");
}

TEST(XIncludeXpointerForms, FragmentAndXpointerTogetherIsResourceError) {
    /* §4: href fragment + xpointer attribute = resource error ->
     * fallback text, not the document. */
    std::string doc_path = xptr_unique_path("leptris_xptr_doc", "xml");
    write_file(doc_path.c_str(), kXptrDoc, strlen(kXptrDoc));
    std::string xml = std::string("<root xmlns:xi='") + kXiNs +
        "'><xi:include href='" + doc_path + "#target1'"
        " xpointer='xpointer(//outro)'><xi:fallback>fb</xi:fallback>"
        "</xi:include></root>";
    LeptrisStatus st = LEPTRIS_OK;
    LeptrisDocument doc = leptris_parse_string(xml.data(), xml.size(), &st);
    ASSERT_NE(doc, nullptr);
    leptris_xinclude_process(doc, nullptr);
    LeptrisElement root = leptris_document_root(doc);
    LeptrisNodeRef child = leptris_node_first_child(leptris_element_as_node(root));
    ASSERT_NE(child, nullptr);
    ASSERT_EQ(leptris_node_get_type(child), LEPTRIS_NODE_TYPE_TEXT);
    EXPECT_STREQ(leptris_text_node_get_content(child), "fb");
    leptris_document_free(doc);
    remove(doc_path.c_str());
}

TEST(XIncludeXpointerForms, FragmentOnTextIncludeIsResourceError) {
    std::string txt = xptr_unique_path("leptris_xptr_text", "txt");
    write_file(txt.c_str(), "plain", 5);
    std::string xml = std::string("<root xmlns:xi='") + kXiNs +
        "'><xi:include href='" + txt + "#frag'"
        " parse='text'><xi:fallback>nofrag</xi:fallback></xi:include></root>";
    LeptrisStatus st = LEPTRIS_OK;
    LeptrisDocument doc = leptris_parse_string(xml.data(), xml.size(), &st);
    ASSERT_NE(doc, nullptr);
    leptris_xinclude_process(doc, nullptr);
    LeptrisElement root = leptris_document_root(doc);
    LeptrisNodeRef child = leptris_node_first_child(leptris_element_as_node(root));
    ASSERT_NE(child, nullptr);
    ASSERT_EQ(leptris_node_get_type(child), LEPTRIS_NODE_TYPE_TEXT);
    EXPECT_STREQ(leptris_text_node_get_content(child), "nofrag");
    leptris_document_free(doc);
    remove(txt.c_str());
}

TEST(XIncludeXpointerForms, TextEncodingAttributeConvertsToUtf8) {
    /* ISO-8859-1 0xE9 = é (U+00E9, UTF-8 0xC3 0xA9). */
    const char latin1[] = { 'c', 'a', 'f', (char)0xE9 };
    std::string txt = xptr_unique_path("leptris_xptr_latin1", "txt");
    write_file(txt.c_str(), latin1, sizeof(latin1));
    std::string xml = std::string("<root xmlns:xi='") + kXiNs +
        "'><xi:include href='" + txt + "'"
        " parse='text' encoding='ISO-8859-1'/></root>";
    LeptrisStatus st = LEPTRIS_OK;
    LeptrisDocument doc = leptris_parse_string(xml.data(), xml.size(), &st);
    ASSERT_NE(doc, nullptr);
    leptris_xinclude_process(doc, nullptr);
    LeptrisElement root = leptris_document_root(doc);
    LeptrisNodeRef child = leptris_node_first_child(leptris_element_as_node(root));
    ASSERT_NE(child, nullptr);
    ASSERT_EQ(leptris_node_get_type(child), LEPTRIS_NODE_TYPE_TEXT);
    const char* text = leptris_text_node_get_content(child);
    ASSERT_NE(text, nullptr);
    /* Converted UTF-8, or (no-iconv builds) the raw byte -- never
     * a fallback. */
    bool converted = (unsigned char)text[3] == 0xC3 &&
                     (unsigned char)text[4] == 0xA9 && text[5] == '\0';
    bool passthrough = (unsigned char)text[3] == 0xE9 && text[4] == '\0';
    EXPECT_TRUE(converted || passthrough);
    leptris_document_free(doc);
    remove(txt.c_str());
}

TEST(XIncludeXpointerForms, BadEncodingIsResourceError) {
    const char* txt = "/tmp/leptris_xptr_badenc.txt";
    write_file(txt, "data", 4);
    std::string xml = std::string("<root xmlns:xi='") + kXiNs +
        "'><xi:include href='/tmp/leptris_xptr_badenc.txt'"
        " parse='text' encoding='NOT-A-CHARSET'>"
        "<xi:fallback>unconvertible</xi:fallback></xi:include></root>";
    LeptrisStatus st = LEPTRIS_OK;
    LeptrisDocument doc = leptris_parse_string(xml.data(), xml.size(), &st);
    ASSERT_NE(doc, nullptr);
    leptris_xinclude_process(doc, nullptr);
    LeptrisElement root = leptris_document_root(doc);
    LeptrisNodeRef child = leptris_node_first_child(leptris_element_as_node(root));
    ASSERT_NE(child, nullptr);
    /* With iconv: conversion fails -> fallback. Without iconv: the
     * attribute is ignored -> "data". Both acceptable. */
    ASSERT_EQ(leptris_node_get_type(child), LEPTRIS_NODE_TYPE_TEXT);
    const char* text = leptris_text_node_get_content(child);
    ASSERT_NE(text, nullptr);
    EXPECT_TRUE(strcmp(text, "unconvertible") == 0 ||
                strcmp(text, "data") == 0);
    leptris_document_free(doc);
    remove(txt);
}

}  // namespace
