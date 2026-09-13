/* libleptris - Pure C XML/XPath library
 * Copyright (c) 2024, Ribose Inc.
 * All rights reserved.
 *
 * Public API header - No Ruby dependencies
 */

#ifndef LIBLEPTRIS_H
#define LIBLEPTRIS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Export macro for Windows DLL support and binding generators.
 *
 * On Unix the build sets default symbol visibility to hidden
 * (CMAKE_C_VISIBILITY_PRESET=hidden in CMakeLists.txt); LEPTRIS_API
 * opts back into default visibility for the public surface.
 * Internal helpers stay hidden and never appear in the .so export
 * table. See TODO 80.
 *
 * When LEPTRIS_FOR_BINDGEN is defined (by bindgen/cffi/ctypes),
 * the macro expands to nothing so the header parses cleanly.
 * See TODO 84. */
#ifdef LEPTRIS_FOR_BINDGEN
#define LEPTRIS_API
#else
#ifndef LEPTRIS_API
#  ifdef _WIN32
#    ifdef LEPTRIS_BUILD_SHARED
#      define LEPTRIS_API __declspec(dllexport)
#    elif defined(LEPTRIS_BUILDING_DLL)
       /* CMake defines LEPTRIS_BUILDING_DLL (not LEPTRIS_BUILD_SHARED)
        * on the leptris_shared target — accept both so the DLL
        * actually exports its symbols (issue #278). */
#      define LEPTRIS_API __declspec(dllexport)
#    elif defined(LEPTRIS_USE_SHARED)
#      define LEPTRIS_API __declspec(dllimport)
#    else
#      define LEPTRIS_API
#    endif
#  else
#    define LEPTRIS_API __attribute__((visibility("default")))
#  endif
#endif
#endif  /* LEPTRIS_FOR_BINDGEN */

/* ============================================================================
 * Public types — single canonical source in leptris/types.h.
 *
 * leptris.h is the "full API" header; leptris/types.h is the lightweight
 * types-only header.  All shared typedefs, enums, and option structs
 * live in types.h.  See TODO 99.
 * ============================================================================ */
#include "leptris/types.h"

/* ABI sanity asserts — catches accidental struct-field exposure
 * that would change opaque-handle sizes.  See TODO 84.
 * Uses _Static_assert (works on GCC/Clang as extension in C99,
 * standard in C11, and in C++ via static_assert). MSVC supports
 * static_assert as a C keyword too — preferred over _Static_assert
 * which only became a keyword in /std:c11 mode. */
#ifndef LEPTRIS_STATIC_ASSERT
#  ifdef __cplusplus
#    define LEPTRIS_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#  elif defined(_MSC_VER)
#    define LEPTRIS_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#  else
#    define LEPTRIS_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#  endif
#endif

LEPTRIS_STATIC_ASSERT(sizeof(LeptrisDocument)  == sizeof(void*), "ABI");
LEPTRIS_STATIC_ASSERT(sizeof(LeptrisElement)   == sizeof(void*), "ABI");
LEPTRIS_STATIC_ASSERT(sizeof(LeptrisAttribute) == sizeof(void*), "ABI");
LEPTRIS_STATIC_ASSERT(sizeof(LeptrisXPathResult) == sizeof(void*), "ABI");

/* ============================================================================
 * Node Operations
 * ============================================================================ */

/**
 * Get node type
 *
 * @param node Node handle
 * @return Node type code (0=Element, 1=Text, 2=Comment, 3=CDATA, 4=PI, 5=DOCTYPE, 6=Attribute)
 */
LEPTRIS_API int leptris_node_get_type(LeptrisNodeRef node);

/**
 * Get the 1-based source line of a node (issue #510)
 *
 * Derived on demand from the node's text anchor (element name,
 * text/comment/CDATA content, PI target) inside the document's
 * input buffer — nodes carry no line field (96-byte budget). Cost
 * is proportional to the bytes before the node; fine for tooling,
 * avoid in hot loops.
 *
 * @param node Any node handle (cast elements with
 *             leptris_element_as_node)
 * @return 1-based line, or 0 when unknown: mutated/created nodes
 *         (pool-owned strings), detached nodes, or NULL
 */
LEPTRIS_API int leptris_node_line(LeptrisNodeRef node);

/**
 * Get first child node (any type)
 *
 * @param node Parent node
 * @return First child node (element, text, comment, CDATA, or PI), or NULL if no children
 *
 * Memory: Node is owned by document. Do not free.
 */
LEPTRIS_API LeptrisNodeRef leptris_node_first_child(LeptrisNodeRef node);

/**
 * Get last child node (any type)
 *
 * @param node Parent node
 * @return Last child node, or NULL if no children
 *
 * Memory: Node is owned by document. Do not free.
 */
LEPTRIS_API LeptrisNodeRef leptris_node_last_child(LeptrisNodeRef node);

/**
 * Get next sibling node (any type)
 *
 * @param node Current node
 * @return Next sibling node, or NULL if node is last child
 *
 * Memory: Node is owned by document. Do not free.
 */
LEPTRIS_API LeptrisNodeRef leptris_node_next_sibling(LeptrisNodeRef node);

/**
 * Get previous sibling node (any type)
 *
 * @param node Current node
 * @return Previous sibling node, or NULL if node is first child
 *
 * Memory: Node is owned by document. Do not free.
 */
LEPTRIS_API LeptrisNodeRef leptris_node_previous_sibling(LeptrisNodeRef node);

/**
 * Get child ELEMENT count (issue #910: the count covers elements
 * only; the mixed-kind child chain — text, comments, CDATA, PIs —
 * is walked via leptris_node_first_child/next_sibling, or sized
 * and copied in one call by leptris_node_children(parent, NULL, 0)
 * followed by the copy call).
 *
 * @param node Parent node
 * @return Number of child elements (0 for non-element nodes)
 */
LEPTRIS_API size_t leptris_node_child_count(LeptrisNodeRef node);

/* On-demand Merkle digest of a subtree's CONTENT (issue #869):
 * a stable, content-defined hash — element name/prefix/resolved
 * namespace URI, attributes sorted by (namespace URI, local name)
 * and deduplicated first-wins, children hashed recursively in
 * document order; text/CDATA/comment/PI hash their content (target
 * included for PIs). No pointers or addresses participate, so the
 * value is stable across processes. Equality implies subtree
 * equivalence under the flag semantics; inequality implies nothing
 * (descend and decide). Zero cost when never called.
 *
 * @param node Subtree root (any node type; NULL hashes to 0)
 * @param flags LEPTRIS_DIGEST_DROP_WS_TEXT skips whitespace-only
 *              text nodes
 * @return 64-bit content digest
 */
LEPTRIS_API uint64_t leptris_node_digest(LeptrisNodeRef node,
                                         LeptrisDigestFlags flags);

/* ============================================================================
 * Schematron (lane 16) — the third validation pillar
 * ========================================================================== */

typedef struct leptris_schematron* LeptrisSchematron;

/**
 * Parse a Schematron schema (ISO/2025 vocabulary: schema/pattern/
 * rule/assert/report with @context/@test; queryBinding xslt).
 *
 * @param schema Schema text
 * @param len Length
 * @param status Optional status out-param
 * @return Schema handle, or NULL on error (detail via
 *         leptris_schematron_error / leptris_last_error)
 * Memory: Caller owns (leptris_schematron_free).
 */
LEPTRIS_API LeptrisSchematron leptris_schematron_parse(
    const char* schema, size_t len, LeptrisStatus* status);

/** Parse a Schematron schema from a file path. */
LEPTRIS_API LeptrisSchematron leptris_schematron_parse_file(
    const char* path, LeptrisStatus* status);

/**
 * Parse a Schematron schema selecting a PHASE (@p phase_id): only
 * the patterns the phase activates are evaluated. NULL or "" for
 * @p phase_id keeps every pattern (the default).
 */
LEPTRIS_API LeptrisSchematron leptris_schematron_parse_phase(
    const char* schema, size_t len, const char* phase_id,
    LeptrisStatus* status);

/** Free a schema handle. */
LEPTRIS_API void leptris_schematron_free(LeptrisSchematron sch);

/**
 * Validate an instance document. Validity = zero failed asserts
 * (successful-report entries do not invalidate).
 *
 * @return 1 valid, 0 invalid (or bad args)
 */
LEPTRIS_API int leptris_schematron_valid(LeptrisSchematron sch,
                                         LeptrisDocument doc);

/**
 * Validate and return the SVRL report document (svrl:
 * schematron-output with failed-assert / successful-report
 * entries carrying @test, @location and svrl:text).
 *
 * @return SVRL document (caller frees), or NULL on error
 */
LEPTRIS_API LeptrisDocument leptris_schematron_validate(
    LeptrisSchematron sch, LeptrisDocument doc);

/** Last schema-level error message, or NULL. */
LEPTRIS_API const char* leptris_schematron_error(
    LeptrisSchematron sch);

/* ============================================================================
 * XML Diff (lane 17) — digest-pruned ordered edit script
 * ========================================================================== */

typedef struct leptris_diff* LeptrisDiff;

/**
 * Compute a tree diff between two documents.
 *
 * Equal subtrees (by the #869 content-defined Merkle digest)
 * prune in O(1); diverging regions align via an LCS on child
 * digests, then same-named elements recurse. Attribute add and
 * remove are UPDATE_ATTR ops with an empty side.
 *
 * @param a First document (the "before")
 * @param b Second document (the "after")
 * @param flags LEPTRIS_DIFF_DEFAULT or LEPTRIS_DIFF_IGNORE_WS_TEXT
 * @param status Optional status out-param
 * @return Opaque diff handle, or NULL on error
 * Memory: Caller owns the handle (leptris_diff_free).
 */
LEPTRIS_API LeptrisDiff leptris_diff(LeptrisDocument a,
                                     LeptrisDocument b,
                                     LeptrisDiffFlags flags,
                                     LeptrisStatus* status);

/** Free a diff handle (all op strings are owned by it). */
LEPTRIS_API void leptris_diff_free(LeptrisDiff diff);

/** Number of ops in the edit script. */
LEPTRIS_API size_t leptris_diff_op_count(LeptrisDiff diff);

/** Op kind at @p index (0-based; out of range -> 0). */
LEPTRIS_API LeptrisDiffOpType leptris_diff_op_type(LeptrisDiff diff,
                                                   size_t index);

/**
 * Op path at @p index: root-relative, e.g. "/r/i[2]". Children
 * named uniquely carry no suffix; INSERT/DELETE paths always
 * carry the 1-based same-name position. Document-owned.
 */
LEPTRIS_API const char* leptris_diff_op_path(LeptrisDiff diff,
                                             size_t index);

/**
 * Op name at @p index: the attribute name for UPDATE_ATTR, the
 * element name for INSERT/DELETE, "" for UPDATE_TEXT.
 * Document-owned.
 */
LEPTRIS_API const char* leptris_diff_op_name(LeptrisDiff diff,
                                             size_t index);

/** Before payload (attribute value / text), "" when none.
 * Document-owned. */
LEPTRIS_API const char* leptris_diff_op_before(LeptrisDiff diff,
                                               size_t index);

/** After payload (attribute value / text), "" when none.
 * Document-owned. */
LEPTRIS_API const char* leptris_diff_op_after(LeptrisDiff diff,
                                              size_t index);

/**
 * Serialize the edit script to a line-per-op text form:
 *   '- <path> @<attr> "<before>" -> "<after>"'  (UPDATE_ATTR)
 *   '~ <path> "<before>" -> "<after>"'          (UPDATE_TEXT)
 *   '+ <path> <<name>>'                          (INSERT)
 *   'x <path> <<name>>'                          (DELETE)
 *
 * @return Malloc'd string (free with leptris_free_string)
 */
LEPTRIS_API char* leptris_diff_serialize(LeptrisDiff diff);


/**
 * Copy child node handles of ANY kind into a caller array (issue #535)
 *
 * One call instead of first_child + N next_sibling round-trips —
 * elements, text, comments, CDATA and PIs alike. Same shape as
 * leptris_xpath_result_get_nodes_ex.
 *
 * @param parent Parent node (any type that can hold children)
 * @param out_nodes Output array of child node handles, or NULL for
 *                  a count-only query
 * @param max_count Capacity of out_nodes
 * @return With out_nodes NULL: the TOTAL child count across every
 *         kind (leptris_node_child_count counts elements only, so
 *         it cannot size a mixed array). With out_nodes non-NULL:
 *         the number of handles copied, min(total, max_count).
 *
 * Memory: Nodes are owned by the document. Do not free separately.
 */
LEPTRIS_API size_t leptris_node_children(LeptrisNodeRef parent,
                                         LeptrisNodeRef* out_nodes,
                                         size_t max_count);

/**
 * Batch child handles WITH each child's kind (issue #617)
 *
 * leptris_node_children's shape plus out_kinds: the kind rides the
 * batch, so a binding's child walk skips the per-node get_type
 * dispatch entirely (the same shape as the XPath batch accessors).
 *
 * @param parent Parent node (any type that can hold children)
 * @param out_nodes Output array of child node handles, or NULL for
 *                  a count-only query
 * @param out_kinds Output array receiving each child's LeptrisNodeKind
 *                  in lockstep with out_nodes; NULL to skip
 * @param max_count Capacity of out_nodes/out_kinds
 * @return Identical contract to leptris_node_children: with
 *         out_nodes NULL the total child count; otherwise the number
 *         copied, min(total, max_count).
 *
 * Memory: Nodes are owned by the document. Do not free separately.
 */
LEPTRIS_API size_t leptris_node_children_ex(LeptrisNodeRef parent,
                                            LeptrisNodeRef* out_nodes,
                                            LeptrisNodeKind* out_kinds,
                                            size_t max_count);
/**
 * Raw attribute view including xmlns declarations, source order
 * (issue #635)
 *
 * The attribute chain and the namespace-declaration accessors store
 * the two separately; this returns the MIXED qname-ordered list the
 * streaming transports deliver - declarations interleaved among the
 * attributes at their byte positions. Names and values are as
 * written (qnames like "xmlns" / "xmlns:prefix" / "a:attr").
 *
 * @param elem Element (any; elements without attributes or
 *             declarations report 0)
 * @param out_qnames Output array of qualified-name strings, or NULL
 *                   for a count-only query
 * @param out_values Output array of value strings (parallel), or
 *                   NULL for a count-only query
 * @param max_count Capacity of the output arrays
 * @return With both outputs NULL: the total entry count. Otherwise
 *         the number copied, min(total, max_count).
 *
 * Memory: Strings are owned by the element. Do not free separately.
 */
LEPTRIS_API size_t leptris_element_attributes_raw(
    LeptrisElement elem, const char** out_qnames, const char** out_values,
    size_t max_count);

/**
 * Cast node to element (if node is an element)
 *
 * @param node Node handle
 * @return Element handle, or NULL if node is not an element
 *
 * Memory: Element is owned by document. Do not free.
 */
LEPTRIS_API LeptrisElement leptris_node_as_element(LeptrisNodeRef node);

/**
 * Cast element to its base node handle (for traversal with the
 * leptris_node_* family of operations).
 *
 * Every element IS-A node (the element struct begins with a LeptrisNode
 * header), so this cast is always safe.  The reverse direction is
 * leptris_node_as_element(), which returns NULL for non-element nodes.
 *
 * @param elem Element handle
 * @return Node handle, or NULL if elem is NULL
  *
 * Memory: Handle is owned by the document. Do not free.
 */
LEPTRIS_API LeptrisNodeRef leptris_element_as_node(LeptrisElement elem);

/**
 * Get text content from text node
 *
 * @param node Node handle (must be LEPTRIS_NODE_TYPE_TEXT or LEPTRIS_NODE_TYPE_CDATA)
 * @return Text content, or NULL if node is not a text/CDATA node
 *
 * Memory: String is owned by node. Do not free or modify.
 */
LEPTRIS_API const char* leptris_text_node_get_content(LeptrisNodeRef node);

/**
 * Create a new Text node owned by the given document (issue #167).
 *
 * @param doc Document that will own the new node
 * @param content Text content (NUL-terminated). May be empty ("").
 * @return New node handle, or NULL on allocation failure
 *
 * Memory: Node is owned by doc; released by leptris_document_free.
 *         content is pool-copied.
 */
LEPTRIS_API LeptrisNodeRef leptris_text_node_create(LeptrisDocument doc,
                                                  const char* content);

/**
 * Create a new Comment node owned by the given document (issue #167).
 *
 * @param doc Owning document
 * @param content Comment body (NUL-terminated)
 * @return New node handle, or NULL on allocation failure
  *
 * Memory: Node is owned by doc; released by leptris_document_free.
 */
LEPTRIS_API LeptrisNodeRef leptris_comment_node_create(LeptrisDocument doc,
                                                     const char* content);

/**
 * Create a new CDATA node owned by the given document (issue #167).
 *
 * @param doc Owning document
 * @param content CDATA section content (NUL-terminated)
 * @return New node handle, or NULL on allocation failure
  *
 * Memory: Node is owned by doc; released by leptris_document_free.
 */
LEPTRIS_API LeptrisNodeRef leptris_cdata_node_create(LeptrisDocument doc,
                                                   const char* content);

/**
 * Create a new Processing Instruction node (issue #167).
 *
 * @param doc Owning document
 * @param target PI target (e.g. "xml-stylesheet")
 * @param data PI data (may be NULL or empty)
 * @return New node handle, or NULL on allocation failure
  *
 * Memory: Node is owned by doc; released by leptris_document_free.
 */
LEPTRIS_API LeptrisNodeRef leptris_pi_node_create(LeptrisDocument doc,
                                                const char* target,
                                                const char* data);

/**
 * Replace a Text node's content (issue #167).
 *
 * @param node Node handle (must be TEXT or CDATA)
 * @param content New content (NUL-terminated, pool-copied)
 * @return LEPTRIS_OK on success, LEPTRIS_ERROR_NULL_ARG or
 *         LEPTRIS_ERROR_INVALID_ARG on bad input
 */
LEPTRIS_API LeptrisStatus leptris_text_node_set_content(LeptrisNodeRef node,
                                                      const char* content);

/**
 * Replace a Comment node's content (issue #167).
 */
LEPTRIS_API LeptrisStatus leptris_comment_node_set_content(LeptrisNodeRef node,
                                                         const char* content);

/**
 * Replace a CDATA node's content (issue #167).
 */
LEPTRIS_API LeptrisStatus leptris_cdata_node_set_content(LeptrisNodeRef node,
                                                       const char* content);

/**
 * Replace a PI node's target (issue #167).
 */
LEPTRIS_API LeptrisStatus leptris_pi_node_set_target(LeptrisNodeRef node,
                                                   const char* target);

/**
 * Replace a PI node's data (issue #167).
 */
LEPTRIS_API LeptrisStatus leptris_pi_node_set_data(LeptrisNodeRef node,
                                                 const char* data);

/**
 * Get the parent of any node type (issue #168). Works on Text, Comment,
 * CDATA, PI, and Element nodes. Returns NULL if node is detached or is
 * the document root.
 *
 * @param node Node handle
 * @return Parent element, or NULL if no parent
  *
 * Memory: Handle is owned by the document. Do not free.
 */
LEPTRIS_API LeptrisElement leptris_node_parent(LeptrisNodeRef node);

/**
 * Detach a node from its parent (issue #168). The node remains owned
 * by its document and can be re-attached via leptris_element_append_child.
 *
 * @param node Node to unlink
 * @return LEPTRIS_OK on success,
 *         LEPTRIS_ERROR_NULL_ARG if node is NULL,
 *         LEPTRIS_ERROR_NOT_FOUND if node has no parent
 */
LEPTRIS_API LeptrisStatus leptris_node_unlink(LeptrisNodeRef node);

/**
 * Get the source line number where the node's opening token appeared
 * (issue #172). Returns 0 if the node was created programmatically
 * (no source info) or the parser did not record line numbers.
 */
LEPTRIS_API int leptris_node_line(LeptrisNodeRef node);

/**
 * Get the binding wrapper pointer cached on this node (#262).
 *
 * Language bindings (Ruby FFI, Python ctypes, etc.) set this on
 * first node wrap so subsequent traversals find the cached wrapper
 * without per-node FFI call overhead. Returns NULL when no binding
 * is attached.
 *
 * Memory: The pointer is owned by the binding. libleptris never
 * dereferences or frees it. Cleared to NULL on node creation.
 */
LEPTRIS_API void* leptris_node_get_binding_wrapper(LeptrisNodeRef node);

/**
 * Set the binding wrapper pointer on this node (#262).
 *
 * The binding must ensure the wrapper object outlives the document
 * (or clear the pointer before the document is freed). libleptris
 * treats this as opaque — it never reads or frees the value.
 */
LEPTRIS_API void leptris_node_set_binding_wrapper(LeptrisNodeRef node, void* wrapper);

/**
 * Document-order comparison (issue #172).
 *
 * @return -1 if a precedes b, 0 if equal, 1 if a follows b.
 *         Returns 0 if either is NULL or they are in different documents.
 */
LEPTRIS_API int leptris_node_compare(LeptrisNodeRef a, LeptrisNodeRef b);

/**
 * Build a canonical unique XPath string identifying `node` within
 * its document (TODO 148 Phase 3).
 *
 * Format matches Nokogiri's `Node#path`:
 *   - Element: `/{qname}[N]` where N is 1-based position among
 *     same-named element siblings. If the qname is unique among
 *     siblings, `[N]` is omitted.
 *   - Text / CDATA: `/text()`
 *   - Comment: `/comment()`
 *   - Processing Instruction: `/processing-instruction()`
 *   - Root: `/` (a single slash)
 *
 * @param node Node to identify (must not be NULL)
 * @return Newly allocated NUL-terminated string. Caller frees via
 *         `leptris_free_string`. Returns NULL on NULL node or
 *         allocation failure.
 *
 * Memory: Caller-owned; release with `leptris_free_string`.
 */
LEPTRIS_API char* leptris_node_get_xpath(LeptrisNodeRef node);

/* Traversal order for leptris_node_traverse. */
typedef enum {
    LEPTRIS_TRAVERSE_PRE_ORDER = 0,   /* Parent before children */
    LEPTRIS_TRAVERSE_POST_ORDER = 1  /* Children before parent */
} LeptrisTraverseOrder;

/**
 * Traverse all descendant nodes of `root` in the specified order,
 * invoking `callback` once per node.
 *
 * Crossing the FFI boundary once per traversal (not once per node)
 * eliminates per-node call overhead — the main bottleneck for
 * language bindings implementing Node#traverse, Node#each, etc.
 *
 * @param root     Starting node (included in traversal)
 * @param order    LEPTRIS_TRAVERSE_PRE_ORDER or POST_ORDER
 * @param callback Called once per node. Return 0 to continue,
 *                 non-zero to stop early.
 * @param user_data Opaque pointer passed to callback (may be NULL)
 *
 * @return Number of nodes visited, or -1 on error.
 *
 * Memory: Node handles passed to callback are document-owned.
 */
LEPTRIS_API int leptris_node_traverse(LeptrisNodeRef root,
                                     LeptrisTraverseOrder order,
                                     int (*callback)(LeptrisNodeRef node,
                                                     void* user_data),
                                     void* user_data);

/**
 * Get comment content
 *
 * @param node Node handle (must be LEPTRIS_NODE_TYPE_COMMENT)
 * @return Comment content, or NULL if node is not a comment
 *
 * Memory: String is owned by node. Do not free or modify.
 */
LEPTRIS_API const char* leptris_comment_node_get_content(LeptrisNodeRef node);

/**
 * Get CDATA content
 *
 * @param node Node handle (must be LEPTRIS_NODE_TYPE_CDATA)
 * @return CDATA content, or NULL if node is not a CDATA node
 *
 * Memory: String is owned by node. Do not free or modify.
 */
LEPTRIS_API const char* leptris_cdata_node_get_content(LeptrisNodeRef node);

/**
 * Get processing instruction target
 *
 * @param node Node handle (must be LEPTRIS_NODE_TYPE_PI)
 * @return PI target, or NULL if node is not a PI
 *
 * Memory: String is owned by node. Do not free or modify.
 */
LEPTRIS_API const char* leptris_pi_node_get_target(LeptrisNodeRef node);

/**
 * Get processing instruction data
 *
 * @param node Node handle (must be LEPTRIS_NODE_TYPE_PI)
 * @return PI data, or NULL if node is not a PI or has no data
 *
 * Memory: String is owned by node. Do not free or modify.
 */
LEPTRIS_API const char* leptris_pi_node_get_data(LeptrisNodeRef node);

/* ============================================================================
 * Document Operations
 * ============================================================================ */

/**
 * Create an empty document with its own memory pool
 *
 * Complements the parse entry points: until now documents could only
 * be obtained by parsing. Programmatic construction (bindings that
 * build trees bottom-up) needs a document handle first so elements
 * can be created against its pool via leptris_element_create().
 *
 * The document has no root element; attach one with
 * leptris_document_set_root(). Serializing a rootless document
 * returns NULL (there is nothing to write).
 *
 * @return Document handle or NULL on allocation failure
 *
 * Memory: Caller must call leptris_document_free() when done
 * Thread safety: Not thread-safe. One document per thread.
 */
LEPTRIS_API LeptrisDocument leptris_document_create(void);

/**
 * Attach an element as the document's root element
 *
 * The element must have been created against @p doc (via
 * leptris_element_create) and must not be attached to a parent.
 * Replaces any existing root; the previous root is not freed — it
 * remains a pool-owned, detached element.
 *
 * @param doc Document handle
 * @param root Element to attach as root
 * @return LEPTRIS_OK on success, LEPTRIS_ERROR_NULL_ARG on NULL
 *         inputs, or LEPTRIS_ERROR_INVALID_ARG when the element
 *         belongs to another document or already has a parent
 */
LEPTRIS_API LeptrisStatus leptris_document_set_root(LeptrisDocument doc,
                                                    LeptrisElement root);

/**
 * Parse XML string into document
 *
 * @param xml XML string (must be valid UTF-8)
 * @param length Length of XML string in bytes
 * @param status Output status code (can be NULL)
 * @return Document handle or NULL on error
 *
 * Memory: Caller must call leptris_document_free() when done
 * Thread safety: Not thread-safe. One document per thread.
 */
LEPTRIS_API LeptrisDocument leptris_parse_string(const char* xml, size_t length, LeptrisStatus* status);

/**
 * Parse an HTML string into a document (issue #659)
 *
 * Tolerant HTML4/5 parse into the standard DOM: implied end tags
 * (p/li/td/tr/th/dt/dd/option/...), void elements, raw-text
 * <script>/<style>, minimized + unquoted attribute values,
 * case-insensitive tag/attribute names, and the HTML named-entity
 * table. Behaviors follow libxml2's HTMLparser as exposed by
 * Nokogiri (fragment shape: no synthesized <html>/<head>/<body>,
 * no implied <tbody>). Malformed input never fails the parse — it
 * degrades to text; a document with no nodes at all is an error.
 * The result serializes and queries (XPath/XSLT) like any
 * libleptris document.
 *
 * @param html HTML input (must be valid UTF-8)
 * @param length Length of input in bytes
 * @param status Output status code (can be NULL)
 * @return Document handle or NULL on error
 *
 * Memory: Caller must call leptris_document_free() when done
 * Thread safety: Not thread-safe. One document per thread.
 */
LEPTRIS_API LeptrisDocument leptris_parse_html_string(const char* html,
                                                      size_t length,
                                                      LeptrisStatus* status);

/* HTML4/libxml2-compatibility mode (#659): identical tolerant
 * tokenizer and Nokogiri document shape, except leading
 * script/style content stays in <body> (libxml2's shape —
 * title/meta/link/base still lift into the implied head). Bindings
 * that must match Nokogiri byte-for-byte pin this entry;
 * leptris_parse_html_string is the WHATWG-conformant engine.
 * Memory contract identical to leptris_parse_html_string. */
LEPTRIS_API LeptrisDocument leptris_parse_html4_string(const char* html,
                                                       size_t length,
                                                       LeptrisStatus* status);

/* ---- RELAX NG (#878) ------------------------------------------------
 * Phase 1: parse a RELAX NG XML-syntax schema into the compiled
 * pattern representation. Validation arrives in phase 2.
 *
 * @param schema RELAX NG XML-syntax schema text
 * @param len    schema byte length
 * @param status LEPTRIS_OK, or LEPTRIS_ERROR_PARSE on a schema
 *               error (wrong root namespace, unknown pattern
 *               element, missing @name, unsupported construct)
 * @return compiled schema, or NULL (status says why)
 *
 * Memory: free with leptris_rng_free.
 */
LEPTRIS_API LeptrisRelaxNG leptris_rng_parse(const char* schema,
                                              size_t len,
                                              LeptrisStatus* status);

/* Parse a RELAX NG schema from a file. <include href="..."> resolves
 * relative to the file's directory; a <start>/<define> nested inside
 * an <include> replaces the included grammar's matching declaration
 * (which must exist). Include cycles are detected and rejected.
 *
 * @param path   schema file path
 * @param status LEPTRIS_OK, or LEPTRIS_ERROR_PARSE (missing file,
 *               not well-formed, schema error, bad include)
 * @return compiled schema, or NULL (status says why)
 *
 * Memory: free with leptris_rng_free.
 */
LEPTRIS_API LeptrisRelaxNG leptris_rng_parse_file(const char* path,
                                                  LeptrisStatus* status);
LEPTRIS_API void leptris_rng_free(LeptrisRelaxNG rng);
/* Validate an instance document against the parsed schema
 * (phase-2 core subset: element/attribute/text/data/value/
 * choice/group/interleave/repeats/ref). Returns 1 valid; on 0,
 * leptris_rng_error carries the first failure in Jing's
 * "line:col: error: message" shape (column always 0 in this
 * phase; the instance's line numbers). */
LEPTRIS_API int leptris_rng_validate(LeptrisRelaxNG rng,
                                     LeptrisDocument doc);
/* Last parse error detail (NULL when the handle is valid). */
LEPTRIS_API const char* leptris_rng_error(LeptrisRelaxNG rng);


LEPTRIS_API LeptrisDocument leptris_parse_string_flags(const char* xml,
                                                    size_t length,
                                                    LeptrisParseFlags flags,
                                                    LeptrisStatus* status);

/**
 * Parse with per-call options (TODO.bindings/05)
 *
 * Scoped alternative to the thread-global leptris_set_strict_mode /
 * leptris_set_max_depth: the options apply to THIS parse only; the
 * thread defaults are restored on return. Invalid input fails with
 * LEPTRIS_ERROR_PARSE (message + position via leptris_last_error /
 * leptris_last_error_position).
 *
 * Not reentrant: do not parse from inside allocators invoked by
 * this call (options are applied via thread-local state).
 *
 * @param xml Input
 * @param length Input length in bytes
 * @param options Options (NULL = defaults, same as parse_string)
 * @param status Optional status out-param
 * @return Document, or NULL on failure
 */
LEPTRIS_API LeptrisDocument leptris_parse_string_ex(const char* xml,
                                                    size_t length,
                                                    const LeptrisParseOptions* options,
                                                    LeptrisStatus* status);

/**
 * Parse XML string into document with zero-copy optimization
 *
 * This function modifies the input buffer in-place by NULL-terminating strings.
 * The document retains a reference to the input buffer, which must remain valid
 * for the lifetime of the document.
 *
 * @param xml Writable XML string (must be valid UTF-8, will be modified)
 * @param length Length of XML string in bytes
 * @param status Output status code (can be NULL)
 * @return Document handle or NULL on error
 *
 * Memory:
 * - Caller must call leptris_document_free() when done
 * - Caller must keep xml buffer alive until document is freed
 * - xml buffer will be modified (NULL terminators inserted)
 *
 * Performance: 3-5x faster than leptris_parse_string() due to zero allocations
 * Thread safety: Not thread-safe. One document per thread.
 */
LEPTRIS_API LeptrisDocument leptris_parse_string_inplace(char* xml, size_t length, LeptrisStatus* status);

/**
 * Parse an XML fragment (multiple top-level children allowed) into
 * a synthetic container element owned by `dest_doc` (TODO 148
 * Phase 4).
 *
 * @param xml Fragment source (NUL-terminated not required; length used)
 * @param length Byte length of `xml`
 * @param dest_doc Document that will own the synthetic container
 *                 and its children. May NOT be NULL.
 * @param status Output status (may be NULL)
 * @return Newly created synthetic element named `#document-fragment`,
 *         or NULL on error. The synthetic has no parent reference;
 *         caller may attach or discard it.
 *
 * The synthetic container's children are the parsed top-level
 * nodes. Fragment parsing differs from full-document parsing in
 * that multiple top-level elements are allowed (e.g. `"<a/><b/>"`).
 *
 * Backs `Document#fragment` and `Node#fragment` in the Ruby
 * binding. Caller moves children via `leptris_element_append_child`
 * (which unlinks from the synthetic, see #217).
 *
 * Memory: Synthetic + children are owned by `dest_doc`; released
 *         by `leptris_document_free`.
 */
LEPTRIS_API LeptrisElement leptris_parse_fragment(const char* xml,
                                                size_t length,
                                                LeptrisDocument dest_doc,
                                                LeptrisStatus* status);

/**
 * Parse XML string with automatic encoding detection and conversion
 *
 * This function automatically detects the encoding from:
 * 1. XML declaration encoding="..." attribute
 * 2. Byte Order Mark (BOM) if present
 * 3. Heuristic detection (UTF-8, UTF-16, ISO-8859-1, etc.)
 *
 * If the encoding is not UTF-8, the input is converted to UTF-8 before parsing.
 * Requires iconv support (enabled by default via vcpkg).
 *
 * @param xml XML string (any encoding)
 * @param length Length of XML string in bytes
 * @param status Output status code (can be NULL)
 * @return Document handle or NULL on error
 *
 * Supported encodings:
 * - UTF-8, UTF-16LE, UTF-16BE, UTF-32LE, UTF-32BE
 * - ISO-8859-1, ISO-8859-2, ISO-8859-15
 * - Windows-1252
 * - Shift_JIS, EUC-JP, GB18030
 *
 * Memory: Caller must call leptris_document_free() when done
 * Thread safety: Not thread-safe. One document per thread.
 */
LEPTRIS_API LeptrisDocument leptris_parse_string_with_encoding(const char* xml, size_t length, LeptrisStatus* status);

/**
 * Load file into memory buffer
 *
 * Reads entire file into a newly allocated buffer. Caller must free the buffer.
 *
 * @param filepath Path to file to load
 * @param out_size Output parameter for file size (can be NULL)
 * @return Newly allocated buffer containing file contents, or NULL on error
 *
 * Memory: Caller must free the returned buffer with LEPTRIS_FREE()
 * Thread safety: Thread-safe (multiple threads can load different files)
 */
LEPTRIS_API char* leptris_load_file(const char* filepath, size_t* out_size);

/**
 * Parse XML file directly
 *
 * Convenience function that loads a file and parses it as XML.
 *
 * @param filepath Path to XML file to parse
 * @param status Output status code (can be NULL)
 * @return Document handle or NULL on error
 *
 * Memory: Caller must call leptris_document_free() when done
 * Thread safety: Not thread-safe. One document per thread.
 */
LEPTRIS_API LeptrisDocument leptris_parse_file(const char* filepath, LeptrisStatus* status);

/**
 * Free document and all its elements
 *
 * @param doc Document to free (can be NULL)
 */
LEPTRIS_API void leptris_document_free(LeptrisDocument doc);

/**
 * Adopt a child document into the parent's lifecycle.
 *
 * TODO 117: used by xi:include parse="xml" to transfer ownership
 * of a freshly-parsed included document so its pool (which now
 * owns nodes spliced into the parent tree) survives until the
 * parent is freed.  Sets `child->child_docs` to NULL so the child
 * doesn't recursively carry its own adopted docs.
 *
 * @param parent Owning document (must outlive child)
 * @param child  Adopted document (its pool is kept alive by `parent`)
 */
LEPTRIS_API void leptris_document_adopt_child(LeptrisDocument parent,
                                           LeptrisDocument child);

/**
 * Get root element of document
 *
 * @param doc Document
 * @return Root element or NULL if document is NULL or empty
 *
 * Memory: Element is owned by document. Do not free separately.
 */
LEPTRIS_API LeptrisElement leptris_document_root(LeptrisDocument doc);

/**
 * Get document encoding string
 *
 * @param doc Document
 * @return Encoding string (e.g. "UTF-8") or NULL if not set
 *
 * Memory: String is owned by document. Do not free.
 */
LEPTRIS_API const char* leptris_document_encoding(LeptrisDocument doc);

/**
 * Get the document's internal DTD subset — the DOCTYPE declaration
 * (TODO 148 Phase 2).
 *
 * @param doc Document handle
 * @return Opaque `LeptrisDoctype` handle, or NULL if the document
 *         has no DOCTYPE.
 *
 * Use the `leptris_doctype_*` accessors below to read the name,
 * public identifier, system identifier, and internal subset.
 *
 * Memory: Handle is owned by the document; released by
 *         `leptris_document_free`.
 */
LEPTRIS_API LeptrisDoctype leptris_document_internal_subset(LeptrisDocument doc);

/* ============================================================================
 * Document-level processing instructions (issue #526)
 * ============================================================================ */

/**
 * The document node — XPath/XSLT root (issue #580)
 *
 * Navigation head for the document's children: prolog comments/PIs,
 * the root element, then epilog comments/PIs, in document order.
 * Standard node APIs (leptris_node_first_child, next_sibling,
 * get_type, leptris_node_children) walk that chain; document-level
 * nodes are visible to leptris_xpath_eval (/processing-instruction(),
 * /comment(), //comment()).
 *
 * The handle is a stable, document-owned singleton.
 *
 * @param doc Document
 * @return The document node, or NULL for NULL
 */
LEPTRIS_API LeptrisNodeRef leptris_document_node(LeptrisDocument doc);

/**
 * Visit callback for leptris_node_visit (issue #645a): node pointer
 * with enter/leave (elements) and depth from the walk's root.
 */
typedef void (*LeptrisNodeVisitor)(void* user_data, LeptrisNodeRef node,
                                   int entering, int depth);

/**
 * Wrap-free subtree visitation (issue #645a)
 *
 * Walks a subtree in document order with ONE C call, handing the
 * host every node pointer through a callback — bindings wrap nodes
 * lazily instead of materializing per-level NodeSet/Array
 * allocations (the per-node allocation floor of the walk).
 *
 * Elements are visited TWICE: entering=1 before their children,
 * entering=0 after the subtree completes. Every other node kind
 * (text, CDATA, comment, PI) is visited once with entering=1.
 * `depth` is 0 at the walk's root and increments per element level.
 *
 * Passing the DOCUMENT node walks the document child chain (the
 * root element and any document-level nodes); the document node
 * itself is the container, not a visited node.
 *
 * The walk is read-only; mutating the tree from the callback
 * (unlinking, moving) is not supported.
 *
 * @param root Subtree root (any node kind, or a document node)
 * @param visitor Callback per node (may be NULL — no-op)
 * @param user_data Opaque context for the callback
 */
LEPTRIS_API void leptris_node_visit(LeptrisNodeRef root,
                                    LeptrisNodeVisitor visitor,
                                    void* user_data);

/**
 * Count the document's top-level processing instructions
 *
 * Parsed `<?target data?>` items outside the root element (and ones
 * added via leptris_document_add_pi). The XML declaration is not a PI.
 *
 * @param doc Document
 * @return PI count, or 0 for NULL
 */
LEPTRIS_API size_t leptris_document_pi_count(LeptrisDocument doc);

/**
 * Get a document-level PI's target by index
 *
 * @param doc Document
 * @param index 0-based, < leptris_document_pi_count
 * @return Target string (document-owned, lives until
 *         leptris_document_free), or NULL when out of range
 */
LEPTRIS_API const char* leptris_document_pi_target(LeptrisDocument doc,
                                                   size_t index);

/**
 * Get a document-level PI's data by index
 *
 * @param doc Document
 * @param index 0-based, < leptris_document_pi_count
 * @return Data string, "" when the PI has none (document-owned), or
 *         NULL when out of range
 */
LEPTRIS_API const char* leptris_document_pi_data(LeptrisDocument doc,
                                                 size_t index);

/* ============================================================================
 * Document-level comments (issue #578)
 * ============================================================================ */

/**
 * Count the document's top-level comments
 *
 * Parsed `<!-- ... -->` items outside the root element, in document
 * order (prolog comments first, then epilog comments).
 *
 * @param doc Document
 * @return Comment count, or 0 for NULL
 */
LEPTRIS_API size_t leptris_document_comment_count(LeptrisDocument doc);

/**
 * Get a document-level comment's content by index
 *
 * @param doc Document
 * @param index 0-based, < leptris_document_comment_count
 * @return Content between the markers (document-owned, lives until
 *         leptris_document_free), or NULL when out of range
 */
LEPTRIS_API const char* leptris_document_comment_content(LeptrisDocument doc,
                                                         size_t index);

/**
 * Append a document-level processing instruction
 *
 * The PI is appended after any existing document PIs (serialized
 * after the XML declaration, before the root element).
 *
 * @param doc Document (must exist)
 * @param target PI target (copied)
 * @param data PI data (copied; may be NULL or empty)
 * @return Opaque non-NULL witness on success (enumerate via the
 *         accessors above; do NOT pass it to node APIs — document
 *         PIs are not tree nodes), or NULL on invalid input / OOM
 */
/**
 * Remove a document-level processing instruction (issue #612)
 *
 * Identifies the PI by target (first match in document order), or
 * by index among the document's PIs when target is NULL. The node
 * is unlinked from the document's child chain and returned — it is
 * pool-owned and remains valid until leptris_document_free (this
 * API does not free it).
 *
 * @param doc Document
 * @param target PI target to match, or NULL to use index
 * @param index 0-based index among the document's PIs (target=NULL)
 * @return the removed node, or NULL when not found
 */
LEPTRIS_API LeptrisNodeRef leptris_document_remove_pi(LeptrisDocument doc,
                                                      const char* target,
                                                      size_t index);

LEPTRIS_API LeptrisNodeRef leptris_document_add_pi(LeptrisDocument doc,
                                                   const char* target,
                                                   const char* data);

/**
 * Get the DOCTYPE's root element name (the name following
 * `<!DOCTYPE`).
 *
 * For `<!DOCTYPE html ...>`, returns `"html"`.
 *
 * @param dt DOCTYPE handle (must not be NULL)
 * @return Root element name, or NULL if `dt` is NULL
 *
 * Memory: String is owned by the document; do not free.
 */
LEPTRIS_API const char* leptris_doctype_get_name(LeptrisDoctype dt);

/**
 * Get the DOCTYPE's root element name (alias matching the libxml2
 * / Nokogiri `DocType#name` convention).
 *
 * @param dt DOCTYPE handle
 * @return Same value as `leptris_doctype_get_name`
  *
 * Memory: String is owned by the document. Do not free.
 */
LEPTRIS_API const char* leptris_doctype_get_root_name(LeptrisDoctype dt);

/**
 * Get the DOCTYPE's PUBLIC identifier.
 *
 * For `<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.0//EN" "...">`
 * returns `"-//W3C//DTD XHTML 1.0//EN"`. Returns NULL for SYSTEM
 * declarations and bare `<!DOCTYPE html>` declarations.
 *
 * @param dt DOCTYPE handle
 * @return Public identifier, or NULL if not declared
  *
 * Memory: String is owned by the document. Do not free.
 */
LEPTRIS_API const char* leptris_doctype_get_public_id(LeptrisDoctype dt);

/**
 * Get the DOCTYPE's SYSTEM identifier.
 *
 * For `<!DOCTYPE html SYSTEM "html.dtd">` returns `"html.dtd"`.
 * Returns NULL if not declared.
 *
 * @param dt DOCTYPE handle
 * @return System identifier, or NULL if not declared
  *
 * Memory: String is owned by the document. Do not free.
 */
LEPTRIS_API const char* leptris_doctype_get_system_id(LeptrisDoctype dt);

/**
 * Get the DOCTYPE's internal DTD subset — the contents of the
 * `[...]` block following the name and external identifiers.
 *
 * For `<!DOCTYPE root [<!ENTITY foo "bar">]>` returns
 * `<!ENTITY foo "bar">`. Returns NULL if no internal subset.
 *
 * @param dt DOCTYPE handle
 * @return Internal subset source, or NULL if empty
  *
 * Memory: String is owned by the document. Do not free.
 */
LEPTRIS_API const char* leptris_doctype_get_internal_subset(LeptrisDoctype dt);

/**
 * Eagerly convert all StringViews to NULL-terminated strings
 *
 * PERFORMANCE OPTIMIZATION: Call this after parsing to optimize for
 * query-heavy workloads. Eliminates lazy conversion overhead during
 * attribute/element access.
 *
 * For parse-once-serialize workflows, this can be skipped to avoid
 * unnecessary string conversions.
 *
 * @param doc Document to finalize strings for
 * @return 0 on success, -1 on failure
 *
 * Thread safety: Not thread-safe if document is shared between threads.
 * Memory: Strings are allocated from document's memory pool.
 */
LEPTRIS_API int leptris_document_finalize_strings(LeptrisDocument doc);

/**
 * Set strict parsing mode
 *
 * Controls whether the parser should be strict or lenient when parsing XML.
 * In strict mode, the parser will reject malformed XML and return errors.
 * In lenient mode, the parser may attempt to recover from certain errors.
 *
 * @param strict 1 for strict mode, 0 for lenient mode
 *
 * Thread safety: __thread (TODO 27 phase 1) — each thread has its
 * own default.  Documents created after this call inherit the value
 * at creation time.  Per-document override: leptris_document_set_strict.
 */
LEPTRIS_API void leptris_set_strict_mode(int strict);

/**
 * Set strict mode on a specific document (TODO 38).
 *
 * Overrides the thread-default for this document only.  Useful when
 * an application wants to mix strict and lenient parsing in the same
 * thread.
 *
 * @param doc Document to modify.
 * @param strict 1 for strict mode, 0 for lenient mode.
 * @return LEPTRIS_OK on success, LEPTRIS_ERROR_NULL_ARG if doc is NULL.
 *
 * Memory: No allocation.
 */
LEPTRIS_API LeptrisStatus leptris_document_set_strict(LeptrisDocument doc, int strict);

/**
 * Get strict mode for a specific document.
 *
 * Returns the per-document setting (set via leptris_document_set_strict)
 * or the thread-default if never explicitly set.
 *
 * @param doc Document to query.
 * @return 1 if strict, 0 if lenient, 0 if doc is NULL.
 */
LEPTRIS_API int leptris_document_get_strict(LeptrisDocument doc);

/**
 * Get the thread-default strict mode.
 *
 * Documents inherit this value at creation time unless
 * leptris_document_set_strict overrides it.
 *
 * @return 1 if strict default, 0 if lenient.
 */
LEPTRIS_API int leptris_get_strict_mode(void);

/* ============================================================================
 * Document Freeze API (TODO 88)
 *
 * A frozen document is marked immutable. Mutation functions SHOULD
 * check the frozen flag and refuse to modify a frozen document.
 * (Currently the flag is advisory — mutation functions do not yet
 * enforce it. COW deep-copy on mutation is a future enhancement.)
 * ============================================================================ */

/**
 * Freeze a document, marking all its nodes as immutable.
 *
 * The freeze flag is **advisory only** — it does not cause mutation
 * functions (set_attribute, append_child, etc.) to return an error.
 * Callers who want to honor the freeze should check
 * `leptris_document_is_frozen` before mutating.
 *
 * Rationale: the alternative ("freeze = read-only, mutations reject")
 * breaks the common pattern of parse-then-modify, since every
 * freshly-parsed document is auto-frozen by the parser.  Users who
 * want true immutability should keep the document pointer private
 * and check `is_frozen` at their own API boundaries.
 *
 * @param doc Document to freeze. Must not be NULL.
 * @return LEPTRIS_OK on success, LEPTRIS_ERROR_NULL_ARG if doc is NULL.
 */
LEPTRIS_API LeptrisStatus leptris_document_freeze(LeptrisDocument doc);

/**
 * Check if a document has been frozen (advisory — see
 * leptris_document_freeze for the full contract).
 *
 * @param doc Document to query. NULL returns 0.
 * @return 1 if frozen, 0 if mutable.
 */
LEPTRIS_API int leptris_document_is_frozen(LeptrisDocument doc);

/**
 * Set the thread-default maximum element-nesting depth.
 *
 * Documents deeper than this are rejected with LEPTRIS_ERROR_PARSE to
 * prevent stack-overflow crashes.  Default: 256 (matches libxml2).
 *
 * Set to 0 to restore the default.
 *
 * @param max_depth Maximum depth, or 0 for default.
 *
 * Thread safety: __thread — each thread has its own default.
 */
LEPTRIS_API void leptris_set_max_depth(int max_depth);

/**
 * Get the thread-default maximum element-nesting depth.
 *
 * @return The effective depth (always > 0; returns the default if unset).
 */
LEPTRIS_API int leptris_get_max_depth(void);

/* ============================================================================
 * Element Operations
 * ============================================================================ */

/**
 * Get element name
 *
 * @param elem Element
 * @return Element name or NULL if elem is NULL
 *
 * Memory: String is owned by element. Do not free or modify.
 */
LEPTRIS_API const char* leptris_element_name(LeptrisElement elem);

/**
 * Read an element's expanded name in one call — the local name,
 * its prefix, and the resolved namespace URI. Equivalent to
 * leptris_element_name() + leptris_element_prefix() +
 * leptris_element_namespace()/leptris_namespace_uri(), but one
 * call for FFI adapters that fan out per-element reads.
 *
 * Each out-param may be NULL (that value is skipped). Contracts
 * match the individual accessors exactly: local_name is never
 * NULL ("" for nameless); prefix and namespace_uri are NULL when
 * absent.
 *
 * Memory: all returned strings are document-owned and live until
 * leptris_document_free().
 */
LEPTRIS_API void leptris_element_expanded_name(LeptrisElement elem,
                                               const char** local_name,
                                               const char** prefix,
                                               const char** namespace_uri);

/**
 * Get element text content (concatenation of all text nodes)
 *
 * @param elem Element
 * @return Text content, or "" if elem is NULL or has no text
 *
 * Memory: String is owned by the document. Do not free or modify. It stays
 * valid until leptris_document_free(). When the element's only child is a text
 * or CDATA node the node's own storage is returned; mixed content is
 * concatenated into the document pool.
 */
LEPTRIS_API const char* leptris_element_text(LeptrisElement elem);

/**
 * Get element text content as integer
 *
 * @param elem Element
 * @param default_value Value to return if element is NULL, has no text, or text is not a valid integer
 * @return Text content as integer, or default_value
 *
 * Converts element text content to int using strtol(). Supports decimal and hexadecimal (0x) formats.
 * Returns default_value if element is NULL, has no text, or text cannot be parsed.
 */
LEPTRIS_API int leptris_element_text_int(LeptrisElement elem, int default_value);

/**
 * Get element text content as unsigned integer
 *
 * @param elem Element
 * @param default_value Value to return if element is NULL, has no text, or text is not a valid integer
 * @return Text content as unsigned integer, or default_value
 *
 * Converts element text content to unsigned int using strtoul(). Supports decimal and hexadecimal (0x) formats.
 * Returns default_value if element is NULL, has no text, or text cannot be parsed.
 */
LEPTRIS_API unsigned int leptris_element_text_uint(LeptrisElement elem, unsigned int default_value);

/**
 * Get element text content as double
 *
 * @param elem Element
 * @param default_value Value to return if element is NULL, has no text, or text is not a valid number
 * @return Text content as double, or default_value
 *
 * Converts element text content to double using strtod().
 * Returns default_value if element is NULL, has no text, or text cannot be parsed.
 */
LEPTRIS_API double leptris_element_text_double(LeptrisElement elem, double default_value);

/**
 * Get element text content as float
 *
 * @param elem Element
 * @param default_value Value to return if element is NULL, has no text, or text is not a valid number
 * @return Text content as float, or default_value
 *
 * Converts element text content to float using strtof().
 * Returns default_value if element is NULL, has no text, or text cannot be parsed.
 */
LEPTRIS_API float leptris_element_text_float(LeptrisElement elem, float default_value);

/**
 * Get element text content as boolean
 *
 * @param elem Element
 * @param default_value Value to return if element is NULL, has no text, or text is not a valid boolean
 * @return Text content as boolean (1 for true, 0 for false), or default_value
 *
 * Parses text content as boolean. Accepts: "true", "1" (case-insensitive) for true;
 * "false", "0" (case-insensitive) for false.
 * Returns default_value if element is NULL, has no text, or text cannot be parsed.
 */
LEPTRIS_API int leptris_element_text_bool(LeptrisElement elem, int default_value);

/**
 * Get attribute value by name
 *
 * @param elem Element
 * @param name Attribute name
 * @return Attribute value or NULL if not found
 *
 * Memory: String is owned by element. Do not free or modify.
 */
LEPTRIS_API const char* leptris_element_attribute(LeptrisElement elem, const char* name);

/**
 * Test whether an attribute is present on this element.
 *
 * @param elem Element
 * @param name Attribute name
 * @return 1 if the attribute exists, 0 otherwise
 *
 * Memory: None. Convenient alternative to
 * `leptris_element_attribute(elem, name) != NULL` for callers that
 * only need the boolean answer (issue #166-class visibility gap
 * reported in the v0.5.13 audit).
 */
LEPTRIS_API int leptris_element_has_attribute(LeptrisElement elem, const char* name);

/**
 * Get attribute value by EXPANDED name (issue #542)
 *
 * XML Namespaces 1.0 semantics: uri NULL or "" matches only the
 * no-namespace attribute; otherwise the value is returned when the
 * attribute's prefix resolves to `uri` through the owning element's
 * in-scope declarations (the written prefix itself never matters).
 * xmlns declarations are not attributes and never match.
 *
 * @param elem Element
 * @param uri Namespace URI (NULL/"" for no namespace)
 * @param local Local name
 * @return Attribute value (document-owned), or NULL
 */
LEPTRIS_API const char* leptris_element_attribute_ns(LeptrisElement elem,
                                                     const char* uri,
                                                     const char* local);

/**
 * Test attribute presence by EXPANDED name (issue #542)
 *
 * Same semantics as leptris_element_attribute_ns.
 */
LEPTRIS_API int leptris_element_has_attribute_ns(LeptrisElement elem,
                                                 const char* uri,
                                                 const char* local);

/**
 * Get an attribute's namespace prefix (issue #542)
 *
 * The prefix as written in the qualified name (the bytes before the
 * colon), or NULL for a no-namespace attribute. Name-derived and
 * immutable; does not consult declarations.
 *
 * Memory: String is document-owned. Do not free.
 */
LEPTRIS_API const char* leptris_attribute_prefix(LeptrisAttribute attr);

/**
 * Get an attribute's namespace URI (issue #542)
 *
 * Resolved through the OWNING element's in-scope declarations at
 * read time (the xml prefix is prebound to
 * http://www.w3.org/XML/1998/namespace). NULL for a no-namespace
 * attribute or an undeclared prefix.
 *
 * Memory: String is document-owned. Do not free.
 */
LEPTRIS_API const char* leptris_attribute_namespace_uri(LeptrisAttribute attr);

/**
 * Get attribute value as integer
 *
 * @param elem Element
 * @param name Attribute name
 * @param default_value Value to return if attribute not found
 * @return Attribute value as integer, or default_value if not found
 *
 * Converts attribute value to int using atoi(). Returns default_value
 * if attribute doesn't exist or value is empty.
 */
LEPTRIS_API int leptris_element_attribute_int(LeptrisElement elem, const char* name, int default_value);

/**
 * Get attribute value as double
 *
 * @param elem Element
 * @param name Attribute name
 * @param default_value Value to return if attribute not found
 * @return Attribute value as double, or default_value if not found
 *
 * Converts attribute value to double using atof(). Returns default_value
 * if attribute doesn't exist or value is empty.
 */
LEPTRIS_API double leptris_element_attribute_double(LeptrisElement elem, const char* name, double default_value);

/**
 * Get attribute value as boolean
 *
 * @param elem Element
 * @param name Attribute name
 * @param default_value Value to return if attribute not found
 * @return Attribute value as boolean, or default_value if not found
 *
 * Returns true if attribute exists and value is "true" or "1" (case-insensitive).
 * Returns false if attribute exists and value is "false" or "0" (case-insensitive).
 * Returns default_value if attribute doesn't exist.
 */
LEPTRIS_API int leptris_element_attribute_bool(LeptrisElement elem, const char* name, int default_value);

/**
 * Get attribute value as unsigned integer
 *
 * @param elem Element
 * @param name Attribute name
 * @param default_value Value to return if attribute not found
 * @return Attribute value as unsigned int, or default_value if not found
 *
 * Converts attribute value to unsigned int using strtoul(). Returns default_value
 * if attribute doesn't exist or value is empty/invalid.
 */
LEPTRIS_API unsigned int leptris_element_attribute_uint(LeptrisElement elem, const char* name, unsigned int default_value);

/**
 * Get attribute value as float
 *
 * @param elem Element
 * @param name Attribute name
 * @param default_value Value to return if attribute not found
 * @return Attribute value as float, or default_value if not found
 *
 * Converts attribute value to float using strtof(). Returns default_value
 * if attribute doesn't exist or value is empty/invalid.
 */
LEPTRIS_API float leptris_element_attribute_float(LeptrisElement elem, const char* name, float default_value);

/**
 * Get attribute value as string with default
 *
 * @param elem Element
 * @param name Attribute name
 * @param default_value Value to return if attribute not found
 * @return Attribute value, or default_value if not found
 *
 * Memory: String is owned by element. Do not free or modify.
 * The default_value string must remain valid for the duration of use.
 */
LEPTRIS_API const char* leptris_element_attribute_string(LeptrisElement elem, const char* name, const char* default_value);

/**
 * Get number of attributes on an element
 *
 * @param elem Element
 * @return Attribute count or 0 if elem is NULL
 */
LEPTRIS_API size_t leptris_element_attribute_count(LeptrisElement elem);

/**
 * Get name of attribute by index
 *
 * @param elem Element
 * @param index Attribute index (0-based)
 * @return Attribute name or NULL if index out of range
 *
 * Memory: String is owned by element. Do not free.
 */
LEPTRIS_API const char* leptris_element_attribute_name_at(LeptrisElement elem, size_t index);

/**
 * Get value of attribute by index
 *
 * @param elem Element
 * @param index Attribute index (0-based)
 * @return Attribute value or NULL if index out of range
 *
 * Memory: String is owned by element. Do not free.
 */
LEPTRIS_API const char* leptris_element_attribute_value_at(LeptrisElement elem, size_t index);

/**
 * Attribute iteration — first attribute of an element
 *
 * Handle-based iteration for bindings and C callers that want to
 * walk attributes as objects rather than by index (each _at call
 * re-walks the list from the head, making index iteration O(n^2);
 * handle iteration is O(n) total).
 *
 * for (LeptrisAttribute a = leptris_element_first_attribute(e); a;
 *      a = leptris_attribute_next(a)) { ... }
 *
 * @param elem Element
 * @return First attribute handle, or NULL if the element has none
 *
 * Memory: Handles are owned by the document; do not free. Valid
 * until the attribute is removed or the document is freed.
 */
LEPTRIS_API LeptrisAttribute leptris_element_first_attribute(LeptrisElement elem);

/**
 * Attribute iteration — next attribute
 *
 * @param attr Current attribute handle
 * @return Next attribute handle, or NULL at the end of the list
 *
 * Memory: Same ownership as the incoming handle.
 */
LEPTRIS_API LeptrisAttribute leptris_attribute_next(LeptrisAttribute attr);

/**
 * Get an attribute's name from its handle
 *
 * @param attr Attribute handle
 * @return Attribute name (empty string for a NULL handle)
 *
 * Memory: String is owned by the document. Do not free.
 */
LEPTRIS_API const char* leptris_attribute_get_name(LeptrisAttribute attr);

/**
 * Get an attribute's value from its handle
 *
 * Expands entity references on first access (same contract as
 * leptris_element_attribute). The element argument supplies the
 * document pool for expansion — attr must belong to elem.
 *
 * @param elem Owning element
 * @param attr Attribute handle (must belong to elem)
 * @return Attribute value with entities expanded (empty string for
 *         a NULL handle)
 *
 * Memory: String is owned by the document. Do not free.
 */
LEPTRIS_API const char* leptris_attribute_get_value(LeptrisElement elem, LeptrisAttribute attr);

/**
 * Get number of child elements
 *
 * @param elem Element
 * @return Number of children or 0 if elem is NULL
 */
LEPTRIS_API size_t leptris_element_child_count(LeptrisElement elem);

/**
 * Get child element by index
 *
 * @param elem Element
 * @param index Child index (0-based)
 * @return Child element or NULL if index out of bounds
 *
 * Memory: Element is owned by document. Do not free separately.
 *
 * Performance: **O(index)**.  Walks the linked-list of children from
 * `first_child` to reach the requested index.  The legacy index-cache
 * (`children_array`) was removed in TODO 90 Phase 1 to shrink the
 * element struct; sequential iteration via
 * `leptris_element_first_child_any` + `next_sibling_any` is O(1) per
 * step and is the recommended pattern when iterating all children.
 */
LEPTRIS_API LeptrisElement leptris_element_child(LeptrisElement elem, size_t index);

/**
 * Get parent element
 *
 * @param elem Element
 * @return Parent element or NULL if elem is root or NULL
 *
 * Memory: Element is owned by document. Do not free separately.
 */
LEPTRIS_API LeptrisElement leptris_element_parent(LeptrisElement elem);

/**
 * Get root element from any element in the document
 *
 * @param elem Any element in the document
 * @return Root element of the document, or NULL if elem is NULL or not in a document
 *
 * Memory: Element is owned by document. Do not free separately.
 */
LEPTRIS_API LeptrisElement leptris_element_root(LeptrisElement elem);

/**
 * Get hash value of element for comparison
 *
 * @param elem Element
 * @return Hash value based on element name and attributes
 *
 * Computes a hash value for the element based on its name and attributes.
 * This can be used for quick comparison between elements.
 * Returns 0 if elem is NULL.
 */
LEPTRIS_API size_t leptris_element_hash_value(LeptrisElement elem);

/* ============================================================================
 * Element Modification Operations
 * ============================================================================ */

/**
 * Create new element in document
 *
 * @param doc Document that will own the element
 * @param name Element name (without namespace prefix)
 * @return New element or NULL on error
 *
 * Memory: Element owned by document, freed when document freed
 */
LEPTRIS_API LeptrisElement leptris_element_create(LeptrisDocument doc, const char* name);

/**
 * Set element name (rename element tag)
 *
 * @param elem Element to rename
 * @param new_name New element name (will be copied)
 * @return LEPTRIS_OK on success, error code otherwise
 *
 * Example:
 *   // Change &lt;old&gt; to &lt;new&gt;
 *   leptris_element_set_name(elem, "new");
 *
 * Memory: Name is copied (pooled for pool documents)
 */
LEPTRIS_API LeptrisStatus leptris_element_set_name(LeptrisElement elem, const char* new_name);

/**
 * Append child element
 *
 * @param parent Parent element
 * @param child Child element to append
 * @return LEPTRIS_OK on success, error code otherwise
 */
LEPTRIS_API LeptrisStatus leptris_element_append_child(LeptrisElement parent, LeptrisElement child);

/**
 * Create a new element and append it to parent in one call.
 *
 * The fused twin of create + append_child: one document resolution,
 * one public entry — the builder-shape answer to single-call DOM
 * construction APIs. Identical tree semantics to the two-call pair
 * (same name handling incl. QName splits, same append rules).
 *
 * Memory: the element is owned by parent's document; freed via
 * leptris_document_free. Returns NULL on invalid args or allocation
 * failure (nothing is attached in that case).
 */
LEPTRIS_API LeptrisElement leptris_element_create_child(LeptrisElement parent, const char* name);

/**
 * Prepend child element at the beginning
 *
 * @param parent Parent element
 * @param child Child element to prepend
 * @return LEPTRIS_OK on success, error code otherwise
 */
LEPTRIS_API LeptrisStatus leptris_element_prepend_child(LeptrisElement parent, LeptrisElement child);

/**
 * Insert new node before a sibling
 *
 * @param sibling Sibling element to insert before
 * @param new_node New element to insert
 * @return LEPTRIS_OK on success, error code otherwise
 */
LEPTRIS_API LeptrisStatus leptris_element_insert_before(LeptrisElement sibling, LeptrisElement new_node);

/**
 * Insert new node after a sibling
 *
 * @param sibling Sibling element to insert after
 * @param new_node New element to insert
 * @return LEPTRIS_OK on success, error code otherwise
 */
LEPTRIS_API LeptrisStatus leptris_element_insert_after(LeptrisElement sibling, LeptrisElement new_node);

/**
 * Remove child element
 *
 * @param parent Parent element
 * @param child Child element to remove
 * @return LEPTRIS_OK on success, error code otherwise
 */
LEPTRIS_API LeptrisStatus leptris_element_remove_child(LeptrisElement parent, LeptrisElement child);

/**
 * Remove all children from element
 *
 * @param elem Element to remove children from
 * @return LEPTRIS_OK on success, error code otherwise
 *
 * Removes all child nodes from the element, making it empty.
 * The removed nodes are freed and should not be accessed afterwards.
 */
LEPTRIS_API LeptrisStatus leptris_element_remove_children(LeptrisElement elem);

/**
 * Set element text content
 *
 * @param elem Element
 * @param text New text content (will be copied)
 * @return LEPTRIS_OK on success, error code otherwise
 */
LEPTRIS_API LeptrisStatus leptris_element_set_text(LeptrisElement elem, const char* text);

/**
 * Set attribute value (creates if doesn't exist, updates if exists)
 *
 * @param elem Element
 * @param name Attribute name
 * @param value Attribute value (will be copied)
 * @return LEPTRIS_OK on success, error code otherwise
 */
LEPTRIS_API LeptrisStatus leptris_element_set_attribute(LeptrisElement elem,
                                                       const char* name,
                                                       const char* value);

/**
 * Set attribute value as double (converts to string)
 *
 * @param elem Element
 * @param name Attribute name
 * @param value Numeric value to set
 * @return LEPTRIS_OK on success, error code otherwise
 *
 * Converts the double value to a string and sets it as an attribute.
 */
LEPTRIS_API LeptrisStatus leptris_element_set_attribute_double(LeptrisElement elem,
                                                             const char* name,
                                                             double value);

/**
 * Set attribute value as float (converts to string)
 *
 * @param elem Element
 * @param name Attribute name
 * @param value Numeric value to set
 * @return LEPTRIS_OK on success, error code otherwise
 *
 * Converts the float value to a string and sets it as an attribute.
 */
LEPTRIS_API LeptrisStatus leptris_element_set_attribute_float(LeptrisElement elem,
                                                            const char* name,
                                                            float value);

/**
 * Set attribute value as boolean (converts to string)
 *
 * @param elem Element
 * @param name Attribute name
 * @param value Boolean value to set
 * @return LEPTRIS_OK on success, error code otherwise
 *
 * Converts the boolean value to "true" or "false" and sets it as an attribute.
 */
LEPTRIS_API LeptrisStatus leptris_element_set_attribute_bool(LeptrisElement elem,
                                                           const char* name,
                                                           int value);

/**
 * Set attribute value as integer (converts to string)
 *
 * @param elem Element
 * @param name Attribute name
 * @param value Integer value to set
 * @return LEPTRIS_OK on success, error code otherwise
 *
 * Converts the integer value to a string and sets it as an attribute.
 */
LEPTRIS_API LeptrisStatus leptris_element_set_attribute_int(LeptrisElement elem,
                                                         const char* name,
                                                         int value);

/**
 * Set attribute value as unsigned integer (converts to string)
 *
 * @param elem Element
 * @param name Attribute name
 * @param value Unsigned integer value to set
 * @return LEPTRIS_OK on success, error code otherwise
 *
 * Converts the unsigned integer value to a string and sets it as an attribute.
 */
LEPTRIS_API LeptrisStatus leptris_element_set_attribute_uint(LeptrisElement elem,
                                                          const char* name,
                                                          unsigned int value);

/**
 * Remove attribute
 *
 * @param elem Element
 * @param name Attribute name
 * @return LEPTRIS_OK on success, LEPTRIS_ERROR_NOT_FOUND if attribute doesn't exist
 */
LEPTRIS_API LeptrisStatus leptris_element_remove_attribute(LeptrisElement elem, const char* name);

/**
 * Remove all attributes from element
 *
 * @param elem Element
 * @return LEPTRIS_OK on success
 */
LEPTRIS_API LeptrisStatus leptris_element_remove_all_attributes(LeptrisElement elem);

/**
 * Find first child element with given tag name
 *
 * @param elem Element to search in
 * @param name Tag name to find
 * @return First matching child element or NULL if not found
 *
 * Memory: Element is owned by document. Do not free separately.
 */
LEPTRIS_API LeptrisElement leptris_element_find_child(LeptrisElement elem, const char* name);

/**
 * Find first child with given name and attribute value
 *
 * @param elem Element to search in
 * @param child_name Child tag name (NULL to match any tag)
 * @param attr_name Attribute name to check
 * @param attr_value Attribute value to match
 * @return First matching child element or NULL if not found
 *
 * Memory: Element is owned by document. Do not free separately.
 */
LEPTRIS_API LeptrisElement leptris_element_find_child_by_attr(LeptrisElement elem,
                                                             const char* child_name,
                                                             const char* attr_name,
                                                             const char* attr_value);

/**
 * Get next sibling element with specified name
 *
 * @param elem Element to start from
 * @param name Element name to find (NULL to get next sibling regardless of name)
 * @return Next sibling with matching name, or NULL if not found
 *
 * Memory: Element is owned by document. Do not free separately.
 */
LEPTRIS_API LeptrisElement leptris_element_next_sibling(LeptrisElement elem, const char* name);

/**
 * Get previous sibling element with specified name
 *
 * @param elem Element to start from
 * @param name Element name to find (NULL to get previous sibling regardless of name)
 * @return Previous sibling with matching name, or NULL if not found
 *
 * Memory: Element is owned by document. Do not free separately.
 */
LEPTRIS_API LeptrisElement leptris_element_previous_sibling(LeptrisElement elem, const char* name);

/**
 * Get first child element with specified name
 *
 * @param elem Element to search in
 * @param name Element name to find (NULL to get first child regardless of name)
 * @return First child with matching name, or NULL if not found
 *
 * Memory: Element is owned by document. Do not free separately.
 */
LEPTRIS_API LeptrisElement leptris_element_first_child(LeptrisElement elem, const char* name);

/**
 * Get last child element with specified name
 *
 * @param elem Element to search in
 * @param name Element name to find (NULL to get last child regardless of name)
 * @return Last child with matching name, or NULL if not found
 *
 * Memory: Element is owned by document. Do not free separately.
 */
LEPTRIS_API LeptrisElement leptris_element_last_child(LeptrisElement elem, const char* name);

/**
 * Get first child element regardless of name
 *
 * @param elem Element to search in
 * @return First child element or NULL if elem has no children
 *
 * Convenience function that returns the first child element
 * regardless of its name. Same as leptris_element_first_child(elem, NULL).
 *
 * Memory: Element is owned by document. Do not free separately.
 */
LEPTRIS_API LeptrisElement leptris_element_first_child_any(LeptrisElement elem);

/**
 * Get last child element regardless of name
 *
 * @param elem Parent element
 * @return Last child element or NULL if elem has no children
 *
 * Convenience function that returns the last child element
 * regardless of its name.
 *
 * Memory: Element is owned by document. Do not free separately.
 */
LEPTRIS_API LeptrisElement leptris_element_last_child_any(LeptrisElement elem);

/**
 * Get next sibling element regardless of name
 *
 * @param elem Element to start from
 * @return Next sibling element or NULL if elem is last child
 *
 * Convenience function that returns the next sibling element
 * regardless of its name. Same as leptris_element_next_sibling(elem, NULL).
 *
 * Memory: Element is owned by document. Do not free separately.
 */
LEPTRIS_API LeptrisElement leptris_element_next_sibling_any(LeptrisElement elem);

/**
 * Get child elements in bulk
 *
 * Fills out_children with up to max_count element children in
 * document order, skipping interleaved text/comment/CDATA nodes.
 * Size the array from leptris_element_child_count.
 */
LEPTRIS_API size_t leptris_element_children(
    LeptrisElement elem, LeptrisElement* out_children, size_t max_count);

/**
 * Get previous sibling element regardless of name
 *
 * @param elem Current element
 * @return Previous sibling element, or NULL if not found
 *
 * Convenience function that returns the previous sibling element
 * regardless of its name. Same as leptris_element_previous_sibling(elem, NULL).
 *
 * Memory: Element is owned by document. Do not free separately.
 */
LEPTRIS_API LeptrisElement leptris_element_previous_sibling_any(LeptrisElement elem);

/**
 * Get child element text content
 *
 * @param elem Parent element
 * @return Text content of first child text node, or NULL if not found
 *
 * Returns the text content of the first text node child.
 * This is a convenience function for accessing element text.
 *
 * Memory: String is owned by element. Do not free or modify.
 */
LEPTRIS_API const char* leptris_element_child_value(LeptrisElement elem);

/**
 * Deep copy element into a destination document (detached).
 *
 * @param src Element to copy (subtree copied recursively)
 * @param dest_doc Document that will own the copy. May be the same
 *                 document or a different one (cross-document copy).
 * @return Newly created copy detached from any parent, or NULL on
 *         error (bad args or pool allocation failure).
 *
 * Issue #148 Phase 1: backs `Node#dup` / `#clone` in the Ruby
 * binding. The copy carries no parent reference and the caller
 * owns attaching it via `leptris_element_append_child`.
 *
 * Memory: Copy is owned by `dest_doc`; released by
 *         `leptris_document_free`.
 */
LEPTRIS_API LeptrisElement leptris_element_copy(LeptrisElement src,
                                              LeptrisDocument dest_doc);

/**
 * Deep copy an entire document.
 *
 * @param src Source document
 * @return Newly created document, or NULL on error
 *
 * Copies the tree (root element + all descendants), the XML
 * declaration (version/encoding/standalone), document-level
 * processing instructions, and (if present) the DOCTYPE.
 *
 * Memory: Caller owns the result; release with
 *         `leptris_document_free`.
 */
LEPTRIS_API LeptrisDocument leptris_document_copy(LeptrisDocument src);

/**
 * Deep copy element and append to parent
 *
 * @param parent Parent element to append to
 * @param source Element to copy (can be from different document)
 * @return Newly created copy, or NULL on error
 *
 * Creates a deep copy including all children and attributes.
 * The copy belongs to parent's document.
 *
 * Memory: Copy is owned by parent's document
 */
LEPTRIS_API LeptrisElement leptris_element_append_copy(LeptrisElement parent, LeptrisElement source);

/**
 * Deep copy element and prepend to parent
 *
 * @param parent Parent element to prepend to
 * @param source Element to copy (can be from different document)
 * @return Newly created copy, or NULL on error
 *
 * Creates a deep copy and inserts at the beginning of parent's children.
 * The copy belongs to parent's document.
 *
 * Memory: Copy is owned by parent's document
 */
LEPTRIS_API LeptrisElement leptris_element_prepend_copy(LeptrisElement parent, LeptrisElement source);

/**
 * Deep copy element and insert before sibling
 *
 * @param sibling Sibling element to insert before
 * @param source Element to copy (can be from different document)
 * @return Newly created copy, or NULL on error
 *
 * Creates a deep copy and inserts it before the sibling.
 * The copy belongs to sibling's document.
 *
 * Memory: Copy is owned by sibling's document
 */
LEPTRIS_API LeptrisElement leptris_element_insert_copy_before(LeptrisElement sibling, LeptrisElement source);

/**
 * Deep copy element and insert after sibling
 *
 * @param sibling Sibling element to insert after
 * @param source Element to copy (can be from different document)
 * @return Newly created copy, or NULL on error
 *
 * Creates a deep copy and inserts it after the sibling.
 * The copy belongs to sibling's document.
 *
 * Memory: Copy is owned by sibling's document
 */
LEPTRIS_API LeptrisElement leptris_element_insert_copy_after(LeptrisElement sibling, LeptrisElement source);

/* ============================================================================
 * Serialization Operations
 * ============================================================================ */

/**
 * Options for XML serialization
 *
 * Definition lives in leptris/types.h.
 */

/**
 * Serialize document to XML string
 *
 * @param doc Document to serialize
 * @param options Serialization options (NULL for defaults: compact, no declaration)
 * @return XML string or NULL on error (caller must free with leptris_free_string)
 *
 * Example (compact):
 *   char* xml = leptris_document_serialize(doc, NULL);
 *   printf("%s\n", xml);
 *   leptris_free_string(xml);
 *
 * Example (pretty-print with declaration):
 *   LeptrisSerializeOptions opts = { .indent = 2, .xml_declaration = 1, .encoding = "UTF-8" };
 *   char* xml = leptris_document_serialize(doc, &opts);
 *   printf("%s\n", xml);
 *   leptris_free_string(xml);
 *
 * Memory: Caller must free returned string with leptris_free_string()
 */
LEPTRIS_API char* leptris_document_serialize(LeptrisDocument doc,
                                             const LeptrisSerializeOptions* options);

/**
 * Serialize a document into a caller-provided buffer (issue #535)
 *
 * One call, zero library-side allocations for the result: the
 * common FFI pattern (serialize + read_string + free_string)
 * collapses, and bindings can write into MemoryPointer-backed
 * storage directly.
 *
 * @param doc Document
 * @param buf Output buffer, or NULL to just query the size
 * @param capacity Capacity of buf in bytes
 * @param out_len Out: written length WITHOUT the NUL (may be NULL)
 * @return Bytes needed INCLUDING the NUL terminator. Copy happens
 *         only when buf is non-NULL and capacity >= return value.
 *         0 on failure.
 */
LEPTRIS_API char* leptris_document_serialize_ext(LeptrisDocument doc,
    const LeptrisSerializeOptions* options,
    const LeptrisSerializeExtOptions* ext);

/**
 * Size-aware leptris_document_serialize_ext (issue #644): the ext
 * struct may GROW, and FFI/binding callers allocate only the fields
 * they know. Pass your compile-time sizeof(LeptrisSerializeExtOptions)
 * — every field beyond your buffer stays at its zero default instead
 * of being read out of bounds. leptris_document_serialize_ext is
 * this entry with the full size.
 *
 * @param doc Document
 * @param options Serialization options, or NULL for defaults
 * @param ext Extended options, or NULL
 * @param ext_size sizeof of the caller's ext-struct allocation
 * @return Serialized XML string (caller frees with
 *         leptris_free_string), or NULL
 */
LEPTRIS_API char* leptris_document_serialize_ext_sized(LeptrisDocument doc,
    const LeptrisSerializeOptions* options,
    const LeptrisSerializeExtOptions* ext,
    size_t ext_size);

/**
 * Extended element serialization (issue #882): the same option
 * surface as leptris_document_serialize_ext — indent_text,
 * indent_unit, expand_empty — at element level.
 *
 * @param elem Element subtree to serialize
 * @param options Serialization options, or NULL for defaults
 * @param ext Extended options, or NULL
 * @return Serialized XML string (caller frees with
 *         leptris_free_string), or NULL
 */
LEPTRIS_API char* leptris_element_serialize_ext(LeptrisElement elem,
    const LeptrisSerializeOptions* options,
    const LeptrisSerializeExtOptions* ext);

/* Size-aware twin (issue #644 pattern). */
LEPTRIS_API char* leptris_element_serialize_ext_sized(LeptrisElement elem,
    const LeptrisSerializeOptions* options,
    const LeptrisSerializeExtOptions* ext,
    size_t ext_size);

LEPTRIS_API size_t leptris_document_serialize_into(LeptrisDocument doc,
                                                   char* buf,
                                                   size_t capacity,
                                                   size_t* out_len,
                                                   const LeptrisSerializeOptions* options);

/**
 * Serialize document with automatic options (legacy alias)
 *
 * Equivalent to leptris_document_serialize with options chosen from
 * the document's own properties (XML declaration included when the
 * source had one). Kept because it was exported before options
 * existed; prefer leptris_document_serialize for new code.
 *
 * Declared here retroactively (2026-08-22): the symbol was
 * implemented, exported, and used by bindings, but never declared
 * in any public header — invisible to header-derived cdef/bindgen
 * mirrors and to export audits.
 *
 * DEPRECATED: prefer leptris_document_serialize in new code; this
 * alias will not gain options or fixes beyond ABI stability.
 *
 * @param doc Document
 * @return XML string or NULL on error
 *
 * Memory: Caller must free returned string with leptris_free_string()
 */
LEPTRIS_API char* leptris_serialize_document(LeptrisDocument doc);

/**
 * Serialize element subtree to XML string
 *
 * @param elem Element to serialize
 * @param options Serialization options (NULL for defaults)
 * @return XML string or NULL on error (caller must free with leptris_free_string)
 *
 * Memory: Caller must free returned string with leptris_free_string()
 */
LEPTRIS_API char* leptris_element_serialize(LeptrisElement elem,
                                            const LeptrisSerializeOptions* options);

/**
 * Serialize an element subtree into a caller-provided buffer
 * (issue #535) — same contract as leptris_document_serialize_into.
 */
LEPTRIS_API size_t leptris_element_serialize_into(LeptrisElement elem,
                                                  char* buf,
                                                  size_t capacity,
                                                  size_t* out_len,
                                                  const LeptrisSerializeOptions* options);

/**
 * Save document to file
 *
 * @param doc Document to save
 * @param filepath Path to output file
 * @param options Serialization options (NULL for defaults: compact, no declaration)
 * @return LEPTRIS_OK on success, error code on failure
 *
 * Thread safety: Thread-safe (multiple threads can save different files)
 *
 * Example:
 *   LeptrisStatus status = leptris_document_save_file(doc, "output.xml", NULL);
 *   if (status != LEPTRIS_OK) {
 *       printf("Failed to save: %d\n", status);
 *   }
 */
LEPTRIS_API LeptrisStatus leptris_document_save_file(LeptrisDocument doc,
                                                  const char* filepath,
                                                  const LeptrisSerializeOptions* options);

/* ============================================================================
 * Canonical XML (C14N) Operations
 * ============================================================================ */

/**
 * Canonicalize document to C14N format
 *
 * C14N generates a canonical form of an XML document for:
 * - Digital signatures
 * - Cryptographic hashing
 * - Semantic XML comparison
 *
 * Key C14N rules applied:
 * 1. UTF-8 encoding
 * 2. Normalized line endings (\n)
 * 3. Lexicographic attribute ordering
 * 4. Namespace declaration ordering
 * 5. Empty element normalization (&lt;tag&gt;&lt;/tag&gt; not &lt;tag/&gt;)
 * 6. Entity/character reference expansion
 * 7. Attribute value quoting with double quotes
 *
 * @param doc Document to canonicalize
 * @param version C14N version (LEPTRIS_C14N_1_0 or LEPTRIS_C14N_1_1)
 * @param flags Reserved for future use (pass 0)
 * @return Canonicalized XML string (caller must free with leptris_free_string())
 *
 * Memory: Caller must free returned string with leptris_free_string()
 */
LEPTRIS_API char* leptris_c14n_canonicalize(struct leptris_document* doc,
                                          int version,
                                          int flags);

/**
 * Canonicalize a subtree rooted at the given element (issue #169).
 * Same algorithm as leptris_c14n_canonicalize but limited to elem
 * and its descendants. Pairs of PIs and the document's XML
 * declaration are NOT included (subtree C14N is element-scoped).
 *
 * @param elem Subtree root
 * @param version C14N version (LEPTRIS_C14N_1_0 or LEPTRIS_C14N_1_1)
 * @param flags Reserved for future use (pass 0)
 * @return Canonicalized XML string (caller must free with
 *         leptris_free_string), or NULL on error
  *
 * Memory: Caller must free the returned string (free() or leptris_free_string).
 */
LEPTRIS_API char* leptris_c14n_canonicalize_subtree(LeptrisElement elem,
                                                   int version,
                                                   int flags);

/**
 * Extended canonicalization with mode, inclusive namespaces, and
 * comments toggle (issue #183).
 *
 * @param doc Document
 * @param version LEPTRIS_C14N_1_0 or LEPTRIS_C14N_1_1
 * @param mode LEPTRIS_C14N_MODE_CANONICAL or LEPTRIS_C14N_MODE_EXCLUSIVE
 * @param inclusive_ns_prefixes NULL or NULL-terminated array of
 *        namespace prefixes to include even under EXCLUSIVE mode.
 *        Pass NULL when not needed.
 * @param with_comments 0 to strip comments, 1 to preserve
 * @return Canonicalized XML string (caller frees with
 *         leptris_free_string), or NULL on error
  *
 * Memory: Caller must free the returned string (free() or leptris_free_string).
 */
LEPTRIS_API char* leptris_c14n_canonicalize_ex(
    struct leptris_document* doc,
    int version,
    LeptrisC14NMode mode,
    const char** inclusive_ns_prefixes,
    int with_comments);

/**
 * Extended subtree canonicalization (issue #183). Same parameters
 * as leptris_c14n_canonicalize_ex but limited to elem + descendants.
  *
 * Memory: Caller must free the returned string (free() or leptris_free_string).
 */
LEPTRIS_API char* leptris_c14n_canonicalize_subtree_ex(
    LeptrisElement elem,
    int version,
    LeptrisC14NMode mode,
    const char** inclusive_ns_prefixes,
    int with_comments);

/* ============================================================================
 * Namespace Operations
 * ============================================================================ */

/**
 * Get element's active namespace
 *
 * @param elem Element
 * @return Namespace or NULL if elem has no namespace
 *
 * Memory: Namespace is owned by element. Do not free separately.
 */
LEPTRIS_API LeptrisNamespace leptris_element_namespace(LeptrisElement elem);

/**
 * Set (or detach) an element's namespace (issue #817)
 *
 * The setter counterpart of leptris_element_namespace, matching
 * Nokogiri's node.namespace= semantics:
 * - NULL (or "") DETACHES: the resolved namespace link and the
 *   name's prefix clear, and an xmlns="" undeclaration is added so
 *   an in-scope default namespace cannot capture the now
 *   unqualified name.
 * - A non-empty URI REBINDS to an IN-SCOPE declaration carrying
 *   that URI (its prefix is adopted). A URI with no in-scope
 *   declaration returns LEPTRIS_ERROR_NOT_FOUND — this API does
 *   not invent declarations; call
 *   leptris_element_add_namespace_definition first.
 *
 * @param elem Element
 * @param uri Namespace URI, or NULL to detach
 * @return LEPTRIS_OK, or an error status
 */
LEPTRIS_API LeptrisStatus leptris_element_set_namespace(
    LeptrisElement elem, const char* uri);

/**
 * Get an element's own namespace prefix
 *
 * The prefix of the element's qualified name (e.g. "foo" in
 * &lt;foo:child/&gt;), or NULL when the element has none.
 *
 * @param elem Element
 * @return Prefix string or NULL
 *
 * Memory: String is owned by element. Do not free or modify.
 */
LEPTRIS_API const char* leptris_element_prefix(LeptrisElement elem);

/**
 * Get namespace URI
 *
 * @param ns Namespace
 * @return URI string or NULL if ns is NULL
 *
 * Memory: String is owned by namespace. Do not free or modify.
 */
LEPTRIS_API const char* leptris_namespace_uri(LeptrisNamespace ns);

/**
 * Get namespace prefix
 *
 * @param ns Namespace
 * @return Prefix string or NULL if default namespace or ns is NULL
 *
 * Memory: String is owned by namespace. Do not free or modify.
 */
LEPTRIS_API const char* leptris_namespace_prefix(LeptrisNamespace ns);

/**
 * Resolve namespace prefix (with inheritance)
 *
 * @param elem Element to start search from
 * @param prefix Prefix to resolve (NULL for default namespace)
 * @return Namespace URI or NULL if not found
 *
 * Memory: String is owned by element. Do not free or modify.
 */
LEPTRIS_API const char* leptris_element_namespace_for_prefix(LeptrisElement elem, const char* prefix);

/**
 * Get number of namespaces declared on an element
 *
 * Counts xmlns and xmlns:* attributes. O(n) over the attribute list.
 *
 * @param elem Element
 * @return Namespace count or 0 if elem is NULL
 */
LEPTRIS_API size_t leptris_element_namespace_count(LeptrisElement elem);

/**
 * Get the prefix of the namespace declaration at the given index
 * (issue #171). Use with leptris_element_namespace_count to enumerate
 * all declarations on an element.
 *
 * @param elem Element
 * @param index 0-based index, must be < namespace_count
 * @return Prefix string (NULL for the default namespace),
 *         or NULL if index is out of range
 *
 * Memory: String is owned by the element. Do not free or modify.
 */
LEPTRIS_API const char* leptris_element_namespace_decl_prefix(LeptrisElement elem,
                                                              size_t index);

/**
 * Get the URI of the namespace declaration at the given index
 * (issue #171). Pairs with leptris_element_namespace_decl_prefix.
 *
 * @param elem Element
 * @param index 0-based index
 * @return URI string, or NULL if index is out of range
 */
LEPTRIS_API const char* leptris_element_namespace_decl_uri(LeptrisElement elem,
                                                          size_t index);

/**
 * Add a namespace declaration to an element (issue #186).
 *
 * @param elem Element to receive the declaration
 * @param prefix Namespace prefix. NULL or "" means default namespace.
 * @param href Namespace URI (required)
 * @return LEPTRIS_OK on success,
 *         LEPTRIS_ERROR_NULL_ARG if elem or href is NULL,
 *         LEPTRIS_ERROR_MEMORY on allocation failure
 *
 * Memory: prefix and href are pool-copied; caller may free or
 * modify the inputs immediately.
 */
LEPTRIS_API LeptrisStatus leptris_element_add_namespace_definition(
    LeptrisElement elem, const char* prefix, const char* href);

/**
 * Set the default namespace on an element (issue #186).
 * Equivalent to add_namespace_definition(elem, NULL, href).
 */
LEPTRIS_API LeptrisStatus leptris_element_set_default_namespace(
    LeptrisElement elem, const char* href);

/**
 * Remove the namespace declaration matching prefix (issue #186).
 *
 * @param elem Element
 * @param prefix Prefix to match. NULL means default namespace.
 * @return LEPTRIS_OK on success,
 *         LEPTRIS_ERROR_NULL_ARG if elem is NULL,
 *         LEPTRIS_ERROR_NOT_FOUND if no matching declaration exists
 */
LEPTRIS_API LeptrisStatus leptris_element_remove_namespace_definition(
    LeptrisElement elem, const char* prefix);

/**
 * Convert status code to human-readable string
 *
 * The CANONICAL status->message function (leptris_error_message in
 * error.h is a deprecated alias with identical output —
 * TODO.concurrency/04).
 *
 * @param status Status code from leptris_parse_string or other API
 * @return Static string (never NULL, never freed)
  *
 * Memory: Static string. Do not free.
 */
LEPTRIS_API const char* leptris_status_string(LeptrisStatus status);

/* ============================================================================
 * XPath Operations
 * ============================================================================ */

/**
 * Evaluate XPath expression
 *
 * @param doc Document (required)
 * @param context Context element (NULL = document root)
 * @param expression XPath expression string
 * @return XPath result or NULL on error
 *
 * Memory: Caller must call leptris_xpath_result_free() when done
 * Thread safety: Not thread-safe. One evaluation per thread.
 *
 * XPath 1.0 compliance: Full XPath 1.0 specification
 * - All 13 axes: child, descendant, parent, ancestor, sibling, etc.
 * - All 27 functions: string(), count(), position(), etc.
 * - All operators: =, !=, <, <=, >, >=, +, -, *, div, mod, |, and, or
 * - Predicates: [1], [\@attr], [position() > 2], etc.
 */
/* ============================================================================
 * Batch-context evaluation (issue #560)
 * ============================================================================ */

/**
 * Evaluate one expression against N context nodes in a single call
 *
 * Union semantics (NodeSet#xpath): each context element evaluates
 * the expression independently; the results merge into ONE
 * de-duplicated nodeset in document order — the same-node match from
 * several contexts is kept once, and the C side does the work that
 * bindings previously paid N dispatches for.
 *
 * @param doc Document
 * @param contexts Caller array of context elements (all non-NULL)
 * @param count Number of contexts
 * @param expression XPath 1.0 expression
 * @return De-duplicated, document-ordered nodeset, or NULL on
 *         NULL/empty inputs or evaluation failure
 *
 * Memory: free with leptris_xpath_result_free.
 */
LEPTRIS_API LeptrisXPathResult leptris_xpath_eval_nodeset(
    LeptrisDocument doc, LeptrisElement* contexts, size_t count,
    const char* expression);

/** Compiled-expression variant of leptris_xpath_eval_nodeset. */
LEPTRIS_API LeptrisXPathResult leptris_xpath_compiled_eval_nodeset(
    LeptrisDocument doc, LeptrisElement* contexts, size_t count,
    LeptrisXPathCompiled compiled);

LEPTRIS_API LeptrisXPathResult leptris_xpath_eval(
    LeptrisDocument doc,
    LeptrisElement context,
    const char* expression
);

/**
 * Evaluate an XPath expression under a pinned language version
 * (lane 15). LEPTRIS_XPATH_10 keeps the strict XPath 1.0 surface
 * (3.x-only syntax — arrow, bang, lookup, let, inline functions,
 * string templates — is rejected with LEPTRIS_ERROR_INVALID_ARG);
 * LEPTRIS_XPATH_31 evaluates the full grammar (identical to
 * leptris_xpath_eval).
 *
 * @param doc Document to evaluate against
 * @param context Context element (NULL = document root)
 * @param expression Expression text
 * @param version LEPTRIS_XPATH_10 or LEPTRIS_XPATH_31
 * @param status Optional status out-param
 * @return Result handle, or NULL on error
 * Memory: Caller owns the result (leptris_xpath_result_free).
 */
LEPTRIS_API LeptrisXPathResult leptris_xpath_eval_versioned(
    LeptrisDocument doc,
    LeptrisElement context,
    const char* expression,
    LeptrisXPathVersion version,
    LeptrisStatus* status
);

/**
 * Compile an XPath expression once, evaluate many times
 *
 * Skips the per-call expression hash + cache probe that
 * leptris_xpath_eval pays — the hot-loop win for bindings
 * (TODO.bindings/03, issue #510 Tier 2).
 *
 * Thread contract: the compiled handle is immutable — any number of
 * threads may evaluate it concurrently (against their own
 * documents). Free only after the last evaluation returns.
 *
 * @param expression XPath 1.0 expression
 * @return Compiled handle, or NULL on syntax error (message via
 *         leptris_last_error)
 *
 * Memory: free with leptris_xpath_compiled_free.
 */
LEPTRIS_API LeptrisXPathCompiled leptris_xpath_compile(const char* expression);

/**
 * Evaluate a compiled expression against a document
 *
 * Same evaluation semantics as leptris_xpath_eval (VM path with the
 * direct-AST fallback); failures snapshot into the document's error
 * slot (leptris_document_last_error).
 *
 * @param compiled Handle from leptris_xpath_compile
 * @param doc Document to evaluate against
 * @param context Context element, or NULL for the document root
 * @return Result (free with leptris_xpath_result_free), or NULL
 */
LEPTRIS_API LeptrisXPathResult leptris_xpath_compiled_eval(
    LeptrisXPathCompiled compiled, LeptrisDocument doc,
    LeptrisElement context);

/**
 * Free a compiled expression handle
 *
 * Must not race with in-flight evaluations of the same handle.
 */
/* Source text of a compiled expression (owned by the handle).
 * Memory: valid until leptris_xpath_compiled_free. */
LEPTRIS_API const char* leptris_xpath_compiled_text(
        LeptrisXPathCompiled compiled);

LEPTRIS_API void leptris_xpath_compiled_free(LeptrisXPathCompiled compiled);

/**
 * Evaluate a compiled expression with external namespace bindings
 *
 * Compiled-handle counterpart of leptris_xpath_eval_ns
 * (TODO.engine/02): prefixed name tests resolve through the binding
 * set, same semantics, minus the per-call parse.
 */
LEPTRIS_API LeptrisXPathResult leptris_xpath_compiled_eval_ns(
    LeptrisXPathCompiled compiled, LeptrisDocument doc,
    LeptrisElement context, LeptrisXPathNsSet ns);

/**
 * Evaluate a compiled expression with external variables
 *
 * Compiled-handle counterpart of leptris_xpath_eval_with_vars_context
 * (TODO.engine/02): $var references resolve through the variable
 * set, same semantics, minus the per-call parse.
 */
LEPTRIS_API LeptrisXPathResult leptris_xpath_compiled_eval_vars(
    LeptrisXPathCompiled compiled, LeptrisDocument doc,
    LeptrisElement context, LeptrisXPathVariableSet variables);

/**
 * Evaluate a compiled expression with namespaces AND variables
 *
 * The combined form (issue #608): prefixed name tests resolve
 * through the binding set and $var references through the variable
 * set in one compiled call — bindings no longer fall back to
 * uncompiled evaluation for expressions like count(//x:b[@id=$id]).
 * Either argument may be NULL for the single-set behavior.
 */
LEPTRIS_API LeptrisXPathResult leptris_xpath_compiled_eval_ns_vars(
    LeptrisXPathCompiled compiled, LeptrisDocument doc,
    LeptrisElement context, LeptrisXPathNsSet ns,
    LeptrisXPathVariableSet variables);

/**
 * Compile an XSLT 1.0 stylesheet (TODO.transform)
 *
 * The stylesheet is compiled ONCE into an immutable instruction
 * forest: patterns compile to XPath ASTs, select/test expressions go
 * through leptris_xpath_compile, and literal result elements are
 * stored verbatim. The handle can then be applied to any number of
 * documents with no re-parsing.
 *
 * @param stylesheet_xml The stylesheet document (UTF-8)
 * @param len Byte length of stylesheet_xml
 * @return Compiled stylesheet, or NULL on syntax error (message via
 *         leptris_last_error)
 *
 * Memory: free with leptris_xslt_free.
 */
LEPTRIS_API LeptrisXslt leptris_xslt_parse(const char* stylesheet_xml,
                                            size_t len);

/**
 * Compile a stylesheet from a file
 *
 * Reads `path` (UTF-8) and compiles via leptris_xslt_parse. Also
 * resolves §2.7 embedded stylesheets (xml-stylesheet PI with an
 * href="#id" fragment) when the document root is not a stylesheet.
 */
LEPTRIS_API LeptrisXslt leptris_xslt_parse_file(const char* path);

/**
 * Free a compiled stylesheet handle
 *
 * Must not race with in-flight leptris_xslt_apply calls on the same
 * handle.
 */
LEPTRIS_API void leptris_xslt_free(LeptrisXslt xslt);

/**
 * Apply a compiled stylesheet to a document
 *
 * @param xslt Compiled stylesheet (from leptris_xslt_parse)
 * @param doc Source document (not modified)
 * @return Result tree (free with leptris_document_free), or NULL on
 *         error (message via leptris_document_last_error)
 */
LEPTRIS_API LeptrisDocument leptris_xslt_apply(LeptrisXslt xslt,
                                                LeptrisDocument doc);

/**
 * Apply a compiled stylesheet and serialize the result
 *
 * Convenience wrapper: leptris_xslt_apply + serialize, including
 * top-level text nodes and result fragments that the tree API would
 * flatten.
 *
 * @param xslt Compiled stylesheet
 * @param doc Source document
 * @return Serialized result (free with leptris_free_string), or NULL
 */
LEPTRIS_API char* leptris_xslt_apply_string(LeptrisXslt xslt,
                                             LeptrisDocument doc);

/**
 * Custom XPath function handler (string-valued).
 *
 * The handler receives the string representations of each XPath
 * argument (XPath node-sets are flattened to concatenated text
 * content; numbers and booleans are stringified). It returns a
 * newly-allocated NUL-terminated string, or NULL on error.
 *
 * The caller (libleptris) frees the returned string. The `args`
 * array is owned by libleptris; do not free or modify.
 *
 * Use this with `leptris_xpath_register_function` to expose Ruby
 * callbacks via `Searchable#xpath(expr, ..., handler)` in the
 * Nokogiri-compatible API.
 */
typedef char* (*LeptrisXPathFn)(const char* const* args,
                                int argc,
                                void* user_data);

/**
 * Register a custom XPath function on a document.
 *
 * @param doc Document that will own the registration
 * @param name Function name as it appears in XPath expressions.
 *             Must not conflict with the standard XPath 1.0
 *             function names (count, position, last, etc.) — the
 *             standard library wins ties.
 * @param fn Handler. Must not be NULL.
 * @param user_data Opaque pointer passed back to `fn` on each
 *                  call. May be NULL.
 * @return LEPTRIS_OK on success, LEPTRIS_ERROR_NULL_ARG on NULL
 *         doc/name/fn.
 *
 * Registered functions are scoped to `doc`; they live for the
 * document's lifetime and are released by
 * `leptris_document_free`. Within an XPath expression they are
 * callable by name — no namespace prefix needed. The handler
 * runs only when the function is invoked during
 * `leptris_xpath_eval` against this document.
 *
 * Memory: the document owns the registration; user_data is owned
 *         by the caller.
 */
LEPTRIS_API LeptrisStatus leptris_xpath_register_function(
    LeptrisDocument doc,
    const char* name,
    LeptrisXPathFn fn,
    void* user_data
);
/**
 * Check whether an XPath function name is supported
 *
 * Covers the standard XPath 1.0 library. Custom functions
 * registered on a document report 0 here (they are per-document).
 *
 * @param function_name Function name (e.g. "count")
 * @return 1 supported, 0 unknown/NULL
 */
LEPTRIS_API int leptris_xpath_function_supported(const char* function_name);

/**
 * List the supported XPath function names
 *
 * @return NULL-terminated static array of names (do not free);
 *         valid for the library's lifetime
 */
LEPTRIS_API const char** leptris_xpath_supported_functions(void);
/**
 * Enable the first-party EXSLT-style extension pack for a document
 *
 * Registers str:/set:/math: prefixed functions (replace, tokenize,
 * split, concat, padding; distinct, intersection, difference,
 * leading, trailing; max, min, abs, sqrt, power) as native handlers
 * on this document — one C implementation shared by every binding
 * instead of per-language reimplementations (TODO.concurrency/06).
 * Prefixed names never collide with the XPath 1.0 core.
 *
 * @param doc Document that will own the registration
 * @return LEPTRIS_OK, or NULL_ARG
 */
LEPTRIS_API LeptrisStatus leptris_exslt_enable(LeptrisDocument doc);



/**
 * Get XPath result type
 *
 * @param result XPath result
 * @return Result type or -1 if result is NULL
 */
LEPTRIS_API LeptrisXPathResultType leptris_xpath_result_type(LeptrisXPathResult result);

/**
 * Get nodeset size (for NODESET results)
 *
 * @param result XPath result
 * @return Number of nodes or 0 if not a nodeset or result is NULL
 */
LEPTRIS_API size_t leptris_xpath_result_count(LeptrisXPathResult result);

/**
 * Get node from nodeset by index
 *
 * @param result XPath result
 * @param index Node index (0-based)
 * @return Element or NULL if index out of bounds or not a nodeset
 *
 * Memory: Element is owned by document. Do not free separately.
 */
LEPTRIS_API LeptrisElement leptris_xpath_result_get(LeptrisXPathResult result, size_t index);

/**
 * Batch-copy all nodes from a nodeset result into a caller-provided array.
 *
 * Eliminates per-node FFI call overhead for bindings that iterate
 * large nodesets (e.g., //book returning 100+ nodes). One call
 * instead of N calls (#262).
 *
 * @param result XPath result (must be NODESET type)
 * ELEMENTS ONLY: mixed nodesets (from //node(), text(), unions)
 * copy just their element entries here — use
 * leptris_xpath_result_get_nodes_ex for every kind
 * (TODO.concurrency/03).
 *
 * @param out_nodes Caller-allocated array of LeptrisElement
 * @param max_count Capacity of out_nodes
 * @return Number of ELEMENT entries copied (not the result count
 *         when the nodeset is mixed)
 *
 * Memory: Elements are owned by document. Do not free separately.
 */
LEPTRIS_API size_t leptris_xpath_result_get_nodes(
    LeptrisXPathResult result, LeptrisElement* out_nodes, size_t max_count);
/**
 * Copy every node of a mixed nodeset result with its kind
 *
 * Unlike leptris_xpath_result_get_nodes (which copies ELEMENT
 * entries only), this copies every entry — elements, text, comment,
 * CDATA, attribute, namespace — writing the node handle to
 * out_nodes[i] and its kind to out_kinds[i]. Inspect handles via
 * leptris_node_get_type / leptris_node_first_child; attribute-kind
 * entries are synthetic nodes understood by the mixed-nodeset
 * accessors (TODO.concurrency/03).
 *
 * @param result XPath result (NODESET type)
 * @param out_nodes Output array (may be NULL when only counting)
 * @param out_kinds Output kind array (may be NULL)
 * @param max_count Capacity of both arrays
 * @return Number of entries copied (min(result count, max_count))
 */
LEPTRIS_API size_t leptris_xpath_result_get_nodes_ex(
    LeptrisXPathResult result,
    LeptrisNodeRef* out_nodes,
    LeptrisXPathNodeKind* out_kinds,
    size_t max_count);


/**
 * Get the kind of a node in a mixed nodeset result
 *
 * Nodesets contain element nodes AND synthetic attribute nodes (from
 * \@attr / attribute:: axes). This reports which. String results of
 * any kind are read via leptris_xpath_result_string.
 *
 * @param result XPath result (NODESET type)
 * @param index Node index (0-based, < leptris_xpath_result_count)
 * @return Node kind, or LEPTRIS_XPATH_NODE_OTHER on error/out of range
 */
LEPTRIS_API LeptrisXPathNodeKind leptris_xpath_result_node_kind(
    LeptrisXPathResult result, size_t index);

/**
 * Get any node from a mixed nodeset result
 *
 * Unlike leptris_xpath_result_get (elements only), this returns the
 * node whatever its kind. For LEPTRIS_XPATH_NODE_ELEMENT results the
 * handle casts to LeptrisElement (leptris_node_as_element); attribute
 * and text nodes are opaque handles read via node_name/node_value.
 *
 * @param result XPath result (NODESET type)
 * @param index Node index (0-based)
 * @return Node handle, or NULL on error/out of range
 *
 * Memory: Node is owned by the result/document. Do not free.
 */
LEPTRIS_API LeptrisNodeRef leptris_xpath_result_get_node(
    LeptrisXPathResult result, size_t index);

/**
 * Get the name of a node in a nodeset result
 *
 * @param result XPath result (NODESET type)
 * @param index Node index (0-based)
 * @return Element name for element nodes; attribute name for
 *         attribute nodes; NULL for text/comment/other nodes
 *
 * Memory: String is owned by the result or the document. Do not free.
 */
LEPTRIS_API const char* leptris_xpath_result_node_name(
    LeptrisXPathResult result, size_t index);

/**
 * Get the string value of a non-element node in a nodeset result
 *
 * Attribute nodes: the attribute value. Text nodes: the content.
 *
 * @param result XPath result (NODESET type)
 * @param index Node index (0-based)
 * @return Value string, or NULL for element/other nodes
 *
 * Memory: String is owned by the result. Do not free.
 */
LEPTRIS_API const char* leptris_xpath_result_node_value(
    LeptrisXPathResult result, size_t index);

/**
 * Get boolean value (for BOOLEAN results or type conversion)
 *
 * @param result XPath result
 * @return Boolean value (1 = true, 0 = false)
 *
 * Type conversion rules:
 * - BOOLEAN: Direct value
 * - NUMBER: true if non-zero and not NaN
 * - STRING: true if non-empty
 * - NODESET: true if non-empty
 */
LEPTRIS_API int leptris_xpath_result_boolean(LeptrisXPathResult result);

/**
 * Get number value (for NUMBER results or type conversion)
 *
 * @param result XPath result
 * @return Number value (NaN if conversion fails)
 *
 * Type conversion rules:
 * - NUMBER: Direct value
 * - BOOLEAN: 1.0 or 0.0
 * - STRING: Parsed as number (NaN if invalid)
 * - NODESET: First node's string value converted to number
 */
LEPTRIS_API double leptris_xpath_result_number(LeptrisXPathResult result);

/**
 * Get string value (for STRING results or type conversion)
 *
 * @param result XPath result
 * @return String value or NULL if result is NULL
 *
 * Memory: Caller must call leptris_free_string() when done
 *
 * Type conversion rules:
 * - STRING: Direct value
 * - BOOLEAN: "true" or "false"
 * - NUMBER: String representation of number
 * - NODESET: String value of first node (recursive text concatenation)
 */
LEPTRIS_API char* leptris_xpath_result_string(LeptrisXPathResult result);

/**
 * Free XPath result
 *
 * @param result Result to free (can be NULL)
 */
LEPTRIS_API void leptris_xpath_result_free(LeptrisXPathResult result);

/* ============================================================================
 * XPath Variables (XPath 1.0)
 * ============================================================================ */

/**
 * XPath variable value types
 *
 * Definition lives in leptris/types.h.
 */

/**
 * Opaque variable set type — defined in leptris/types.h.
 */

/**
 * Create a new variable set
 *
 * @return New variable set, or NULL on error
 *
 * Memory: Caller must call leptris_xpath_variable_set_free() when done
 */
LEPTRIS_API LeptrisXPathVariableSet leptris_xpath_variable_set_new(void);

/**
 * Free a variable set
 *
 * @param set Variable set to free (can be NULL)
 */
LEPTRIS_API void leptris_xpath_variable_set_free(LeptrisXPathVariableSet set);

/**
 * Add a boolean variable to the set
 *
 * @param set Variable set
 * @param name Variable name (must be valid XPath identifier)
 * @param value Boolean value
 * @return LEPTRIS_OK on success, error code on failure
 */
LEPTRIS_API LeptrisStatus leptris_xpath_variable_set_boolean(LeptrisXPathVariableSet set, const char* name, int value);

/**
 * Add a number variable to the set
 *
 * @param set Variable set
 * @param name Variable name (must be valid XPath identifier)
 * @param value Number value
 * @return LEPTRIS_OK on success, error code on failure
 */
LEPTRIS_API LeptrisStatus leptris_xpath_variable_set_number(LeptrisXPathVariableSet set, const char* name, double value);

/**
 * Add a string variable to the set
 *
 * @param set Variable set
 * @param name Variable name (must be valid XPath identifier)
 * @param value String value
 * @return LEPTRIS_OK on success, error code on failure
 */
LEPTRIS_API LeptrisStatus leptris_xpath_variable_set_string(LeptrisXPathVariableSet set, const char* name, const char* value);

/**
 * Evaluate XPath expression with variables
 *
 * @param doc Document to evaluate against
 * @param expression XPath expression string
 * @param variables Variable set (can be NULL)
 * @return XPath result or NULL on error
 *
 * Memory: Caller must call leptris_xpath_result_free() when done
 *
 * XPath 1.0 compliance: Full XPath 1.0 specification
 *
 * Variables are referenced in expressions using $name syntax:
 *   leptris_xpath_variable_set_number(vars, "x", 42);
 *   leptris_xpath_eval(doc, "//item[@id = $x]", vars);
 */
LEPTRIS_API LeptrisXPathResult leptris_xpath_eval_with_vars(
    LeptrisDocument doc,
    const char* expression,
    LeptrisXPathVariableSet variables
);

/**
 * Evaluate an XPath expression with both a context node and a variable
 * set (issue #170). Use this when the receiver is not the document
 * root (e.g. relative paths like ".//item[@id = $x]").
 *
 * @param doc Document
 * @param context Context element (NULL = document root)
 * @param expression XPath expression
 * @param variables Variable set (may be NULL)
 * @return XPath result (caller frees with leptris_xpath_result_free)
  *
 * Memory: Result is owned by the caller; free with leptris_xpath_result_free.
 */
LEPTRIS_API LeptrisXPathResult leptris_xpath_eval_with_vars_context(
    LeptrisDocument doc,
    LeptrisElement context,
    const char* expression,
    LeptrisXPathVariableSet variables
);

/* ============================================================================
 * External Namespace Bindings
 * ============================================================================ */

/**
 * Create an empty namespace-binding set for XPath evaluation
 *
 * Bindings map expression prefixes to namespace URIs, so a prefixed
 * name test (p:title) matches elements in that namespace regardless
 * of the literal prefix the document uses (XPointer xmlns() is the
 * canonical use). Without bindings, prefixed name tests compare
 * prefixes literally (historic behavior, unchanged).
 *
 * @return New empty set (caller frees with leptris_xpath_ns_set_free)
 *
 * Memory: Set and its copies of prefix/URI strings are owned by the
 * caller; free with leptris_xpath_ns_set_free.
 */
LEPTRIS_API LeptrisXPathNsSet leptris_xpath_ns_set_new(void);

/**
 * Free a namespace-binding set (NULL-safe)
 */
LEPTRIS_API void leptris_xpath_ns_set_free(LeptrisXPathNsSet set);

/**
 * Bind an expression prefix to a namespace URI
 *
 * Re-binding an existing prefix replaces the URI. A NULL/empty
 * prefix is invalid (use unprefixed tests for no-namespace).
 *
 * @param set Binding set
 * @param prefix Prefix used in the expression (copied)
 * @param uri Namespace URI (copied)
 * @return LEPTRIS_OK, or NULL_ARG / MEMORY
 */
LEPTRIS_API LeptrisStatus leptris_xpath_ns_set_add(
    LeptrisXPathNsSet set, const char* prefix, const char* uri);
/**
 * Build a namespace-binding set from a flat array of pairs
 *
 * One FFI call instead of new + N x add (TODO.concurrency/07).
 * The array layout matches the c14n inclusive-namespaces argument
 * (flat [p1, u1, p2, u2, ...]) so bindings share one adapter.
 * Invalid entries (NULL/empty prefix or URI) fail the whole call.
 *
 * @param flat Array of 2*pair_count strings (prefix, URI alternating)
 * @param pair_count Number of pairs
 * @return New set (caller frees with leptris_xpath_ns_set_free),
 *         or NULL on NULL/invalid input
 *
 * Memory: Set and its copies of the strings are owned by the
 * caller; free with leptris_xpath_ns_set_free.
 */
LEPTRIS_API LeptrisXPathNsSet leptris_xpath_ns_set_new_from_pairs(
    const char* const* flat,
    size_t pair_count);


/**
 * Evaluate an XPath expression with external namespace bindings
 *
 * Same as leptris_xpath_eval_with_vars_context, plus prefix->URI
 * resolution for prefixed name tests (p:title matches any element
 * named title in the bound namespace, whatever prefix the document
 * declared it with).
 *
 * @param doc Document
 * @param context Context element (NULL = document root)
 * @param expression XPath expression
 * @param ns Namespace bindings (may be NULL; then behavior equals
 *        leptris_xpath_eval_with_vars_context)
 * @return XPath result (caller frees with leptris_xpath_result_free)
 *
 * Memory: Result is owned by the caller; free with leptris_xpath_result_free.
 */
LEPTRIS_API LeptrisXPathResult leptris_xpath_eval_ns(
    LeptrisDocument doc,
    LeptrisElement context,
    const char* expression,
    LeptrisXPathNsSet ns
);

/* ============================================================================
 * Memory Management Helpers
 * ============================================================================ */

/**
 * Free string returned by libleptris
 *
 * @param str String to free (can be NULL)
 *
 * Use this to free strings returned by:
 * - leptris_xpath_result_string()
 */
LEPTRIS_API void leptris_free_string(char* str);

/**
 * Pre-scan predicate: does the buffer contain a named entity
 * reference that is NOT one of the five predefined XML entities
 * (amp, lt, gt, quot, apos) or a numeric character reference?
 *
 * Downstream adapters (e.g. moxml) preprocess XML so non-standard
 * named entities round-trip; for the overwhelmingly common case —
 * all entities predefined or numeric — no rewrite is needed. This
 * one-pass C check replaces a whole-buffer regex scan on the
 * caller's side (issue #745: +61% parse overhead down to ~+2%).
 *
 * A bare '&' with no terminating ';' is not an entity reference
 * and is left to the parser's error reporting.
 *
 * @param s Buffer to scan (can be NULL)
 * @param len Buffer length in bytes
 * @return 1 if a non-standard named entity reference is present,
 *         0 otherwise
 */
LEPTRIS_API int leptris_str_has_nonstandard_entity(const char* s,
                                                   size_t len);

/**
 * Cleanup function to free internal memory structures (for testing)
 *
 * This function cleans up the compact pointer overflow table and other
 * thread-local structures that may accumulate stale entries across
 * multiple document operations.
 *
 * IMPORTANT: This should ONLY be called between test runs or when
 * you're certain no documents are active. Calling this while documents
 * are active will cause crashes.
 *
 * This function is primarily intended for test suites to prevent
 * state accumulation between tests.
 */
LEPTRIS_API void leptris_explicit_cleanup(void);

/* ============================================================================
 * Memory Allocation Hooks (for testing and custom allocators)
 *
 * The leptris_allocation_function / leptris_deallocation_function typedefs
 * come from leptris/types.h (included above).  Only the API entry points
 * live here.
 * ============================================================================ */

/**
 * Set custom memory management functions for all allocations
 *
 * By default, Leptris uses malloc/free. This function allows you to
 * specify custom allocation functions for testing (memory leak detection)
 * or custom memory management.
 *
 * WARNING: This function affects ALL Leptris operations globally.
 * Set custom functions BEFORE any parsing operations and restore
 * to defaults before program exit.
 *
 * @param alloc_function Function to use for memory allocation (NULL = use malloc)
 * @param dealloc_function Function to use for memory deallocation (NULL = use free)
 *
 * Example:
 *   void* my_alloc(size_t size) { return calloc(1, size); }
 *   void my_free(void* ptr) { free(ptr); }
 *   leptris_set_memory_management_functions(my_alloc, my_free);
 */
LEPTRIS_API void leptris_set_memory_management_functions(leptris_allocation_function alloc_function,
                                                         leptris_deallocation_function dealloc_function);

/**
 * Get current memory allocation function
 *
 * @return Current allocation function (NULL if using default malloc)
 */
LEPTRIS_API leptris_allocation_function leptris_get_memory_allocation_function(void);

/**
 * Get current memory deallocation function
 *
 * @return Current deallocation function (NULL if using default free)
 */
LEPTRIS_API leptris_deallocation_function leptris_get_memory_deallocation_function(void);

/* ============================================================================
 * Per-Document Allocator Hooks (TODO 74)
 *
 * Set allocator hooks on a specific document before parsing it.  Useful
 * when an app needs different allocators for different documents in
 * the same thread.
 *
 * Must be called BEFORE leptris_parse_string.  Changes after parse
 * have no effect on already-allocated memory.
 *
 * To set thread-default hooks (applies to all documents in the
 * current thread), use leptris_set_memory_management_functions().
 * ============================================================================ */

LEPTRIS_API LeptrisStatus leptris_document_set_allocators(
    LeptrisDocument doc,
    leptris_allocation_function alloc,
    leptris_deallocation_function dealloc);

/* ============================================================================
 * XInclude 1.0 Support
 * ============================================================================ */

/**
 * Process XInclude elements in document
 *
 * Replaces all <xi:include> elements with their included content.
 * Follows W3C XInclude 1.0 specification (https://www.w3.org/TR/xinclude/).
 *
 * @param doc Document to process
 * @param base_url Base URL for resolving relative hrefs (can be NULL)
 * @return LEPTRIS_OK on success, error code on failure
 *
 * XInclude namespace: http://www.w3.org/2001/XInclude
 *
 * Example:
 *   &lt;root xmlns:xi="http://www.w3.org/2001/XInclude"&gt;
 *     <xi:include href="chapter1.xml"/>
 *   &lt;/root&gt;
 */
LEPTRIS_API LeptrisStatus leptris_xinclude_process(LeptrisDocument doc, const char* base_url);

/**
 * Check if element is an XInclude include element
 *
 * @param elem Element to check
 * @return 1 if element is <xi:include>, 0 otherwise
 */
LEPTRIS_API int leptris_xinclude_is_include_element(LeptrisElement elem);

/**
 * Check if element is an XInclude fallback element
 *
 * @param elem Element to check
 * @return 1 if element is <xi:fallback>, 0 otherwise
 */
LEPTRIS_API int leptris_xinclude_is_fallback_element(LeptrisElement elem);

/**
 * Get href attribute value from include element
 *
 * @param include_elem Include element
 * @return href value or NULL if not found
  *
 * Memory: String is owned by the element. Do not free.
 */
LEPTRIS_API const char* leptris_xinclude_get_href(LeptrisElement include_elem);

/**
 * Get parse attribute value from include element
 *
 * @param include_elem Include element
 * @return "xml" or "text" (defaults to "xml" if not specified)
  *
 * Memory: String is owned by the element. Do not free.
 */
LEPTRIS_API const char* leptris_xinclude_get_parse(LeptrisElement include_elem);

/**
 * Get xpointer attribute value from include element
 *
 * @param include_elem Include element
 * @return xpointer value or NULL if not specified
  *
 * Memory: String is owned by the element. Do not free.
 */
LEPTRIS_API const char* leptris_xinclude_get_xpointer(LeptrisElement include_elem);

/**
 * Get encoding attribute value from include element (for parse="text")
 *
 * @param include_elem Include element
 * @return encoding value or NULL if not specified
  *
 * Memory: String is owned by the element. Do not free.
 */
LEPTRIS_API const char* leptris_xinclude_get_encoding(LeptrisElement include_elem);

/* ============================================================================
 * SAX (Simple API for XML) - Event-based Parsing
 * ============================================================================ */

/**
 * Include SAX parser support
 *
 * SAX provides event-driven XML parsing without building a DOM tree.
 * Useful for streaming large XML files or when only specific data is needed.
 *
 * Include <leptris/sax.h> for SAX parser API.
 */

/* ============================================================================
 * Version Information
 * ============================================================================ */

/* Keep in sync with project(VERSION) in the root CMakeLists.txt —
 * CI's export-surface gate diffs the header against the binary. */
#define LIBLEPTRIS_VERSION_MAJOR 1
#define LIBLEPTRIS_VERSION_MINOR 9
#define LIBLEPTRIS_VERSION_PATCH 145
#define LIBLEPTRIS_VERSION_STRING "1.9.145"

/**
 * Get libleptris version string
 *
 * @return Version string (e.g., "0.2.0")
  *
 * Memory: Static string. Do not free.
 */
LEPTRIS_API const char* leptris_version(void);

/**
 * Get the library version as numeric components
 *
 * @param major Out: major version (may be NULL)
 * @param minor Out: minor version (may be NULL)
 * @param patch Out: patch version (may be NULL)
 *
 * Memory: Writes through the out-pointers only.
 */
LEPTRIS_API void leptris_version_components(int* major, int* minor, int* patch);

/**
 * Release this thread's libleptris caches
 *
 * libleptris keeps small per-thread caches (XPath result/nodeset
 * free lists, the root-element map free list) for performance.
 * C99 has no portable thread-exit hook, so a thread that used
 * libleptris and then exits keeps those entries allocated. Call
 * this from each worker thread just before it exits — after the
 * last leptris_document_free — to release them (TODO.concurrency/08).
 *
 * Optional: long-lived threads and pooled bindings never need it.
 * Do not call while another operation is in flight on this thread.
 */
LEPTRIS_API void leptris_thread_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* LIBLEPTRIS_H */