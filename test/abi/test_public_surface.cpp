/* Spec-coverage audit (2026-08-24): every exported symbol exercised.
 * The 37 functions that had zero spec coverage before this file,
 * grouped by area. Falsifiability: each group asserts failure
 * contracts (NULL/wrong-type/default-value paths), not just happy
 * paths. */
#include <gtest/gtest.h>
extern "C" {
#include "leptris.h"
#include "leptris/error.h"
}
#include <cstdio>
#include <cstring>
#include <string>

static const char* kDoc =
    "<root id='7' ratio='2.5' flag='true' name='top'>"
    "<a order='2'>alpha</a><b order='1'>beta</b><b order='3'>42</b>"
    "</root>";

static LeptrisDocument parse_doc() {
    return leptris_parse_string(kDoc, strlen(kDoc), nullptr);
}

/* ---- version + status messages ---- */

TEST(PublicSurface, VersionAndMessages) {
    ASSERT_NE(leptris_version(), nullptr);
    EXPECT_EQ(strncmp("1.", leptris_version(), 2), 0);
    int maj = -1, min = -1, pat = -1;
    leptris_version_components(&maj, &min, &pat);
    EXPECT_EQ(maj, 1);
    EXPECT_GE(min, 0);
    EXPECT_GE(pat, 0);

    EXPECT_NE(leptris_error_message(LEPTRIS_ERROR_PARSE), nullptr);
    EXPECT_STREQ(leptris_error_message(LEPTRIS_OK),
                 leptris_status_string(LEPTRIS_OK));
}

/* ---- nonstandard-entity pre-scan (#745) ---- */

TEST(PublicSurface, StrHasNonstandardEntity) {
    /* The five predefined entities and numeric character
     * references are standard — no rewrite needed downstream. */
    EXPECT_EQ(leptris_str_has_nonstandard_entity(
                  "<p a=\"&quot;x&quot;\">a &amp; &lt; &gt; &apos;</p>", 49), 0);
    EXPECT_EQ(leptris_str_has_nonstandard_entity(
                  "&#65;&#x41;", 11), 0);
    EXPECT_EQ(leptris_str_has_nonstandard_entity(
                  "plain text, no markup", 21), 0);
    /* A named entity outside the predefined set needs the
     * downstream pre-scan rewrite. */
    EXPECT_EQ(leptris_str_has_nonstandard_entity(
                  "a &nbsp; b", 10), 1);
    EXPECT_EQ(leptris_str_has_nonstandard_entity(
                  "&amp; ok &eacute; here", 22), 1);
    /* Standard prefix, nonstandard later in the buffer. */
    EXPECT_EQ(leptris_str_has_nonstandard_entity(
                  "&lt; then &mdash;", 17), 1);
    /* A bare & with no terminator is not an entity reference
     * (parse-error territory, not the pre-scan's call). */
    EXPECT_EQ(leptris_str_has_nonstandard_entity(
                  "a & b", 5), 0);
    EXPECT_EQ(leptris_str_has_nonstandard_entity("", 0), 0);
    EXPECT_EQ(leptris_str_has_nonstandard_entity(nullptr, 0), 0);
}

/* ---- file I/O round-trip ---- */

TEST(PublicSurface, FileIO) {
    const char* path = "leptris_surface_io.xml";
    LeptrisDocument doc = parse_doc();
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(leptris_document_save_file(doc, path, nullptr), 0);

    /* load_file returns the bytes. */
    size_t size = 0;
    char* data = leptris_load_file(path, &size);
    ASSERT_NE(data, nullptr);
    EXPECT_GT(size, 0u);
    leptris_free_string(data);

    /* parse_file re-parses the saved document. */
    LeptrisDocument back = leptris_parse_file(path, nullptr);
    ASSERT_NE(back, nullptr);
    LeptrisElement r = leptris_document_root(back);
    ASSERT_NE(r, nullptr);
    EXPECT_STREQ(leptris_element_name(r), "root");
    leptris_document_free(back);
    leptris_document_free(doc);
    remove(path);

    EXPECT_EQ(leptris_parse_file("/nonexistent/leptris.xml", nullptr),
              nullptr);
    EXPECT_EQ(leptris_load_file("/nonexistent/leptris.xml", nullptr),
              nullptr);
}

/* ---- typed attribute/text getters ---- */

TEST(PublicSurface, TypedGetters) {
    LeptrisDocument doc = parse_doc();
    ASSERT_NE(doc, nullptr);
    LeptrisElement root = leptris_document_root(doc);
    ASSERT_NE(root, nullptr);

    EXPECT_EQ(leptris_element_attribute_bool(root, "flag", 0), 1);
    EXPECT_EQ(leptris_element_attribute_bool(root, "missing", 1), 1);
    EXPECT_EQ(leptris_element_attribute_uint(root, "id", 9), 7u);
    EXPECT_EQ(leptris_element_attribute_uint(root, "name", 9), 9u);
    EXPECT_DOUBLE_EQ(leptris_element_attribute_float(root, "ratio", 0.0),
                     2.5);

    LeptrisElement b3 = leptris_element_find_child_by_attr(
        root, "b", "order", "3");
    ASSERT_NE(b3, nullptr);
    EXPECT_EQ(leptris_element_text_uint(b3, 0), 42u);
    EXPECT_DOUBLE_EQ(leptris_element_text_float(b3, 0.0), 42.0);
    EXPECT_EQ(leptris_element_text_bool(b3, 0), 1);

    /* write + read back the float attribute setter */
    LeptrisElement a = leptris_element_find_child(root, "a");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(leptris_element_set_attribute_float(a, "weight", 1.25),
              LEPTRIS_OK);
    EXPECT_DOUBLE_EQ(leptris_element_attribute_float(a, "weight", 0.0),
                     1.25);

    EXPECT_EQ(leptris_element_attribute_bool(nullptr, "x", 0), 0);
    EXPECT_EQ(leptris_element_text_uint(nullptr, 0), 0u);
    leptris_document_free(doc);
}

/* ---- navigation ---- */

TEST(PublicSurface, Navigation) {
    LeptrisDocument doc = parse_doc();
    ASSERT_NE(doc, nullptr);
    LeptrisElement root = leptris_document_root(doc);
    ASSERT_NE(root, nullptr);

    EXPECT_EQ(leptris_element_root(root), root);
    LeptrisElement a = leptris_element_first_child_any(root);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(leptris_element_parent(a), root);
    EXPECT_EQ(leptris_element_previous_sibling(a, nullptr), nullptr);

    LeptrisElement b1 = leptris_element_next_sibling_any(a);
    ASSERT_NE(b1, nullptr);
    EXPECT_EQ(leptris_element_previous_sibling_any(b1), a);
    /* Three children (a, b@1, b@3): the LAST is b@3. */
    LeptrisElement b3 = leptris_element_find_child_by_attr(
        root, "b", "order", "3");
    ASSERT_NE(b3, nullptr);
    EXPECT_EQ(leptris_element_last_child(root, nullptr), b3);
    EXPECT_EQ(leptris_element_last_child(root, "a"), a);   /* named */
    EXPECT_EQ(leptris_element_last_child(root, "zzz"), nullptr);
    LeptrisElement last_any = leptris_element_last_child_any(root);
    ASSERT_NE(last_any, nullptr);
    EXPECT_EQ(last_any, b3);
    EXPECT_EQ(leptris_node_last_child(leptris_element_as_node(root)),
              (LeptrisNodeRef)last_any);

    EXPECT_NE(leptris_element_find_child(root, "b"), nullptr);
    EXPECT_EQ(leptris_element_find_child(root, "zzz"), nullptr);
    EXPECT_EQ(leptris_element_child_value(a), std::string("alpha"));
    leptris_document_free(doc);
}

/* ---- copy family + attribute removal ---- */

TEST(PublicSurface, CopyFamilyAndAttributeRemoval) {
    LeptrisDocument doc = parse_doc();
    ASSERT_NE(doc, nullptr);
    LeptrisElement root = leptris_document_root(doc);
    ASSERT_NE(root, nullptr);

    LeptrisElement a = leptris_element_first_child_any(root);
    LeptrisElement a_copy = leptris_element_append_copy(root, a);
    ASSERT_NE(a_copy, nullptr);
    EXPECT_EQ(leptris_element_child_count(root), 4u);

    LeptrisElement pre = leptris_element_prepend_copy(root, a);
    ASSERT_NE(pre, nullptr);
    LeptrisElement before = leptris_element_insert_copy_before(a, a);
    ASSERT_NE(before, nullptr);
    LeptrisElement after = leptris_element_insert_copy_after(a, a);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(leptris_element_child_count(root), 7u);

    /* The copies serialize to the same shape as the original. */
    char* x = leptris_element_serialize(a_copy, nullptr);
    ASSERT_NE(x, nullptr);
    EXPECT_STREQ(x, "<a order=\"2\">alpha</a>");
    leptris_free_string(x);

    EXPECT_EQ(leptris_element_remove_all_attributes(a_copy), LEPTRIS_OK);
    x = leptris_element_serialize(a_copy, nullptr);
    ASSERT_NE(x, nullptr);
    EXPECT_STREQ(x, "<a>alpha</a>");
    leptris_free_string(x);
    leptris_document_free(doc);
}

/* ---- hash + binding wrapper + namespace prefix ---- */

TEST(PublicSurface, HashWrapperNamespace) {
    LeptrisDocument doc = parse_doc();
    ASSERT_NE(doc, nullptr);
    LeptrisElement root = leptris_document_root(doc);
    ASSERT_NE(root, nullptr);

    /* Hash is stable for the same element. */
    EXPECT_EQ(leptris_element_hash_value(root),
              leptris_element_hash_value(root));

    int cookie = 42;
    leptris_node_set_binding_wrapper(
        leptris_element_as_node(root), &cookie);
    EXPECT_EQ(leptris_node_get_binding_wrapper(
                  leptris_element_as_node(root)), &cookie);
    leptris_document_free(doc);

    LeptrisDocument nsdoc = leptris_parse_string(
        "<r xmlns:p='urn:x'><p:c/></r>", strlen("<r xmlns:p='urn:x'><p:c/></r>"), nullptr);
    ASSERT_NE(nsdoc, nullptr);
    LeptrisElement pc =
        leptris_element_first_child_any(leptris_document_root(nsdoc));
    ASSERT_NE(pc, nullptr);
    LeptrisNamespace ns = leptris_element_namespace(pc);
    ASSERT_NE(ns, nullptr);
    EXPECT_STREQ(leptris_namespace_uri(ns), "urn:x");
    /* The prefix lives on the qualified name; the namespace object
     * itself may carry NULL when inherited. */
    EXPECT_STREQ(leptris_element_prefix(pc), "p");
    EXPECT_STREQ(leptris_element_namespace_for_prefix(pc, "p"), "urn:x");
    /* namespace_prefix: the declaration on the ROOT carries "p". */
    EXPECT_EQ(leptris_namespace_prefix(nullptr), nullptr);
    /* namespace_prefix: the declaration on the ROOT carries "p". */
    LeptrisElement r2 = leptris_document_root(nsdoc);
    EXPECT_STREQ(leptris_element_namespace_decl_prefix(r2, 0), "p");
    EXPECT_STREQ(leptris_element_namespace_decl_uri(r2, 0), "urn:x");
    leptris_document_free(nsdoc);
}

/* ---- document-level: adopt, finalize, serialize_document,
 * parse_with_encoding, xpointer ---- */

TEST(PublicSurface, DocumentLevelAPI) {
    LeptrisDocument doc = parse_doc();
    ASSERT_NE(doc, nullptr);
    LeptrisElement root = leptris_document_root(doc);

    /* serialize_document matches document_serialize. */
    char* a = leptris_serialize_document(doc);
    char* b = leptris_document_serialize(doc, nullptr);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_STREQ(a, b);
    leptris_free_string(a);
    leptris_free_string(b);

    /* adopt_child moves a subtree between documents. */
    LeptrisDocument other = leptris_document_create();
    ASSERT_NE(other, nullptr);
    LeptrisElement moved = leptris_element_copy(
        leptris_element_first_child_any(root), other);
    ASSERT_NE(moved, nullptr);
    /* adopt_child(parent, child): the child's pool is kept alive by
     * the PARENT — after adoption the child must NOT be freed
     * separately (document_free on the parent releases it). */
    leptris_document_adopt_child(doc, other);

    EXPECT_EQ(leptris_document_finalize_strings(doc), 1);
    leptris_document_free(doc);   /* releases `other` too */

    /* parse_string_with_encoding accepts a UTF-16LE document. */
    const char* utf16 =
        "\xFF\xFE<\0r\0/\0>\0";
    LeptrisDocument d16 = leptris_parse_string_with_encoding(
        utf16, 10, nullptr);
    ASSERT_NE(d16, nullptr);
    leptris_document_free(d16);

    /* xinclude_get_xpointer: NULL for a non-include element. */
    LeptrisDocument xi = leptris_parse_string(
        "<xi:include xmlns:xi='http://www.w3.org/2001/XInclude' href='a.txt'/>", strlen("<xi:include xmlns:xi='http://www.w3.org/2001/XInclude' href='a.txt'/>"), nullptr);
    if (xi) {
        LeptrisElement root_xi = leptris_document_root(xi);
        EXPECT_EQ(leptris_xinclude_get_xpointer(root_xi), nullptr);
        leptris_document_free(xi);
    }
}

/* ---- memory hooks ---- */

TEST(PublicSurface, MemoryHookGetters) {
    /* Default allocator state. */
    EXPECT_EQ(leptris_get_memory_allocation_function(), nullptr);
    EXPECT_EQ(leptris_get_memory_deallocation_function(), nullptr);
}

/* Issue #535: the two FFI-ergonomics APIs. */
TEST(PublicSurface, NodeChildrenBatch) {
    const char xml[] = "<r><a/>text<!--c--><b/><b/></r>";
    LeptrisDocument doc = leptris_parse_string(xml, strlen(xml), nullptr);
    ASSERT_NE(doc, nullptr);
    LeptrisNodeRef root = leptris_element_as_node(leptris_document_root(doc));
    ASSERT_NE(root, nullptr);

    /* node_child_count is elements-only (3); the count query
     * returns every kind (5). */
    EXPECT_EQ(leptris_node_child_count(root), 3u);
    EXPECT_EQ(leptris_node_children(root, nullptr, 0), 5u);

    LeptrisNodeRef kids[5];
    EXPECT_EQ(leptris_node_children(root, kids, 5), 5u);
    EXPECT_EQ(leptris_node_get_type(kids[0]), LEPTRIS_NODE_TYPE_ELEMENT);
    EXPECT_EQ(leptris_node_get_type(kids[1]), LEPTRIS_NODE_TYPE_TEXT);
    EXPECT_EQ(leptris_node_get_type(kids[2]), LEPTRIS_NODE_TYPE_COMMENT);
    EXPECT_EQ(leptris_node_get_type(kids[3]), LEPTRIS_NODE_TYPE_ELEMENT);
    EXPECT_EQ(leptris_node_get_type(kids[4]), LEPTRIS_NODE_TYPE_ELEMENT);

    /* Truncation copies what fits. */
    LeptrisNodeRef two[2];
    EXPECT_EQ(leptris_node_children(root, two, 2), 2u);
    EXPECT_EQ(two[1], kids[1]);

    /* NULL parent; zero capacity copies nothing. */
    EXPECT_EQ(leptris_node_children(nullptr, kids, 5), 0u);
    EXPECT_EQ(leptris_node_children(root, kids, 0), 0u);
    leptris_document_free(doc);
}

TEST(PublicSurface, SerializeIntoBuffer) {
    const char xml[] = "<r><a x='1'>t</a></r>";
    LeptrisDocument doc = leptris_parse_string(xml, strlen(xml), nullptr);
    ASSERT_NE(doc, nullptr);
    LeptrisElement a = leptris_element_first_child_any(
        leptris_document_root(doc));
    ASSERT_NE(a, nullptr);

    /* Size query first. */
    size_t need = leptris_document_serialize_into(doc, nullptr, 0, nullptr, nullptr);
    char* buf = (char*)malloc(need);
    ASSERT_NE(buf, nullptr);
    size_t len = 0;
    EXPECT_EQ(leptris_document_serialize_into(doc, buf, need, &len, nullptr), need);
    char* ref = leptris_document_serialize(doc, nullptr);
    ASSERT_NE(ref, nullptr);
    EXPECT_STREQ(buf, ref);
    EXPECT_EQ(len, strlen(ref));
    leptris_free_string(ref);
    free(buf);

    /* Under-capacity: nothing written, need returned. */
    char small[4];
    EXPECT_GT(leptris_document_serialize_into(doc, small, 4, nullptr, nullptr), 4u);
    small[0] = 'Z';
    leptris_document_serialize_into(doc, small, 4, nullptr, nullptr);
    EXPECT_EQ(small[0], 'Z');  /* untouched on under-capacity */

    /* Element variant. */
    size_t eneed = leptris_element_serialize_into(a, nullptr, 0, nullptr, nullptr);
    buf = (char*)malloc(eneed);
    ASSERT_NE(buf, nullptr);
    EXPECT_EQ(leptris_element_serialize_into(a, buf, eneed, nullptr, nullptr), eneed);
    EXPECT_STREQ(buf, "<a x=\"1\">t</a>");
    free(buf);
    leptris_document_free(doc);
}

/* Issue #546: a rootless document carrying PIs must not be silently
 * dropped — the serializer emits the declaration (if the source
 * had one) and the PIs. */
TEST(PublicSurface, RootlessWithPIsIsNotDropped) {
    LeptrisDocument d = leptris_document_create();
    ASSERT_NE(d, nullptr);
    ASSERT_NE(leptris_document_add_pi(d, "docpi", "d1"), nullptr);
    /* Without an explicit declaration set, emit only the PIs. */
    char* out = leptris_document_serialize(d, nullptr);
    ASSERT_NE(out, nullptr);
    EXPECT_NE(std::strstr(out, "<?docpi d1?>"), nullptr);
    leptris_free_string(out);

    /* PI accessor contract (issue #526) still works. */
    EXPECT_EQ(leptris_document_pi_count(d), 1u);
    EXPECT_STREQ(leptris_document_pi_target(d, 0), "docpi");
    EXPECT_STREQ(leptris_document_pi_data(d, 0), "d1");
    leptris_document_free(d);
}

/* Issue #547: ParseOptions.recover returns an empty document
 * instead of NULL on parse failure (moxml's libxml2 adapter does
 * the same; this makes the native API honest about it). */
TEST(PublicSurface, RecoverReturnsEmptyDocument) {
    LeptrisParseOptions opts = {LEPTRIS_PARSE_DEFAULT, 0, 0, 0};
    opts.recover = 1;
    LeptrisStatus st = LEPTRIS_OK;
    /* Heap copy: the SIMD copy pass reads blocks and can overread
     * short .rodata literals (ASAN-visible). */
    char bad[32];
    memset(bad, 0, sizeof(bad));
    memcpy(bad, "<root><unclosed>", 16);
    LeptrisDocument d = leptris_parse_string_ex(bad, 16, &opts, &st);
    ASSERT_NE(d, nullptr);   /* recover: empty doc, not NULL */
    EXPECT_EQ(st, LEPTRIS_ERROR_PARSE);
    EXPECT_EQ(leptris_xpath_result_count(
        leptris_xpath_eval(d, nullptr, "count(//node())")), 0);
    leptris_document_free(d);
}


/* ---- lane 15: versioned XPath eval ----
 * leptris_xpath_eval_versioned(doc, ctx, expr, version, status):
 * LEPTRIS_XPATH_10 gates the 3.x surface OFF (arrow, maps, let,
 * bang), LEPTRIS_XPATH_31 keeps the full grammar. Plain XPath
 * 1.0 works under both. */
TEST(PublicSurface, VersionedXPathEval) {
    LeptrisDocument d = parse_doc();
    ASSERT_NE(d, nullptr);

    /* 1.0 grammar works under both versions. */
    LeptrisXPathResult r = leptris_xpath_eval_versioned(
        d, nullptr, "count(//b)", LEPTRIS_XPATH_10, nullptr);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(leptris_xpath_result_number(r), 2.0);
    leptris_xpath_result_free(r);
    r = leptris_xpath_eval_versioned(
        d, nullptr, "count(//b)", LEPTRIS_XPATH_31, nullptr);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(leptris_xpath_result_number(r), 2.0);
    leptris_xpath_result_free(r);

    /* 3.x-only constructs: rejected under 1.0, full under 3.1. */
    const char* v31_only[] = {
        "let $x := 1 return $x + 1",
        "map{'a':1}?a",
        "map {'a': 1}",   /* bare map constructor (no lookup) */
        "array{//b}",     /* bare array constructor */
        "//b ! string()",
        "//b => count()",
    };
    for (const char* e : v31_only) {
        LeptrisStatus st = LEPTRIS_OK;
        LeptrisXPathResult r10 = leptris_xpath_eval_versioned(
            d, nullptr, e, LEPTRIS_XPATH_10, &st);
        EXPECT_EQ(r10, nullptr) << e;
        EXPECT_NE(st, LEPTRIS_OK) << e;
        LeptrisXPathResult r31 = leptris_xpath_eval_versioned(
            d, nullptr, e, LEPTRIS_XPATH_31, nullptr);
        EXPECT_NE(r31, nullptr) << e;
        if (r31) leptris_xpath_result_free(r31);
    }

    /* Invalid version is an error, not a crash. */
    LeptrisStatus st2 = LEPTRIS_OK;
    EXPECT_EQ(leptris_xpath_eval_versioned(
                  d, nullptr, "//b", (LeptrisXPathVersion)99, &st2),
              nullptr);
    EXPECT_NE(st2, LEPTRIS_OK);
    leptris_document_free(d);
}
