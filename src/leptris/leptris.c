/* leptris.c - Leptris public API implementation
 * Copyright (c) 2024, Ribose Inc.
 *
 * Pure C XML parser and XPath evaluator - Public API.
 */

#include "../include/leptris.h"
#include "leptris_internal.h"
#include "leptris/memory/arena.h"
#include "xpath/parser.h"
#include "xpath/evaluator.h"
#include "xpath/xpath_variables.h"
#include "dom/element.h"
#include "dom/element_index.h"
#include "dom/node.h"
#include "dom/text.h"
#include "dom/comment.h"
#include "dom/cdata.h"
#include "dom/pi.h"
#include "dom/doctype.h"
#include "encoding/utf16.h"
#include "dtd/model.h"
/* The flat/ headers are no longer included — flat_parser and
 * flat_promote are deleted. direct_parse (flat/direct_parse.c)
 * is the sole parser. */
#include "common/entities.h"
#include "common/port.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

/* Thread-local globals defined in core.c — extern so leptris_parse can
 * read the strict-mode default at document creation time. */
extern LEPTRIS_THREAD_LOCAL int g_leptris_strict_mode;
extern LEPTRIS_THREAD_LOCAL int g_leptris_max_depth;


/* ============================================================================
 * Utility Macros
 * ============================================================================ */

/* Macro for appending strings to a dynamic buffer */
#define APPEND_STRING(str, len) do { \
    if (!buffer) goto cleanup; \
    while (*size + (len) + 1 > *capacity) { \
        size_t new_cap = *capacity * 2; \
        char* new_buf = (char*)realloc(*buffer, new_cap); \
        if (!new_buf) { \
            free(*buffer); \
            *buffer = NULL; \
            goto cleanup; \
        } \
        *buffer = new_buf; \
        *capacity = new_cap; \
    } \
    memcpy(*buffer + *size, str, len); \
    *size += len; \
    (*buffer)[*size] = '\0'; \
} while(0)

/* ============================================================================
 * Version Constants
 * ============================================================================ */


/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

/* Forward declarations for new parser/serializer */
/* Legacy parser (parser_new.c) deleted — direct_parse + flat_parse
 * cover the full XML feature set. These extern declarations are
 * retained as a marker; the functions no longer exist. */
extern void leptris_element_free(LeptrisElement elem);
extern void leptris_doctype_free(LeptrisDoctypeNode* doctype);

/* ============================================================================
 * Internal Parse Function Implementation
 * ============================================================================ */

/**
 * Parse XML string into document (internal implementation)
 *
 * PERFORMANCE: Uses in-place parsing to avoid buffer copy.
 * The buffer is stored in the document and freed when document is freed.
 */
/* TODO 139 Phase D: detect DOCTYPE with internal subset.
 *
 * The flat parser silently strips DTD entity declarations. If the
 * input has `<!DOCTYPE ... [...]>`, we must route to the legacy
 * parser so entities expand correctly. The check scans up to 4 KB
 * of input — sufficient for any reasonable DOCTYPE declaration.
 *
 * Returns 1 if the input has a DOCTYPE with internal subset, 0
 * otherwise (including no DOCTYPE at all). */
struct leptris_document* leptris_parse(const char* xml, size_t len) {
    if (!xml || len == 0) return NULL;

    /* Fast path: try direct_parse first. It now handles DTD internal
     * subsets (custom entity declarations) via leptris_dtd_parse_-
     * internal_subset, so the DTD gate is removed. The flat_parse
     * + lazy promote fallback remains for inputs direct_parse
     * rejects (malformed constructs, edge cases).
     *
     * Predefined entities (&amp;, &lt;, etc.) expand lazily via
     * leptris_text_get_content / attr accessor. Custom entities
     * (&foo; from DTD) expand eagerly via DTD-aware decoder.
     *
     * direct_parse respects g_leptris_max_depth when set by the
     * caller (custom depth limit). No separate parser is needed. */
    /* direct_parse is the sole parser. It handles:
     * - Elements, attributes, text, CDATA, comments, PIs
     * - XML declaration, DOCTYPE (with DTD entity expansion)
     * - Namespaces (xmlns, prefix:local splitting)
     * - Predefined entities (&amp;, &lt;, etc.) via lazy expansion
     * - Custom DTD entities (&foo;) via DTD-aware expansion
     * - UTF-8 multibyte names (<café>) via CT_UTF8 chartype flag
     * - BOM detection, encoding passthrough
     * - Custom depth limits (g_leptris_max_depth)
     *
     * The flat_parser + lazy-promote path (TODO 139) and the legacy
     * parser (parser_new.c) have been removed. One parser, like
     * pugixml. If direct_parse returns NULL, the input is genuinely
     * malformed. */
    extern struct leptris_document* direct_parse(const char*, size_t);
    return direct_parse(xml, len);
}

/**
 * Parse XML string into document with in-place optimization (internal implementation)
 *
 * The caller-owned writable buffer is passed to leptris_parse, which
 * The caller's writable buffer is parsed IN-PLACE — no copy. The
 * buffer is modified (NUL-terminated at name/value boundaries).
 * The document does NOT free the buffer — caller must ensure it
 * outlives the document.
 */
static struct leptris_document* leptris_parse_inplace(char* xml, size_t len) {
    extern struct leptris_document* direct_parse_inplace(char*, size_t);
    return direct_parse_inplace(xml, len);
}

/* ============================================================================
 * Version Information
 * ============================================================================ */

/* ============================================================================
 * Parse Options
 * ============================================================================ */

/**
 * Initialize parse options with defaults
 */
void leptris_parse_options_init(leptris_parse_options* opts) {
    if (!opts) return;

    opts->strict = 1;              /* Strict mode by default */
    opts->preserve_whitespace = 0; /* Don't preserve whitespace by default */
    opts->track_positions = 0;     /* Don't track positions by default */
}

/* ============================================================================
 * Document Functions
 * ============================================================================ */

/**
 * Create an empty document (Public API)
 *
 * Mirrors the document initialization direct_parse performs for
 * parsed documents, minus the buffer and tree: a fresh pool, one
 * pool-allocated (TODO 154) zeroed struct, strict mode inherited
 * from the thread-local setting, standalone unset (-1).
 */
LEPTRIS_API LeptrisDocument leptris_document_create(void) {
    extern LeptrisMemoryPool* leptris_pool_create(void);
    extern void* leptris_pool_alloc(LeptrisMemoryPool* pool, size_t size);
    extern void* leptris_pool_get_base(LeptrisMemoryPool* pool);

    LeptrisMemoryPool* pool = leptris_pool_create();
    if (!pool) return NULL;

    struct leptris_document* doc =
        (struct leptris_document*)leptris_pool_alloc(pool, sizeof(struct leptris_document));
    if (!doc) {
        extern void leptris_pool_destroy(LeptrisMemoryPool* pool);
        leptris_pool_destroy(pool);
        return NULL;
    }
    memset(doc, 0, sizeof(*doc));
    doc->doc_pool_allocated = 1;
    doc->strict_mode = g_leptris_strict_mode;
    doc->pool = pool;
    doc->page_base = leptris_pool_get_base(pool);
    doc->ref_count = 1;
    doc->standalone = -1;
    return doc;
}

/* Internal (issue #563): a document over a CALLER-OWNED arena.
 * The pool is arena-backed and does not own the arena — destroying
 * the document frees the pool struct and its extension blocks, the
 * arena lives on (and may be bump-reset) under the caller. Used by
 * iterparse, which materializes one subtree per yield and reuses
 * one arena across children instead of paying a 32 KB page create/
 * destroy cycle per element. */
struct leptris_document* leptris_document_create_on_arena(
        LeptrisArena* arena) {
    extern LeptrisMemoryPool* leptris_pool_create_arena_backed(
        LeptrisArena*, int);
    if (!arena) return NULL;
    LeptrisMemoryPool* pool =
        leptris_pool_create_arena_backed(arena, 0);
    if (!pool) return NULL;
    struct leptris_document* doc =
        (struct leptris_document*)leptris_pool_alloc(
            pool, sizeof(struct leptris_document));
    if (!doc) {
        extern void leptris_pool_destroy(LeptrisMemoryPool*);
        leptris_pool_destroy(pool);
        return NULL;
    }
    memset(doc, 0, sizeof(*doc));
    doc->doc_pool_allocated = 1;
    doc->strict_mode = g_leptris_strict_mode;
    doc->pool = pool;
    doc->page_base = leptris_arena_base(arena);
    doc->ref_count = 1;
    doc->standalone = -1;
    return doc;
}

/**
 * Attach an element as the document root (Public API)
 *
 * Validation mirrors what the parser guarantees for parsed roots:
 * no parent, owned by this document's pool, registered in the
 * thread-local root→doc map. The previous root (if any) is left
 * detached but alive — the pool owns it until document free.
 */
LEPTRIS_API LeptrisStatus leptris_document_set_root(LeptrisDocument doc,
                                                    LeptrisElement root) {
    if (!doc || !root) return LEPTRIS_ERROR_NULL_ARG;

    /* Already attached under a parent? (parent_off == 0 encodes NULL;
     * leptris_elem_parent is the static inline accessor from dom/element.h) */
    if (leptris_elem_parent(root))
        return LEPTRIS_ERROR_INVALID_ARG;

    /* Cross-document attach would dangle the source pool on free.
     * Lane 18 P1: resolve via get_document (map + namebp fallback)
     * — public mut-block creates carry no map entry, and the raw
     * lookup rejected every one of them. */
    extern struct leptris_document* leptris_element_get_document(
        LeptrisElement elem);
    if (leptris_element_get_document(root) != doc)
        return LEPTRIS_ERROR_INVALID_ARG;

    doc->root = root;
    doc->new_dom_root = root;
    extern void leptris_root_doc_register(LeptrisElement root,
                                          struct leptris_document* doc);
    leptris_root_doc_register(root, doc);
    /* #612: keep the document-child chain coherent — the document
     * node's view must list the NEW root at the old root's slot
     * (prolog nodes before, epilog after), not the previous one. */
    {
        LeptrisNode* newn = (LeptrisNode*)root;
        LeptrisNode* c = (LeptrisNode*)doc->doc_children_head;
        LeptrisNode* prev = NULL;
        while (c && c->type != LEPTRIS_NODE_TYPE_ELEMENT) {
            prev = c;
            c = leptris_node_get_next_sibling(c);
        }
        if (doc->doc_children_head) {
            /* Maintain an EXISTING chain only — parsed documents and
             * ones with document-level nodes. Result-fragment docs
             * chain top-level nodes as root SIBLINGS without a chain
             * (their serializer owns that walk); creating one here
             * double-emitted their epilog. */
            if (c) {
                /* Splice new in at the old root's position. */
                if (prev) leptris_node_set_next_sibling(prev, newn);
                else doc->doc_children_head = newn;
                leptris_node_set_next_sibling(
                    newn, leptris_node_get_next_sibling(c));
            } else {
                /* Rootless chain: append the root at the tail. */
                LeptrisNode* t = (LeptrisNode*)doc->doc_children_head;
                while (leptris_node_get_next_sibling(t))
                    t = leptris_node_get_next_sibling(t);
                leptris_node_set_next_sibling(t, newn);
            }
            doc->doc_children_tail = newn;
        }
    }
    return LEPTRIS_OK;
}

/* #612: remove a document-level PI. Identifies it by target string
 * (first match in document order) or, when target is NULL, by index
 * among the document's PIs. The node is pool-owned — it is unlinked,
 * not freed; its lifetime remains the document's. */
LEPTRIS_API LeptrisNodeRef leptris_document_remove_pi(LeptrisDocument doc,
                                                      const char* target,
                                                      size_t index) {
    if (!doc) return NULL;
    LeptrisNode* prev = NULL;
    size_t i = 0;
    for (LeptrisNode* c = (LeptrisNode*)doc->doc_children_head; c; ) {
        LeptrisNode* next = leptris_node_get_next_sibling(c);
        if (c->type == LEPTRIS_NODE_TYPE_PI) {
            int hit;
            if (target) {
                const char* t = leptris_pi_get_target((LeptrisPINode*)c);
                hit = t && strcmp(t, target) == 0;
            } else {
                hit = (i == index);
            }
            if (hit) {
                if (prev)
                    leptris_node_set_next_sibling(prev, next);
                else
                    doc->doc_children_head = next;
                if (doc->doc_children_tail == c)
                    doc->doc_children_tail = prev;
                return (LeptrisNodeRef)c;
            }
            i++;
        }
        prev = c;
        c = next;
    }
    return NULL;
}

/**
 * Parse XML string into document (Public API wrapper)
 *
 * This function automatically detects and converts various encodings to UTF-8,
 * including: UTF-16 (LE/BE), EBCDIC, ISO-8859-*, EUC-JP, Shift-JIS, etc.
 */
LEPTRIS_API LeptrisDocument leptris_parse_string(const char* xml, size_t length, LeptrisStatus* status) {
    if (status) *status = LEPTRIS_OK;

    /* Fast path: skip encoding detection for the overwhelmingly common
     * case — pure UTF-8 (or ASCII subset) input with no XML declaration.
     * Saves two malloc+memcpy per call (iconv auto-convert + direct_parse
     * internal copy) by going straight to direct_parse, which is the
     * only path that needs to copy. Mirrors pugixml's parse_fast path.
     *
     * Triggers when ALL of:
     *   - First non-WS byte is '<' (so it's likely XML).
     *   - Not the UTF-16 BOM byte sequence.
     *   - No NULL byte in the prefix (which would indicate UTF-16).
     *   - Doesn't start with "<?xml" (declaration may specify a
     *     non-UTF-8 encoding → fall through to iconv). */
    if (xml && length > 0) {
        const unsigned char* d = (const unsigned char*)xml;
        size_t i = 0;
        while (i < length && (d[i] == ' ' || d[i] == '\t' ||
                              d[i] == '\n' || d[i] == '\r')) {
            i++;
        }
        /* UTF-16 BOM? */
        if (length - i >= 2 &&
            ((d[i] == 0xFF && d[i + 1] == 0xFE) ||
             (d[i] == 0xFE && d[i + 1] == 0xFF))) {
            /* Fall through to slow path. */
        } else if (i < length && d[i] == '<' &&
                   !(length >= 5 && d[i + 1] == '?' &&
                     d[i + 2] == 'x' && d[i + 3] == 'm' && d[i + 4] == 'l')) {
            /* Not a "<?xml" declaration. Check for embedded NUL bytes
             * in the first 64 bytes (UTF-16 without BOM indicator). */
            int has_nul = 0;
            size_t check = (length < 64) ? length : 64;
            for (size_t j = i; j < check; j++) {
                if (d[j] == 0) { has_nul = 1; break; }
            }
            if (!has_nul) {
                struct leptris_document* doc = leptris_parse(xml, length);
                if (!doc && status) *status = LEPTRIS_ERROR_PARSE;
                return doc;
            }
        }
    }

    /* Try native UTF-16 detection first (works without iconv) */
    #include "encoding/utf16.h"

    const unsigned char* data = (const unsigned char*)xml;
    utf16_bom_t bom = utf16_detect_bom(data, length);

    if (bom == UTF16_BOM_LE || bom == UTF16_BOM_BE) {
        /* UTF-16 with BOM - convert to UTF-8 */
        utf16_encoding_t encoding = (bom == UTF16_BOM_LE) ? UTF16_LE : UTF16_BE;

        size_t utf8_size = utf16_to_utf8_size(data, length, encoding);
        char* utf8_buffer = (char*)malloc(utf8_size);
        if (!utf8_buffer) {
            if (status) *status = LEPTRIS_ERROR_MEMORY;
            return NULL;
        }

        size_t utf8_len = utf16_to_utf8(data, length, utf8_buffer, utf8_size, encoding);
        utf8_buffer[utf8_len] = '\0';

        struct leptris_document* doc = leptris_parse(utf8_buffer, utf8_len);

        /* leptris_parse() heap-copies its input into doc->xml_buffer and
         * parses in-place from there; the document's StringViews point
         * into that inner copy, NOT into utf8_buffer.  Free our
         * intermediate conversion buffer — its contents are already
         * preserved inside the document. */
        free(utf8_buffer);

        if (doc) {
            if (doc->encoding) {
                LEPTRIS_FREE(doc->encoding);
            }
            doc->encoding = leptris_strdup((encoding == UTF16_LE) ? "UTF-16LE" : "UTF-16BE");
        }

        if (!doc && status) {
            *status = LEPTRIS_ERROR_PARSE;
        }

        return doc;
    }

    /* Check for UTF-16 without BOM using heuristic detection */
    utf16_encoding_t detected = utf16_detect_encoding(data, length);
    if (detected == UTF16_LE || detected == UTF16_BE) {
        /* UTF-16 without BOM - convert to UTF-8 */
        size_t utf8_size = utf16_to_utf8_size(data, length, detected);
        char* utf8_buffer = (char*)malloc(utf8_size);
        if (!utf8_buffer) {
            if (status) *status = LEPTRIS_ERROR_MEMORY;
            return NULL;
        }

        size_t utf8_len = utf16_to_utf8(data, length, utf8_buffer, utf8_size, detected);
        utf8_buffer[utf8_len] = '\0';

        struct leptris_document* doc = leptris_parse(utf8_buffer, utf8_len);

        /* See UTF-16-with-BOM comment above: leptris_parse already copied
         * the buffer; ours is now redundant. */
        free(utf8_buffer);

        if (doc) {
            if (doc->encoding) {
                LEPTRIS_FREE(doc->encoding);
            }
            doc->encoding = leptris_strdup((detected == UTF16_LE) ? "UTF-16LE" : "UTF-16BE");
        }

        if (!doc && status) {
            *status = LEPTRIS_ERROR_PARSE;
        }

        return doc;
    }

#ifdef LEPTRIS_HAS_ICONV
    /* Include encoding support for other encodings */
    #include "encoding/encoding.h"

    /* Auto-detect and convert to UTF-8 */
    size_t utf8_len = 0;
    char* detected_encoding = NULL;
    char* utf8_xml = leptris_encoding_auto_convert(xml, length, &utf8_len, &detected_encoding);

    if (!utf8_xml) {
        if (status) *status = LEPTRIS_ERROR_PARSE;
        if (detected_encoding) free(detected_encoding);
        return NULL;
    }

    /* Parse the UTF-8 content */
    struct leptris_document* doc = leptris_parse(utf8_xml, utf8_len);

    /* Store detected encoding in document if parsed successfully */
    if (doc && detected_encoding) {
        if (doc->encoding) {
            LEPTRIS_FREE(doc->encoding);
        }
        doc->encoding = leptris_strdup(detected_encoding);
    }

    /* leptris_parse() already heap-copied utf8_xml into doc->xml_buffer;
     * the document's StringViews point into that inner copy.  Our
     * conversion buffer is redundant — free it. */
    if (utf8_xml != xml) {
        free(utf8_xml);
    }

    if (detected_encoding) {
        free(detected_encoding);
    }

    if (!doc && status) {
        *status = LEPTRIS_ERROR_PARSE;
    }

    return doc;
#else
    /* No iconv support - fall back to regular parsing (assumes UTF-8) */
    struct leptris_document* doc = leptris_parse(xml, length);

    if (!doc && status) {
        *status = LEPTRIS_ERROR_PARSE;
    }

    return doc;
#endif
}

/**
 * Parse XML string into document with in-place optimization (Public API wrapper)
 */

/* TODO.bindings/05: per-parse options — the thread-global
 * strict/depth setters stay for compatibility, but callers sharing a
 * thread (e.g. pooled bindings) can scope configuration to one call.
 * Save/restore around the parse: NOT reentrant — do not parse from
 * inside custom allocators invoked by this call. */
LEPTRIS_API LeptrisDocument leptris_parse_string_ex(const char* xml,
                                                    size_t length,
                                                    const LeptrisParseOptions* options,
                                                    LeptrisStatus* status) {
    if (status) *status = LEPTRIS_OK;
    if (!options) {
        return leptris_parse_string(xml, length, status);
    }
    int saved_strict = g_leptris_strict_mode;
    int saved_depth = g_leptris_max_depth;
    if (options->strict_mode >= 0) g_leptris_strict_mode = options->strict_mode;
    if (options->max_depth > 0) g_leptris_max_depth = options->max_depth;

    LeptrisDocument doc = leptris_parse_string_flags(xml, length,
                                                     options->flags, status);
    g_leptris_strict_mode = saved_strict;
    g_leptris_max_depth = saved_depth;
    if (!doc && options->recover) {
        /* #547: recovery mode returns an empty document. The
         * failure is already recorded in leptris_last_error via
         * direct_parse. */
        doc = leptris_document_create();
        if (status) *status = LEPTRIS_ERROR_PARSE;
    }
    return doc;
}

LEPTRIS_API LeptrisDocument leptris_parse_string_flags(const char* xml,
                                                    size_t length,
                                                    LeptrisParseFlags flags,
                                                    LeptrisStatus* status) {
    extern struct leptris_document* direct_parse_flags(const char* xml,
                                                      size_t len,
                                                      unsigned parse_flags);
    LeptrisDocument doc = direct_parse_flags(xml, length, (unsigned)flags);
    if (status) {
        *status = doc ? LEPTRIS_OK : LEPTRIS_ERROR_PARSE;
    }
    return doc;
}

LEPTRIS_API LeptrisDocument leptris_parse_string_inplace(char* xml, size_t length, LeptrisStatus* status) {
    if (status) *status = LEPTRIS_OK;

    struct leptris_document* doc = leptris_parse_inplace(xml, length);

    if (!doc && status) {
        *status = LEPTRIS_ERROR_PARSE;
    }

    return doc;
}

LEPTRIS_API LeptrisElement leptris_parse_fragment(const char* xml,
                                                size_t length,
                                                LeptrisDocument dest_doc,
                                                LeptrisStatus* status) {
    if (status) *status = LEPTRIS_OK;
    if (!xml || length == 0 || !dest_doc) {
        if (status) *status = LEPTRIS_ERROR_NULL_ARG;
        return NULL;
    }

    /* Wrap the fragment in a synthetic root so the parser sees a
     * well-formed single-rooted document. Use a name with characters
     * that can never appear in real XML names so collisions are
     * impossible. */
    static const char frag_open[] = "<__leptris_frag__>";
    static const char frag_close[] = "</__leptris_frag__>";
    size_t open_len = sizeof(frag_open) - 1;
    size_t close_len = sizeof(frag_close) - 1;

    size_t total = open_len + length + close_len;
    char* wrapped = (char*)malloc(total + 1);
    if (!wrapped) {
        if (status) *status = LEPTRIS_ERROR_MEMORY;
        return NULL;
    }
    memcpy(wrapped, frag_open, open_len);
    memcpy(wrapped + open_len, xml, length);
    memcpy(wrapped + open_len + length, frag_close, close_len);
    wrapped[total] = '\0';

    LeptrisStatus parse_st = LEPTRIS_OK;
    LeptrisDocument tmp_doc = leptris_parse_string(wrapped, total, &parse_st);
    free(wrapped);
    if (!tmp_doc) {
        if (status) *status = parse_st;
        return NULL;
    }

    /* Force lazy promotion so we can read the tree. */
    leptris_document_ensure_promoted(tmp_doc);
    LeptrisElement tmp_root = leptris_document_root(tmp_doc);
    if (!tmp_root) {
        leptris_document_free(tmp_doc);
        if (status) *status = LEPTRIS_ERROR_PARSE;
        return NULL;
    }

    /* Build the synthetic container in dest_doc and move children. */
    LeptrisElement frag = leptris_element_create(dest_doc, "#document-fragment");
    if (!frag) {
        leptris_document_free(tmp_doc);
        if (status) *status = LEPTRIS_ERROR_MEMORY;
        return NULL;
    }

    /* Walk children of tmp_root; copy each into dest_doc via
     * leptris_element_append_copy on a temp parent, then detach.
     * (TODO 148 Phase 1 will add leptris_element_copy which makes
     * this a one-liner; until that lands we use the well-tested
     * append_copy path.) */
    LeptrisElement tmp_holder = leptris_element_create(dest_doc, "__frag_holder__");
    if (!tmp_holder) {
        leptris_document_free(tmp_doc);
        if (status) *status = LEPTRIS_ERROR_MEMORY;
        return NULL;
    }
    LeptrisNodeRef child = leptris_node_first_child(leptris_element_as_node(tmp_root));
    while (child) {
        LeptrisNodeRef next = leptris_node_next_sibling(child);
        int t = leptris_node_get_type(child);
        if (t == 0 /* ELEMENT */) {
            LeptrisElement copy = leptris_element_append_copy(tmp_holder,
                                                             (LeptrisElement)child);
            if (copy) {
                /* Detach from tmp_holder, attach to frag. */
                leptris_node_unlink(leptris_element_as_node(copy));
                leptris_element_append_child(frag, copy);
            }
        } else if (t == 1 /* TEXT */) {
            const char* s = leptris_text_node_get_content(child);
            LeptrisNodeRef n = leptris_text_node_create(dest_doc, s ? s : "");
            if (n) leptris_element_append_child(frag, (LeptrisElement)n);
        } else if (t == 2 /* COMMENT */) {
            const char* s = leptris_comment_node_get_content(child);
            LeptrisNodeRef n = leptris_comment_node_create(dest_doc, s ? s : "");
            if (n) leptris_element_append_child(frag, (LeptrisElement)n);
        } else if (t == 3 /* CDATA */) {
            const char* s = leptris_cdata_node_get_content(child);
            LeptrisNodeRef n = leptris_cdata_node_create(dest_doc, s ? s : "");
            if (n) leptris_element_append_child(frag, (LeptrisElement)n);
        } else if (t == 4 /* PI */) {
            const char* tgt = leptris_pi_node_get_target(child);
            const char* data = leptris_pi_node_get_data(child);
            LeptrisNodeRef n = leptris_pi_node_create(dest_doc,
                                                     tgt ? tgt : "",
                                                     data ? data : "");
            if (n) leptris_element_append_child(frag, (LeptrisElement)n);
        }
        child = next;
    }
    /* tmp_holder is pool-allocated; leptris_document_free reclaims it. */

    leptris_document_free(tmp_doc);
    return frag;
}


/**
 * Parse XML with custom options
 */
struct leptris_document* leptris_parse_with_options(
    const char* xml,
    size_t len,
    const leptris_parse_options* opts
) {
    if (!xml || len == 0) return NULL;

    /* For now, ignore options and use new parser
     * TODO: Implement options support in parser */
    (void)opts; /* Suppress unused parameter warning */
    return leptris_parse(xml, len);
}

/* ============================================================================
 * File I/O Operations
 * ============================================================================ */

/**
 * Load file into memory buffer (Public API)
 */
LEPTRIS_API char* leptris_load_file(const char* filepath, size_t* out_size) {
    if (!filepath) {
        if (out_size) *out_size = 0;
        return NULL;
    }

    FILE* f = fopen(filepath, "rb");
    if (!f) {
        if (out_size) *out_size = 0;
        return NULL;
    }

    /* Get file size */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize < 0) {
        fclose(f);
        if (out_size) *out_size = 0;
        return NULL;
    }

    /* Allocate buffer (+1 for null terminator) */
    char* buffer = LEPTRIS_ALLOC_N(char, fsize + 1);
    if (!buffer) {
        fclose(f);
        if (out_size) *out_size = 0;
        return NULL;
    }

    /* Read file */
    size_t read_size = fread(buffer, 1, fsize, f);
    fclose(f);

    /* Null terminate */
    buffer[read_size] = '\0';

    if (out_size) *out_size = read_size;

    return buffer;
}

/**
 * Parse XML file directly (Public API)
 */
LEPTRIS_API LeptrisDocument leptris_parse_file(const char* filepath, LeptrisStatus* status) {
    if (status) *status = LEPTRIS_OK;

    if (!filepath) {
        if (status) *status = LEPTRIS_ERROR_NULL_ARG;
        return NULL;
    }

    size_t size;
    char* buffer = leptris_load_file(filepath, &size);
    if (!buffer) {
        if (status) *status = LEPTRIS_ERROR_NOT_FOUND;
        return NULL;
    }

    /* Parse the buffer */
    LeptrisDocument doc = leptris_parse_string(buffer, size, status);

    /* Free the buffer (document makes its own copies) */
    LEPTRIS_FREE(buffer);

    return doc;
}

/**
 * Free document and all its contents
 */
LEPTRIS_API void leptris_document_adopt_child(LeptrisDocument parent,
                                           LeptrisDocument child) {
    if (!parent || !child) return;
    /* Single-linked list threaded through each child's `next_adopted`.
     * Note: child_docs / child_docs_tail are CHILD's OWN adopted-
     * children list (its nested xi:include descendants).  Do NOT
     * touch them here -- overwriting destroys the chain that frees
     * the child's grandchildren when we recurse into leptris_document_free.
     *
     * Bug found by CI leak check: prior version NULLed child->child_docs
     * here, which orphaned all nested xi:include docs that had
     * already been adopted into the child. */
    child->next_adopted = NULL;
    if (parent->child_docs_tail) {
        parent->child_docs_tail->next_adopted = child;
    } else {
        parent->child_docs = child;
    }
    parent->child_docs_tail = child;
}

LEPTRIS_API void leptris_document_free(struct leptris_document* doc) {
    {
        extern void leptris_root_doc_memo_invalidate(
            const struct leptris_document* doc);
        leptris_root_doc_memo_invalidate(doc);
    }
    if (!doc) return;

    /* Decrement reference count */
    if (doc->ref_count > 0) {
        doc->ref_count--;
        if (doc->ref_count > 0) return;
    }

    /* Handle new DOM tree cleanup based on allocation method */
    if (doc->new_dom_root) {
        /* In compact mode, elements are pool-allocated and freed with the pool
         * No individual free needed */
        /* The pool will be destroyed below, freeing all elements */
    }

    /* Free document fields */
    if (doc->encoding) {
        LEPTRIS_FREE(doc->encoding);
    }

    if (doc->xml_version) {
        LEPTRIS_FREE(doc->xml_version);
    }

    /* Free DOCTYPE if present */
    if (doc->doctype) {
        leptris_doctype_free((LeptrisDoctypeNode*)doc->doctype);
    }

    /* Free custom XPath function registrations (TODO 148 Phase 5). */
    extern void leptris_xpath_free_custom_fns(struct leptris_document*);
    leptris_xpath_free_custom_fns(doc);

    /* Constructed-node anchors (#691): snapshot copies and
     * analyze-string trees borrowed by evaluation results. */
    for (size_t i = 0; i < doc->n_anchored_docs; i++)
        if (doc->anchored_docs[i])
            leptris_document_free(doc->anchored_docs[i]);
    LEPTRIS_FREE(doc->anchored_docs);

    /* Free element index (TODO 132) */
    if (doc->element_index) {
        leptris_element_index_free(doc->element_index);
        doc->element_index = NULL;
    }

    /* Free document-order rank cache (issue #485) */
    leptris_doc_order_index_free(doc->doc_order_index);
    doc->doc_order_index = NULL;

    /* FlatDoc is no longer used — direct_parse builds the
     * LeptrisElement tree eagerly. doc->flat_doc is always NULL. */

    /* Free DTD if present */
    if (doc->dtd) {
        ttdtd_free((LeptrisDTD*)doc->dtd);
    }

    /* Issue #541: release the serialization mem-cache. */
    free(doc->ser_cache);

    /* Document-level comment/PI nodes are pool-owned (issue #580) —
     * reclaimed with the pool; nothing to free here. */

    /* TODO 117: release adopted child documents from xi:include
     * parse="xml".  Each child was parsed into its own pool; its
     * nodes were MOVED (not copied) into our tree, so the child's
     * pool must outlive us.  Walk `next_adopted` (siblings of THIS doc)
     * and recurse into each via leptris_document_free. */
    {
        struct leptris_document* ch = doc->child_docs;
        while (ch) {
            struct leptris_document* next = ch->next_adopted;
            ch->new_dom_root = NULL;  /* Detach so leptris_document_free
                                       * doesn't try to walk the
                                       * adopted subtree. */
            ch->root = NULL;
            leptris_document_free(ch);
            ch = next;
        }
        doc->child_docs = NULL;
        doc->child_docs_tail = NULL;
    }

    /* CRITICAL: drop index/root-map registrations BEFORE any
     * element storage is freed — programmatic roots live in the
     * mutation blocks and pool, and both cleanups read element
     * headers. (Round 18 added the mutation-block frees further
     * down; a programmatic root set via leptris_document_set_root
     * was read after its block was freed — ASAN use-after-free.) */
    if (doc->ref_count == 0) {
        extern void leptris_compact_cleanup_document(struct leptris_document* doc);
        leptris_compact_cleanup_document(doc);
    }

    if (doc->new_dom_root) {
        extern void leptris_root_doc_unregister(LeptrisElement);
        leptris_root_doc_unregister((LeptrisElement)doc->new_dom_root);
    }

    /* Free owned XML buffer if present
     * For regular parsing, the document owns the buffer (copied during parsing)
     * For in-place parsing, the document also owns the buffer for consistency
     * The buffer is freed here to ensure StringViews remain valid for document lifetime
     * For stack-allocated buffers (files <= 4KB), xml_buffer_needs_free = 0 */
    if (doc->xml_buffer && doc->xml_buffer_needs_free) {
        /* Release through the retained-buffer free list (see
         * memory/arena.c): large inputs would otherwise be munmapped
         * here and re-faulted page-by-page on the next parse. */
        leptris_arena_buffer_release(
            doc->xml_buffer,
            doc->xml_buffer_len + 1 + doc->xml_buffer_slack);
        doc->xml_buffer = NULL;
        /* The newline-offset table indexes the buffer; nothing can
         * resolve lines after it is gone. */
        free(doc->line_breaks);
        doc->line_breaks = NULL;
    }
    /* Free mutation element blocks (round 18). */
    while (doc->mut_elem_blocks) {
        struct leptris_mut_elem_block* next = doc->mut_elem_blocks->next;
        free(doc->mut_elem_blocks);
        doc->mut_elem_blocks = next;
    }
    doc->mut_elem_cursor = NULL;
    doc->mut_elem_end = NULL;

    /* Free mutation name blocks (round 21). */
    while (doc->mut_name_blocks) {
        struct leptris_mut_name_block* next = doc->mut_name_blocks->next;
        free(doc->mut_name_blocks);
        doc->mut_name_blocks = next;
    }
    doc->mut_name_cursor = NULL;
    doc->mut_name_end = NULL;

    /* Free mutation attr blocks (round 22). */
    while (doc->mut_attr_blocks) {
        struct leptris_mut_attr_block* next = doc->mut_attr_blocks->next;
        free(doc->mut_attr_blocks);
        doc->mut_attr_blocks = next;
    }
    doc->mut_attr_cursor = NULL;
    doc->mut_attr_end = NULL;

    if (doc->attr_index) {
        free(doc->attr_index->slots);
        free(doc->attr_index);
        doc->attr_index = NULL;
    }

    /* Cache the pool-allocated flag BEFORE destroying the pool —
     * pool-allocated docs are freed by leptris_pool_destroy, so
     * reading doc->doc_pool_allocated AFTER destroy is UAF. TODO 154. */
    int doc_pool_allocated = doc->doc_pool_allocated;

    /* Destroy memory pool (frees all DOM nodes allocated from it) */
    if (doc->pool) {
        leptris_pool_destroy(doc->pool);
    }

    /* Free document — UNLESS it was pool-allocated (TODO 154).
     * When pool_alloc allocated the doc, pool_destroy above already
     * reclaimed its memory. Freeing again would be a double-free.
     * leptris_document_copy and leptris_parse_fragment use calloc and
     * leave doc_pool_allocated=0, so they still get LEPTRIS_FREE'd. */
    if (!doc_pool_allocated) {
        LEPTRIS_FREE(doc);
    }
}

/**
 * Get root element of document
 */
/* No-op — direct_parse builds the LeptrisElement tree eagerly. The
 * FlatDoc + lazy-promote path (TODO 139) has been removed. Retained
 * as a single mutation-site chokepoint so callers don't need to
 * change; future lazy-construction strategies can plug in here. */
void leptris_document_ensure_promoted(struct leptris_document* doc) {
    (void)doc;
}

LEPTRIS_API LeptrisElement leptris_document_root(struct leptris_document* doc) {
    if (!doc) return NULL;

    /* TODO 139 Phase D: lazy promote. If the document was produced
     * by the flat-parse fast path, the compact-pointer tree isn't
     * built yet. Build it now — the caller is about to traverse. */
    leptris_document_ensure_promoted(doc);

    /* Check new_dom_root first (new parser), then fall back to root (old parser) */
    if (doc->new_dom_root) {
        return (LeptrisElement)doc->new_dom_root;
    }
    return (LeptrisElement)doc->root;
}

LEPTRIS_API LeptrisDTD* leptris_document_get_dtd(LeptrisDocument doc) {
    if (!doc) return NULL;
    if (!doc->dtd) {
        /* No DOCTYPE internal subset: lazily create an empty DTD on
         * the document's pool — the handle for attaching an external
         * subset. Document-owned (owns_pool stays 0): released by
         * leptris_document_free with the pool. */
        doc->dtd = leptris_dtd_create(doc->pool);
    }
    return (LeptrisDTD*)doc->dtd;
}

/**
 * Serialize document to XML string
 */
LEPTRIS_API char* leptris_serialize_document(struct leptris_document* doc) {
    if (!doc) return NULL;

    /* Set up serialization options based on document properties */
    LeptrisSerializeOptions opts = { 0 };

    /* If document had XML declaration, include it */
    if (doc->had_declaration && doc->xml_version) {
        opts.xml_declaration = 1;
        opts.encoding = doc->encoding;
    } else {
        opts.xml_declaration = 0;
        opts.encoding = NULL;
    }

    /* Default to compact mode */
    opts.indent = 0;

    /* Use the new serialization API */
    return leptris_document_serialize(doc, &opts);
}

/* ============================================================================
 * Memory Management Helpers
 * ============================================================================ */

/**
 * Free string returned by libleptris (Public API)
 */
LEPTRIS_API void leptris_free_string(char* str) {
    if (str) {
        LEPTRIS_FREE(str);
    }
}

/* One-pass entity pre-scan (#745): 1 when the buffer carries a
 * named entity that is not one of the five predefined XML entities
 * (numeric character references are fine too). A bare '&' without
 * a ';' is not an entity reference — the parser reports it. */
LEPTRIS_API int leptris_str_has_nonstandard_entity(const char* s,
                                                   size_t len) {
    if (!s || len < 3) return 0;
    const char* end = s + len;
    for (const char* p = s; p < end; p++) {
        if (*p != '&') continue;
        /* Entity names have no fixed length cap in XML; anything
         * beyond the longest realistic name is not an entity
         * reference (or the parser will say so). */
        size_t span = 0;
        const char* q = p + 1;
        while (q < end && *q != ';' && *q != '&' && *q != '<' &&
               span < 64) {
            q++;
            span++;
        }
        if (q >= end || *q != ';' || span == 0) continue;   /* not &name; */
        if (p[1] == '#') continue;                          /* numeric */
        const char* name = p + 1;
        switch (span) {
            case 2:
                if (name[0] == 'l' && name[1] == 't') continue;
                if (name[0] == 'g' && name[1] == 't') continue;
                break;
            case 3:
                if (name[0] == 'a' && name[1] == 'm' &&
                    name[2] == 'p') continue;
                break;
            case 4:
                if (name[0] == 'q' && name[1] == 'u' &&
                    name[2] == 'o' && name[3] == 't') continue;
                if (name[0] == 'a' && name[1] == 'p' &&
                    name[2] == 'o' && name[3] == 's') continue;
                break;
            default:
                break;
        }
        return 1;
    }
    return 0;
}


/* ============================================================================
 * Document String Finalization
 * ============================================================================ */

/* Forward declaration for recursive helper */
static void finalize_element_strings(LeptrisElement elem, LeptrisMemoryPool* pool);

/* Helper: Finalize strings for a single element.
 *
 * `pool` is passed explicitly (rather than read from elem->document)
 * because child elements may not have their document back-pointer set
 * yet during this recursion — TODO 25.
 *
 * We bypass the lazy-conversion accessors (leptris_element_get_name etc.)
 * and route directly through leptris_sv_to_cstr_pooled so the resulting
 * strings are pool-owned.  The accessors would otherwise fall back to
 * calloc when elem->document is NULL, leaking. */
static void finalize_element_strings(LeptrisElement elem, LeptrisMemoryPool* pool) {
    if (!elem) return;

    /* Convert element name StringView to NULL-terminated string.
     *
     * TODO 25: pool is always non-NULL when called from
     * name_view removed (TODO 90) — name is set eagerly by create_with_view.
     * namespace_uri_view + prefix_view removed (TODO 90) — both are now
     * set eagerly by the parser via pool-strdup. */

    /* Convert all attribute StringViews — direct walk (TODO 185):
     * the old `for i < attr_count: get_attribute_by_index(elem, i)`
     * re-walked the list from the head for every index — O(K²) per
     * element, 4,950 walks at K=100 (~5M per parse). */
    for (struct leptris_attribute* attr = leptris_element_get_first_attribute(elem);
         attr; attr = leptris_attr_next(attr)) {
        if ((uintptr_t)attr < 0x1000) continue;  /* Invalid pointer */

        /* Single representation (round 4): entity values expand INTO
         * the view (owned pool copy); everything else already reads
         * correctly from the zero-copy view. */
        if (attr_has_entities(attr) &&
            !leptris_sv_is_empty(&attr->value_view)) {
            if ((uintptr_t)attr->value_view.data >= 0x1000) {
                char* expanded =
                    leptris_decode_entities_view(&attr->value_view, pool);
                if (expanded) {
                    attr->value_view = leptris_sv_from_cstr(expanded);
                    attr_set_entities(attr, 0);
                }
            }
        }
        /* TODO 173: namespace_uri lives in the attr ns_cache side
         * table (int32 offset, round 19). The cache struct itself
         * never moves, so decode once and mutate through it. */
        struct leptris_attr_ns_cache* nsc = attr_get_ns_cache(attr);
        if (nsc && !nsc->namespace_uri &&
            !leptris_sv_is_empty(&nsc->namespace_uri_view)) {
            if ((uintptr_t)nsc->namespace_uri_view.data >= 0x1000) {
                nsc->namespace_uri =
                    leptris_sv_to_cstr_pooled(&nsc->namespace_uri_view, pool);
            }
        }
    }

    /* Recursively finalize strings for all children */
    LeptrisNode* child = (LeptrisNode*)leptris_element_get_first_child(elem);
    while (child) {
        /* Only process element children recursively */
        if (child->type == LEPTRIS_NODE_TYPE_ELEMENT) {
            finalize_element_strings((LeptrisElement)child, pool);
        }

        /* CRITICAL FIX: Use generic next_sibling accessor instead of manual field access!
         * The old code assumed all node types had next_sibling at the same offset,
         * but LeptrisPINode has extra fields (target, data) before next_sibling.
         * This was causing bus errors when accessing next_sibling for PI nodes.
         * The generic leptris_node_get_next_sibling() function handles all node types correctly. */
        child = leptris_node_get_next_sibling(child);
    }
}

/**
 * Finalize all StringViews in document to NULL-terminated strings (Public API)
 *
 * This function eagerly converts all StringViews to NULL-terminated strings,
 * eliminating lazy conversion overhead during queries. This is especially
 * important for Read-Many workloads where the same document is queried multiple times.
 */
LEPTRIS_API int leptris_document_finalize_strings(LeptrisDocument doc) {
    if (!doc) return 0;

    LeptrisElement root = leptris_document_root(doc);
    if (!root) return 0;

    /* Recursively finalize all strings in the document tree.
     * Pass doc->pool explicitly — see TODO 25. */
    finalize_element_strings(root, doc->pool);

    return 1; /* Success */
}

/* ---- Freeze API (TODO 88) ---- */

LEPTRIS_API LeptrisStatus leptris_document_freeze(LeptrisDocument doc) {
    if (!doc) return LEPTRIS_ERROR_NULL_ARG;
    leptris_document_freeze_tree(doc);
    return LEPTRIS_OK;
}

LEPTRIS_API int leptris_document_is_frozen(LeptrisDocument doc) {
    if (!doc) return 0;
    LeptrisElement root = leptris_document_root(doc);
    if (!root) return 0;
    LeptrisNode* root_node = (LeptrisNode*)root;
    return root_node->frozen ? 1 : 0;
}

