/* lib/src/serialize/serialize.c - XML Serialization Implementation
 * Copyright (c) 2024, Ribose Inc.
 *
 * CRITICAL PRINCIPLES:
 * 1. Character-perfect output - must match input exactly
 * 2. No escaping in CDATA - CDATA content is literal
 * 3. Proper escaping elsewhere - text needs <>&"' escaped
 * 4. Document order - traverse children in correct order
 */

#include "serialize.h"
#include "../include/leptris.h"     /* LEPTRIS_API (visibility attribute) */
#include "../dom/node.h"            /* LeptrisNodeVTable + leptris_node_vtable_for */
#include "../leptris_internal.h"
#include "../common/entities.h"
#include "../common/string_view.h"
#include "../common/simd_text.h"
/* LeptrisSerializeOptions comes from leptris/types.h via leptris_internal.h's
 * pool.h include.  No local redefinition — it would conflict with the
 * public type. */
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <ctype.h>

/* Initial buffer capacity */
#define INITIAL_BUFFER_CAPACITY 1024

/* ============================================================================
 * Buffer Management
 * ============================================================================ */

SerializeBuffer* buffer_create(int indent_spaces) {
    SerializeBuffer* buf = LEPTRIS_ALLOC(SerializeBuffer);
    if (!buf) return NULL;

    buf->capacity = INITIAL_BUFFER_CAPACITY;
    buf->data = LEPTRIS_ALLOC_N(char, buf->capacity);
    if (!buf->data) {
        LEPTRIS_FREE(buf);
        return NULL;
    }

    buf->size = 0;
    buf->data[0] = '\0';
    buf->indent = 0;
    buf->indent_spaces = indent_spaces;
    buf->indent_unit = NULL;   /* #633: callers opt in per unit string */
    buf->html_method = 0;
    buf->indent_text = 0;
    buf->expand_empty = 0;
    buf->at_line_start = 0;
    /* Leave the xhtml mode OFF until a caller with extended options
     * enables it — an unset flag here otherwise serializes empties as
     * <x></x> wherever the malloc garbage is nonzero (Linux ASAN). */
    buf->xhtml = 0;
    buf->xhtml_encoding = NULL;
    buf->ws_mixed = 0;
    buf->cdata_names = NULL;   /* callers with cdata-section-elements
                                * install theirs; every other buffer
                                * must read "none", not heap garbage */
    buf->cdata_count = 0;
    buf->alloc_failed = 0;

    return buf;
}

void buffer_ensure_capacity(SerializeBuffer* buf, size_t needed) {
    /* Invariant: buf->size <= buf->capacity.  Use subtraction so the
     * remaining-space check can never wrap. */
    if (needed <= buf->capacity - buf->size) return;

    /* Grow by doubling, overflow-safe.  If doubling would exceed SIZE_MAX,
     * clamp to SIZE_MAX — caller's append will fail to find space and the
     * alloc_failed flag will be set. */
    size_t new_cap = buf->capacity > 0 ? buf->capacity : INITIAL_BUFFER_CAPACITY;
    while (new_cap - buf->size < needed) {
        if (new_cap > (SIZE_MAX / 2)) {
            new_cap = SIZE_MAX;
            break;
        }
        new_cap *= 2;
    }

    /* If we still don't fit (only possible if new_cap == SIZE_MAX and the
     * request is genuinely too large), bail without touching buf->capacity. */
    if (new_cap - buf->size < needed) {
        buf->alloc_failed = 1;
        return;
    }

    char* new_data = LEPTRIS_REALLOC_N(buf->data, char, new_cap);
    if (!new_data) {
        /* Realloc failed: buf->data and buf->capacity are unchanged
         * (preserves the valid state).  Mark the failure so the caller
         * can detect it; subsequent appends will silently truncate. */
        buf->alloc_failed = 1;
        return;
    }

    buf->data = new_data;
    buf->capacity = new_cap;
}

void buffer_append(SerializeBuffer* buf, const char* str) {
    if (!str) return;

    size_t len = strlen(str);
    buffer_ensure_capacity(buf, len + 1);

    memcpy(buf->data + buf->size, str, len);
    buf->size += len;
    buf->data[buf->size] = '\0';
}

void buffer_append_len(SerializeBuffer* buf, const char* str, size_t len) {
    if (!str || len == 0) return;

    buffer_ensure_capacity(buf, len + 1);

    memcpy(buf->data + buf->size, str, len);
    buf->size += len;
    buf->data[buf->size] = '\0';
    buf->at_line_start = (len > 0 && str[len - 1] == '\n');
}

void buffer_append_char(SerializeBuffer* buf, char c) {
    buffer_ensure_capacity(buf, 2);

    buf->data[buf->size++] = c;
    buf->data[buf->size] = '\0';
    buf->at_line_start = (c == '\n');
}

void buffer_append_indent(SerializeBuffer* buf) {
    if (!buf || buf->indent_spaces <= 0) return;
    /* §16.2 html method: newline-only layout — libxml2's HTML dump
     * never nests with spaces. */
    if (buf->html_method) return;

    if (buf->indent_unit && buf->indent_unit[0]) {
        size_t ul = strlen(buf->indent_unit);
        int levels = buf->indent;
        buffer_ensure_capacity(buf, levels * ul + 1);
        for (int i = 0; i < levels; i++) {
            memcpy(buf->data + buf->size, buf->indent_unit, ul);
            buf->size += ul;
        }
        buf->data[buf->size] = '\0';
        return;
    }

    int spaces = buf->indent * buf->indent_spaces;
    buffer_ensure_capacity(buf, spaces + 1);

    for (int i = 0; i < spaces; i++) {
        buf->data[buf->size++] = ' ';
    }
    buf->data[buf->size] = '\0';
}

void buffer_append_newline(SerializeBuffer* buf) {
    if (!buf || buf->indent_spaces <= 0) return;
    /* #129 indent_text collapses consecutive breaks; every other
     * mode keeps the historical (stacking) newline sites — html
     * layout relies on them. */
    if (buf->indent_text && buf->at_line_start) return;
    buffer_append_char(buf, '\n');
}

char* buffer_to_string(SerializeBuffer* buf) {
    if (!buf) return NULL;

    /* Allocate exact size needed */
    char* result = LEPTRIS_ALLOC_N(char, buf->size + 1);
    if (result) {
        memcpy(result, buf->data, buf->size + 1);
    }

    return result;
}

void buffer_free(SerializeBuffer* buf) {
    if (buf) {
        if (buf->data) {
            LEPTRIS_FREE(buf->data);
        }
        LEPTRIS_FREE(buf);
    }
}

int buffer_has_error(SerializeBuffer* buf) {
    return buf ? buf->alloc_failed : 1;
}

/* ============================================================================
 * XML Entity Escaping
 * ============================================================================ */


/* Inline escaped-text emitter (TODO 194d): writes into a caller-
 * reserved worst-case buffer (6 bytes per input byte), preserving
 * the entity semantics of the previous per-run walkers — entity
 * references emit as-is (text mode), bare & escapes, quotes are
 * ordinary in text mode and escaped in attribute mode. */
/* Escape-class table (serialize endgame): the run scan was 3-4
 * compares per byte — the dominant serializer cost at 57% of the
 * text-heavy profile. One indexed load + one test per byte, the
 * same chartype-table shape as the parser (common/chartype.h) and
 * pugixml's g_escape_table. Bit 1 = must escape in text mode
 * (& < >), bit 2 = additional attr-mode chars (" '). */
static const unsigned char leptris_escape_table[256] = {
    ['&'] = 3, ['<'] = 3, ['>'] = 3,
    ['"'] = 2, ['\''] = 2,
};

/* Long-run threshold (round 20): three SIMD finds cost ~60ns of
 * dispatch; the per-byte table scan reaches that around 256 bytes
 * of run. Text-heavy documents (one multi-hundred-KB text node,
 * no specials) spent their entire serialize time in the per-byte
 * scan — this is the shape the matrix benchmark loses 1.27x on. */
#define ESCAPE_SIMD_RUN 256

static char* emit_escaped_inline(char* out, const char* content,
                                 size_t len, int attr_mode) {
    unsigned mode = attr_mode ? 3u : 1u;
    /* Control characters escape as numeric references: attributes
     * escape \t \n \r (attribute-value normalization would eat
     * them); text escapes \r (newline translation). libxslt-
     * verified behavior. Rare: pre-scan and take the plain path
     * when absent so the SIMD hot loop is untouched. */
    int has_ctrl = 0;
    for (size_t k = 0; k < len; k++) {
        char c = content[k];
        if (c == '\r' || (attr_mode && (c == '\t' || c == '\n'))) {
            has_ctrl = 1;
            break;
        }
    }
    size_t i = 0;
    if (has_ctrl) {
        while (i < len) {
            char c = content[i];
            if (c == '\r' || (attr_mode && (c == '\t' || c == '\n'))) {
                out += sprintf(out, "&#%d;", (int)(unsigned char)c);
            } else if (c == '&') {
                memcpy(out, "&amp;", 5); out += 5;
            } else if (c == '<') {
                memcpy(out, "&lt;", 4); out += 4;
            } else if (c == '>') {
                memcpy(out, "&gt;", 4); out += 4;
            } else if (attr_mode && c == '"') {
                memcpy(out, "&quot;", 6); out += 6;
            } else if (attr_mode && c == '\'') {
                /* libxml2 leaves apostrophes raw in double-quoted
                 * attribute values (bug-86). */
                *out++ = c;
            } else {
                *out++ = c;
            }
            i++;
        }
        return out;
    }
    while (i < len) {
        size_t run = i;
        if (!attr_mode && len - i >= ESCAPE_SIMD_RUN) {
            /* Text mode over a long remainder: SIMD-find the first
             * of & < > (three passes beat one per-byte pass by an
             * order of magnitude at multi-KB lengths). */
            size_t rem = len - i;
            ptrdiff_t h1 = leptris_text_find(&content[i], rem, '&');
            ptrdiff_t h2 = leptris_text_find(&content[i], rem, '<');
            ptrdiff_t h3 = leptris_text_find(&content[i], rem, '>');
            ptrdiff_t best = -1;
            if (h1 >= 0) best = h1;
            if (h2 >= 0 && (best < 0 || h2 < best)) best = h2;
            if (h3 >= 0 && (best < 0 || h3 < best)) best = h3;
            if (best < 0) {
                memcpy(out, &content[i], rem);
                out += rem;
                break;
            }
            run = i + (size_t)best;
        } else {
            while (run < len &&
                   !(leptris_escape_table[(unsigned char)content[run]] & mode)) {
                run++;
            }
        }
        if (run > i) {
            memcpy(out, &content[i], run - i);
            out += run - i;
            i = run;
            if (i >= len) break;
        }

        if (content[i] == '&') {
            size_t j = i + 1;
            int found_semicolon = 0;
            while (j < len && j < i + 12) {
                if (content[j] == ';') { found_semicolon = 1; break; }
                if (!isalnum((unsigned char)content[j]) && content[j] != '#' && content[j] != '-') break;
                j++;
            }
            if (!attr_mode && found_semicolon && j > i + 1) {
                size_t n = j - i + 1;
                memcpy(out, &content[i], n);
                out += n;
                i = j + 1;
            } else {
                memcpy(out, "&amp;", 5);
                out += 5;
                i++;
            }
            continue;
        }
        if (content[i] == '<') { memcpy(out, "&lt;", 4); out += 4; }
        else if (content[i] == '>') { memcpy(out, "&gt;", 4); out += 4; }
        else if (attr_mode && content[i] == '"') { memcpy(out, "&quot;", 6); out += 6; }
        else { *out++ = content[i]; }   /* apostrophe: raw (libxml2) */
        i++;
    }
    return out;
}

/* Escape special XML characters in text content.
 * Ordinary-character RUNS are bulk-appended (one capacity check +
 * one memcpy per run) instead of per-character appends — the
 * per-byte path was the serializer's dominant cost (TODO 194). */
/* ============================================================================
 * Node Serialization Functions
 * ============================================================================ */

/* §16.1 cdata-section-elements: does this element's QName match? */
static int is_cdata_element(SerializeBuffer* buf, LeptrisElement e) {
    if (!buf || !buf->cdata_count || !e) return 0;
    const char* local = leptris_element_get_name(e);
    if (!local) return 0;
    const char* prefix = leptris_element_get_prefix(e);
    int has_pfx = prefix && *prefix;
    char qn[256];
    if (has_pfx)
        snprintf(qn, sizeof(qn), "%s:%s", prefix, local);
    else
        snprintf(qn, sizeof(qn), "%s", local);
    for (size_t i = 0; i < buf->cdata_count; i++) {
        if (!buf->cdata_names[i]) continue;
        if (strcmp(buf->cdata_names[i], qn) == 0) return 1;
        /* An unprefixed entry also matches a no-namespace element. */
        if (!strchr(buf->cdata_names[i], ':') && !has_pfx &&
            strcmp(buf->cdata_names[i], local) == 0) return 1;
    }
    return 0;
}

/* §16.2 HTML element descriptor table (libxml2 html40ElementTable
 * parity, sorted for bsearch). EMPTY mirrors the table's empty flag;
 * INLINE mirrors its isinline flag (values 1 and 2 both inline).
 * Absent elements return 0 — unknown markup never joins the HTML
 * indent decisions. */
typedef struct {
    const char* name;
    unsigned char flags;
} HtmlElemFlags;

static const HtmlElemFlags kHtmlElems[] = {
    { "a", HTML_F_INLINE }, { "abbr", HTML_F_INLINE },
    { "acronym", HTML_F_INLINE }, { "address", HTML_F_BLOCK },
    { "applet", HTML_F_INLINE }, { "area", HTML_F_EMPTY | HTML_F_BLOCK },
    { "b", HTML_F_INLINE }, { "base", HTML_F_EMPTY | HTML_F_BLOCK },
    { "basefont", HTML_F_EMPTY | HTML_F_INLINE },
    { "bdo", HTML_F_INLINE },
    { "bgsound", HTML_F_EMPTY | HTML_F_BLOCK },
    { "big", HTML_F_INLINE }, { "blockquote", HTML_F_BLOCK },
    { "body", HTML_F_BLOCK },
    { "br", HTML_F_EMPTY | HTML_F_INLINE }, { "button", HTML_F_INLINE },
    { "caption", HTML_F_BLOCK }, { "center", HTML_F_BLOCK },
    { "cite", HTML_F_INLINE },
    { "code", HTML_F_INLINE }, { "col", HTML_F_EMPTY | HTML_F_BLOCK },
    { "colgroup", HTML_F_BLOCK }, { "dd", HTML_F_BLOCK },
    { "del", HTML_F_INLINE },
    { "dfn", HTML_F_INLINE }, { "dir", HTML_F_BLOCK },
    { "div", HTML_F_BLOCK }, { "dl", HTML_F_BLOCK },
    { "dt", HTML_F_BLOCK }, { "em", HTML_F_INLINE },
    { "embed", HTML_F_EMPTY | HTML_F_INLINE },
    { "fieldset", HTML_F_BLOCK },
    { "font", HTML_F_INLINE }, { "form", HTML_F_BLOCK },
    { "frame", HTML_F_EMPTY | HTML_F_BLOCK },
    { "frameset", HTML_F_BLOCK }, { "h1", HTML_F_BLOCK },
    { "h2", HTML_F_BLOCK }, { "h3", HTML_F_BLOCK },
    { "h4", HTML_F_BLOCK }, { "h5", HTML_F_BLOCK },
    { "h6", HTML_F_BLOCK }, { "head", HTML_F_BLOCK },
    { "hr", HTML_F_EMPTY | HTML_F_BLOCK }, { "html", HTML_F_BLOCK },
    { "i", HTML_F_INLINE }, { "iframe", HTML_F_INLINE | HTML_F_RAW },
    { "img", HTML_F_EMPTY | HTML_F_INLINE },
    { "input", HTML_F_EMPTY | HTML_F_INLINE }, { "ins", HTML_F_INLINE },
    { "isindex", HTML_F_EMPTY | HTML_F_BLOCK },
    { "kbd", HTML_F_INLINE },
    { "keygen", HTML_F_EMPTY | HTML_F_BLOCK },
    { "label", HTML_F_INLINE },
    { "legend", HTML_F_BLOCK }, { "li", HTML_F_BLOCK },
    { "link", HTML_F_EMPTY | HTML_F_BLOCK },
    { "map", HTML_F_INLINE }, { "menu", HTML_F_BLOCK },
    { "meta", HTML_F_EMPTY | HTML_F_BLOCK },
    { "noembed", HTML_F_BLOCK | HTML_F_RAW }, { "noframes", HTML_F_BLOCK | HTML_F_RAW },
    { "noscript", HTML_F_BLOCK },
    { "object", HTML_F_INLINE }, { "ol", HTML_F_BLOCK },
    { "optgroup", HTML_F_BLOCK },
    { "option", HTML_F_BLOCK }, { "p", HTML_F_BLOCK },
    { "param", HTML_F_EMPTY | HTML_F_BLOCK },
    { "plaintext", HTML_F_BLOCK | HTML_F_RAW }, { "pre", HTML_F_BLOCK },
    { "q", HTML_F_INLINE }, { "s", HTML_F_INLINE },
    { "samp", HTML_F_INLINE },
    { "script", HTML_F_INLINE | HTML_F_RAW }, { "select", HTML_F_INLINE },
    { "small", HTML_F_INLINE },
    { "source", HTML_F_EMPTY | HTML_F_BLOCK },
    { "span", HTML_F_INLINE }, { "strike", HTML_F_INLINE },
    { "strong", HTML_F_INLINE }, { "style", HTML_F_BLOCK | HTML_F_RAW },
    { "sub", HTML_F_INLINE },
    { "sup", HTML_F_INLINE }, { "table", HTML_F_BLOCK },
    { "tbody", HTML_F_BLOCK },
    { "td", HTML_F_BLOCK }, { "textarea", HTML_F_INLINE },
    { "tfoot", HTML_F_BLOCK },
    { "th", HTML_F_BLOCK }, { "thead", HTML_F_BLOCK },
    { "title", HTML_F_BLOCK }, { "tr", HTML_F_BLOCK },
    { "track", HTML_F_EMPTY | HTML_F_BLOCK }, { "tt", HTML_F_INLINE },
    { "u", HTML_F_INLINE }, { "ul", HTML_F_BLOCK },
    { "var", HTML_F_INLINE },
    { "wbr", HTML_F_EMPTY | HTML_F_BLOCK }, { "xmp", HTML_F_INLINE | HTML_F_RAW },
};

int html_elem_flags(const char* name, size_t len) {
    if (!name || !len) return 0;
    size_t lo = 0, hi = sizeof(kHtmlElems) / sizeof(kHtmlElems[0]);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strncmp(kHtmlElems[mid].name, name, len);
        if (c == 0 && kHtmlElems[mid].name[len] == '\0')
            return (int)kHtmlElems[mid].flags;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

/* HTML names are matched case-insensitively; the serializer sees the
 * document's spelling, so lowercase into a bounded scratch first. */
static int html_elem_flags_ci(const char* name, size_t len) {
    char lower[16];
    if (!name || !len || len >= sizeof(lower)) return 0;
    for (size_t i = 0; i < len; i++)
        lower[i] = (char)tolower((unsigned char)name[i]);
    return html_elem_flags(lower, len);
}

/* HTML element semantics (void/raw/inline/block site rules) apply
 * only to UNPREFIXED names: a namespaced s:img is a foreign
 * element, not the HTML void img — it keeps its close tag and
 * takes no break rules (bug-117). */
static int html_elem_sem_flags(const LeptrisElement e) {
    if (!e) return 0;
    const char* pfx = leptris_element_get_prefix(e);
    if (pfx && pfx[0]) return 0;
    const char* n = e->name;
    if (!n) return 0;
    size_t nl = (e->name_len != 0xFF) ? (size_t)e->name_len : strlen(n);
    return html_elem_flags_ci(n, nl);
}

/* ---- libxml2 XHTML serialization (xmlsave.c xhtmlNodeDumpOutput) ---- */

#define LEPTRIS_XHTML_NS "http://www.w3.org/1999/xhtml"

/* xhtmlIsEmpty: exactly these 13 names may minimize as <x /> —
 * deliberately NOT the html-method void table, which carries extra
 * HTML5 names libxml2's XHTML path does not know. */
static int xhtml_is_empty_name(const char* name, size_t len) {
    static const char k[] = "area\0base\0basefont\0br\0col\0frame\0hr\0"
                            "img\0input\0isindex\0link\0meta\0param";
    const char* p = k;
    const char* end = k + sizeof(k) - 1;
    while (p < end) {
        size_t l = (size_t)(end - p);
        const char* z = memchr(p, '\0', l);
        size_t wl = z ? (size_t)(z - p) : l;
        if (wl == len && memcmp(p, name, len) == 0) return 1;
        p += wl + 1;
    }
    return 0;
}

/* libxml2 C.2/C.3: the empty-name rule applies only to no-namespace
 * (or XHTML-namespace) elements. */
static int xhtml_is_empty_elem(LeptrisElement e, const char* name,
                               size_t nl) {
    if (!xhtml_is_empty_name(name, nl)) return 0;
    const char* u = leptris_element_get_namespace_uri(e);
    return u == NULL || strcmp(u, LEPTRIS_XHTML_NS) == 0;
}

static int ser_case_eq(const char* a, const char* b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++; b++;
    }
    return *a == *b;
}

/* head injects the Content-Type meta unless a child meta already
 * carries http-equiv="Content-Type" (xmlStrcasecmp parity). */
static int xhtml_head_wants_meta(LeptrisElement head) {
    for (LeptrisNode* m = leptris_node_first_child_internal(
             (LeptrisNode*)head);
         m; m = leptris_node_get_next_sibling(m)) {
        if (m->type != LEPTRIS_NODE_TYPE_ELEMENT) continue;
        LeptrisElement me = (LeptrisElement)m;
        const char* mn = me->name;
        size_t mnl = (me->name_len != 0xFF)
            ? (size_t)me->name_len : (mn ? strlen(mn) : 0);
        if (!mn || mnl != 4 || memcmp(mn, "meta", 4) != 0) continue;
        const char* he = leptris_element_attribute(me, "http-equiv");
        if (he && ser_case_eq(he, "Content-Type")) return 0;
    }
    return 1;
}

static void emit_xhtml_meta(SerializeBuffer* buf) {
    buffer_append(buf, "<meta http-equiv=\"Content-Type\" "
                       "content=\"text/html; charset=");
    buffer_append(buf, buf->xhtml_encoding ? buf->xhtml_encoding
                                           : "UTF-8");
    buffer_append(buf, "\" />");
}

/* §16.2 newline sites (libxml2 htmlNodeDumpInternal parity). The
 * p/pre/param first-byte rule gates the PARENT-side sites only — a
 * block child named p* still takes its after-close newline when its
 * parent allows it. */
static int html_breaks_block(const char* name, size_t len, int flags) {
    (void)len;
    return (flags & HTML_F_BLOCK) && name != NULL;
}

/* Site 1: newline after this element's opening tag — block element,
 * first child exists and is not text, more than one child. */
static int html_break_before_children(LeptrisElement elem,
                                      const char* name, size_t len,
                                      int flags) {
    if (!html_breaks_block(name, len, flags)) return 0;
    if (name[0] == 'p') return 0;
    LeptrisNode* first =
        leptris_node_first_child_internal((LeptrisNode*)elem);
    if (!first || first->type == LEPTRIS_NODE_TYPE_TEXT) return 0;
    return leptris_node_get_next_sibling(first) != NULL;
}

/* Site 3: newline before this element's closing tag — block element,
 * more than one child, last child is not text. */
static int html_break_before_close(LeptrisElement elem,
                                   const char* name, size_t len,
                                   int flags) {
    if (!html_breaks_block(name, len, flags)) return 0;
    if (name[0] == 'p') return 0;
    LeptrisNode* c =
        leptris_node_first_child_internal((LeptrisNode*)elem);
    if (!c) return 0;
    LeptrisNode* last = c;
    while (leptris_node_get_next_sibling(last))
        last = leptris_node_get_next_sibling(last);
    if (last == c) return 0;   /* single child: libxml2 keeps inline */
    return last->type != LEPTRIS_NODE_TYPE_TEXT;
}

/* Site 2/4 (merged): newline after this element's closing tag (or
 * lone void tag) — block element, a non-text next sibling exists,
 * and the parent is not a p* element. */
static int html_break_after_elem(LeptrisElement elem,
                                 const char* name, size_t len,
                                 int flags) {
    if (!html_breaks_block(name, len, flags)) return 0;
    LeptrisNode* next =
        leptris_node_get_next_sibling((LeptrisNode*)elem);
    if (!next || next->type == LEPTRIS_NODE_TYPE_TEXT) return 0;
    LeptrisElement parent = leptris_element_get_parent(elem);
    if (parent && parent->name && parent->name[0] == 'p') return 0;
    return 1;
}

/* Emit the CDATA BODY, splitting "]]>" across section closes. The
 * caller owns the surrounding <![CDATA[ / ]]> brackets (run merging).
 * The close scan runs to len-3 INCLUSIVE — a "]]>" at the very END
 * of the span is a close too (the old j+2<len bound missed it and
 * emitted it raw, terminating the section early — bug-132). */
static void emit_cdata_body(SerializeBuffer* buf, const char* s, size_t len) {
    size_t i = 0;
    while (i < len) {
        const char* close = NULL;
        for (size_t j = i; j + 3 <= len; j++) {
            if (s[j] == ']' && s[j+1] == ']' && s[j+2] == '>') {
                close = s + j;
                break;
            }
        }
        /* chunk ends after "]]"; the '>' carries into the reopened
         * section (i advances by chunk only). */
        size_t chunk = close ? (size_t)(close - (s + i)) + 2 : len - i;
        buffer_append_len(buf, s + i, chunk);
        if (close) buffer_append(buf, "]]><![CDATA[");
        i += chunk;
    }
}

/* Standalone single-node CDATA emission (fallback sites). */
static void emit_cdata(SerializeBuffer* buf, const char* s, size_t len) {
    size_t i = 0;
    while (i < len) {
        const char* close = NULL;
        for (size_t j = i; j + 3 <= len; j++) {
            if (s[j] == ']' && s[j+1] == ']' && s[j+2] == '>') {
                close = s + j;
                break;
            }
        }
        /* chunk ends after "]]"; the '>' opens the next section. */
        size_t chunk = close ? (size_t)(close - (s + i)) + 2 : len - i;
        buffer_append(buf, "<![CDATA[");
        buffer_append_len(buf, s + i, chunk);
        buffer_append(buf, "]]>");
        i += chunk;
    }
}

/* libxslt rule: whitespace-only text is never wrapped in CDATA. */
static int text_is_ws_only(const char* s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return 0;
    }
    return 1;
}

/* Convert one text node's content per §16.1: CDATA unless the text
 * is whitespace-only (libxslt keeps ws plain). */
static void emit_cdata_checked(SerializeBuffer* buf, const char* s) {
    if (!s) return;
    size_t l = strlen(s);
    if (text_is_ws_only(s, l)) {
        buffer_append_len(buf, s, l);
        return;
    }
    emit_cdata(buf, s, l);
}


void serialize_text_internal(LeptrisTextNode* text, SerializeBuffer* buf) {
    if (!text) return;

    /* Materialize + expand entities on first access. For borrowed
     * text nodes (the direct_parse fast path), this expands
     * predefined entities (&amp; → &) and, when a DTD is present
     * on the document, custom entities (&foo; → declared value).
     * Without this call the serializer reads raw borrowed bytes
     * and emits unexpanded entity references. */
    const char* content = leptris_text_get_content(text);
    if (!content) return;
    size_t content_len = text->content_len;

    /* disable-output-escaping (XSLT §16.4) / method=html script and
     * style content: emit the string-value verbatim — no entity
     * escaping, characters pass through as-is. */
    if (text->base.raw) {
        buffer_append_len(buf, content, content_len);
        return;
    }

    /* Run-batched (TODO 194b): bulk-append ordinary bytes up to the
     * next special character; the entity lookahead only runs at '&'.
     * Quotes and apostrophes are ordinary in text content. */
    size_t i = 0;
    while (i < content_len) {
        size_t run = i;
        while (run < content_len) {
            char c = content[run];
            if (c == '&' || c == '<' || c == '>') break;
            run++;
        }
        if (run > i) {
            buffer_append_len(buf, &content[i], run - i);
            i = run;
            if (i >= content_len) break;
        }

        if (content[i] == '&') {
            /* Look ahead for ';' to detect entity reference */
            size_t j = i + 1;
            int found_semicolon = 0;
            while (j < text->content_len && j < i + 12) {
                if (content[j] == ';') {
                    found_semicolon = 1;
                    break;
                }
                if (!isalnum((unsigned char)content[j]) && content[j] != '#' && content[j] != '-') {
                    break;
                }
                j++;
            }
            if (found_semicolon && j > i + 1) {
                buffer_append_len(buf, &content[i], j - i + 1);
                i = j + 1;
            } else {
                buffer_append_len(buf, "&amp;", 5);
                i++;
            }
            continue;
        }

        if (content[i] == '<') {
            buffer_append_len(buf, "&lt;", 4);
        } else {
            buffer_append_len(buf, "&gt;", 4);
        }
        i++;
    }
}

void serialize_comment_internal(LeptrisCommentNode* comment, SerializeBuffer* buf) {
    if (!comment || !comment->content) return;

    buffer_append(buf, "<!--");
    buffer_append(buf, comment->content);
    buffer_append(buf, "-->");
}

void serialize_cdata_internal(LeptrisCDATANode* cdata, SerializeBuffer* buf) {
    if (!cdata || !cdata->content) return;

    buffer_append(buf, "<![CDATA[");
    /* XML 1.0 §2.7: "]]>" cannot appear inside a CDATA section.
     * Content containing the terminator is split across two CDATA
     * sections by rewriting it as "]]]]><![CDATA[>" — the same
     * technique libxml2 uses. Everything else in CDATA is literal. */
    const char* p = cdata->content;
    const char* run_start = p;
    while (*p) {
        if (p[0] == ']' && p[1] == ']' && p[2] == '>') {
            buffer_append_len(buf, run_start, (size_t)(p - run_start));
            buffer_append(buf, "]]]]><![CDATA[>");
            p += 3;
            run_start = p;
        } else {
            p++;
        }
    }
    buffer_append(buf, run_start);
    buffer_append(buf, "]]>");
}

void serialize_pi_internal(LeptrisPINode* pi, SerializeBuffer* buf) {
    if (!pi || !pi->target) return;

    buffer_append(buf, "<?");
    buffer_append(buf, pi->target);

    if (pi->data && pi->data[0] != '\0') {
        buffer_append_char(buf, ' ');
        buffer_append(buf, pi->data);
    }

    /* §16.2: HTML PIs close with '>' — no trailing '?' (libxml2
     * HTMLtree.c). */
    buffer_append(buf, buf->html_method ? ">" : "?>");
}

void serialize_doctype_internal(LeptrisDoctypeNode* doctype, SerializeBuffer* buf) {
    if (!doctype || !doctype->name) return;

    buffer_append(buf, "<!DOCTYPE ");
    buffer_append(buf, doctype->name);

    if (doctype->public_id) {
        buffer_append(buf, " PUBLIC \"");
        buffer_append(buf, doctype->public_id);
        buffer_append_char(buf, '"');

        if (doctype->system_id) {
            buffer_append(buf, " \"");
            buffer_append(buf, doctype->system_id);
            buffer_append_char(buf, '"');
        }
    } else if (doctype->system_id) {
        buffer_append(buf, " SYSTEM \"");
        buffer_append(buf, doctype->system_id);
        buffer_append_char(buf, '"');
    }

    /* Output internal subset if present */
    if (doctype->internal_subset) {
        /* Trim the source's bracket whitespace, then lay the
         * declarations out libxml2-style: each on its own line
         * (issue #633). Declarations open with '<!' and close with
         * '>', so '><' is the boundary. */
        const char* s = doctype->internal_subset;
        size_t sl = strlen(s);
        while (sl && (s[0] == ' ' || s[0] == '\t' || s[0] == '\n' ||
                      s[0] == '\r')) { s++; sl--; }
        while (sl && (s[sl-1] == ' ' || s[sl-1] == '\t' ||
                      s[sl-1] == '\n' || s[sl-1] == '\r')) sl--;
        if (sl) {
            buffer_append(buf, " [\n");
            for (size_t i = 0; i < sl; i++) {
                if (i + 1 < sl && s[i] == '>' && s[i + 1] == '<') {
                    /* Declaration boundary: line-break after '>';
                     * the next '<' prints on the following pass —
                     * skipping it dropped every later declaration's
                     * opening byte (issue #687). */
                    buffer_append_char(buf, '>');
                    buffer_append_char(buf, '\n');
                } else {
                    buffer_append_char(buf, s[i]);
                }
            }
            buffer_append(buf, "\n]");
        }
        /* Empty subset: no brackets at all (libxml2). */
    }

    buffer_append_char(buf, '>');
}

/* Emit a qualified element/attribute name: XML requires names in a
 * namespace to serialize with their prefix (prefix:name). libleptris
 * stores the prefix beside the local name, so every name-emission
 * site routes through this helper. */
static void append_qualified_name(SerializeBuffer* buf, const char* prefix,
                                  const char* name, size_t name_len) {
    if (prefix && prefix[0]) {
        buffer_append(buf, prefix);
        buffer_append_char(buf, ':');
    }
    buffer_append_len(buf, name, name_len);
}

/* Issue #546: rootless documents may still carry document-level
 * nodes. Emit the declaration (if the original had one) and the
 * child chain (no root element exists in it) instead of returning
 * an empty string. */
static char* serialize_rootless_pis(struct leptris_document* doc,
                                    const LeptrisSerializeOptions* options) {
    if (!doc || !doc->doc_children_head) return NULL;
    SerializeBuffer* pibuf = buffer_create(0);
    if (!pibuf) return NULL;
    int xml_declaration = options ? options->xml_declaration : 1;
    if (xml_declaration && doc->had_declaration) {
        const char* enc = options && options->encoding
                           ? options->encoding
                           : (doc->encoding ? doc->encoding : "UTF-8");
        const char* ver = doc->xml_version ? doc->xml_version : "1.0";
        buffer_append(pibuf, "<?xml version=\"");
        buffer_append(pibuf, ver);
        buffer_append(pibuf, "\"");
        if (enc) {
            buffer_append(pibuf, " encoding=\"");
            buffer_append(pibuf, enc);
            buffer_append(pibuf, "\"");
        }
        if (doc->standalone >= 0) {
            buffer_append(pibuf, " standalone=\"");
            buffer_append(pibuf, doc->standalone ? "yes" : "no");
            buffer_append(pibuf, "\"");
        }
        buffer_append(pibuf, "?>");
    }
    /* Document children (issue #580): comment/PI nodes in document
     * order — the interleaving the old side lists lost. */
    for (LeptrisNode* c = (LeptrisNode*)doc->doc_children_head; c;
         c = leptris_node_get_next_sibling(c)) {
        if (c->type == LEPTRIS_NODE_TYPE_PI) {
            LeptrisPINode* pi = (LeptrisPINode*)c;
            buffer_append(pibuf, "<?");
            if (pi->target) buffer_append(pibuf, pi->target);
            if (pi->data && pi->data[0]) {
                buffer_append_char(pibuf, ' ');
                buffer_append(pibuf, pi->data);
            }
            buffer_append(pibuf, "?>");
        } else if (c->type == LEPTRIS_NODE_TYPE_COMMENT) {
            buffer_append(pibuf, "<!--");
            buffer_append(pibuf,
                leptris_comment_get_content((LeptrisCommentNode*)c));
            buffer_append(pibuf, "-->");
        }
    }
    char* out = buffer_to_string(pibuf);
    buffer_free(pibuf);
    return out;
}

#define SER_WALK_STACK_MAX 512

typedef struct { LeptrisElement e; size_t nl; int mixed; int cd_open;
                 int close_brk; char* cd_buf; size_t cd_len, cd_cap; } SerFrame;

/* Issue #534: an element whose children include TEXT (non-whitespace)
 * or CDATA is MIXED CONTENT — indenting inside it would change the
 * text on round-trip (inserted whitespace becomes new text nodes on
 * reparse). libxml2 keeps such elements on one line; so do we.
 * Whitespace-only text nodes are formatting artifacts and do NOT
 * count (pretty documents keep indenting). */
/* XSLT-result variant: WHITESPACE-ONLY text children count —
 * libxslt's serializer stops formatting below any text node
 * (bug-98: the copied source whitespace is the visible indent). */
static int ser_children_have_any_text(LeptrisNode* fc) {
    for (LeptrisNode* c = fc; c; c = leptris_node_get_next_sibling(c)) {
        if (c->type == LEPTRIS_NODE_TYPE_CDATA) return 1;
        if (c->type == LEPTRIS_NODE_TYPE_TEXT) return 1;
    }
    return 0;
}

static int ser_children_have_text(LeptrisNode* fc) {
    for (LeptrisNode* c = fc; c; c = leptris_node_get_next_sibling(c)) {
        if (c->type == LEPTRIS_NODE_TYPE_CDATA) return 1;
        if (c->type == LEPTRIS_NODE_TYPE_TEXT) {
            LeptrisTextNode* tn = (LeptrisTextNode*)c;
            const char* tc;
            size_t tl;
            if (tn->borrowed && tn->content_len > 0) {
                tc = tn->content;
                tl = tn->content_len;
            } else {
                tc = leptris_text_get_content(tn);
                tl = tc ? tn->content_len : 0;
            }
            for (size_t i = 0; i < tl; i++) {
                if (tc[i] != ' ' && tc[i] != '\t' &&
                    tc[i] != '\n' && tc[i] != '\r') {
                    return 1;
                }
            }
        }
    }
    return 0;
}

static int ser_frame_cd_append(SerFrame* f, const char* s) {
    if (!f || !s) return 1;
    size_t len = strlen(s);
    if (len == 0) return 1;
    if (f->cd_len + len + 1 > f->cd_cap) {
        size_t cap = f->cd_cap ? f->cd_cap * 2 : 128;
        while (cap < f->cd_len + len + 1) cap *= 2;
        char* grown = (char*)realloc(f->cd_buf, cap);
        if (!grown) return 0;
        f->cd_buf = grown;
        f->cd_cap = cap;
    }
    memcpy(f->cd_buf + f->cd_len, s, len);
    f->cd_len += len;
    f->cd_buf[f->cd_len] = '\0';
    f->cd_open = 1;
    return 1;
}

static void ser_frame_cd_flush(SerializeBuffer* buf, SerFrame* f) {
    if (!f || !f->cd_open) return;
    buffer_append(buf, "<![CDATA[");
    if (f->cd_buf && f->cd_len)
        emit_cdata_body(buf, f->cd_buf, f->cd_len);
    buffer_append(buf, "]]>");
    f->cd_open = 0;
    f->cd_len = 0;
}

static void ser_frames_free(SerFrame* st, int sp) {
    if (!st) return;
    for (int i = 0; i < sp; i++) free(st[i].cd_buf);
    free(st);
}

void serialize_element_internal(LeptrisElement root_elem, SerializeBuffer* buf, int is_root) {
    if (!root_elem || !root_elem->name) return;

    /* Heap-grown frames (TODO 194e follow-up): a fixed 512-deep
     * array needed a recursive fallback walker that duplicated the
     * whole emission path — every serializer feature landed twice.
     * The frames grow by doubling; the duplicate walker is gone. */
    size_t st_cap = SER_WALK_STACK_MAX;
    SerFrame* st = (SerFrame*)malloc(st_cap * sizeof(SerFrame));
    if (!st) return;
    int sp = 0;

    LeptrisNode* cur = (LeptrisNode*)root_elem;
    int is_root_cur = is_root;

    int trailing_text_nl = 0;
    for (;;) {
        if (cur->type != LEPTRIS_NODE_TYPE_ELEMENT) {
            /* #534 companion: in pretty mode the formatter owns the
             * whitespace between elements of a NON-mixed parent —
             * the source's ws-only text nodes are dropped and
             * replaced by the canonical newline+indent the open and
             * close paths already emit. (Before, both were emitted,
             * doubling the blank lines on every round-trip.) Text
             * inside a mixed parent is verbatim content: never
             * dropped. HTML mode prints text verbatim (libxml2 has
             * no formatter-owned whitespace there). */
            if (buf->indent_spaces > 0 && !buf->html_method &&
                cur->type == LEPTRIS_NODE_TYPE_TEXT &&
                !((sp > 0) && st[sp - 1].mixed)) {
                LeptrisTextNode* wsn = (LeptrisTextNode*)cur;
                const char* wsc;
                size_t wsl;
                if (wsn->borrowed && wsn->content_len > 0) {
                    wsc = wsn->content;
                    wsl = wsn->content_len;
                } else {
                    wsc = leptris_text_get_content(wsn);
                    wsl = wsc ? wsn->content_len : 0;
                }
                int ws_only = 1;
                for (size_t i = 0; i < wsl; i++) {
                    if (wsc[i] != ' ' && wsc[i] != '\t' &&
                        wsc[i] != '\n' && wsc[i] != '\r') {
                        ws_only = 0;
                        break;
                    }
                }
                if (ws_only) goto advance;   /* skip, formatter owns it */
            }
            /* #129 indent_text: non-whitespace text sits on its own
             * indented line (display form — not round-trip). */
            if (buf->indent_text && buf->indent_spaces > 0 &&
                cur->type == LEPTRIS_NODE_TYPE_TEXT) {
                LeptrisTextNode* itn = (LeptrisTextNode*)cur;
                const char* isc;
                size_t isl;
                if (itn->borrowed && itn->content_len > 0) {
                    isc = itn->content; isl = itn->content_len;
                } else {
                    isc = leptris_text_get_content(itn);
                    isl = isc ? itn->content_len : 0;
                }
                int ws_only = 1;
                for (size_t k = 0; k < isl; k++) {
                    char c = isc[k];
                    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                        ws_only = 0; break;
                    }
                }
                if (!ws_only) {
                    buffer_append_newline(buf);
                    buffer_append_indent(buf);
                    /* text ends the line: the next child or the close
                     * tag indents from a fresh line. */
                    trailing_text_nl = 1;
                }
            }
            /* §16.1 cdata-section-elements: text children of a
             * listed element join ONE CDATA run per consecutive
             * text span (libxslt merges the section). */
            int cdata_parent = sp > 0 && st[sp - 1].e &&
                               is_cdata_element(buf, st[sp - 1].e);
            if ((cur->type == LEPTRIS_NODE_TYPE_TEXT &&
                 !((LeptrisTextNode*)cur)->base.raw) ||
                (cur->type == LEPTRIS_NODE_TYPE_CDATA)) {
                if (cdata_parent) {
                    const char* tc =
                        (cur->type == LEPTRIS_NODE_TYPE_CDATA)
                            ? leptris_cdata_get_content(
                                  (LeptrisCDATANode*)cur)
                            : leptris_text_get_content(
                                  (LeptrisTextNode*)cur);
                    if (tc && !ser_frame_cd_append(&st[sp - 1], tc)) {
                        ser_frames_free(st, sp);
                        return;
                    }
                    goto advance;
                }
                if (sp > 0 && st[sp - 1].cd_open)
                    ser_frame_cd_flush(buf, &st[sp - 1]);
                /* §16.2 rawtext parent: mixed-content text inside
                 * script/style is verbatim under method=html. */
                if (buf->html_method && sp > 0 && st[sp - 1].e) {
                    const char* rn = st[sp - 1].e->name;
                    if (rn &&
                        (html_elem_sem_flags(st[sp - 1].e) & HTML_F_RAW)) {
                        const char* tr = leptris_text_get_content(
                            (LeptrisTextNode*)cur);
                        if (tr) buffer_append(buf, tr);
                        goto advance;
                    }
                }
                serialize_node_internal(cur, buf);
                if (trailing_text_nl) {
                    buffer_append_newline(buf);
                    trailing_text_nl = 0;
                }
                goto advance;
            }
            if (sp > 0 && st[sp - 1].cd_open)
                ser_frame_cd_flush(buf, &st[sp - 1]);
            /* libxml2 xmlIndentTreeOutput (issue #633): comments and
             * PIs under a NON-mixed parent get their own indented
             * line, like element siblings. Mixed-content parents
             * keep them inline — a PI between text must not move. */
            if (buf->indent_spaces > 0 && sp > 0 && !st[sp - 1].mixed &&
                !buf->html_method &&
                (cur->type == LEPTRIS_NODE_TYPE_COMMENT ||
                 cur->type == LEPTRIS_NODE_TYPE_PI)) {
                buffer_append_indent(buf);
                serialize_node_internal(cur, buf);
                buffer_append_newline(buf);
                goto advance;
            }
            serialize_node_internal(cur, buf);
            goto advance;
        }

        LeptrisElement e = (LeptrisElement)cur;
        /* #534: children of a mixed-content parent emit inline. */
        int parent_mixed = (sp > 0) && st[sp - 1].mixed;
        const char* name = e->name;
        size_t nl = (e->name_len != 0xFF) ? (size_t)e->name_len
                                          : strlen(name);
        const char* epfx = leptris_element_get_prefix(e);
        size_t epl = (epfx && epfx[0]) ? strlen(epfx) + 1 : 0;

        /* libxml2 addmeta: a head directly under the root html
         * element injects the Content-Type meta at serialization
         * time (case-sensitive head/html names, xmlStrEqual). */
        int addmeta = 0;
        if (buf->xhtml && sp == 1 && st[0].e &&
            nl == 4 && memcmp(name, "head", 4) == 0) {
            const char* rn = st[0].e->name;
            size_t rnl = (st[0].e->name_len != 0xFF)
                ? (size_t)st[0].e->name_len
                : (rn ? strlen(rn) : 0);
            if (rn && rnl == 4 && memcmp(rn, "html", 4) == 0)
                addmeta = xhtml_head_wants_meta(e);
        }

        /* --- total-fusion fast path (TODO 194f): a compact-mode leaf
         * element with no attributes and no namespaces emits its ENTIRE
         * `<name>text</name>` from one reservation — the dominant shape
         * in text-heavy documents paid two reservations + finalize.
         * RAW parents (html script/style) escape the fusion: their
         * text is verbatim. */
        if (buf->indent_text == 0 && !addmeta &&
            leptris_element_get_first_attribute(e) == NULL &&
            leptris_elem_namespaces(e) == NULL &&
            !(buf->html_method &&
              (html_elem_sem_flags(e) & HTML_F_RAW))) {
            LeptrisNode* fc0 = leptris_node_first_child_internal((LeptrisNode*)e);
            if (fc0 && fc0->type == LEPTRIS_NODE_TYPE_TEXT &&
                leptris_node_get_next_sibling(fc0) == NULL) {
                /* §16.1 cdata-section-elements wins over the fused
                 * escaped emission. */
                if (buf->cdata_count && !((LeptrisTextNode*)fc0)->base.raw &&
                    is_cdata_element(buf, e)) {
                    const char* tcd =
                        leptris_text_get_content((LeptrisTextNode*)fc0);
                    buffer_append_char(buf, '<');
                    append_qualified_name(
                        buf, leptris_element_get_prefix(e), name, nl);
                    buffer_append_char(buf, '>');
                    if (tcd)
                        emit_cdata_checked(buf, tcd);
                    buffer_append(buf, "</");
                    append_qualified_name(
                        buf, leptris_element_get_prefix(e),
                        name, nl);
                    buffer_append_char(buf, '>');
                    goto advance;
                }
                LeptrisTextNode* tn0 = (LeptrisTextNode*)fc0;
                /* View-direct (round 18): for entity-free borrowed
                 * text, read the view directly — get_content would
                 * materialize a pool copy (alloc + memcpy) just to
                 * NUL-terminate, which the escaper doesn't need
                 * (it's length-bounded). Entity-bearing text still
                 * materializes for correct expansion. */
                const char* tc0;
                size_t tl0;
                if (tn0->borrowed && tn0->content_len > 0 &&
                    !memchr(tn0->content, '&', tn0->content_len)) {
                    tc0 = tn0->content;
                    tl0 = tn0->content_len;
                } else {
                    tc0 = leptris_text_get_content(tn0);
                    tl0 = tc0 ? tn0->content_len : 0;
                }
                /* Pretty mode fuses newline + indent + open + text +
                 * close into the same single reservation — leaves
                 * otherwise pay ~8 capacity-checked appends each
                 * (the raw-only guard was why pretty trailed). */
                int pretty0 = buf->indent_spaces > 0 &&
                              !buf->html_method && !parent_mixed;
                /* HTML mode: zero lead ever; trail follows the
                 * after-close break rule (mixed is not a gate). */
                int htrail0 = buf->html_method && !is_root_cur &&
                              html_break_after_elem(
                                  e, name, nl, html_elem_sem_flags(e));
                /* #658: honor the indent unit here too — computing
                 * indent*indent_spaces directly lost the unit on
                 * every fused leaf (buffer_append_indent's #633
                 * branch never ran). */
                const char* iunit =
                    (buf->indent_unit && buf->indent_unit[0])
                        ? buf->indent_unit : NULL;
                size_t iul = iunit ? strlen(iunit) : 0;
                int ilevels = (!is_root_cur && pretty0)
                    ? buf->indent : 0;
                int lead = ilevels
                    ? (iunit ? (int)(ilevels * iul)
                             : buf->indent * buf->indent_spaces)
                    : 0;
                int trail = htrail0 ||
                            (pretty0 && !is_root_cur);
                buffer_ensure_capacity(
                    buf, (size_t)(lead + trail) + 2 * (epl + nl) + 6 * tl0 + 6);
                char* q = buf->data + buf->size;
                if (lead) {
                    if (iunit) {
                        for (int i = 0; i < ilevels; i++) {
                            memcpy(q, iunit, iul);
                            q += iul;
                        }
                    } else {
                        memset(q, ' ', (size_t)lead);
                        q += lead;
                    }
                }
                *q++ = '<';
                if (epl) {
                    memcpy(q, epfx, epl - 1); q += epl - 1;
                    *q++ = ':';
                }
                memcpy(q, name, nl); q += nl;
                *q++ = '>';
                if (tc0 && tl0) q = emit_escaped_inline(q, tc0, tl0, 0);
                *q++ = '<'; *q++ = '/';
                if (epl) {
                    memcpy(q, epfx, epl - 1); q += epl - 1;
                    *q++ = ':';
                }
                memcpy(q, name, nl); q += nl;
                *q++ = '>';
                if (trail) *q++ = '\n';
                buf->size = (size_t)(q - buf->data);
                buf->data[buf->size] = '\0';
                goto advance;
            }
        }

        /* --- open tag (batched, as before) --- */
        if (!is_root_cur && buf->indent_spaces > 0 && !parent_mixed)
            buffer_append_indent(buf);
        buffer_ensure_capacity(buf, 1 + epl + nl + 1);
        char* ot = buf->data + buf->size;
        *ot++ = '<';
        if (epl) {
            memcpy(ot, epfx, epl - 1);
            ot += epl - 1;
            *ot++ = ':';
        }
        memcpy(ot, name, nl);
        ot += nl;
        buf->size = (size_t)(ot - buf->data);
        buf->data[buf->size] = '\0';

        /* --- namespace declarations in CHAIN order (libxml2
         * serializes the nsDef list verbatim; parse appends in
         * source order — bug-104 keeps the source's declaration
         * order, default namespace included). */
        {
        for (struct leptris_namespace* ns = leptris_elem_namespaces(e); ns; ns = ns->next) {
            const char* prefix = ns->prefix ? ns->prefix : "";
            size_t pnl = ns->prefix ? strlen(ns->prefix) : 0;
            const char* uri = ns->uri ? ns->uri : "";
            size_t ul = ns->uri ? strlen(ns->uri) : 0;
            buffer_ensure_capacity(buf, 7 + pnl + 2 + 6 * ul + 2);
            char* nw = buf->data + buf->size;
            *nw++ = ' ';
            memcpy(nw, "xmlns", 5);
            nw += 5;
            if (pnl) { *nw++ = ':'; memcpy(nw, prefix, pnl); nw += pnl; }
            *nw++ = '=';
            *nw++ = '"';
            for (size_t i = 0; i < ul; i++) {
                switch (uri[i]) {
                    case '<':  memcpy(nw, "&lt;", 4);   nw += 4; break;
                    case '>':  memcpy(nw, "&gt;", 4);   nw += 4; break;
                    case '&':  memcpy(nw, "&amp;", 5);  nw += 5; break;
                    case '"':  memcpy(nw, "&quot;", 6); nw += 6; break;
                    case '\'': *nw++ = '\''; break;   /* raw (libxml2) */
                    default:   *nw++ = uri[i]; break;
                }
            }
            *nw++ = '"';
            buf->size = (size_t)(nw - buf->data);
            buf->data[buf->size] = '\0';
        }
        }

        /* libxml2 xmlsave: an element in NO namespace whose nearest
         * default-ns DECLARATION on the ancestor chain binds non-empty
         * must reset it with xmlns="" or the element would inherit
         * that namespace (bug-130's imported-module <div> under a
         * default-namespaced <html>). Parsed documents resolve
         * inherited namespaces onto the element, so only
         * namespace-less result elements reach this. */
        if (!epl) {
            const char* ens = leptris_element_get_namespace_uri(e);
            if (!ens || !ens[0]) {
            int own_default = 0;
            for (struct leptris_namespace* ns = leptris_elem_namespaces(e);
                 ns; ns = ns->next)
                if (!ns->prefix || !ns->prefix[0]) { own_default = 1; break; }
            if (!own_default) {
                int reset_needed = 0;
                for (int f = (int)sp - 1; f >= 0; f--) {
                    LeptrisElement ae = st[f].e;
                    if (!ae) continue;
                    int decided = 0;
                    for (struct leptris_namespace* ns =
                             leptris_elem_namespaces(ae);
                         ns; ns = ns->next) {
                        if (!ns->prefix || !ns->prefix[0]) {
                            reset_needed = ns->uri && ns->uri[0];
                            decided = 1;
                            break;
                        }
                    }
                    if (decided) break;
                }
                if (reset_needed)
                    buffer_append(buf, " xmlns=\"\"");
            }
            }
        }

        /* libxml2 3.1.1: an <html> in no namespace with no local ns
         * declarations gets the XHTML namespace injected. */
        if (buf->xhtml && !epl && leptris_elem_namespaces(e) == NULL &&
            leptris_element_get_namespace_uri(e) == NULL &&
            nl == 4 && memcmp(name, "html", 4) == 0)
            buffer_append(buf, " xmlns=\"" LEPTRIS_XHTML_NS "\"");

        /* --- namespaces (identical emission) --- */
        for (struct leptris_attribute* attr = leptris_element_get_first_attribute(e); attr; attr = leptris_attr_next(attr)) {
            const char* val;
            if (attr_has_entities(attr)) {
                struct leptris_document* d = leptris_element_get_document(e);
                LeptrisMemoryPool* pool = d ? d->pool : NULL;
                LeptrisStringView av = leptris_attr_value_sv(attr);
                char* resolved = pool
                    ? leptris_decode_entities_view(&av, pool) : NULL;
                if (resolved) {
                    leptris_attr_value_set_heap(attr, leptris_sv_from_cstr(resolved));
                    attr_set_entities(attr, 0);
                }
                val = attr_cvalue(attr);
            } else {
                val = attr_cvalue(attr);
            }
            const char* name_c = attr_cname(attr);
            size_t anl = attr->name_view.length;
            const char* apfx = attr_get_prefix(attr);
            size_t apl = (apfx && apfx[0]) ? strlen(apfx) + 1 : 0;
            /* Issue #542: name_view carries the QUALIFIED name —
             * when the prefix is emitted separately, skip its bytes
             * so the prefix is not doubled (ns:ns:attr). */
            if (apl && anl > apl &&
                memcmp(name_c, apfx, apl - 1) == 0 && name_c[apl - 1] == ':') {
                name_c += apl;
                anl -= apl;
            }
            /* View length is authoritative (entity resolution
             * replaces the view) — same as the recursive path. */
            size_t vlen = leptris_attr_value_sv(attr).length;
            /* §16.2 html: href/src values percent-encode as URIs
             * (libxml2 htmlAttrDumpOutput, bug-83); apostrophes
             * stay raw (the XML serializer's &apos; is HTML-wrong). */
            int html_uri_attr = 0;
            if (buf->html_method && anl &&
                ((anl == 4 && memcmp(name_c, "href", 4) == 0) ||
                 (anl == 3 && memcmp(name_c, "src", 3) == 0) ||
                 /* libxml2 also URI-encodes name attributes (the
                  * .out for bug-159: name="%D1%91"). */
                 (anl == 4 && memcmp(name_c, "name", 4) == 0)))
                html_uri_attr = 1;
            if (html_uri_attr) {
                buffer_ensure_capacity(
                    buf, 1 + apl + anl + 2 + 3 * vlen + 2 + 1);
                char* out = buf->data + buf->size;
                *out++ = ' ';
                if (apl) {
                    memcpy(out, apfx, apl - 1);
                    out += apl - 1;
                    *out++ = ':';
                }
                memcpy(out, name_c, anl);
                out += anl;
                *out++ = '=';
                *out++ = '"';
                for (size_t i = 0; i < vlen; i++) {
                    unsigned char uc = (unsigned char)val[i];
                    if (uc <= 0x20 || uc >= 0x7f) {
                        /* UTF-8 bytes ≥ 0x80 and ASCII controls/spaces
                         * percent-encode; keep XML-unsafe chars escaped
                         * below for safety. */
                        if (uc == '<' || uc == '>' || uc == '"' ||
                            uc == '&' || uc == '\'') {
                            /* fall into entity handling */
                        } else {
                            out += sprintf(out, "%%%02X", uc);
                            continue;
                        }
                    }
                    switch (val[i]) {
                        case '<':  memcpy(out, "&lt;", 4);   out += 4; break;
                        case '>':  memcpy(out, "&gt;", 4);   out += 4; break;
                        case '&':  memcpy(out, "&amp;", 5);  out += 5; break;
                        case '"':  memcpy(out, "&quot;", 6); out += 6; break;
                        case '\'': *out++ = '\''; break;
                        default:   *out++ = val[i]; break;
                    }
                }
                *out++ = '"';
                buf->size = (size_t)(out - buf->data);
                buf->data[buf->size] = '\0';
                continue;
            }
            buffer_ensure_capacity(buf, 1 + apl + anl + 2 + 6 * vlen + 2 + 1);
            char* out = buf->data + buf->size;
            *out++ = ' ';
            if (apl) {
                memcpy(out, apfx, apl - 1);
                out += apl - 1;
                *out++ = ':';
            }
            memcpy(out, name_c, anl);
            out += anl;
            *out++ = '=';
            *out++ = '"';
            for (size_t i = 0; i < vlen; i++) {
                switch (val[i]) {
                    case '<':  memcpy(out, "&lt;", 4);   out += 4; break;
                    case '>':  memcpy(out, "&gt;", 4);   out += 4; break;
                    case '&':  memcpy(out, "&amp;", 5);  out += 5; break;
                    case '"':  memcpy(out, "&quot;", 6); out += 6; break;
                    case '\'':
                        /* Apostrophes stay raw in double-quoted
                         * attribute values (libxml2, bug-86). */
                        *out++ = '\''; break;
                    case '\t':
                    case '\n':
                        /* libxml2's HTML serializer leaves tabs and
                         * newlines RAW in attribute values (bug-5-:
                         * <tr class="\nPROD\n">). XML keeps the
                         * char-ref escapes for round-trip fidelity. */
                        if (buf->html_method) { *out++ = val[i]; break; }
                        out += sprintf(out, val[i] == '\t' ? "&#9;" : "&#10;");
                        break;
                    case '\r': out += sprintf(out, "&#13;"); break;
                    default:   *out++ = val[i]; break;
                }
            }
            *out++ = '"';
            buf->size = (size_t)(out - buf->data);
            buf->data[buf->size] = '\0';
        }

        /* --- children --- */
        LeptrisNode* fc = leptris_node_first_child_internal((LeptrisNode*)e);
        if (!fc) {
            /* §16.2 html method: void elements end at '>'; every
             * other element closes explicitly (never `<x/>`). */
            if (buf->xhtml) {
                if (addmeta) {
                    buffer_append_char(buf, '>');
                    if (buf->indent_spaces > 0) {
                        buffer_append_newline(buf);
                        buf->indent++;
                        buffer_append_indent(buf);
                        buf->indent--;
                    }
                    emit_xhtml_meta(buf);
                    if (buf->indent_spaces > 0)
                        buffer_append_newline(buf);
                    buffer_append(buf, "</");
                    append_qualified_name(buf, epfx, name, nl);
                    buffer_append_char(buf, '>');
                } else if (!epl && xhtml_is_empty_elem(e, name, nl)) {
                    buffer_append(buf, " />");
                } else {
                    buffer_append(buf, "></");
                    append_qualified_name(buf, epfx, name, nl);
                    buffer_append_char(buf, '>');
                }
                if (!is_root_cur && buf->indent_spaces > 0 &&
                    !parent_mixed)
                    buffer_append_newline(buf);
                goto advance;
            }
            if (buf->html_method) {
                int hf = html_elem_sem_flags(e);
                if (hf & HTML_F_EMPTY) {
                    buffer_append(buf, ">");
                } else {
                    buffer_append(buf, "></");
                    append_qualified_name(buf, epfx, name, nl);
                    buffer_append_char(buf, '>');
                }
            } else if (buf->expand_empty) {
                buffer_append(buf, "></");
                append_qualified_name(buf, epfx, name, nl);
                buffer_append_char(buf, '>');
            } else {
                buffer_append(buf, "/>");
            }
            if (!is_root_cur && buf->indent_spaces > 0 &&
                (buf->html_method
                     ? html_break_after_elem(e, name, nl,
                                             html_elem_sem_flags(e))
                     : !parent_mixed))
                buffer_append_newline(buf);
            goto advance;
        }
        if (buf->indent_text == 0 && !addmeta &&
            fc->type == LEPTRIS_NODE_TYPE_TEXT &&
            leptris_node_get_next_sibling(fc) == NULL) {
            /* text-only element */
            if (is_cdata_element(buf, e) &&
                !((LeptrisTextNode*)fc)->base.raw) {
                buffer_append_char(buf, '>');
                const char* tcd = leptris_text_get_content(
                    (LeptrisTextNode*)fc);
                if (tcd)
                    emit_cdata_checked(buf, tcd);
                buffer_append(buf, "</");
                append_qualified_name(buf, epfx, name, nl);
                buffer_append_char(buf, '>');
                goto advance;
            }
            /* §16.2 rawtext elements (script/style/…): the single
             * text child is VERBATIM under method=html. */
            if (buf->html_method &&
                (html_elem_sem_flags(e) & HTML_F_RAW)) {
                const char* tr = leptris_text_get_content(
                    (LeptrisTextNode*)fc);
                buffer_append_char(buf, '>');
                if (tr) buffer_append(buf, tr);
                buffer_append(buf, "</");
                append_qualified_name(buf, epfx, name, nl);
                buffer_append_char(buf, '>');
                goto advance;
            }
            if (buf->indent_spaces == 0) {
                LeptrisTextNode* tn = (LeptrisTextNode*)fc;
                const char* tc;
                size_t tlen;
                if (tn->borrowed && tn->content_len > 0 &&
                    !memchr(tn->content, '&', tn->content_len)) {
                    tc = tn->content;
                    tlen = tn->content_len;
                } else {
                    tc = leptris_text_get_content(tn);
                    tlen = tc ? tn->content_len : 0;
                }
                buffer_ensure_capacity(buf, 2 + epl + nl + 6 * tlen + 2 + 1);
                char* te = buf->data + buf->size;
                *te++ = '>';
                if (tc && tlen) te = emit_escaped_inline(te, tc, tlen, 0);
                *te++ = '<'; *te++ = '/';
                if (epl) {
                    memcpy(te, epfx, epl - 1); te += epl - 1;
                    *te++ = ':';
                }
                memcpy(te, name, nl); te += nl;
                *te++ = '>';
                buf->size = (size_t)(te - buf->data);
                buf->data[buf->size] = '\0';
            } else {
                buffer_append_char(buf, '>');
                if (is_cdata_element(buf, e) &&
                    (fc->type == LEPTRIS_NODE_TYPE_TEXT ||
                     fc->type == LEPTRIS_NODE_TYPE_CDATA) &&
                    !((LeptrisTextNode*)fc)->base.raw) {
                    const char* tc2 =
                        leptris_text_get_content((LeptrisTextNode*)fc);
                    if (tc2)
                        emit_cdata_checked(buf, tc2);
                } else {
                    serialize_node_internal(fc, buf);
                }
                buffer_append(buf, "</");
                append_qualified_name(buf, epfx, name, nl);
                buffer_append_char(buf, '>');
                if (buf->html_method) {
                    if (html_break_after_elem(e, name, nl,
                                              html_elem_sem_flags(e)))
                        buffer_append_newline(buf);
                } else if (!is_root_cur && !parent_mixed) {
                    /* is_root guard (#633): the document's last line
                     * carries no trailing newline — the fusion path
                     * already had the guard; this open-tag path (any
                     * element with attributes) did not. */
                    buffer_append_newline(buf);
                }
            }
            goto advance;
        }

        /* complex children: descend — grow the frame stack at the
         * cap instead of switching walkers (unbounded depth is safe;
         * malloc failure truncates rather than recursing). */
        if ((size_t)sp == st_cap) {
            size_t nc = st_cap * 2;
            SerFrame* grown = (SerFrame*)realloc(st, nc * sizeof(SerFrame));
            if (!grown) {
                buffer_append_char(buf, '>');
                buffer_append(buf, "</");
                append_qualified_name(buf, epfx, name, nl);
                buffer_append_char(buf, '>');
                ser_frames_free(st, sp);
                return;
            }
            st = grown;
            st_cap = nc;
        }

        /* An element child interrupts any CDATA run of this parent. */
        if (sp > 0 && st[sp - 1].cd_open)
            ser_frame_cd_flush(buf, &st[sp - 1]);
        buffer_append_char(buf, '>');
        int mixed = 0;
        int close_brk = 0;
        if (buf->html_method) {
            /* §16.2: mixed is not a gate — the site rules own the
             * newlines. mixed=1 keeps ws-only text verbatim. */
            mixed = 1;
            int hf = html_elem_sem_flags(e);
            if (html_break_before_children(e, name, nl, hf))
                buffer_append_newline(buf);
            close_brk = html_break_before_close(e, name, nl, hf);
        } else {
            /* #129 indent_text: the formatter owns ALL whitespace —
             * no element is "mixed" for layout purposes. */
            if (buf->indent_spaces > 0 && !buf->indent_text)
                mixed = buf->ws_mixed
                    ? ser_children_have_any_text(fc)
                    : ser_children_have_text(fc);
            /* libxslt: once an element is mixed, formatting stops
             * for the whole subtree below it (bug-98) — mixed
             * inherits down the frame stack. */
            if (!mixed && sp > 0 && st[sp - 1].mixed) mixed = 1;
            if (buf->indent_spaces > 0 && !mixed)
                buffer_append_newline(buf);
        }
        if (addmeta) {
            /* libxml2: the meta leads head's children — indented on
             * its own line when formatting, inline when not. */
            if (buf->indent_spaces > 0 && !mixed) {
                buf->indent++;
                buffer_append_indent(buf);
                buf->indent--;
                emit_xhtml_meta(buf);
                buffer_append_newline(buf);
            } else {
                emit_xhtml_meta(buf);
            }
        }
        st[sp].e = e;
        st[sp].nl = nl;
        st[sp].mixed = mixed;
        st[sp].cd_open = 0;
        st[sp].cd_buf = NULL;
        st[sp].cd_len = 0;
        st[sp].cd_cap = 0;
        st[sp].close_brk = close_brk;
        sp++;
        buf->indent++;
        cur = fc;
        is_root_cur = 0;
        continue;

    advance:
        /* The walk's root completed (any completion path — close-tag,
         * leaf fast path, text-only, self-closing). Emit exactly the
         * caller's subtree: do NOT continue to the root's siblings
         * (issue #523 — the leak made element serialization both
         * wrong and O(document)). */
        if (sp == 0 && (LeptrisNode*)cur == (LeptrisNode*)root_elem) {
            free(st);
            return;
        }

        /* next sibling, else ascend closing frames */
        for (;;) {
            LeptrisNode* nsib = leptris_node_get_next_sibling(cur);
            if (nsib) {
                cur = nsib;
                break;
            }
            if (sp == 0) { free(st); return; }  /* root fully emitted */
            sp--;
            buf->indent--;
            LeptrisElement pe = st[sp].e;
            if (st[sp].cd_open)   /* close this element's CDATA run */
                ser_frame_cd_flush(buf, &st[sp]);
            free(st[sp].cd_buf);
            st[sp].cd_buf = NULL;
            const char* pn = pe->name;
            size_t pnl2 = st[sp].nl;
            const char* pfx2 = leptris_element_get_prefix(pe);
            /* #534: no indent inside a mixed-content element. HTML
             * mode: site-3 newline before the close tag, no spaces. */
            if (buf->html_method) {
                if (st[sp].close_brk) buffer_append_newline(buf);
            } else if (buf->indent_spaces > 0 && !st[sp].mixed) {
                buffer_append_indent(buf);
            }
            buffer_append(buf, "</");
            append_qualified_name(buf, pfx2, pn, pnl2);
            buffer_append_char(buf, '>');
            if (buf->indent_spaces > 0 &&
                !(sp == 0 && (LeptrisNode*)pe == (LeptrisNode*)root_elem && is_root)) {
                if (buf->html_method) {
                    if (html_break_after_elem(
                            pe, pn, pnl2, html_elem_sem_flags(pe)))
                        buffer_append_newline(buf);
                } else if (buf->indent_spaces > 0) {
                    /* Formatter-owned separator between element
                     * children — suppressed when the next sibling
                     * is a TEXT node: under a mixed parent its
                     * verbatim content already carries the layout
                     * (libxslt bug-98). */
                    LeptrisNode* nx = leptris_node_get_next_sibling(
                        (LeptrisNode*)pe);
                    if (!nx || nx->type != LEPTRIS_NODE_TYPE_TEXT)
                        buffer_append_newline(buf);
                }
            }
            if (sp == 0 && (LeptrisNode*)pe == (LeptrisNode*)root_elem) {
                /* The walk's root frame closed. STOP: an element
                 * serialize must emit exactly the caller's subtree —
                 * continuing to the root's siblings leaked every
                 * following element into the output (issue #523:
                 * both the garbage results and the O(document)
                 * per-call cost). */
                ser_frames_free(st, sp);
                return;
            }
            cur = (LeptrisNode*)pe;
        }
    }
}

void serialize_node_internal(LeptrisNode* node, SerializeBuffer* buf) {
    if (!node) return;

    /* Dispatch via the per-type vtable registry — adding a new node
     * type is purely additive (register a vtable in node_vtable.c). */
    const LeptrisNodeVTable* vt = leptris_node_vtable_for(node->type);
    if (vt && vt->serialize) {
        vt->serialize(node, buf);
    }
    /* Unknown/unregistered type — silently skip (matches old default). */
}

/* ============================================================================
 * Public API
 * ============================================================================ */

char* leptris_serialize_node(LeptrisNode* node) {
    if (!node) return NULL;

    SerializeBuffer* buf = buffer_create(0);  /* Compact mode */
    if (!buf) return NULL;

    serialize_node_internal(node, buf);

    char* result = buffer_to_string(buf);
    buffer_free(buf);

    return result;
}

char* leptris_serialize_element(LeptrisElement elem) {
    if (!elem) return NULL;

    SerializeBuffer* buf = buffer_create(0);  /* Compact mode */
    if (!buf) return NULL;

    serialize_element_internal(elem, buf, 1);  /* is_root=1 */

    char* result = buffer_to_string(buf);
    buffer_free(buf);

    return result;
}

/* ============================================================================
 * Utility: Serialize with XML Declaration
 * ============================================================================ */

char* leptris_serialize_document_with_declaration(LeptrisElement root,
                                                   const char* encoding,
                                                   const char* version,
                                                   int standalone,
                                                   int has_bom,
                                                   LeptrisDoctypeNode* doctype) {
    if (!root) return NULL;

    SerializeBuffer* buf = buffer_create(0);  /* Compact mode by default */
    if (!buf) return NULL;

    /* Output UTF-8 BOM if present in original */
    if (has_bom) {
        buffer_append_char(buf, (char)(unsigned char)0xEF);
        buffer_append_char(buf, (char)(unsigned char)0xBB);
        buffer_append_char(buf, (char)(unsigned char)0xBF);
    }

    /* Add XML declaration only if version is provided */
    if (version) {
        buffer_append(buf, "<?xml version=\"");
        buffer_append(buf, version);
        buffer_append_char(buf, '"');

        if (encoding) {
            buffer_append(buf, " encoding=\"");
            buffer_append(buf, encoding);
            buffer_append_char(buf, '"');
        }

        if (standalone >= 0) {
            buffer_append(buf, " standalone=\"");
            buffer_append(buf, standalone ? "yes" : "no");
            buffer_append_char(buf, '"');
        }

        buffer_append(buf, "?>");
    }

    /* Output DOCTYPE if present */
    if (doctype) {
        if (version) {
            buffer_append_char(buf, '\n');
        }
        serialize_doctype_internal(doctype, buf);
    }

    /* Serialize root element */
    if (version || doctype) {
        buffer_append_char(buf, '\n');
    }
    serialize_element_internal(root, buf, 1);  /* is_root=1 */

    char* result = buffer_to_string(buf);
    buffer_free(buf);

    return result;
}
/* ============================================================================
 * Document Serialization (matches leptris.h public API)
 * ============================================================================ */

/* Forward declarations for document structure access */
struct leptris_document;

/* Serialize document with options */
/* Full entry: public options + the internal extended settings.
 * extended==NULL leaves cdata/html off (the public default). */
/* #129: public extended options (LeptrisSerializeExtOptions) bridge
 * onto the internal LeptrisSerializeExtended — one conversion site,
 * the walker below reads only the SerializeBuffer fields. */
LEPTRIS_API char* leptris_document_serialize_ext(
    struct leptris_document* doc,
    const LeptrisSerializeOptions* options,
    const LeptrisSerializeExtOptions* ext) {
    return leptris_document_serialize_ext_sized(
        doc, options, ext, ext ? sizeof(*ext) : 0);
}

/* Size-aware entry (issue #644): the ext struct MAY grow, and FFI
 * callers allocate only the fields they know — reading past their
 * buffer picked up heap garbage as indent_unit (an MSVC crash in
 * leptris-ruby). Each field is read only when the caller's buffer
 * actually covers it. */
LEPTRIS_API char* leptris_document_serialize_ext_sized(
    struct leptris_document* doc,
    const LeptrisSerializeOptions* options,
    const LeptrisSerializeExtOptions* ext,
    size_t ext_size) {
    LeptrisSerializeExtended internal;
    memset(&internal, 0, sizeof(internal));
    if (ext) {
        if (ext_size >= offsetof(LeptrisSerializeExtOptions, indent_text) +
                            sizeof(int))
            internal.indent_text = ext->indent_text;
        if (ext_size >= offsetof(LeptrisSerializeExtOptions, indent_unit) +
                            sizeof(const char*))
            internal.indent_unit = ext->indent_unit;
        if (ext_size >= offsetof(LeptrisSerializeExtOptions, expand_empty) +
                            sizeof(int))
            internal.expand_empty = ext->expand_empty;
    }
    return leptris_document_serialize_ex(doc, options, &internal);
}

/* #882: element-level twins of the ext entries — the bridge logic
 * duplicated here is exactly the document version's (one conversion
 * site per entry point; the walker reads only buffer fields). */
LEPTRIS_API char* leptris_element_serialize_ext(
    LeptrisElement elem,
    const LeptrisSerializeOptions* options,
    const LeptrisSerializeExtOptions* ext) {
    return leptris_element_serialize_ext_sized(
        elem, options, ext, ext ? sizeof(*ext) : 0);
}

LEPTRIS_API char* leptris_element_serialize_ext_sized(
    LeptrisElement elem,
    const LeptrisSerializeOptions* options,
    const LeptrisSerializeExtOptions* ext,
    size_t ext_size) {
    LeptrisSerializeExtended internal;
    memset(&internal, 0, sizeof(internal));
    if (ext) {
        if (ext_size >= offsetof(LeptrisSerializeExtOptions, indent_text) +
                            sizeof(int))
            internal.indent_text = ext->indent_text;
        if (ext_size >= offsetof(LeptrisSerializeExtOptions, indent_unit) +
                            sizeof(const char*))
            internal.indent_unit = ext->indent_unit;
        if (ext_size >= offsetof(LeptrisSerializeExtOptions, expand_empty) +
                            sizeof(int))
            internal.expand_empty = ext->expand_empty;
    }
    return leptris_element_serialize_ex(elem, options, &internal);
}

/* Element-level twin of leptris_document_serialize_ex: the XSLT
 * layer serializes a result FRAGMENT's top-level elements one by
 * one (bug-90) — without this entry they lost cdata-section-elements,
 * html and xhtml semantics, which the public options struct cannot
 * carry (#568). */
char* leptris_element_serialize_ex(LeptrisElement elem,
                                   const LeptrisSerializeOptions* options,
                                   const LeptrisSerializeExtended* extended) {
    if (!elem) return NULL;
    int indent_spaces = options ? options->indent : 0;
    SerializeBuffer* buf = buffer_create(indent_spaces);
    if (!buf) return NULL;
    if (extended) {
        buf->cdata_names = extended->cdata_elements;
        buf->cdata_count = extended->cdata_element_count;
        buf->html_method = extended->html_method != 0;
        buf->indent_text = extended->indent_text != 0;
        buf->expand_empty = extended->expand_empty != 0;
        buf->indent_unit = extended->indent_unit;
        buf->ws_mixed = extended->ws_mixed != 0;
        buf->xhtml = extended->xhtml != 0;
        buf->xhtml_encoding = options && options->encoding
                                  ? options->encoding : "UTF-8";
    }
    serialize_element_internal(elem, buf, 1);
    char* result = buffer_to_string(buf);
    buffer_free(buf);
    return result;
}

char* leptris_document_serialize_ex(struct leptris_document* doc,
                                    const LeptrisSerializeOptions* options,
                                    const LeptrisSerializeExtended* extended) {
    if (!doc) return NULL;

    /* FlatDoc serialize fast path removed — direct_parse builds the
     * LeptrisElement tree eagerly. Serialization always walks the DOM. */

    /* Get root element from new_dom_root field */
    /* TODO 139 Phase D: trigger lazy promote if the doc was produced
     * by the flat-parse fast path. */
    leptris_document_ensure_promoted(doc);
    LeptrisElement root = (LeptrisElement)doc->new_dom_root;
    if (!root) {
        char* rootless = serialize_rootless_pis(doc, options);
        if (rootless) return rootless;
        return NULL;
    }

    /* Use default options if NULL */
    int xml_declaration = 0;
    int indent_spaces = 0;
    const char* encoding = NULL;

    if (options) {
        xml_declaration = options->xml_declaration;
        indent_spaces = options->indent;
        encoding = options->encoding;
    }

    /* Create buffer with indent support */
    SerializeBuffer* buf = buffer_create(indent_spaces);
    if (!buf) return NULL;
    if (extended) {
        buf->cdata_names = extended->cdata_elements;
        buf->cdata_count = extended->cdata_element_count;
        /* §16.2: html semantics (void elements, attr escaping, raw
         * text, PI form) apply whenever the method is html — also
         * with indent="no". The newline LAYOUT self-gates: every
         * html break site goes through buffer_append_newline, which
         * is a no-op at indent_spaces == 0. */
        buf->html_method = extended->html_method != 0;
        buf->indent_text = extended->indent_text != 0;
        buf->expand_empty = extended->expand_empty != 0;
        buf->indent_unit = extended->indent_unit;
        buf->ws_mixed = extended->ws_mixed != 0;
        buf->xhtml = extended->xhtml != 0;
        buf->xhtml_encoding = encoding ? encoding : "UTF-8";
    }

    /* Output UTF-8 BOM if present in original */
    if (doc->has_bom) {
        buffer_append_char(buf, (char)(unsigned char)0xEF);
        buffer_append_char(buf, (char)(unsigned char)0xBB);
        buffer_append_char(buf, (char)(unsigned char)0xBF);
    }

    /* Add XML declaration if requested or if doc had one */
    const char* xml_version = doc->xml_version;
    int standalone = doc->standalone;

    /* If xml_declaration is explicitly requested but doc has no version,
     * fall back to XML 1.0 with no standalone attribute. */
    if (xml_declaration && !xml_version) {
        xml_version = "1.0";
        standalone = -1;
    }

    if ((xml_declaration || (doc->had_declaration && xml_version)) && xml_version) {
        buffer_append(buf, "<?xml version=\"");
        buffer_append(buf, xml_version);
        buffer_append_char(buf, '"');

        /* TODO.bindings/06: with iconv (default build) the body was
         * transcoded to UTF-8 at parse time — the declaration says
         * so, always. Without iconv, bytes pass through unchanged:
         * echoing the original encoding stays truthful. Either way
         * the declaration never lies, and serialize(serialize(x))
         * is byte-stable. */
        const char* enc = encoding ? encoding : doc->encoding;
        if (enc) {
            buffer_append(buf, " encoding=\"");
            if ((extended && extended->decl_encoding_verbatim &&
                 options && options->encoding) ||
                (enc[0] == 'U' || enc[0] == 'u')) {
                /* Verbatim: either a UTF-8-spelled name, or an
                 * explicit request from a caller that transcodes the
                 * body to match (the XSLT layer, bug-140). The
                 * parse-echo fallback below only applies to
                 * encodings inherited from a parsed document. */
                buffer_append(buf, enc);
#ifdef LEPTRIS_HAS_ICONV
            } else if (enc[0]) {
                buffer_append(buf, "UTF-8");
            }
#else
            } else {
                /* No iconv: bytes pass through unchanged at parse —
                 * echoing the source encoding stays truthful. */
                buffer_append(buf, enc);
            }
#endif
            buffer_append_char(buf, '"');
        }

        if (standalone >= 0) {
            buffer_append(buf, " standalone=\"");
            buffer_append(buf, standalone ? "yes" : "no");
            buffer_append_char(buf, '"');
        }

        buffer_append(buf, "?>");

        /* Add newline after declaration if indenting */
        if (indent_spaces > 0) {
            buffer_append_newline(buf);
        }
    }

    /* Output DOCTYPE if present */
    if (doc->doctype) {
        serialize_doctype_internal((LeptrisDoctypeNode*)doc->doctype, buf);
        if (indent_spaces > 0) {
            buffer_append_newline(buf);
        }
    }

    /* Document-level nodes (issue #580): the child chain in document
     * order — prolog nodes before the root, the root itself
     * serialized below, epilog nodes after. The single chain keeps
     * comment/PI interleaving the old side lists lost. */
    LeptrisNode* doc_root_node = (LeptrisNode*)root;
    for (LeptrisNode* c = (LeptrisNode*)doc->doc_children_head; c;
         c = leptris_node_get_next_sibling(c)) {
        if (c == doc_root_node) break;
        if (c->type == LEPTRIS_NODE_TYPE_PI) {
            LeptrisPINode* pi = (LeptrisPINode*)c;
            buffer_append(buf, "<?");
            if (pi->target) buffer_append(buf, pi->target);
            if (pi->data && pi->data[0]) {
                buffer_append_char(buf, ' ');
                buffer_append(buf, pi->data);
            }
            buffer_append(buf, "?>");
            if (indent_spaces > 0) {
                buffer_append_newline(buf);
            }
        } else if (c->type == LEPTRIS_NODE_TYPE_COMMENT) {
            buffer_append(buf, "<!--");
            buffer_append(buf,
                leptris_comment_get_content((LeptrisCommentNode*)c));
            buffer_append(buf, "-->");
            if (indent_spaces > 0) {
                buffer_append_newline(buf);
            }
        } else if (c->type == LEPTRIS_NODE_TYPE_TEXT) {
            /* Top-level text (copied source whitespace between
             * prolog nodes — bug-195) emits verbatim. */
            const char* tc =
                leptris_text_get_content((LeptrisTextNode*)c);
            if (tc) buffer_append(buf, tc);
        }
    }

    /* Serialize root element */
    serialize_element_internal(root, buf, 1);  /* is_root=1 */

    /* Epilog: the chain nodes AFTER the root element. Gated on the
     * #580 chain — result-fragment documents chain top-level nodes
     * as root siblings WITHOUT a doc_children chain, and their
     * serializer (xslt_public) owns that walk. */
    if (doc->doc_children_head) {
    for (LeptrisNode* c = leptris_node_get_next_sibling(doc_root_node); c;
         c = leptris_node_get_next_sibling(c)) {
        if (c->type == LEPTRIS_NODE_TYPE_PI) {
            LeptrisPINode* pi = (LeptrisPINode*)c;
            buffer_append(buf, "<?");
            if (pi->target) buffer_append(buf, pi->target);
            if (pi->data && pi->data[0]) {
                buffer_append_char(buf, ' ');
                buffer_append(buf, pi->data);
            }
            buffer_append(buf, "?>");
            if (indent_spaces > 0) {
                buffer_append_newline(buf);
            }
        } else if (c->type == LEPTRIS_NODE_TYPE_COMMENT) {
            buffer_append(buf, "<!--");
            buffer_append(buf,
                leptris_comment_get_content((LeptrisCommentNode*)c));
            buffer_append(buf, "-->");
            if (indent_spaces > 0) {
                buffer_append_newline(buf);
            }
        } else if (c->type == LEPTRIS_NODE_TYPE_TEXT) {
            const char* tc =
                leptris_text_get_content((LeptrisTextNode*)c);
            if (tc) buffer_append(buf, tc);
        }
    }
    }

    char* result = buffer_to_string(buf);
    buffer_free(buf);

    return result;
}

/* Public entry (ABI-frozen options only — issue #568): reads
 * exclusively the three frozen fields; cdata/html stay off. */
LEPTRIS_API char* leptris_document_serialize(struct leptris_document* doc,
                                 const LeptrisSerializeOptions* options) {
    return leptris_document_serialize_ex(doc, options, NULL);
}


/* ============================================================================
 * Element Serialization (matches leptris.h public API)
 * ============================================================================ */

/* Serialize element subtree to XML string */
LEPTRIS_API char* leptris_element_serialize(LeptrisElement elem,
                                 const LeptrisSerializeOptions* options) {
    if (!elem) return NULL;

    int indent_spaces = 0;
    if (options) {
        indent_spaces = options->indent;
    }

    SerializeBuffer* buf = buffer_create(indent_spaces);
    if (!buf) return NULL;

    serialize_element_internal(elem, buf, 1);  /* is_root=1 */

    char* result = buffer_to_string(buf);
    buffer_free(buf);

    return result;
}

/* Issue #541: mem-cache the serialized string so the size-query
 * + into-call pattern reuses a single serialization. */
static int ser_cache_valid(const struct leptris_document* doc,
                           const LeptrisSerializeOptions* options) {
    if (!doc->ser_cache) return 0;
    if (doc->ser_version != doc->ser_cache_version) return 0;
    int text = options ? !options->xml_declaration : 1;
    int omit = options ? !options->xml_declaration : 0;
    int indent = options ? !!options->indent : 0;
    return text == doc->ser_cache_text && omit == doc->ser_cache_omit &&
           indent == doc->ser_cache_indent;
}

LEPTRIS_API size_t leptris_document_serialize_into(LeptrisDocument doc,
                                                   char* buf,
                                                   size_t capacity,
                                                   size_t* out_len,
                                                   const LeptrisSerializeOptions* options) {
    if (!doc) return 0;
    if (ser_cache_valid(doc, options)) {
        size_t need = doc->ser_cache_len + 1;
        if (buf && capacity >= need) {
            memcpy(buf, doc->ser_cache, need);
        }
        /* #550: the sizing pass (buf NULL / too small) reports the
         * needed length through out_len too — callers allocate from
         * it. The cached path previously left it untouched. */
        if (out_len) *out_len = doc->ser_cache_len;
        return need;
    }
    char* tmp = leptris_document_serialize(doc, options);
    if (!tmp) return 0;
    size_t need = strlen(tmp) + 1;
    if (buf && capacity >= need) {
        memcpy(buf, tmp, need);
    }
    if (out_len) *out_len = need - 1;
    /* Cache for the no-options (defaults) path; option permutations
     * stay one-shot to keep the cache key simple. */
    if (!options) {
        free(doc->ser_cache);
        doc->ser_cache = tmp;                 /* ownership transferred */
        doc->ser_cache_len = need - 1;
        doc->ser_cache_version = doc->ser_version;
        doc->ser_cache_text = 0;
        doc->ser_cache_omit = 0;
        doc->ser_cache_indent = 0;
    } else {
        leptris_free_string(tmp);
    }
    return need;
}

LEPTRIS_API size_t leptris_element_serialize_into(LeptrisElement elem,
                                                  char* buf,
                                                  size_t capacity,
                                                  size_t* out_len,
                                                  const LeptrisSerializeOptions* options) {
    char* tmp = leptris_element_serialize(elem, options);
    if (!tmp) return 0;
    size_t need = strlen(tmp) + 1;
    if (buf && capacity >= need) {
        memcpy(buf, tmp, need);
    }
    if (out_len) *out_len = need - 1;   /* sizing pass too (#550) */
    leptris_free_string(tmp);
    return need;
}

/* Save document to file */
LEPTRIS_API int leptris_document_save_file(struct leptris_document* doc,
                               const char* filepath,
                               const LeptrisSerializeOptions* options) {
    if (!doc) return -4;  /* LEPTRIS_ERROR_NULL_ARG */
    if (!filepath) return -4;  /* LEPTRIS_ERROR_NULL_ARG */

    /* Serialize document to XML string */
    char* xml = leptris_document_serialize(doc, options);
    if (!xml) return -1;  /* LEPTRIS_ERROR_MEMORY */

    /* Open file for writing */
    FILE* file = fopen(filepath, "wb");
    if (!file) {
        LEPTRIS_FREE(xml);
        return -7;  /* LEPTRIS_ERROR_IO */
    }

    /* Write XML content to file */
    size_t len = strlen(xml);
    size_t written = fwrite(xml, 1, len, file);
    fclose(file);

    /* Free the XML string */
    LEPTRIS_FREE(xml);

    /* Check if all bytes were written */
    if (written != len) {
        return -7;  /* LEPTRIS_ERROR_IO */
    }

    return 0;  /* LEPTRIS_OK */
}
