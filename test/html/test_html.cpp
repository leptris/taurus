/* TODO.xslt-full/14 — HTML parsing mode (#659), Nokogiri-parity
 * core. Each spec parses lenient HTML and pins the resulting DOM
 * (serialized). Reference behaviors: libxml2 HTMLparser as exposed
 * by Nokogiri (no implied <tbody>, no synthesized <html>/<body>,
 * stray end tags pop-until-matched or are ignored). */
#include <gtest/gtest.h>
extern "C" {
#include "leptris.h"
}
#include <cstring>
#include <string>

namespace {

std::string Html(const char* in) {
    LeptrisStatus st = LEPTRIS_OK;
    LeptrisDocument doc = leptris_parse_html_string(in, std::strlen(in), &st);
    if (!doc) return "(parse-failed)";
    char* out = leptris_document_serialize(doc, nullptr);
    std::string r = out ? out : "(null)";
    leptris_free_string(out);
    leptris_document_free(doc);
    /* Strip the declaration for shape comparisons. */
    const char* decl = "<?xml version=\"1.0\"?>";
    if (r.compare(0, std::strlen(decl), decl) == 0) {
        size_t rest = std::strlen(decl);
        if (rest < r.size() && r[rest] == '\n') rest++;
        r = r.substr(rest);
    }
    /* WHATWG shape is <html><head/><body>...</body></html> (head
     * always present, possibly empty) — strip the EMPTY head with
     * the wrapper so content specs stay shape-agnostic. Non-empty
     * heads (HeadContentLift) keep the full form. */
    const char* open_wh = "<html><head/><body>";
    const char* open_w = "<html><body>";
    const char* close_w = "</body></html>";
    /* Document-level prolog comments (WHATWG initial mode) ride
     * ahead of the wrapper — keep them, strip around them. */
    size_t pl = 0;
    while (r.compare(pl, 4, "<!--") == 0) {
        size_t e = r.find("-->", pl + 4);
        if (e == std::string::npos) break;
        pl = e + 3;
    }
    for (const char* ow : {open_wh, open_w}) {
        if (r.compare(pl, std::strlen(ow), ow) == 0 &&
            r.size() >= pl + std::strlen(ow) + std::strlen(close_w) &&
            r.compare(r.size() - std::strlen(close_w),
                      std::strlen(close_w), close_w) == 0) {
            return r.substr(0, pl) +
                   r.substr(pl + std::strlen(ow),
                            r.size() - pl - std::strlen(ow) -
                                std::strlen(close_w));
        }
    }
    return r;
}
std::string Html4(const char* in) {
    LeptrisStatus st = LEPTRIS_OK;
    LeptrisDocument doc = leptris_parse_html4_string(in, std::strlen(in), &st);
    if (!doc) return "(parse-failed)";
    char* out = leptris_document_serialize(doc, nullptr);
    std::string r = out ? out : "(null)";
    leptris_free_string(out);
    leptris_document_free(doc);
    /* Strip the declaration for shape comparisons. */
    const char* decl = "<?xml version=\"1.0\"?>";
    if (r.compare(0, std::strlen(decl), decl) == 0) {
        size_t rest = std::strlen(decl);
        if (rest < r.size() && r[rest] == '\n') rest++;
        r = r.substr(rest);
    }
    /* The parser synthesizes the Nokogiri document shape
     * <html><body>...</body></html> (no empty <head> — Nokogiri
     * omits it without head content); the specs below pin the BODY
     * content (the tolerant-parsing behaviors under test). */
    const char* open_w = "<html><body>";
    const char* close_w = "</body></html>";
    if (r.compare(0, std::strlen(open_w), open_w) == 0 &&
        r.size() >= std::strlen(open_w) + std::strlen(close_w) &&
        r.compare(r.size() - std::strlen(close_w), std::strlen(close_w),
                  close_w) == 0) {
        return r.substr(std::strlen(open_w),
                        r.size() - std::strlen(open_w) -
                            std::strlen(close_w));
    }
    return r;
}


/* #659 (html5lib corpus fallout): inputs that append NOTHING
 * (stray end tag only, a doctype-only document, a second doctype,
 * an empty string) must still parse to the empty Nokogiri shape —
 * "nothing parsed" is not an error in lenient HTML mode. */
TEST(HtmlParse, EmptyShapeInputsAreDocuments) {
    const char* cases[] = {
        "", "</menuitem>", "</b>", "<!DOCTYPE html><!DOCTYPE html>",
        "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\"\n"
        "\"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\">",
    };
    for (const char* in : cases) {
        LeptrisStatus st = LEPTRIS_OK;
        LeptrisDocument doc =
            leptris_parse_html_string(in, std::strlen(in), &st);
        EXPECT_NE(doc, nullptr) << "input: " << in;
        if (!doc) continue;
        LeptrisElement root = leptris_document_root(doc);
        ASSERT_NE(root, nullptr);
        EXPECT_STREQ(leptris_element_name(root), "html");
        leptris_document_free(doc);
    }
}

TEST(HtmlParse, SynthesizesNokogiriDocumentShape) {
    /* Nokogiri/libxml2 shape is the html4 entry's contract; the
     * WHATWG entry always has a head (StructuralHeadBodyTags). */
    LeptrisStatus st = LEPTRIS_OK;
    const char in[] = "<p>x</p>";
    LeptrisDocument doc = leptris_parse_html4_string(in, std::strlen(in), &st);
    ASSERT_NE(doc, nullptr);
    LeptrisElement root = leptris_document_root(doc);
    ASSERT_NE(root, nullptr);
    EXPECT_STREQ(leptris_element_name(root), "html");
    /* Nokogiri shape: html > body only (no empty head). */
    EXPECT_EQ(leptris_element_child_count(root), 1u);
    LeptrisElement body = (LeptrisElement)leptris_node_first_child(
        (LeptrisNodeRef)root);
    ASSERT_NE(body, nullptr);
    EXPECT_STREQ(leptris_element_name(body), "body");
    char* out = leptris_document_serialize(doc, nullptr);
    ASSERT_NE(out, nullptr);
    EXPECT_TRUE(std::strstr(out, "<html><body><p>x</p></body></html>") !=
                nullptr);
    leptris_free_string(out);
    leptris_document_free(doc);
}

TEST(HtmlParse, LeadingCommentsBelongToTheDocument) {
    /* WHATWG initial/before-html: a comment token inserts as a
     * child of the DOCUMENT, before the (implied) <html> element
     * (html5lib tests6/ html5test-com ground truth). Text first
     * starts body content — later comments stay in the flow. The
     * html4 entry keeps the libxml2/Nokogiri shape (comment rides
     * inside the synthesized body). */
    LeptrisStatus st = LEPTRIS_OK;

    const char w1[] = "<!-- lead --><p>hi</p>";
    LeptrisDocument d1 = leptris_parse_html_string(w1, std::strlen(w1), &st);
    ASSERT_NE(d1, nullptr);
    char* o1 = leptris_document_serialize(d1, nullptr);
    ASSERT_NE(o1, nullptr);
    EXPECT_STREQ(o1,
        "<!-- lead --><html><head/><body><p>hi</p></body></html>");
    leptris_free_string(o1);
    leptris_document_free(d1);

    const char w2[] = "<!-- a --><!-- b --><p>x</p>";
    LeptrisDocument d2 = leptris_parse_html_string(w2, std::strlen(w2), &st);
    ASSERT_NE(d2, nullptr);
    char* o2 = leptris_document_serialize(d2, nullptr);
    ASSERT_NE(o2, nullptr);
    EXPECT_STREQ(o2,
        "<!-- a --><!-- b --><html><head/><body><p>x</p></body>"
        "</html>");
    leptris_free_string(o2);
    leptris_document_free(d2);

    /* Non-whitespace text opens body content; the comment that
     * follows it is in-body. */
    const char w3[] = "hi<!-- c -->";
    LeptrisDocument d3 = leptris_parse_html_string(w3, std::strlen(w3), &st);
    ASSERT_NE(d3, nullptr);
    char* o3 = leptris_document_serialize(d3, nullptr);
    ASSERT_NE(o3, nullptr);
    EXPECT_STREQ(o3, "<html><head/><body>hi<!-- c --></body></html>");
    leptris_free_string(o3);
    leptris_document_free(d3);

    const char n1[] = "<!-- lead --><p>hi</p>";
    LeptrisDocument d4 = leptris_parse_html4_string(n1, std::strlen(n1), &st);
    ASSERT_NE(d4, nullptr);
    char* o4 = leptris_document_serialize(d4, nullptr);
    ASSERT_NE(o4, nullptr);
    EXPECT_STREQ(o4, "<html><body><!-- lead --><p>hi</p></body></html>");
    leptris_free_string(o4);
    leptris_document_free(d4);
}

TEST(HtmlParse, ExplicitHtmlElementIsHonored) {
    LeptrisStatus st = LEPTRIS_OK;
    const char in[] = "<html><body><p>x</p></body></html>";
    LeptrisDocument doc = leptris_parse_html4_string(in, std::strlen(in), &st);
    ASSERT_NE(doc, nullptr);
    LeptrisElement root = leptris_document_root(doc);
    ASSERT_NE(root, nullptr);
    EXPECT_STREQ(leptris_element_name(root), "html");
    EXPECT_EQ(leptris_element_child_count(root), 1u);
    leptris_document_free(doc);
}

TEST(HtmlParse, VoidElementsNeverNest) {
    EXPECT_EQ(Html("<br><img src='x.png'><hr>"),
              "<br/><img src=\"x.png\"/><hr/>");
    /* A following element is a SIBLING of the void element. */
    EXPECT_EQ(Html("<br><p>hi</p>"), "<br/><p>hi</p>");
}

TEST(HtmlParse, ImpliedEndTagsForListsAndCells) {
    EXPECT_EQ(Html("<ul><li>one<li>two</ul>"),
              "<ul><li>one</li><li>two</li></ul>");
    /* WHATWG implies tbody; the html4 entry keeps it bare. */
    EXPECT_EQ(Html("<table><tr><td>a<td>b<tr><td>c</table>"),
              "<table><tbody><tr><td>a</td><td>b</td></tr>"
              "<tr><td>c</td></tr></tbody></table>");
    EXPECT_EQ(Html4("<table><tr><td>a<td>b<tr><td>c</table>"),
              "<table><tr><td>a</td><td>b</td></tr>"
              "<tr><td>c</td></tr></table>");
    /* <p> closes on block-level starts. */
    EXPECT_EQ(Html("<p>one<p>two"),
              "<p>one</p><p>two</p>");
    EXPECT_EQ(Html("<p>text<div>block</div>"),
              "<p>text</p><div>block</div>");
    EXPECT_EQ(Html("<select><option>a<option>b</select>"),
              "<select><option>a</option><option>b</option></select>");
    EXPECT_EQ(Html("<dl><dt>t<dd>d</dl>"),
              "<dl><dt>t</dt><dd>d</dd></dl>");
}

TEST(HtmlParse, NamesAndAttributesLowercased) {
    EXPECT_EQ(Html("<DIV CLASS='Big'>x</DIV>"),
              "<div class=\"Big\">x</div>");
}

TEST(HtmlParse, MinimizedAndUnquotedAttributes) {
    /* Boolean attribute: value is the EMPTY string (the
     * html5lib/Nokogiri DOM stores checked=""). */
    EXPECT_EQ(Html("<input type=checkbox checked>"),
              "<input type=\"checkbox\" checked=\"\"/>");
    /* Unquoted values end at whitespace (HTML5 §13.2.5.43): the
     * remainder is a NEW minimized attribute, exactly as libxml2. */
    EXPECT_EQ(Html("<a href=/x>y</a>"),
              "<a href=\"/x\">y</a>");
    EXPECT_EQ(Html("<a href=/x y.html>t</a>"),
              "<a href=\"/x\" y.html=\"\">t</a>");
}

TEST(HtmlParse, ScriptAndStyleAreRawText) {
    /* Raw text: the DOM text keeps < and > verbatim (assert on the
     * text content — XML-method serialization escaping is
     * orthogonal; the html method emits it raw). */
    LeptrisStatus st = LEPTRIS_OK;
    const char in[] = "<script>if (a < b) { x(); }</script>"
                      "<style>p > b { color: red }</style>";
    LeptrisDocument doc = leptris_parse_html4_string(in, std::strlen(in), &st);
    ASSERT_NE(doc, nullptr);
    LeptrisXPathResult r = leptris_xpath_eval(doc, nullptr, "//script");
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(leptris_xpath_result_count(r), 1u);
    LeptrisElement sc = (LeptrisElement)leptris_xpath_result_get(r, 0);
    ASSERT_NE(sc, nullptr);
    EXPECT_STREQ(leptris_element_text(sc), "if (a < b) { x(); }");
    leptris_xpath_result_free(r);
    LeptrisXPathResult r2 = leptris_xpath_eval(doc, nullptr, "//style");
    ASSERT_NE(r2, nullptr);
    LeptrisElement stl = (LeptrisElement)leptris_xpath_result_get(r2, 0);
    ASSERT_NE(stl, nullptr);
    EXPECT_STREQ(leptris_element_text(stl), "p > b { color: red }");
    leptris_xpath_result_free(r2);
    leptris_document_free(doc);
    /* Uppercase close still ends raw text (html4 entry: the
     * leading-script-in-body libxml2 shape this spec pins). */
    LeptrisStatus st2 = LEPTRIS_OK;
    LeptrisDocument d2 = leptris_parse_html4_string(
        "<script>1<2</SCRIPT>after",
        sizeof("<script>1<2</SCRIPT>after") - 1, &st2);
    ASSERT_NE(d2, nullptr);
    LeptrisXPathResult r3 =
        leptris_xpath_eval(d2, nullptr, "string(/html/body/script)");
    ASSERT_NE(r3, nullptr);
    char* sv3 = leptris_xpath_result_string(r3);
    EXPECT_STREQ(sv3 ? sv3 : "", "1<2");
    leptris_free_string(sv3);
    leptris_xpath_result_free(r3);
    leptris_document_free(d2);
}

TEST(HtmlParse, EntitiesDecodeInTextAndValues) {
    /* &nbsp; &amp; &copy; numeric — text and attribute values. */
    LeptrisStatus st = LEPTRIS_OK;
    const char in[] = "<p title='a &amp; b'>x &nbsp;&copy; &#65;</p>";
    LeptrisDocument doc = leptris_parse_html_string(in, std::strlen(in), &st);
    ASSERT_NE(doc, nullptr);
    /* The wrapper puts <p> under html/body; XPath reaches it. */
    LeptrisXPathResult pr = leptris_xpath_eval(doc, nullptr, "//p");
    ASSERT_NE(pr, nullptr);
    ASSERT_EQ(leptris_xpath_result_count(pr), 1u);
    LeptrisElement root = (LeptrisElement)leptris_xpath_result_get(pr, 0);
    ASSERT_NE(root, nullptr);
    EXPECT_STREQ(leptris_element_name(root), "p");
    EXPECT_STREQ(leptris_element_attribute(root, "title"), "a & b");
    leptris_xpath_result_free(pr);
    /* nbsp serializes as the raw UTF-8 byte; copy as U+00A9. */
    char* out = leptris_document_serialize(doc, nullptr);
    ASSERT_NE(out, nullptr);
    EXPECT_TRUE(std::strstr(out, "x \xC2\xA0\xC2\xA9 A") != nullptr);
    leptris_free_string(out);
    leptris_document_free(doc);
}

TEST(HtmlParse, CommentsAndDoctypeSurvive) {
    EXPECT_EQ(Html("<!-- note --><p>x</p>"),
              "<!-- note --><p>x</p>");
    /* Legacy doctype strings are accepted verbatim. */
    LeptrisStatus st = LEPTRIS_OK;
    const char in[] =
        "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01//EN\" "
        "\"http://www.w3.org/TR/html4/strict.dtd\"><html/>";
    LeptrisDocument doc = leptris_parse_html_string(in, std::strlen(in), &st);
    EXPECT_NE(doc, nullptr);   /* never a hard failure */
    if (doc) leptris_document_free(doc);
}

TEST(HtmlParse, HeadCommentsNestIntoHead) {
    /* WHATWG "in head": a comment token is a child of the head
     * element — it must not end the head run (html5lib
     * tests19.dat:87 class). The html4 entry keeps the libxml2
     * shape (comment rides in body). */
    LeptrisStatus st = LEPTRIS_OK;

    const char w1[] =
        "<!doctype html><head><!--c--><meta charset=\"utf8\">";
    LeptrisDocument d1 = leptris_parse_html_string(w1, std::strlen(w1), &st);
    ASSERT_NE(d1, nullptr);
    char* o1 = leptris_document_serialize(d1, nullptr);
    ASSERT_NE(o1, nullptr);
    EXPECT_STREQ(o1,
        "<!DOCTYPE html><html><head><!--c-->"
        "<meta charset=\"utf8\"/></head><body/></html>");
    leptris_free_string(o1);
    leptris_document_free(d1);

    /* A comment BETWEEN head elements must not truncate the run. */
    const char w2[] = "<head><meta><!--c--><title>T</title><p>x";
    LeptrisDocument d2 = leptris_parse_html_string(w2, std::strlen(w2), &st);
    ASSERT_NE(d2, nullptr);
    char* o2 = leptris_document_serialize(d2, nullptr);
    ASSERT_NE(o2, nullptr);
    EXPECT_STREQ(o2,
        "<html><head><meta/><!--c--><title>T</title></head>"
        "<body><p>x</p></body></html>");
    leptris_free_string(o2);
    leptris_document_free(d2);

    const char n1[] =
        "<!doctype html><head><!--c--><meta charset=\"utf8\">";
    LeptrisDocument d3 = leptris_parse_html4_string(n1, std::strlen(n1), &st);
    ASSERT_NE(d3, nullptr);
    char* o3 = leptris_document_serialize(d3, nullptr);
    ASSERT_NE(o3, nullptr);
    EXPECT_STREQ(o3,
        "<!DOCTYPE html><html><body><!--c-->"
        "<meta charset=\"utf8\"/></body></html>");
    leptris_free_string(o3);
    leptris_document_free(d3);
}

TEST(HtmlParse, StrayEndTagsAreIgnoredOrPop) {
    /* libxml2 shape (stray </i> ignored, no clone) — the WHATWG
     * entry keeps the adopted empty <i> (adoption agency). */
    EXPECT_EQ(Html4("<b><i>x</b></i>"), "<b><i>x</i></b>");
    EXPECT_EQ(Html("</p>x"), "x");
    EXPECT_EQ(Html("<ul><li>a</ul></li>"), "<ul><li>a</li></ul>");
}

TEST(HtmlParse, UnclosedElementsCloseAtEof) {
    EXPECT_EQ(Html("<div><span>x"), "<div><span>x</span></div>");
}

TEST(HtmlParse, NakedTextAndLtInTextSurvive) {
    EXPECT_EQ(Html("a < b & c"), "a &lt; b &amp; c");
}

TEST(HtmlParse, CaseInsensitiveCloseMatches) {
    EXPECT_EQ(Html("<P>x</p>"), "<p>x</p>");
}

}  // namespace


/* #659 slice 2: processing-instruction-ish bogus constructs.
 * libxml2/Nokogiri ground truth: <?target data?> becomes a PI
 * node whose data INCLUDES the trailing '?'; <!...> non-comment
 * (DOCTYPE, <![CDATA[) is dropped to first '>' and does not
 * perturb the text. */
TEST(HtmlParse, ProcessingInstructionAndBogus) {
    LeptrisDocument d = leptris_parse_html_string(
        "<div>a<?foo bar?>b</div>",
        strlen("<div>a<?foo bar?>b</div>"), NULL);
    ASSERT_NE(d, nullptr);
    LeptrisElement root = leptris_document_root(d);
    ASSERT_NE(root, nullptr);
    /* WHATWG shape is html>[head, body] — find body by name. */
    LeptrisElement body = (LeptrisElement)leptris_node_first_child(
        (LeptrisNodeRef)root);
    while (body && strcmp(leptris_element_name(body), "body") != 0)
        body = (LeptrisElement)leptris_node_next_sibling(
            (LeptrisNodeRef)body);
    ASSERT_NE(body, nullptr);
    LeptrisElement dv = leptris_element_first_child_any(body);
    ASSERT_NE(dv, nullptr);
    /* children: text "a", bogus comment "?foo bar?", text "b" —
     * html5lib has no PI tokenizer, "<?" makes a comment whose
     * data is "?" + the raw bytes to '>' (tests1:44). The html4
     * entry keeps the libxml2 PI node. */
    LeptrisNodeRef c1 = leptris_node_first_child(
        leptris_element_as_node(dv));
    ASSERT_NE(c1, nullptr);
    EXPECT_EQ(leptris_node_get_type(c1), LEPTRIS_NODE_TYPE_TEXT);
    LeptrisNodeRef c2 = leptris_node_next_sibling(c1);
    ASSERT_NE(c2, nullptr);
    EXPECT_EQ(leptris_node_get_type(c2),
              LEPTRIS_NODE_TYPE_COMMENT);
    const char* cd = leptris_comment_node_get_content(c2);
    EXPECT_STREQ(cd ? cd : "", "?foo bar?");
    LeptrisNodeRef c3 = leptris_node_next_sibling(c2);
    ASSERT_NE(c3, nullptr);
    EXPECT_EQ(leptris_node_get_type(c3), LEPTRIS_NODE_TYPE_TEXT);
    leptris_document_free(d);

    /* bogus <!...> drops to '>' without touching the text */
    LeptrisDocument d2 = leptris_parse_html_string(
        "<p>a<![CDATA[x]]>b</p>",
        strlen("<p>a<![CDATA[x]]>b</p>"), NULL);
    ASSERT_NE(d2, nullptr);
    LeptrisXPathResult r = leptris_xpath_eval(
        d2, NULL, "string(//p)");
    ASSERT_NE(r, nullptr);
    char* sv = leptris_xpath_result_string(r);
    EXPECT_STREQ(sv ? sv : "", "ab");
    leptris_free_string(sv);
    leptris_xpath_result_free(r);
    leptris_document_free(d2);
}


/* #659: head-content lift — a contiguous leading run of
 * title/meta/link/base elements moves into a synthesized <head>
 * (libxml2 shape); after body content starts, nothing lifts; no
 * empty head is synthesized. */
TEST(HtmlParse, HeadContentLift) {
    const char* h1 = "<title>t</title><p>x</p>";
    LeptrisDocument d = leptris_parse_html_string(h1, strlen(h1), NULL);
    ASSERT_NE(d, nullptr);
    LeptrisXPathResult r = leptris_xpath_eval(
        d, NULL, "name(/html/*[1])");
    ASSERT_NE(r, nullptr);
    char* sv = leptris_xpath_result_string(r);
    EXPECT_STREQ(sv ? sv : "", "head");
    leptris_free_string(sv);
    leptris_xpath_result_free(r);
    r = leptris_xpath_eval(d, NULL, "name(/html/head/*[1])");
    ASSERT_NE(r, nullptr);
    sv = leptris_xpath_result_string(r);
    EXPECT_STREQ(sv ? sv : "", "title");
    leptris_free_string(sv);
    leptris_xpath_result_free(r);
    r = leptris_xpath_eval(d, NULL, "name(/html/body/*[1])");
    ASSERT_NE(r, nullptr);
    sv = leptris_xpath_result_string(r);
    EXPECT_STREQ(sv ? sv : "", "p");
    leptris_free_string(sv);
    leptris_xpath_result_free(r);
    leptris_document_free(d);

    /* title after body content does NOT lift — it stays in body.
     * (The WHATWG entry now always has a head, possibly empty.) */
    const char* h2 = "<p>x</p><title>after</title>";
    d = leptris_parse_html_string(h2, strlen(h2), NULL);
    ASSERT_NE(d, nullptr);
    r = leptris_xpath_eval(d, NULL, "count(/html/head/*)");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(leptris_xpath_result_number(r), 0.0);
    leptris_xpath_result_free(r);
    r = leptris_xpath_eval(d, NULL, "count(/html/body/title)");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(leptris_xpath_result_number(r), 1.0);
    leptris_xpath_result_free(r);
    leptris_document_free(d);

    /* meta+link+base prefix lifts; no head without head elements */
    const char* h3 = "<meta charset='utf-8'><link rel='s'><b>b</b>";
    d = leptris_parse_html_string(h3, strlen(h3), NULL);
    ASSERT_NE(d, nullptr);
    r = leptris_xpath_eval(d, NULL, "count(/html/head/*)");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(leptris_xpath_result_number(r), 2.0);
    leptris_xpath_result_free(r);
    leptris_document_free(d);
}


/* #659 characterization (Nokogiri/libxml2 ground truth, probed):
 * <template> is an ORDINARY element with its children in place
 * (libxml2 predates the WHATWG inert-fragment model), and
 * misnesting closes the formatting element at the outer end tag
 * with the stray end tag dropped - "natural nesting + stray-pop".
 * These specs lock the parity in so later slices cannot regress
 * it while chasing the html5lib corpus. */
TEST(HtmlParse, TemplateAndMisnestingLibxml2Shape) {
    const char* h1 = "<template><b>x</b>text</template><p>after</p>";
    LeptrisDocument d = leptris_parse_html4_string(h1, strlen(h1), NULL);
    ASSERT_NE(d, nullptr);
    LeptrisXPathResult r = leptris_xpath_eval(
        d, NULL, "count(/html/body/template/b)");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(leptris_xpath_result_number(r), 1.0);
    leptris_xpath_result_free(r);
    r = leptris_xpath_eval(d, NULL, "string(/html/body/template)");
    ASSERT_NE(r, nullptr);
    char* sv = leptris_xpath_result_string(r);
    EXPECT_STREQ(sv ? sv : "", "xtext");
    leptris_free_string(sv);
    leptris_xpath_result_free(r);
    leptris_document_free(d);

    const char* h2 = "<b>1<i>2</b>3</i>";
    d = leptris_parse_html4_string(h2, strlen(h2), NULL);
    ASSERT_NE(d, nullptr);
    r = leptris_xpath_eval(d, NULL, "count(/html/body/b/i)");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(leptris_xpath_result_number(r), 1.0);
    leptris_xpath_result_free(r);
    r = leptris_xpath_eval(d, NULL, "count(/html/body/i)");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(leptris_xpath_result_number(r), 0.0);
    leptris_xpath_result_free(r);
    r = leptris_xpath_eval(d, NULL, "string(/html/body)");
    ASSERT_NE(r, nullptr);
    sv = leptris_xpath_result_string(r);
    EXPECT_STREQ(sv ? sv : "", "123");
    leptris_free_string(sv);
    leptris_xpath_result_free(r);
    leptris_document_free(d);
}

/* #659 two-mode split: leptris_parse_html_string is the WHATWG
 * engine (leading script/style lift into the implied head —
 * tests16's 189-case shape); leptris_parse_html4_string keeps the
 * libxml2/Nokogiri compat shape (they stay in body). Both floors
 * pin this: html5lib 285 / parity 372. */
TEST(HtmlTwoModes, LeadingScriptPlacement) {
    LeptrisStatus st = LEPTRIS_OK;
    const char html[] = "<!DOCTYPE html><script>x</script><p>t</p>";
    LeptrisDocument d5 = leptris_parse_html_string(
        html, sizeof(html) - 1, &st);
    ASSERT_NE(d5, nullptr);
    LeptrisElement html5 = leptris_document_root(d5);
    ASSERT_NE(html5, nullptr);
    LeptrisElement head5 = leptris_element_first_child_any(html5);
    ASSERT_NE(head5, nullptr);
    EXPECT_STREQ(leptris_element_name(head5), "head");
    EXPECT_EQ(leptris_element_child_count(head5), 1u);

    LeptrisDocument d4 = leptris_parse_html4_string(
        html, sizeof(html) - 1, &st);
    ASSERT_NE(d4, nullptr);
    LeptrisElement html4 = leptris_document_root(d4);
    ASSERT_NE(html4, nullptr);
    /* libxml2: no head content run -> no synthesized <head> (the
     * no-empty-head rule); script stays first in body. */
    LeptrisElement body4 = leptris_element_last_child(html4, NULL);
    ASSERT_NE(body4, nullptr);
    EXPECT_STREQ(leptris_element_name(body4), "body");
    LeptrisElement first4 = leptris_element_first_child_any(body4);
    ASSERT_NE(first4, nullptr);
    EXPECT_STREQ(leptris_element_name(first4), "script");

    leptris_document_free(d5);
    leptris_document_free(d4);
}

/* #659 foster parenting (WHATWG entry only): text and non-table
 * elements arriving in table context insert BEFORE the table in
 * its parent; table-structure elements stay inside; whitespace
 * stays in the table; the html4 entry keeps the libxml2 shape. */
TEST(HtmlTwoModes, FosterParenting) {
    LeptrisStatus st = LEPTRIS_OK;
    const char html[] = "<table>x<tr><td>c</td></tr></table>";
    LeptrisDocument d5 = leptris_parse_html_string(
        html, sizeof(html) - 1, &st);
    ASSERT_NE(d5, nullptr);
    LeptrisXPathResult r = leptris_xpath_eval(
        d5, nullptr, "string(/html/body/text())");
    ASSERT_NE(r, nullptr);
    char* sv = leptris_xpath_result_string(r);
    EXPECT_STREQ(sv ? sv : "", "x");
    leptris_free_string(sv);
    leptris_xpath_result_free(r);
    r = leptris_xpath_eval(d5, nullptr,
                           "count(/html/body/table/tbody/tr/td)");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(leptris_xpath_result_number(r), 1.0);
    leptris_xpath_result_free(r);
    leptris_document_free(d5);

    LeptrisDocument d4 = leptris_parse_html4_string(
        html, sizeof(html) - 1, &st);
    ASSERT_NE(d4, nullptr);
    LeptrisXPathResult r4 = leptris_xpath_eval(
        d4, nullptr, "string(/html/body/table/text())");
    ASSERT_NE(r4, nullptr);
    char* sv4 = leptris_xpath_result_string(r4);
    EXPECT_STREQ(sv4 ? sv4 : "", "x");
    leptris_free_string(sv4);
    leptris_xpath_result_free(r4);
    leptris_document_free(d4);
}

/* #659 adoption agency (WHATWG entry only): a formatting element
 * closed out of order keeps its scope for later content — the
 * inner open formatting elements are cloned at the new insertion
 * point (<b>1<i>2</b>3</i> -> <b>1<i>2</i></b><i>3</i>). The html4
 * entry keeps libxml2's pop-away shape. */
TEST(HtmlTwoModes, AdoptionAgency) {
    EXPECT_EQ(Html("<b>1<i>2</b>3</i>"), "<b>1<i>2</i></b><i>3</i>");
    EXPECT_EQ(Html4("<b>1<i>2</b>3</i>"), "<b>1<i>2</i></b>3");
}

/* #659 AA step 5: the clone carries the original's attributes. */
TEST(HtmlTwoModes, AdoptionAgencyCloneKeepsAttributes) {
    EXPECT_EQ(Html(R"(<a href="h">1<i>2</a>3</i>)"),
              R"(<a href="h">1<i>2</i></a><i>3</i>)");
}

/* #659: the HTML modes record the DOCTYPE (name + legacy PUBLIC/
 * SYSTEM ids) on the document like the XML path — the corpus
 * comparator reads it via leptris_document_internal_subset. */
TEST(HtmlTwoModes, DoctypeIsRecorded) {
    LeptrisStatus st = LEPTRIS_OK;
    const char in[] = "<!doctype html><p>x";
    LeptrisDocument doc = leptris_parse_html_string(in, sizeof(in) - 1, &st);
    ASSERT_NE(doc, nullptr);
    LeptrisDoctype dt = leptris_document_internal_subset(doc);
    ASSERT_NE(dt, nullptr);
    EXPECT_STREQ(leptris_doctype_get_root_name(dt), "html");
    leptris_document_free(doc);

    const char in2[] =
        "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01//EN\""
        " \"http://www.w3.org/TR/html4/strict.dtd\"><p>x";
    LeptrisDocument d2 = leptris_parse_html_string(in2, sizeof(in2) - 1, &st);
    ASSERT_NE(d2, nullptr);
    LeptrisDoctype dt2 = leptris_document_internal_subset(d2);
    ASSERT_NE(dt2, nullptr);
    EXPECT_STREQ(leptris_doctype_get_root_name(dt2), "html");
    EXPECT_STREQ(leptris_doctype_get_public_id(dt2),
                 "-//W3C//DTD HTML 4.01//EN");
    EXPECT_STREQ(leptris_doctype_get_system_id(dt2),
                 "http://www.w3.org/TR/html4/strict.dtd");
    leptris_document_free(d2);

    /* No doctype -> none recorded; a second doctype does not
     * replace the first (WHATWG ignores stray doctypes in body). */
    const char in3[] = "<p>x";
    LeptrisDocument d3 = leptris_parse_html_string(in3, sizeof(in3) - 1, &st);
    ASSERT_NE(d3, nullptr);
    EXPECT_EQ(leptris_document_internal_subset(d3), nullptr);
    leptris_document_free(d3);

    const char in4[] = "<!doctype html><p>x<!doctype html2>";
    LeptrisDocument d4 = leptris_parse_html_string(in4, sizeof(in4) - 1, &st);
    ASSERT_NE(d4, nullptr);
    LeptrisDoctype dt4 = leptris_document_internal_subset(d4);
    ASSERT_NE(dt4, nullptr);
    EXPECT_STREQ(leptris_doctype_get_root_name(dt4), "html");
    leptris_document_free(d4);
}

/* #659: WHATWG insertion modes — every document is html>[head,
 * body] whatever the bare structural tags look like; the html4
 * entry keeps libxml2's shape (no empty head). */
TEST(HtmlTwoModes, StructuralHeadBodyTags) {
    /* All these WHATWG-entry shapes must be html>[head, body]. */
    const char* cases[] = {
        "<html>", "<head>", "<body>", "<html><head>",
        "<html><head></head>", "<html><head></head><body>",
        "<html><body></html>", "<head></html>",
        "<html><head></body></html>",
    };
    for (const char* c : cases) {
        LeptrisStatus st = LEPTRIS_OK;
        LeptrisDocument d = leptris_parse_html_string(c, strlen(c), &st);
        ASSERT_NE(d, nullptr) << c;
        LeptrisElement root = leptris_document_root(d);
        ASSERT_NE(root, nullptr) << c;
        EXPECT_STREQ(leptris_element_name(root), "html") << c;
        LeptrisElement first =
            (LeptrisElement)leptris_node_first_child((LeptrisNodeRef)root);
        LeptrisElement second = first
            ? (LeptrisElement)leptris_node_next_sibling((LeptrisNodeRef)first)
            : nullptr;
        ASSERT_NE(first, nullptr) << c;
        ASSERT_NE(second, nullptr) << c;
        EXPECT_STREQ(leptris_element_name(first), "head") << c;
        EXPECT_STREQ(leptris_element_name(second), "body") << c;
        EXPECT_EQ(leptris_node_next_sibling((LeptrisNodeRef)second),
                  nullptr) << c;
        leptris_document_free(d);
    }
}

TEST(HtmlTwoModes, StructuralTagsKeepContentPlacement) {
    /* Bare head/body tags with content: title lifts to head, the
     * rest stays in body — no nested head/body elements. */
    LeptrisStatus st = LEPTRIS_OK;
    const char in[] = "<head><title>t</title></head><body><p>x</p>";
    LeptrisDocument d = leptris_parse_html_string(in, sizeof(in) - 1, &st);
    ASSERT_NE(d, nullptr);
    LeptrisElement root = leptris_document_root(d);
    LeptrisElement head =
        (LeptrisElement)leptris_node_first_child((LeptrisNodeRef)root);
    LeptrisElement body =
        (LeptrisElement)leptris_node_next_sibling((LeptrisNodeRef)head);
    ASSERT_NE(head, nullptr);
    ASSERT_NE(body, nullptr);
    EXPECT_STREQ(leptris_element_name(head), "head");
    EXPECT_STREQ(leptris_element_name(body), "body");
    EXPECT_STREQ(leptris_element_name(
                     (LeptrisElement)leptris_node_first_child(
                         (LeptrisNodeRef)head)),
                 "title");
    EXPECT_STREQ(leptris_element_name(
                     (LeptrisElement)leptris_node_first_child(
                         (LeptrisNodeRef)body)),
                 "p");
    leptris_document_free(d);
}

/* #659: <template> placement — leading (before any structural
 * <body>) is a HEAD element; after a structural <body> or inside
 * content it stays in place; inside an explicit <html> the same
 * head/body split applies. Comparator flattens the WHATWG content
 * marker, so children sit directly under <template>. */
TEST(HtmlTwoModes, TemplatePlacement) {
    auto first_name = [](LeptrisDocument d, const char* path) {
        LeptrisXPathResult r = leptris_xpath_eval(d, nullptr, path);
        if (!r) return std::string("(null)");
        char* s = leptris_xpath_result_string(r);
        std::string out = s ? s : "";
        leptris_free_string(s);
        leptris_xpath_result_free(r);
        return out;
    };
    struct {
        const char* in;
        const char* head_tpl;   /* name(/html/head/child1) or "" */
        const char* body_tpl;   /* name(/html/body/child1) or "" */
    } cases[] = {
        {"<body><template>Hello</template>", "", "template"},
        {"<template>Hello</template>", "template", ""},
        {"<html><template>Hello</template>", "template", ""},
        {"<div><template></div>Hello", "", "div"},
    };
    for (const auto& c : cases) {
        LeptrisStatus st = LEPTRIS_OK;
        LeptrisDocument d =
            leptris_parse_html_string(c.in, strlen(c.in), &st);
        ASSERT_NE(d, nullptr) << c.in;
        if (c.head_tpl[0]) {
            EXPECT_EQ(first_name(d, "name(/html/head/*[1])"),
                      c.head_tpl) << c.in;
        } else {
            EXPECT_EQ(first_name(d, "count(/html/head/*)"), "0") << c.in;
        }
        if (c.body_tpl[0]) {
            EXPECT_EQ(first_name(d, "name(/html/body/*[1])"),
                      c.body_tpl) << c.in;
        }
        leptris_document_free(d);
    }
}


/* ---- #659 foreign content (WHATWG 12.2.6.5) ----
 *
 * svg/math roots and their descendants carry the SVG / MathML
 * namespace URI (exposed via namespace-uri()), element names get
 * the SVG camelCase adjustment (foreignObject, viewBox), HTML
 * integration points resume HTML rules, breakout tags pop the
 * foreign scope, CDATA in foreign content is TEXT, and a select
 * swallows foreign start tags. The html4 entry keeps everything
 * plain-HTML (libxml2 knows no foreign content). */

static std::string XQ(LeptrisDocument d, const char* path) {
    LeptrisXPathResult r = leptris_xpath_eval(d, nullptr, path);
    if (!r) return std::string("(null)");
    char* s = leptris_xpath_result_string(r);
    std::string out = s ? s : "";
    leptris_free_string(s);
    leptris_xpath_result_free(r);
    return out;
}

static LeptrisDocument ForeignDoc(const char* in) {
    LeptrisStatus st = LEPTRIS_OK;
    LeptrisDocument d = leptris_parse_html_string(in, strlen(in), &st);
    return d;
}

TEST(HtmlForeign, SvgRootAndChildrenCarryNamespace) {
    LeptrisDocument d = ForeignDoc(
        "<svg viewBox=\"0 0 1 1\"><circle/></svg>");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(XQ(d, "namespace-uri(/html/body/*[name(.)=\"svg\"])"),
              "http://www.w3.org/2000/svg");
    EXPECT_EQ(XQ(d, "count(/html/body/*[name(.)=\"svg\"]/*[namespace-uri(.)="
                    "'http://www.w3.org/2000/svg'])"),
              "1");
    /* Self-closed circle: no children, svg has exactly it. */
    EXPECT_EQ(XQ(d, "count(/html/body/*[name(.)=\"svg\"]/*[name(.)=\"circle\"]/*)"), "0");
    /* Attribute-name case adjustment (viewBox, not viewbox). */
    EXPECT_EQ(XQ(d, "string(/html/body/*[name(.)=\"svg\"]/@viewBox)"), "0 0 1 1");
    leptris_document_free(d);
}

TEST(HtmlForeign, SvgCamelCaseNameAdjustment) {
    LeptrisDocument d = ForeignDoc(
        "<svg><foreignObject>x</foreignObject></svg>");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(XQ(d, "name(/html/body/*[name(.)=\"svg\"]/*[1])"), "foreignObject");
    EXPECT_EQ(XQ(d, "string(/html/body/*[name(.)=\"svg\"]/*[name(.)=\"foreignObject\"])"), "x");
    leptris_document_free(d);
}

TEST(HtmlForeign, MathmlNamespaceAndTextIntegration) {
    LeptrisDocument d = ForeignDoc("<math><mi><b>x</b></mi></math>");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(XQ(d, "namespace-uri(/html/body/*[name(.)=\"math\"])"),
              "http://www.w3.org/1998/Math/MathML");
    /* mi is a MathML text integration point: <b> is HTML inside. */
    EXPECT_EQ(XQ(d, "namespace-uri(/html/body/*[name(.)=\"math\"]/*[name(.)=\"mi\"]/*[name(.)=\"b\"])"), "");
    EXPECT_EQ(XQ(d, "string(/html/body/*[name(.)=\"math\"]/*[name(.)=\"mi\"]/*[name(.)=\"b\"])"), "x");
    leptris_document_free(d);
}

TEST(HtmlForeign, ForeignObjectIsHtmlIntegrationPoint) {
    LeptrisDocument d = ForeignDoc(
        "<svg><foreignObject><div>a</div></foreignObject></svg>");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(XQ(d, "name(/html/body/*[name(.)=\"svg\"]/*[1])"), "foreignObject");
    EXPECT_EQ(XQ(d, "string(/html/body/*[name(.)=\"svg\"]/*[name(.)=\"foreignObject\"]/*[name(.)=\"div\"])"), "a");
    EXPECT_EQ(XQ(d, "namespace-uri(/html/body/*[name(.)=\"svg\"]/*[name(.)=\"foreignObject\"]/*[name(.)=\"div\"])"),
              "");
    leptris_document_free(d);
}

TEST(HtmlForeign, BreakoutTagPopsForeignScope) {
    LeptrisDocument d = ForeignDoc("<svg><p>x</p></svg>");
    ASSERT_NE(d, nullptr);
    /* <p> is a breakout tag: svg scope popped, p is an HTML
     * sibling AFTER the (now childless) svg. */
    EXPECT_EQ(XQ(d, "count(/html/body/*)"), "2");
    EXPECT_EQ(XQ(d, "count(/html/body/*[name(.)=\"svg\"]/*)"), "0");
    EXPECT_EQ(XQ(d, "string(/html/body/p)"), "x");
    leptris_document_free(d);
}

TEST(HtmlForeign, ForeignEndTagScopesByMatch) {
    LeptrisDocument d = ForeignDoc("<svg><g>a</g>b</svg>");
    ASSERT_NE(d, nullptr);
    /* </g> closes g; text b stays INSIDE svg (</svg> closes it). */
    EXPECT_EQ(XQ(d, "string(/html/body/*[name(.)=\"svg\"]/*[name(.)=\"g\"])"), "a");
    EXPECT_EQ(XQ(d, "string(/html/body/*[name(.)=\"svg\"])"), "ab");
    EXPECT_EQ(XQ(d, "count(/html/body/*)"), "1");
    leptris_document_free(d);
}

TEST(HtmlForeign, CdataInForeignContentIsText) {
    LeptrisDocument d = ForeignDoc("<svg><![CDATA[x<y]]></svg>");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(XQ(d, "string(/html/body/*[name(.)=\"svg\"]/text())"), "x<y");
    leptris_document_free(d);
}

TEST(HtmlForeign, SelectSwallowsForeignStartTag) {
    LeptrisDocument d = ForeignDoc("<select><svg></svg></select>");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(XQ(d, "count(/html/body/select/*)"), "0");
    leptris_document_free(d);
}

TEST(HtmlForeign, Html4EntryStaysPlainHtml) {
    LeptrisStatus st = LEPTRIS_OK;
    const char in[] = "<svg><g>a</g></svg>";
    LeptrisDocument d = leptris_parse_html4_string(in, sizeof(in) - 1, &st);
    ASSERT_NE(d, nullptr);
    /* libxml2/Nokogiri shape: plain lowercase names, no ns. */
    EXPECT_EQ(XQ(d, "string(/html/body/*[name(.)=\"svg\"]/*[name(.)=\"g\"])"), "a");
    EXPECT_EQ(XQ(d, "namespace-uri(/html/body/*[name(.)=\"svg\"])"), "");
    EXPECT_EQ(XQ(d, "namespace-uri(/html/body/*[name(.)=\"svg\"]/*[name(.)=\"g\"])"), "");
    leptris_document_free(d);
}


/* ---- #659 in-table insertion modes (WHATWG only) ----
 *
 * WHATWG 12.2.6.4: cells/rows/cols arriving where a wrapper is
 * missing get it synthesized — tr under table implies tbody;
 * td/th under table implies tbody>tr; col under table implies
 * colgroup. The html4 entry keeps libxml2/Nokogiri's
 * no-implied-tbody shape (the committed parity reference). */
TEST(HtmlTwoModes, WhatwgImpliesTbodyForBareRows) {
    EXPECT_EQ(Html("<table><tr><td>a</table>"),
              "<table><tbody><tr><td>a</td></tr></tbody></table>");
    EXPECT_EQ(Html4("<table><tr><td>a</table>"),
              "<table><tr><td>a</td></tr></table>");
}

TEST(HtmlTwoModes, WhatwgImpliesRowForBareCells) {
    EXPECT_EQ(Html("<table><td>x<td>y</table>"),
              "<table><tbody><tr><td>x</td><td>y</td>"
              "</tr></tbody></table>");
    EXPECT_EQ(Html4("<table><td>x<td>y</table>"),
              "<table><td>x</td><td>y</td></table>");
}

TEST(HtmlTwoModes, WhatwgImpliesColgroupForBareCols) {
    EXPECT_EQ(Html("<table><col></table>"),
              "<table><colgroup><col/></colgroup></table>");
    EXPECT_EQ(Html4("<table><col></table>"),
              "<table><col/></table>");
}

TEST(HtmlTwoModes, WhatwgSecondRowReusesTbody) {
    EXPECT_EQ(Html("<table><tr><td>a<tr><td>b</table>"),
              "<table><tbody><tr><td>a</td></tr>"
              "<tr><td>b</td></tr></tbody></table>");
}

TEST(HtmlTwoModes, WhatwgExplicitTbodyNotDuplicated) {
    EXPECT_EQ(Html("<table><tbody><tr><td>a</table>"),
              "<table><tbody><tr><td>a</td></tr></tbody></table>");
}


/* ---- #659 tests19 insertion-mode edges (WHATWG only) ---- */

/* Heading END tags pop through the NEAREST heading (any of
 * h1-h6), not just the same name — </h1> with an open h3 above
 * pops to the h3's position; content after lands in the h3's
 * parent. html4 keeps exact-name matching. */
TEST(HtmlTwoModes, HeadingEndTagPopsNearestHeading) {
    EXPECT_EQ(Html("<h1><div><h3><span></h1>foo"),
              "<h1><div><h3><span/></h3>foo</div></h1>");
    EXPECT_EQ(Html("<h3><li>abc</h2>foo"),
              "<h3><li>abc</li></h3>foo");
    EXPECT_EQ(Html4("<h1><div><h3><span></h1>foo"),
              "<h1><div><h3><span/></h3></div></h1>foo");
}

/* Ruby annotation structure: rb/rt/rp close an open p like any
 * block; rt/rp close an open rb/rt/rp so annotations become
 * siblings; rb closes rb. */
TEST(HtmlTwoModes, RubyAnnotationsNestAsSiblings) {
    EXPECT_EQ(Html("<ruby>a<rb>b<rt></ruby>"),
              "<ruby>a<rb>b</rb><rt/></ruby>");
    EXPECT_EQ(Html("<ruby><p><rp>x"),
              "<ruby><p/><rp>x</rp></ruby>");
    EXPECT_EQ(Html("<ruby><rb>a<rb>b</ruby>"),
              "<ruby><rb>a</rb><rb>b</rb></ruby>");
}

/* plaintext closes p and consumes the rest of the input as raw
 * text (RAWTEXT to EOF). */
TEST(HtmlTwoModes, PlaintextClosesPAndEatsRest) {
    EXPECT_EQ(Html("<p><plaintext><b>x"),
              "<p/><plaintext>&lt;b&gt;x</plaintext>");
}

/* html5lib's tree serialization writes comments as
 * "<!-- data -->" with wrapping spaces — the comparator strips
 * that convention (our DOM keeps the data verbatim). */
TEST(HtmlTwoModes, CommentInteriorWhitespacePreserved) {
    EXPECT_EQ(Html("<table>abc<!--foo-->"),
              "abc<table><!--foo--></table>");
}


/* ---- #659 frameset mode + structural-tag attributes (WHATWG) ---- */

/* A <frameset> arriving before any body content REPLACES the
 * would-be body: html > [head, frameset]. Content after is
 * frameset content (<frame> is void). html4 keeps libxml2's
 * ordinary-element shape. */
TEST(HtmlTwoModes, FramesetReplacesEmptyBody) {
    /* The frameset is html's SECOND child — no body exists. */
    LeptrisStatus st = LEPTRIS_OK;
    const char in[] = "<frameset><frame src=a>";
    LeptrisDocument d = leptris_parse_html_string(in, sizeof(in) - 1, &st);
    ASSERT_NE(d, nullptr);
    LeptrisXPathResult r = leptris_xpath_eval(
        d, nullptr, "count(/html/body)");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(leptris_xpath_result_number(r), 0.0);
    leptris_xpath_result_free(r);
    r = leptris_xpath_eval(d, nullptr,
                           "name(/html/*[2])");
    ASSERT_NE(r, nullptr);
    char* ns_ = leptris_xpath_result_string(r);
    EXPECT_STREQ(ns_ ? ns_ : "", "frameset");
    leptris_free_string(ns_);
    leptris_xpath_result_free(r);
    leptris_document_free(d);
    /* html4 entry: ordinary elements inside a body. */
    LeptrisDocument d4 = leptris_parse_html4_string(in, sizeof(in) - 1, &st);
    ASSERT_NE(d4, nullptr);
    /* html4: ordinary elements inside the body (html > body). */
    LeptrisXPathResult r4 = leptris_xpath_eval(
        d4, nullptr, "name(/html/*[1])");
    ASSERT_NE(r4, nullptr);
    char* n4 = leptris_xpath_result_string(r4);
    EXPECT_STREQ(n4 ? n4 : "", "body");
    leptris_free_string(n4);
    leptris_xpath_result_free(r4);
    leptris_document_free(d4);
}

/* <frameset> AFTER body content is ignored (body wins). */
TEST(HtmlTwoModes, FramesetAfterContentIsIgnored) {
    EXPECT_EQ(Html("<p>x<frameset></frameset>"),
              "<p>x</p>");
}

/* Structural <head> start tags keep their ATTRIBUTES on the
 * synthesized head. */
TEST(HtmlTwoModes, StructuralHeadKeepsAttributes) {
    LeptrisStatus st = LEPTRIS_OK;
    const char in[] = "<head profile=\"p1\"><title>t";
    LeptrisDocument d = leptris_parse_html_string(in, sizeof(in) - 1, &st);
    ASSERT_NE(d, nullptr);
    LeptrisXPathResult r = leptris_xpath_eval(
        d, nullptr, "string(/html/head/@profile)");
    ASSERT_NE(r, nullptr);
    char* s2 = leptris_xpath_result_string(r);
    EXPECT_STREQ(s2 ? s2 : "", "p1");
    leptris_free_string(s2);
    leptris_xpath_result_free(r);
    leptris_document_free(d);
}

TEST(HtmlTwoModes, StructuralBodyAttributesLandOnBody) {
    LeptrisStatus st = LEPTRIS_OK;
    const char in[] = "<body bgcolor=\"red\" onload='f()'><p>x";
    LeptrisDocument d = leptris_parse_html_string(in, sizeof(in) - 1, &st);
    ASSERT_NE(d, nullptr);
    LeptrisXPathResult r = leptris_xpath_eval(
        d, nullptr, "string(/html/body/@bgcolor)");
    ASSERT_NE(r, nullptr);
    char* s = leptris_xpath_result_string(r);
    EXPECT_STREQ(s ? s : "", "red");
    leptris_free_string(s);
    leptris_xpath_result_free(r);
    leptris_document_free(d);
}


/* ---- #659 "in head noscript" (scripting off) ----
 *
 * <noscript> opened in head phase: comments and head content
 * stay inside; the first body-ish token pops it (reprocessed at
 * body level); <html> attrs merge onto the html element;
 * </br> means <br>; <noframes> is RAWTEXT. */
TEST(HtmlTwoModes, NoscriptInHeadPopsOnBodyTag) {
    LeptrisStatus st = LEPTRIS_OK;
    const char in[] = "<head><noscript><p>x</noscript>";
    LeptrisDocument d = leptris_parse_html_string(in, sizeof(in) - 1, &st);
    ASSERT_NE(d, nullptr);
    LeptrisXPathResult r = leptris_xpath_eval(
        d, nullptr, "name(/html/head/*[1])");
    ASSERT_NE(r, nullptr);
    char* s = leptris_xpath_result_string(r);
    EXPECT_STREQ(s ? s : "", "noscript");
    leptris_free_string(s);
    leptris_xpath_result_free(r);
    r = leptris_xpath_eval(d, nullptr,
                           "count(/html/head/noscript/*)");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(leptris_xpath_result_number(r), 0.0);
    leptris_xpath_result_free(r);
    r = leptris_xpath_eval(d, nullptr, "string(/html/body/p)");
    ASSERT_NE(r, nullptr);
    s = leptris_xpath_result_string(r);
    EXPECT_STREQ(s ? s : "", "x");
    leptris_free_string(s);
    leptris_xpath_result_free(r);
    leptris_document_free(d);
}

TEST(HtmlTwoModes, NoscriptInHeadKeepsComments) {
    LeptrisStatus st = LEPTRIS_OK;
    const char in[] = "<head><noscript><!--foo--></noscript>";
    LeptrisDocument d = leptris_parse_html_string(in, sizeof(in) - 1, &st);
    ASSERT_NE(d, nullptr);
    LeptrisXPathResult r = leptris_xpath_eval(
        d, nullptr, "count(/html/head/noscript/comment())");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(leptris_xpath_result_number(r), 1.0);
    leptris_xpath_result_free(r);
    leptris_document_free(d);
}

TEST(HtmlTwoModes, NoscriptHtmlAttrsMerge) {
    LeptrisStatus st = LEPTRIS_OK;
    const char in[] = "<head><noscript><html class=foo></noscript>";
    LeptrisDocument d = leptris_parse_html_string(in, sizeof(in) - 1, &st);
    ASSERT_NE(d, nullptr);
    LeptrisXPathResult r = leptris_xpath_eval(
        d, nullptr, "string(/html/@class)");
    ASSERT_NE(r, nullptr);
    char* s = leptris_xpath_result_string(r);
    EXPECT_STREQ(s ? s : "", "foo");
    leptris_free_string(s);
    leptris_xpath_result_free(r);
    leptris_document_free(d);
}

TEST(HtmlTwoModes, EndBrMeansBrStart) {
    LeptrisStatus st = LEPTRIS_OK;
    const char in[] = "<div>a</br>";
    LeptrisDocument d = leptris_parse_html_string(in, sizeof(in) - 1, &st);
    ASSERT_NE(d, nullptr);
    LeptrisXPathResult r = leptris_xpath_eval(
        d, nullptr, "count(/html/body/div/br)");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(leptris_xpath_result_number(r), 1.0);
    leptris_xpath_result_free(r);
    leptris_document_free(d);
}

TEST(HtmlTwoModes, NoframesIsRawText) {
    LeptrisStatus st = LEPTRIS_OK;
    const char in[] = "<noframes>XXX</noscript></noframes>";
    LeptrisDocument d = leptris_parse_html_string(in, sizeof(in) - 1, &st);
    ASSERT_NE(d, nullptr);
    LeptrisXPathResult r = leptris_xpath_eval(
        d, nullptr, "string(/html/head/noframes)");
    ASSERT_NE(r, nullptr);
    char* s = leptris_xpath_result_string(r);
    EXPECT_STREQ(s ? s : "", "XXX</noscript>");
    leptris_free_string(s);
    leptris_xpath_result_free(r);
    leptris_document_free(d);
}


/* ---- #659 character-reference decoding (WHATWG 12.2.5.72-78) ----
 *
 * Legacy references decode WITHOUT the semicolon (longest
 * prefix); in attributes a missing ';' followed by '=' or an
 * alphanumeric stays literal; numeric references decode with or
 * without the ';'. */
TEST(HtmlParse, LegacyEntityDecodesWithoutSemicolon) {
    EXPECT_EQ(Html("FOO&gtBAR"), "FOO&gt;BAR");
    EXPECT_EQ(Html("FOO&amp x"), "FOO&amp; x");
}

TEST(HtmlParse, LongestPrefixMatch) {
    /* "notit;" is not an entity; the longest match "not" wins. */
    EXPECT_EQ(Html("I&apos;m &notit; I tell you"),
              "I'm \xC2\xACit; I tell you");
}

TEST(HtmlParse, AttrEntityLiteralGuard) {
    LeptrisStatus st = LEPTRIS_OK;
    const char in[] = "<div bar=\"ZZ&pound_id=23\" b2=\"ZZ&pound=23\""
                      " b3=\"ZZ&gt YY\">";
    LeptrisDocument d = leptris_parse_html_string(in, sizeof(in) - 1, &st);
    ASSERT_NE(d, nullptr);
    LeptrisXPathResult r = leptris_xpath_eval(
        d, nullptr, "string(/html/body/div/@bar)");
    ASSERT_NE(r, nullptr);
    char* s = leptris_xpath_result_string(r);
    EXPECT_STREQ(s ? s : "", "ZZ\xC2\xA3_id=23");
    leptris_free_string(s);
    leptris_xpath_result_free(r);
    r = leptris_xpath_eval(d, nullptr, "string(/html/body/div/@b2)");
    ASSERT_NE(r, nullptr);
    s = leptris_xpath_result_string(r);
    EXPECT_STREQ(s ? s : "", "ZZ&pound=23");
    leptris_free_string(s);
    leptris_xpath_result_free(r);
    r = leptris_xpath_eval(d, nullptr, "string(/html/body/div/@b3)");
    ASSERT_NE(r, nullptr);
    s = leptris_xpath_result_string(r);
    EXPECT_STREQ(s ? s : "", "ZZ> YY");
    leptris_free_string(s);
    leptris_xpath_result_free(r);
    leptris_document_free(d);
}

TEST(HtmlParse, NumericRefDecodesWithoutSemicolon) {
    EXPECT_EQ(Html("FOO&#41BAR"), "FOO)BAR");
    EXPECT_EQ(Html("FOO&#x41BAR"), "FOO\xE4\x86\xBA" "R");
}


/* ---- #659 comment tokenizer edges (WHATWG 12.2.5.x) ---- */
TEST(HtmlParse, CommentCloseForms) {
    EXPECT_EQ(Html("FOO<!-- BAR --!>BAZ"), "FOO<!-- BAR -->BAZ");
    EXPECT_EQ(Html("FOO<!-- BAR -- <QUX> -- MUX --!>BAZ"),
              "FOO<!-- BAR -- <QUX> -- MUX -->BAZ");
    /* EOF inside a comment: data runs to the end, verbatim. */
    EXPECT_EQ(Html("FOO<!-- BAR --!"), "FOO<!-- BAR --!-->");
}

TEST(HtmlParse, EmptyCommentForms) {
    EXPECT_EQ(Html("FOO<!--->BAZ"), "FOO<!---->BAZ");
    EXPECT_EQ(Html("FOO<!-->BAZ"), "FOO<!---->BAZ");
}

TEST(HtmlParse, AdoptionAgencyMisnest) {
    /* WHATWG 13.2.6.4.7 adoption agency (html5lib adoption01.dat
     * + spec walkthroughs 13.2.10.1/.2): a formatting end tag
     * whose scope holds a special element adopts the furthest
     * block out of the formatting element and re-opens a clone
     * inside it; with no block it pops through and the active
     * formatting list reconstructs at the next insertion. */
    EXPECT_EQ(Html("<a><p></a></p>"), "<a/><p><a/></p>");
    EXPECT_EQ(Html("<a>1<p>2</a>3</p>"), "<a>1</a><p><a>2</a>3</p>");
    EXPECT_EQ(Html("<a>1<button>2</a>3</button>"),
              "<a>1</a><button><a>2</a>3</button>");
    EXPECT_EQ(Html("<a>1<b>2</a>3</b>"), "<a>1<b>2</b></a><b>3</b>");
    EXPECT_EQ(Html("<a>1<div>2<div>3</a>4</div>5</div>"),
              "<a>1</a><div><a>2</a><div><a>3</a>4</div>5</div>");
    EXPECT_EQ(Html("<p>1<b>2<i>3</b>4</i>5</p>"),
              "<p>1<b>2<i>3</i></b><i>4</i>5</p>");
}

TEST(HtmlParse, ForeignIntegrationPointsAreScopeBoundaries) {
    /* WHATWG 13.2.4.2 "in scope": MathML text integration points
     * (mi/mo/mn/ms/mtext/annotation-xml) and SVG integration
     * points (foreignObject/desc/title) terminate every scope
     * walk (html5lib tests10:34-37,7). Consequences: a block
     * start inside an integration point does NOT close an outer
     * p; a breakout start pops only down TO the integration
     * point; an HTML end tag with an integration point between
     * current node and target is ignored. */
    EXPECT_EQ(Html("<svg><desc><svg><ul>a"),
              "<svg><desc><svg/><ul>a</ul></desc></svg>");
    EXPECT_EQ(Html("<p><svg><desc><p>"), "<p><svg><desc><p/></desc></svg></p>");
    EXPECT_EQ(Html("<p><svg><title><p>"), "<p><svg><title><p/></title></svg></p>");
    EXPECT_EQ(Html("<div><svg><path><foreignObject><math></div>a"),
              "<div><svg><path><foreignObject><math>a</math></foreignObject>"
              "</path></svg></div>");
    EXPECT_EQ(Html("<div><svg><path><foreignObject><p></div>a"),
              "<div><svg><path><foreignObject><p>a</p></foreignObject>"
              "</path></svg></div>");
    /* In-select swallows foreign/block start tags as ignorable —
     * their text content joins the select's text (tests10:17/18). */
    EXPECT_EQ(Html("<body><table><select><svg><g>foo</g><g>bar</g>"
                   "<p>baz</table><p>quux"),
              "<select>foobarbaz</select><table/><p>quux</p>");
}

TEST(HtmlParse, FramesetAndInTableClearStack) {
    /* WHATWG "in frameset" (13.2.6.4.18): only frameset/frame/
     * noframes content is live — everything else drops
     * (html5lib tests10:21/22). And "in table" cell/row starts
     * CLEAR THE STACK BACK TO TABLE CONTEXT first — stray open
     * elements above the table (a foster-parented <a>) must not
     * swallow the synthesized cell (adoption01:11): the trailing
     * text then fosters BEFORE the table inside a reconstructed
     * formatting clone. */
    EXPECT_EQ(Html("<frameset><svg><g></g><g></g><p><span>"),
              "<html><head/><frameset/></html>");
    EXPECT_EQ(Html("<table><a>1<td>2</td>3</table>"),
              "<a>1</a><a>3</a><table><tbody><tr><td>2</td></tr>"
              "</tbody></table>");
}

TEST(HtmlParse, BogusMarkupEdgesAndHeadingSelfClose) {
    /* WHATWG tokenizer tails (html5lib tests1:38-49): eof before
     * a tag name emits the pending characters as text; an invalid
     * first tag/comment char makes a bogus comment (<? makes
     * "?"+data — html5lib has no PI tokenizer). Headings
     * self-close: a heading start pops a current heading
     * (tests1:22/95). html4 keeps libxml2 PI/bogus shapes. */
    EXPECT_EQ(Html("</"), "&lt;/");
    EXPECT_EQ(Html("</#"), "<!--#--><html><head/><body/></html>");
    EXPECT_EQ(Html("<?"), "<!--?--><html><head/><body/></html>");
    EXPECT_EQ(Html("<?#"), "<!--?#--><html><head/><body/></html>");
    EXPECT_EQ(Html("<!"), "<!----><html><head/><body/></html>");
    EXPECT_EQ(Html("<!#"), "<!--#--><html><head/><body/></html>");
    EXPECT_EQ(Html("<?COMMENT?>"),
              "<!--?COMMENT?--><html><head/><body/></html>");
    EXPECT_EQ(Html("<!COMMENT>"),
              "<!--COMMENT--><html><head/><body/></html>");
    EXPECT_EQ(Html("</ COMMENT >"),
              "<!-- COMMENT --><html><head/><body/></html>");
    EXPECT_EQ(Html("<?COM--MENT?>"),
              "<!--?COM--MENT?--><html><head/><body/></html>");
    EXPECT_EQ(Html("<!COM--MENT>"),
              "<!--COM--MENT--><html><head/><body/></html>");
    EXPECT_EQ(Html("</ COM--MENT >"),
              "<!-- COM--MENT --><html><head/><body/></html>");
    EXPECT_EQ(Html("<h1>Hello<h2>World"),
              "<h1>Hello</h1><h2>World</h2>");
    EXPECT_EQ(Html("<h1><h2>"), "<h1/><h2/>");
}

TEST(HtmlParse, ScriptDataEscapedStates) {
    /* WHATWG 13.2.5.15-.31: inside <script>, "<!--" enters the
     * escaped states; </script> closes only with a delimiter
     * after the name, "<script" re-enters double-escaped where
     * </script> only escapes one level, and --!>/--> drop back
     * out (html5lib tests16:38-48/64-72). */
    EXPECT_EQ(Html("<script><!--<script </scripta"),
              "<html><head><script>&lt;!--&lt;script &lt;/scripta"
              "</script></head><body/></html>");
    EXPECT_EQ(Html("<script><!--<script </script>"),
              "<html><head><script>&lt;!--&lt;script &lt;/script&gt;"
              "</script></head><body/></html>");
    EXPECT_EQ(Html("<script><!--<script></script><script></script>"
                   "</script>"),
              "<html><head><script>&lt;!--&lt;script&gt;&lt;/script&gt;"
              "&lt;script&gt;&lt;/script&gt;</script></head><body/>"
              "</html>");
    EXPECT_EQ(Html("<script><!--<script>--!></script>X"),
              "<html><head><script>&lt;!--&lt;script&gt;--!&gt;"
              "</script></head><body>X</body></html>");
}

TEST(HtmlParse, RcdataAndRawtextFamily) {
    /* WHATWG 13.2.6.2: title/textarea are RCDATA (entity-
     * decoding, no markup, first close tag ends); iframe/noembed/
     * xmp are raw text (html5lib tests16:81-99). */
    EXPECT_EQ(Html("<title><!--<title></title>--></title>"),
              "<html><head><title>&lt;!--&lt;title&gt;</title>"
              "</head><body>--&gt;</body></html>");
    EXPECT_EQ(Html("<textarea><!--<textarea></textarea>-->"
                   "</textarea>"),
              "<textarea>&lt;!--&lt;textarea&gt;</textarea>"
              "--&gt;");
    EXPECT_EQ(Html("<xmp><!--<xmp></xmp>--></xmp>"),
              "<xmp>&lt;!--&lt;xmp&gt;</xmp>--&gt;");
    EXPECT_EQ(Html("<noscript><iframe></noscript>X"),
              "<html><head><noscript/></head><body>"
              "<iframe>&lt;/noscript&gt;X</iframe></body></html>");
}

TEST(HtmlParse, NumericReferenceEndStates) {
    /* WHATWG 13.2.5.84: NUL, surrogate, and >0x10FFFF references
     * become U+FFFD; the C1 range remaps through the Windows-1252
     * table (0x80 -> euro sign); overflow digit strings become
     * U+FFFD even without a semicolon (html5lib entities01). */
    EXPECT_EQ(Html("FOO&#x0000;ZOO"), "FOO\xEF\xBF\xBDZOO");
    EXPECT_EQ(Html("FOO&#x0080;ZOO"), "FOO\xE2\x82\xACZOO");
    EXPECT_EQ(Html("FOO&#x0082;ZOO"), "FOO\xE2\x80\x9AZOO");
    EXPECT_EQ(Html("FOO&#x009F;ZOO"), "FOO\xC5\xB8ZOO");
    EXPECT_EQ(Html("FOO&#x0081;ZOO"), "FOO\xC2\x81ZOO");  /* no table row */
    EXPECT_EQ(Html("FOO&#xD800;ZOO"), "FOO\xEF\xBF\xBDZOO");
    EXPECT_EQ(Html("FOO&#x110000;ZOO"), "FOO\xEF\xBF\xBDZOO");
    EXPECT_EQ(Html("FOO&#11111111111"), "FOO\xEF\xBF\xBD");
    EXPECT_EQ(Html("FOO&#1111111111ZOO"), "FOO\xEF\xBF\xBDZOO");
}

TEST(HtmlParse, TemplateInsertion) {
    /* WHATWG "in template": end tags never pop past the nearest
     * template (the fence); structural html/body/head tags inside
     * template drop entirely, attrs NOT merged onto the outer
     * elements (html5lib template.dat:7/64-67/78-79). The
     * row/cell synthesis shapes need the per-template
     * insertion-mode stack — banked in the lane-14 ledger. */
    EXPECT_EQ(Html("<div><template></div>Hello"),
              "<div><template>Hello</template></div>");
    /* After the fence ignores </div>, the row nests in the
     * template content verbatim (template.dat:79 shape). */
    EXPECT_EQ(Html("<template><tr><td>Foo</td></tr></template>"),
              "<html><head><template><tr><td>Foo</td></tr>"
              "</template></head><body/></html>");
    EXPECT_EQ(Html("<body><template></div><tr><td>Foo</td>"
                   "</tr></template>"),
              "<template><tr><td>Foo</td></tr></template>");
    EXPECT_EQ(Html("<body a=b><template><div></div><body c=d>"
                   "<div></div></body></template>"),
              "<html><head/><body a=\"b\"><template><div/>"
              "<div/></template></body></html>");
}

TEST(HtmlParse, TemplateStartTagFence) {
    /* 13.2.4.2: an open template is a scope boundary — table-context
     * start tags arriving inside it are TEMPLATE CONTENT (the
     * template's own insertion mode owns them); the start-tag
     * close-stack never pops past the template. html5lib
     * template.dat:28/30 (thead stays in content), 32 (tr), 36
     * (bare td), 27 (td inside a thead's template). */
    EXPECT_EQ(Html("<table><template><thead></template></table>"),
              "<table><template><thead/></template></table>");
    EXPECT_EQ(Html("<table><template><tr></template></table>"),
              "<table><template><tr/></template></table>");
    EXPECT_EQ(Html("<table><template><td></template>"),
              "<table><template><td/></template></table>");
    EXPECT_EQ(Html("<table><thead><template><td></template></table>"),
              "<table><thead><template><td/></template></thead>"
              "</table>");
}

TEST(HtmlParse, TemplateFrameAndFramesetDrop) {
    /* 13.2.6.4.10 anything-else -> in-body: frame start tags are
     * ignored outright; frameset tokens inside a template vanish
     * (html5lib template.dat:41/67/93 — content stays empty). */
    EXPECT_EQ(Html("<template><frame></frame></frameset>"
                   "<frame></frame></template>"),
              "<html><head><template/></head><body/></html>");
    EXPECT_EQ(Html("<html a=b><template><frame></frame><html b=c>"
                   "<frame></frame></template>"),
              "<html a=\"b\"><head><template/></head><body/></html>");
    EXPECT_EQ(Html("<template><template><frame>"),
              "<html><head><template><template/></template>"
              "</head><body/></html>");
}

TEST(HtmlParse, TemplateColumnGroupDropsNonColTokens) {
    /* gumbo handle_in_column_group: with the template as current
     * node (not a colgroup), every token but a col start is a
     * parse error and ignored — colgroup, div and non-whitespace
     * text all vanish (html5lib template.dat:71/73/74/76). */
    EXPECT_EQ(Html("<body><template><col><colgroup>"),
              "<template><col/></template>");
    EXPECT_EQ(Html("<body><template><col><colgroup></template></body>"),
              "<template><col/></template>");
    EXPECT_EQ(Html("<body><template><col><div>"),
              "<template><col/></template>");
    EXPECT_EQ(Html("<body><template><col>Hello"),
              "<template><col/></template>");
}

TEST(HtmlParse, SelectTemplateCloseRestoresSelect) {
    /* </template> inside a select runs the in-head rules (not the
     * in-select ignore-everything gate): the select's insertion
     * point returns, so a following <option> is a select child
     * (html5lib template.dat:22). */
    EXPECT_EQ(Html("<select><template></template><option></select>"),
              "<select><template/><option/></select>");
}

TEST(HtmlParse, TemplateEndPopsThroughForeignContent) {
    /* An SVG-namespaced <template> is foreign content, not an html
     * template: </template> (in-head rules) pops through the whole
     * foreign stack down to the html template and resets, so the
     * trailing div is body content (html5lib template.dat:100). */
    EXPECT_EQ(Html("<template><svg><foo><template><foreignObject>"
                   "<div></template><div>"),
              "<html><head><template><svg><foo><template>"
              "<foreignObject><div/></foreignObject></template>"
              "</foo></svg></template></head><body><div/>"
              "</body></html>");
}

TEST(HtmlParse, TemplateInsertionModes) {
    /* The per-template insertion-mode machine (13.2.6.4.10 pushes
     * in-table/in-table-body/in-row; the reprocess chains decide
     * wrapping): after an explicit row closes, a cell gets an
     * IMPLIED tr (46); a cell without any row is bare and a tbody
     * in row/table-body context without an open section DROPS
     * (48/52); caption after a closed row drops (53); after a
     * closed section the mode is in-table: a row gets its implied
     * TBODY (69); a stray row inside body-mode content drops
     * (57). All shapes from the html5lib template.dat trees. */
    EXPECT_EQ(Html("<body><template><tr></tr><td></td></template>"),
              "<template><tr/><tr><td/></tr></template>");
    EXPECT_EQ(Html("<body><template><td></td><tbody><td></td>"
                   "</template>"),
              "<template><td/><td/></template>");
    EXPECT_EQ(Html("<body><template><tr></tr><tbody><tr></tr>"
                   "</template>"),
              "<template><tr/><tr/></template>");
    EXPECT_EQ(Html("<body><template><tr></tr><caption><tr></tr>"
                   "</template>"),
              "<template><tr/><tr/></template>");
    EXPECT_EQ(Html("<body><template><thead></thead>"
                   "<template><tr></tr></template>"
                   "<tr></tr><tfoot></tfoot></template>"),
              "<template><thead/><template><tr/></template>"
              "<tbody><tr/></tbody><tfoot/></template>");
    EXPECT_EQ(Html("<body><template><div><tr></tr></div></template>"),
              "<template><div/></template>");
}
