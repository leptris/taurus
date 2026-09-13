/* TODO.xslt-full/14 (#659) — html5lib tree-construction corpus
 * harness. Runs every non-fragment, non-script-on case in the
 * vendored snapshot through leptris_parse_html_string and compares
 * the resulting tree against the expected "| " tree.
 *
 * The pinned behavior of this parser is the libxml2/Nokogiri tree
 * shape (test_html.cpp characterization); the corpus uses the
 * WHATWG html5 tree shape. The delta is EXPECTED and MEASURED:
 * every divergence lands in the red-list (html5lib-redlist.txt in
 * the run directory), and the pass count is pinned as a floor so
 * regressions are falsifiable and each later slice (implied head
 * model, adoption agency, foster parenting, foreign content) must
 * only RAISE it.
 *
 * Comparison model (documented divergences that do NOT fail):
 * - an expected EMPTY <head> is optional (Nokogiri omits it);
 * - adjacent text nodes coalesce before comparison (the corpus
 *   merges character tokens).
 * Everything else — element order, names, namespaces (svg/math),
 * attributes, text content, comments, doctype name — is strict. */
#include <gtest/gtest.h>
extern "C" {
#include "leptris.h"
/* Internal (non-exported) accessor needed for the ns comparison;
 * the public surface exposes attribute/namespace-node URIs only. */
const char* leptris_element_get_namespace_uri(LeptrisElement elem);
}
#include <cstdio>
#include <algorithm>
#include <map>
#include "html5lib_cases.h"
#include <cstring>
#include <string>
#include <vector>

namespace {

struct XNode {
    enum Kind { ELEM, TEXT, COMMENT, DOCTYPE, DOC, MARKER } kind;
    std::string name;      /* element local / doctype name */
    std::string ns;        /* expected namespace URI ("" = none) */
    std::string text;      /* text / comment content */
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<XNode> children;
};

struct XCase {
    std::string id;        /* file.dat:N */
    std::string data;
    bool skip = false;     /* fragment pairs / script-on */
    std::string skip_why;
    XNode doc;             /* synthesized document level */
    XCase() { doc.kind = XNode::DOC; }
};

std::string Unescape(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '\\' || i + 1 >= s.size()) { r += s[i]; continue; }
        switch (s[i + 1]) {
            case 'n': r += '\n'; i++; break;
            case 't': r += '\t'; i++; break;
            case 'r': r += '\r'; i++; break;
            case '"': r += '"'; i++; break;
            case '\\': r += '\\'; i++; break;
            default: r += s[i]; break;
        }
    }
    return r;
}

/* Parse the "| "-prefixed expected document into a node tree.
 * Indent = 2 spaces per level; the first line's indent fixes the
 * document level. */
bool ParseExpected(const std::vector<std::string>& lines, XNode* doc,
                   std::string* err) {
    struct Frame { XNode* parent; size_t depth; };
    std::vector<Frame> stack;
    size_t base = (size_t)-1;
    for (const std::string& raw : lines) {
        if (raw.compare(0, 2, "| ") != 0 && raw != "|") {
            *err = "bad expected line: " + raw;
            return false;
        }
        std::string body = raw.size() > 1 ? raw.substr(2) : "";
        size_t depth = 0;
        while (depth < body.size() && body[depth] == ' ') depth++;
        if (depth % 2 != 0) { *err = "odd indent: " + raw; return false; }
        size_t level = depth / 2;
        if (base == (size_t)-1) base = level;
        if (level < base) { *err = "indent below base: " + raw; return false; }
        level -= base;

        XNode n;
        const std::string c = body.substr(depth);
        if (c.compare(0, 9, "<!DOCTYPE") == 0) {
            n.kind = XNode::DOCTYPE;
            size_t s = 9;
            while (s < c.size() && c[s] == ' ') s++;
            size_t e = s;
            while (e < c.size() && c[e] != ' ' && c[e] != '>') e++;
            n.name = c.substr(s, e - s);
        } else if (c.compare(0, 4, "<!--") == 0) {
            n.kind = XNode::COMMENT;
            size_t e = c.rfind("-->");
            n.text = c.substr(4, e == std::string::npos ? std::string::npos
                                                         : e - 4);
        } else if (!c.empty() && c[0] == '"') {
            n.kind = XNode::TEXT;
            size_t e = c.size();
            if (c.size() >= 2 && c[c.size() - 1] == '"') e = c.size() - 1;
            n.text = Unescape(c.substr(1, e - 1));
        } else if (!c.empty() && c[0] == '<') {
            n.kind = XNode::ELEM;
            size_t e = 1;
            while (e < c.size() && c[e] != ' ' && c[e] != '>') e++;
            std::string tok1 = c.substr(1, e - 1);
            size_t rest_off = e;
            /* Foreign content: "svg circle" / "math mrow" (and the
             * root foreign element itself, "svg svg" / "math math")
             * — the namespace marker is its OWN token, followed by
             * the local name, then attributes. */
            if ((tok1 == "svg" || tok1 == "math") && rest_off < c.size() &&
                c[rest_off] == ' ') {
                size_t s2 = rest_off + 1;
                size_t e2 = s2;
                while (e2 < c.size() && c[e2] != ' ' && c[e2] != '>' &&
                       c[e2] != '=')
                    e2++;
                /* '=' right after the second word means it is an
                 * ATTRIBUTE (plain <svg viewBox=...>), not the
                 * foreign local-name form (<svg g>). */
                if (e2 < c.size() && c[e2] == '=') {
                    n.name = tok1;
                } else {
                    n.name = c.substr(s2, e2 - s2);
                    n.ns = tok1 == "svg"
                               ? "http://www.w3.org/2000/svg"
                               : "http://www.w3.org/1998/Math/MathML";
                    rest_off = e2;
                }
            } else {
                n.name = tok1;
            }
            /* Attributes: name="quoted" or name=bare, up to '>'. */
            std::string rest = c.substr(rest_off);
            if (!rest.empty() && rest[0] == '>') rest.clear();
            size_t p = 0;
            while (p < rest.size()) {
                while (p < rest.size() && rest[p] == ' ') p++;
                size_t eq = rest.find('=', p);
                if (eq == std::string::npos) break;
                std::string an = rest.substr(p, eq - p);
                size_t q = eq + 1;
                std::string av;
                if (q < rest.size() && (rest[q] == '"' || rest[q] == '\'')) {
                    char term = rest[q];
                    size_t ve = rest.find(term, q + 1);
                    av = Unescape(rest.substr(q + 1, ve == std::string::npos
                                                     ? std::string::npos
                                                     : ve - q - 1));
                    p = ve == std::string::npos ? rest.size() : ve + 1;
                } else {
                    size_t ve = rest.find(' ', q);
                    av = rest.substr(q, ve == std::string::npos
                                            ? std::string::npos : ve - q);
                    p = ve == std::string::npos ? rest.size() : ve;
                }
                if (!an.empty()) n.attrs.emplace_back(an, av);
            }
        } else {
            /* Attribute-continuation lines and html5lib's template
             * `content` marker arrive here (no '<'/'"' prefix):
             * handled below before the push. */
            n.kind = XNode::MARKER;
        }

        /* html5lib wraps an element's attributes onto deeper lines
         * when the element line grows long ("id=\"foo\""), and
         * renders template CONTENT as a literal `content` node. */
        if (n.kind == XNode::MARKER && c != "content" &&
            c.find('=') != std::string::npos && !stack.empty()) {
            size_t eq = c.find('=');
            std::string an = c.substr(0, eq);
            std::string av;
            size_t q = eq + 1;
            if (q < c.size() && (c[q] == '"' || c[q] == '\'' )) {
                char term = c[q];
                size_t ve = c.rfind(term);
                av = Unescape(c.substr(q + 1, ve == std::string::npos
                                               ? std::string::npos : ve - q - 1));
            } else {
                av = c.substr(q);
            }
            /* Attach to the nearest open ELEMENT. */
            for (int f = (int)stack.size() - 1; f >= 0; f--) {
                XNode* t = stack[f].parent;
                if (t->kind == XNode::ELEM) { t->attrs.emplace_back(an, av); break; }
            }
            continue;
        }
        while (!stack.empty() && stack.back().depth >= level) stack.pop_back();
        if (stack.empty()) {
            doc->children.push_back(n);
            stack.push_back({&doc->children.back(), level});
        } else {
            stack.back().parent->children.push_back(n);
            stack.push_back({&stack.back().parent->children.back(), level});
        }
    }
    return true;
}

/* .dat scanner: directives at column 0; a case starts at #data and
 * completes at the next #data or EOF. Multiple #data sections mean
 * fragment-after-document — skipped (slice 1). */
std::vector<XCase> ScanFile(const std::string& path, const std::string& fname,
                            std::string* err, int truncate_at_nul) {
    std::vector<XCase> cases;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { *err = "open failed: " + path; return cases; }
    std::vector<std::string> lines;
    {
        /* Binary read + manual split for the WHATWG meter:
         * std::string(char*) would truncate every line at its
         * first NUL byte, and the unsafe-inputs corpus has real
         * NULs in #data lines (plain-text-unsafe.dat) — the
         * engine must see them. The Nokogiri-parity meter keeps
         * the truncating read: its reference trees were recorded
         * from that input shape and are byte-pinned. */
        if (!truncate_at_nul) {
            std::string all;
            char buf[8192];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
                all.append(buf, n);
            fclose(f);
            size_t pos = 0;
            while (pos <= all.size()) {
                size_t nl = all.find('\n', pos);
                std::string l = (nl == std::string::npos)
                                    ? all.substr(pos)
                                    : all.substr(pos, nl - pos);
                while (!l.empty() && (l.back() == '\r')) l.pop_back();
                lines.push_back(l);
                if (nl == std::string::npos) break;
                pos = nl + 1;
            }
        } else {
            char buf[8192];
            while (fgets(buf, sizeof(buf), f)) {
                std::string l = buf;
                while (!l.empty() &&
                       (l.back() == '\n' || l.back() == '\r'))
                    l.pop_back();
                lines.push_back(l);
            }
            fclose(f);
        }
    }
    size_t i = 0, case_no = 0;
    while (i < lines.size()) {
        if (lines[i].compare(0, 5, "#data") != 0) { i++; continue; }
        XCase c;
        c.id = fname + ":" + std::to_string(++case_no);
        std::vector<std::string> doclines;
        int data_sections = 0, in_doc = 0, script_on = 0;
        while (i < lines.size()) {
            const std::string& l = lines[i];
            if (!l.empty() && l[0] == '#') {
                if (l.compare(0, 5, "#data") == 0) {
                    data_sections++;
                    if (data_sections > 1) {
                        /* A #data after this case's #document: a
                         * BLANK line above = the next case; no blank
                         * = html5lib's fragment-pair continuation
                         * (two #data groups, one case). #original
                         * provenance lines (Nokogiri reference) are
                         * transparent to the boundary check. */
                        size_t b = i;
                        while (b > 0 &&
                               lines[b - 1].rfind("#original", 0) == 0) b--;
                        if (b > 0 && lines[b - 1].empty()) break;
                        c.skip = true;
                        c.skip_why = "fragment pair";
                        break;
                    }
                    i++;
                    while (i < lines.size() &&
                           !(lines[i].size() && lines[i][0] == '#')) {
                        c.data += lines[i];
                        c.data += '\n';
                        i++;
                    }
                    if (!c.data.empty()) c.data.pop_back();
                } else if (l.compare(0, 7, "#errors") == 0) {
                    i++;
                    while (i < lines.size() &&
                           !(lines[i].size() && lines[i][0] == '#')) i++;
                } else if (l.compare(0, 18, "#document-fragment") == 0) {
                    c.skip = true;
                    c.skip_why = "fragment mode";
                    i++;
                    while (i < lines.size() &&
                           !(lines[i].size() && lines[i][0] == '#')) i++;
                } else if (l.compare(0, 9, "#document") == 0) {
                    in_doc = 1;
                    i++;
                    while (i < lines.size() && lines[i].compare(0, 1, "|") == 0) {
                        doclines.push_back(lines[i]);
                        i++;
                    }
                } else if (l.compare(0, 10, "#script-on") == 0) {
                    script_on = 1;
                    i++;
                } else {
                    i++;   /* unknown directive: skip its line */
                }
            } else {
                i++;   /* directive body handled above */
            }
        }
        if (script_on) {
            c.skip = true;
            c.skip_why = "script-on";
        } else if (!in_doc) {
            c.skip = true;
            c.skip_why = "no document";
        } else if (!ParseExpected(doclines, &c.doc, err)) {
            c.skip = true;
            c.skip_why = "expected-parse: " + *err;
        }
        cases.push_back(c);
    }
    return cases;
}

/* --- our-side tree extraction --- */

struct ONode {
    int kind;              /* 0 elem, 1 text, 2 comment, 3 doctype */
    std::string name, ns, text;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<ONode> children;
};

void CollectElement(LeptrisElement e, ONode* out);
void CollectChildren(LeptrisNodeRef n, ONode* parent) {
    int t = leptris_node_get_type(n);
    ONode o;
    if (t == LEPTRIS_NODE_TYPE_ELEMENT) {
        CollectElement((LeptrisElement)n, &o);
    } else if (t == LEPTRIS_NODE_TYPE_TEXT || t == LEPTRIS_NODE_TYPE_CDATA) {
        o.kind = 1;
        const char* s = leptris_text_node_get_content(n);
        o.text = s ? s : "";
    } else if (t == LEPTRIS_NODE_TYPE_COMMENT) {
        o.kind = 2;
        const char* s = leptris_comment_node_get_content(n);
        o.text = s ? s : "";
    } else {
        return;
    }
    parent->children.push_back(o);
}

void CollectElement(LeptrisElement e, ONode* out) {
    out->kind = 0;
    const char* n = leptris_element_name(e);
    out->name = n ? n : "";
    const char* u = leptris_element_get_namespace_uri(e);
    out->ns = u ? u : "";
    for (LeptrisAttribute a = leptris_element_first_attribute(e); a;
         a = leptris_attribute_next(a)) {
        const char* an = leptris_attribute_get_name(a);
        const char* av = leptris_attribute_get_value(e, a);
        out->attrs.emplace_back(an ? an : "", av ? av : "");
    }
    for (LeptrisNodeRef c = leptris_node_first_child((LeptrisNodeRef)e); c;
         c = leptris_node_next_sibling(c)) {
        CollectChildren(c, out);
    }
}

ONode CollectDocument(LeptrisDocument d) {
    ONode doc;
    doc.kind = -1;   /* document level */
    LeptrisDoctype dt_handle = leptris_document_internal_subset(d);
    if (dt_handle) {
        const char* dn = leptris_doctype_get_root_name(dt_handle);
        if (dn && *dn) {
            ONode dt;
            dt.kind = 3;
            dt.name = dn;
            doc.children.push_back(dt);
        }
    }
    LeptrisElement root = leptris_document_root(d);
    if (root) {
        ONode rootn;
        CollectElement(root, &rootn);
        doc.children.push_back(rootn);
    }
    return doc;
}

/* Coalesce adjacent text children (order-preserving). */
void Coalesce(ONode* n) {
    std::vector<ONode> out;
    for (auto& c : n->children) {
        Coalesce(&c);
        if (c.kind == 1 && !out.empty() && out.back().kind == 1) {
            out.back().text += c.text;
        } else {
            out.push_back(c);
        }
    }
    n->children.swap(out);
}

/* Flatten html5lib's template `content` markers: our template is
 * an ordinary element whose children sit directly under it (the
 * pinned libxml2 characterization). For the html4/parity meter,
 * also drop an EMPTY expected <head> (Nokogiri omits it) — the
 * WHATWG meter compares heads for real (the engine always makes
 * one now). */
void NormalizeExpected(XNode* n, int optional_empty_head) {
    /* optional_empty_head != 0 marks the PARITY meter; the
     * html5lib (WHATWG) meter additionally strips the reference
     * serializer's comment convention ("<!-- data -->" wraps the
     * data with one space each side — the parity reference, like
     * our DOM, stores comment data verbatim). */
    int h5_dialect = !optional_empty_head;
    if (h5_dialect && n->kind == XNode::COMMENT &&
        n->text.size() >= 2 && n->text.front() == ' ' &&
        n->text.back() == ' ')
        n->text = n->text.substr(1, n->text.size() - 2);
    std::vector<XNode> out;
    for (auto& c : n->children) {
        NormalizeExpected(&c, optional_empty_head);
        if (optional_empty_head &&
            c.kind == XNode::ELEM && c.name == "head" && c.ns.empty() &&
            c.children.empty())
            continue;
        if (c.kind == XNode::MARKER) {
            for (auto& mc : c.children) out.push_back(mc);
            continue;
        }
        out.push_back(c);
    }
    n->children.swap(out);
}

bool Compare(const XNode& x, const ONode& o, std::string* why) {
    if (x.kind == XNode::DOC && o.kind == -1) {
        /* wrappers: children only */
    } else if (x.kind == XNode::ELEM && o.kind == 0) {
        if (x.name != o.name) {
            *why = "element name '" + o.name + "' != '" + x.name + "'";
            return false;
        }
        if (x.ns != o.ns) {
            *why = "namespace of <" + x.name + ">: '" + o.ns + "' != '" +
                   x.ns + "'";
            return false;
        }
        if (x.attrs.size() != o.attrs.size()) {
            *why = "attr count on <" + x.name + ">: " +
                   std::to_string(o.attrs.size()) + " != " +
                   std::to_string(x.attrs.size());
            return false;
        }
        for (auto& xa : x.attrs) {
            bool found = false;
            /* html5lib prints foreign attrs with a space
             * ("xlink href"); our DOM stores the colon form. */
            std::string xan = xa.first;
            size_t sp2 = xan.find(' ');
            if (sp2 != std::string::npos) xan[sp2] = ':';
            for (auto& oa : o.attrs) {
                if (oa.first == xan) {
                    if (oa.second != xa.second) {
                        *why = "@" + xa.first + "='" + oa.second +
                               "' != '" + xa.second + "'";
                        return false;
                    }
                    found = true;
                    break;
                }
            }
            if (!found) {
                *why = "missing @" + xa.first + " on <" + x.name + ">";
                return false;
            }
        }
    } else if (x.kind == XNode::TEXT && o.kind == 1) {
        if (x.text != o.text) {
            *why = "text '" + o.text.substr(0, 40) + "' != '" +
                   x.text.substr(0, 40) + "'";
            return false;
        }
    } else if (x.kind == XNode::COMMENT && o.kind == 2) {
        if (x.text != o.text) {
            *why = "comment '" + o.text.substr(0, 40) + "' != '" +
                   x.text.substr(0, 40) + "'";
            return false;
        }
    } else if (x.kind == XNode::DOCTYPE && o.kind == 3) {
        if (x.name != o.name) {
            *why = "doctype '" + o.name + "' != '" + x.name + "'";
            return false;
        }
    } else {
        *why = "node kind mismatch";
        return false;
    }
    if (x.children.size() != o.children.size()) {
        *why = "child count on <" + x.name + " text '" +
               x.text.substr(0, 20) + "': " + std::to_string(o.children.size())
               + " != " + std::to_string(x.children.size());
        return false;
    }
    for (size_t i = 0; i < x.children.size(); i++) {
        std::string w;
        if (!Compare(x.children[i], o.children[i], &w)) {
            *why = w + " [at <" + x.name + "> child " +
                   std::to_string(i) + "]";
            return false;
        }
    }
    return true;
}

}  // namespace

TEST(Html5LibCorpus, TreeConstruction) {
    const char* srcdir = getenv("HTML5LIB_SRC");
    std::string dir = srcdir && *srcdir
        ? srcdir
        : LEPTRIS_HTML5LIB_DIR;
    std::vector<std::string> files(kHtml5libDatFiles,
                                   kHtml5libDatFiles +
                                       sizeof(kHtml5libDatFiles) /
                                           sizeof(kHtml5libDatFiles[0]));
    size_t total = 0, passed = 0, skipped = 0, failed = 0;
    std::vector<std::string> failures;
    std::string err;
    std::map<std::string, size_t> skip_why;
    for (const std::string& fn : files) {
        std::vector<XCase> cs =
            ScanFile(dir + "/" + fn, fn, &err, 0);
        if (!err.empty() && cs.empty()) {
            ADD_FAILURE() << fn << ": " << err;
            continue;
        }
        for (auto& c : cs) {
            total++;
            if (c.skip) {
                skipped++;
                skip_why[c.skip_why.substr(0, 90)]++;
                continue;
            }
            LeptrisStatus st = LEPTRIS_OK;
            LeptrisDocument d = leptris_parse_html_string(
                c.data.c_str(), c.data.size(), &st);
            if (!d) {
                failed++;
                failures.push_back(c.id + ": parse failed");
                continue;
            }
            ONode ours = CollectDocument(d);
            Coalesce(&ours);
            NormalizeExpected(&c.doc, 0);
            std::string why;
            if (getenv("H5DBG") && c.id == "tests1.dat:1") {
                printf("[dbg] expected kind=%d nchild=%zu | ours kind=%d nchild=%zu\n",
                       (int)c.doc.kind, c.doc.children.size(),
                       (int)ours.kind, ours.children.size());
                for (auto& k : c.doc.children)
                    printf("[dbg] exp child kind=%d name=%s nchild=%zu\n",
                           (int)k.kind, k.name.c_str(), k.children.size());
                for (auto& k : ours.children)
                    printf("[dbg] our child kind=%d name=%s nchild=%zu\n",
                           (int)k.kind, k.name.c_str(), k.children.size());
            }
            if (Compare(c.doc, ours, &why)) {
                passed++;
            } else {
                failed++;
                failures.push_back(c.id + ": " + why.substr(0, 120));
            }
            leptris_document_free(d);
        }
    }
    /* Full red-list for slice planning (like open_cases.txt). */
    FILE* rl = fopen("html5lib-redlist.txt", "w");
    if (rl) {
        for (auto& f : failures) fprintf(rl, "%s\n", f.c_str());
        fclose(rl);
    }
    printf("[html5lib] total=%zu pass=%zu fail=%zu skip=%zu "
           "(red-list: html5lib-redlist.txt)\n",
           total, passed, failed, skipped);
    for (size_t i = 0; i < failures.size() && i < 20; i++)
        printf("  FAIL %s\n", failures[i].c_str());
    for (auto& w : skip_why)
        printf("  SKIP %zu x %s\n", w.second, w.first.c_str());
    EXPECT_GT(total, (size_t)1500);
    /* Falsifiable floor — each lane-14 slice must only raise it.
     * 1197 since the per-template insertion-mode machine
     * (13.2.6.4.10 wrap/drop transitions; +10). 1187 was the
     * in-template START-tag fence (13.2.4.2
     * scope boundary — table-context starts become template
     * content; +12 net). 1175 was the end-tag fence + structural
     * drop;
     * (1169 numeric-reference end states — the whole
     * entities01 family (+40; 1129 the RCDATA/rawtext family (1096 script-data
     * escaped states on the merged
     * bogus-markup base (1088 script states standalone, 1046
     * edges + heading self-close
     * (1038 in-table clear-stack + frameset drop, 1027
     * foreign integration-point scope boundaries +
     * in-select swallow (1006 full AFE, 983 simplified-AFE
     * replacement +
     * reconstruct, 933 in-head comments, 928 initial-mode
     * comments, 914 WHATWG character-reference decoding, 897
     * "in head noscript", 887 frameset mode + structural-tag
     * attributes, 859 tests19 insertion-mode edges, 792 in-table
     * wrapper synthesis, 755 MathML/SVG foreign content, 652
     * <template> placement, 623 structural head/body, 556
     * DOCTYPE, 295 adoption agency, 294 foster, 285 two-mode
     * split, 193 before it). */
    EXPECT_GE(passed, (size_t)1200);

    /* ---- Nokogiri PARITY (#659's actual target) ----
     *
     * The goal is libxml2/Nokogiri parity, not WHATWG conformance —
     * libxml2 itself fails most html5lib expectations. The
     * committed nokogiri-tree-tests.dat records what Nokogiri::HTML
     * produces for every runnable corpus case (same "| " dialect),
     * so this pass measures parity directly. Regenerate with
     * benchmarks/html/gen_nokogiri_reference.rb. */
    {
        std::vector<XCase> cs =
            ScanFile(dir + "/../../nokogiri-tree-tests.dat",
                     "nokogiri-tree-tests.dat", &err, 1);
        ASSERT_TRUE(cs.empty() || err.empty()) << err;
        size_t ptotal = 0, ppassed = 0, pfailed = 0, pskip = 0;
        std::vector<std::string> pfails;
        for (auto& c : cs) {
            ptotal++;
            if (c.skip) { pskip++; continue; }
            /* #659 two-mode split: parity is the html4-compat
             * entry's contract; leptris_parse_html_string is the
             * WHATWG engine (measured by the corpus above). */
            LeptrisStatus st = LEPTRIS_OK;
            LeptrisDocument d = leptris_parse_html4_string(
                c.data.c_str(), c.data.size(), &st);
            if (!d) {
                pfailed++;
                pfails.push_back(c.id + ": parse failed");
                continue;
            }
            ONode ours = CollectDocument(d);
            Coalesce(&ours);
            NormalizeExpected(&c.doc, 1);
            std::string why;
            if (Compare(c.doc, ours, &why)) {
                ppassed++;
            } else {
                pfailed++;
                pfails.push_back(c.id + ": " + why.substr(0, 120));
            }
            leptris_document_free(d);
        }
        FILE* rl = fopen("nokogiri-parity-redlist.txt", "w");
        if (rl) {
            for (auto& f : pfails) fprintf(rl, "%s\n", f.c_str());
            fclose(rl);
        }
        printf("[nokogiri-parity] total=%zu pass=%zu fail=%zu skip=%zu "
               "(red-list: nokogiri-parity-redlist.txt)\n",
               ptotal, ppassed, pfailed, pskip);
        for (size_t i = 0; i < pfails.size() && i < 15; i++)
            printf("  PARITY-FAIL %s\n", pfails[i].c_str());
        EXPECT_GT(ptotal, (size_t)1400);
        /* Parity floor — the true #659 metric; only raises. */
        EXPECT_GE(ppassed, (size_t)784);
    }
}
