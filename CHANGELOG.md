## [Unreleased]

## [1.9.157] - 2026-09-13
### Changed

- **Performance: attribute values <= 7 bytes now store INLINE in the attribute's own slot** — zero pool allocation per `set_attribute` overwrite and per fresh small-value insert. The attribute value field became a union (value bytes + NUL in [0..7], the length|flag in the length field's bytes [8..15]; content never overlaps the length field), and every value reader across the tree (~110 sites in 14 files) now goes through the `leptris_attr_value_sv()` accessor. The overwrite path also resolves the document once per call (previously it re-climbed `get_document`/`get_pool` twice) and computes the attr-index hash lazily. Measured on the pugixml twin harnesses (best-of-400, committed under `benchmarks/twins/`): **set-attribute 10k 510 -> 305us** vs pugixml 144us — the gap narrows from 3.54x to **2.12x**; create+append, attr-heavy and text-heavy rows all held. Spec: `AttributeInlineValue.SmallValuesStoreInlineAndRoundTrip` (RED pre-change; it also caught the unsound 15-byte draft, where the flag bytes clobbered value content past 8 — capacity 7 is the sound design for this slot). Lane 18 "beat pugixml in all shapes" continues: mutation bracket (append row), inline PCData (small-text parse row), batched entity decode (attr-heavy row).
## [1.9.156] - 2026-09-13
### Fixed

- **Heap corruption regression (#1038, v1.9.151-155): root-doc map entries now die with their document.** Two registration paths outlived their document — the pool-fallback creates (names > 254 bytes, carve failure) that register DETACHED elements, and the XInclude adopted-child free that nulls `new_dom_root` before recursing. After the doc died, a malloc-recycled element address resolved the FREED document through the stale entry and the engine wrote through its freed pool: roaming crashes in downstream binding suites at ~5% of runs. `document_free` now sweeps every map bucket for the document (never touching element headers — XInclude-adopted pools may already be gone). Validated: 100/100 clean full leptris-py suite runs post-fix (~5-7 expected crashes pre-fix); deterministic never-again specs `RootDocMapLifecycle.FallbackEntryDiesWithDocument` (RED pre-fix) and `RecycleLoopSurvives` (4000-iteration churn). The v1.9.151 perf rows (create 6.6ns, append 262us) are untouched — the sweep is once per document lifetime.
- **HTML (WHATWG mode), carried from the untagged v1.9.155: three template-mode gaps** — (1) in-column-group on a template ignores every token but `col` starts (colgroup/div/non-whitespace text drop); (2) `</template>` inside `<select>` runs the in-head rules so the select's insertion point returns; (3) `</template>` matches only HTML-namespace templates and skips the integration-point fence (an SVG `<template>` is foreign content). html5lib corpus 1200 -> 1206; Nokogiri parity floor 784 held. (v1.9.155 was merged to main but its tag was skipped by a GitHub GraphQL incident; both slices ship here.)
## [1.9.155] - 2026-09-13
### Fixed

- **HTML (WHATWG mode): three template-mode gaps closed.** (1) In-column-group on a `<template>` current node ignores every token but `col` starts (gumbo `handle_in_column_group`): `colgroup`, `div` and non-whitespace text after a `<col>` drop. (2) `</template>` inside `<select>` runs the in-head rules instead of the in-select ignore gate — the select's insertion point returns, so a following `<option>` is a select child. (3) `</template>` matches only HTML-namespace templates and skips the integration-point fence — an SVG `<template>` element is foreign content, so the end tag pops the whole foreign stack down to the html template and resets. html5lib corpus 1200 -> 1206 (template.dat 22/71/73/74/76/100); Nokogiri parity floor 784 held; specs `TemplateColumnGroupDropsNonColTokens`, `SelectTemplateCloseRestoresSelect`, `TemplateEndPopsThroughForeignContent`. Test-only: the corpus runner grows an `H5DUMP=1` expected-vs-ours tree dump per failed case.
## [1.9.154] - 2026-09-13
### Fixed

- **HTML (WHATWG mode): `frame`/`frameset` start tags inside `<template>` content are dropped.** The "in template" insertion mode's anything-else clause (WHATWG HTML 13.2.6.4.10) routes them to the in-body rules, which ignore them. html5lib `template.dat` corpus 1197 -> 1200 (rows 41/67/93); new spec `TemplateFrameAndFramesetDrop` (3 shapes); Nokogiri parity floor 784 held.
## [1.9.153] - 2026-09-13

### Added

- **`leptris_element_create_child(parent, name)` (API)** — the
  fused create+append in one call: one document resolution, one
  public entry, the single-call builder twin of pugixml's
  `append_child(name)`. Tree semantics identical to the two-call
  pair (QName splits, namespace backpointer resolution, append
  rules, cache invalidations); spec pins tree-equality against the
  pair. Measured note (lane 18 verdict, honest): identical wall
  time to the two-call path on this machine — the cross-call
  frames were already free, and the remaining append cost is the
  safety semantics (validation, COW versioning, doc resolution),
  not call overhead.



## [1.9.152] - 2026-09-13

### Performance

- **append fast-path rider**: sequential appends' tail is an
  element — the sibling edge is now the direct offset store
  instead of the type-dispatching node setter, and the two
  documented no-op `set_last_child` calls drop from the hot path
  (append-only 187 → 180 ns per 10k ops; semantics identical,
  54/54 mutation specs). Completes the lane-18 P0–P3 arc with
  v1.9.149–151: append row 401 → ~258 µs, create path 91.6 → 6.6 ns.



## [1.9.151] - 2026-09-13

### Performance

- **DOM create path 19.5 → 6.6 ns/op; append row 390 → 262 µs
  (3.5× → 2.30× vs pugixml at the arc's start/today)**: public
  element creation is namebp-authoritative — the doc backpointer
  stamped ahead of the name is the resolution path (get_document
  already consulted it on map miss), so the per-element root-map
  registration was pure redundancy. Two correctness companions
  fixed the long-standing #905 elision crash class at its root:
  QName splits now RE-STAMP the backpointer into a fresh slot (was:
  clear the flag and rely on registration), and set_root validates
  via get_document instead of the raw map lookup (which rejected
  every unregistered element). Pool-fallback create paths keep
  registering. Spec: MutNameBackpointer.PrefixedSplitRestampsBackpointer.



## [1.9.150] - 2026-09-13

### Performance

- **DOM create path 91.6 → 19.2 ns/op (10k-create harness 916 →
  192 µs)**: the root-doc map malloc'd an entry per *created*
  element whenever its free-list was empty (78% of the whole public
  create path); entries now carve from 128-entry TLS chunks. Same
  map, same registration semantics, same lifetime.
- **set-attr row 459 → 327 µs** (2.3× → 1.60× vs pugixml):
  `attr_count` is uint8_t and wraps at 255 — the walk-vs-index
  threshold keyed on it oscillated, dropping high-attr elements
  back onto the O(N) walk. A latching header bit keeps the decision
  stable; spec pins >255-attr dedup.



## [1.9.149] - 2026-09-12

### Performance

- **attr-heavy DOM parse 6.4% faster** (best-of-2000 harness: 684 →
  641 µs; cumulative with v1.9.147: 2086 → 641 µs, 4.9× → 1.63× vs
  pugixml): the per-element namespace side-cache now carves from
  64-entry pool chunks (the v1.9.147 raw-attr pattern) instead of
  one pool allocation per attribute-bearing element. The first
  attempt's chunk-exhaustion off-by-one (cursor left pointing at
  the entry just returned → cache aliasing → erased xmlns
  declarations) was caught by the libxslt suite and root-caused by
  pointer instrumentation before this release.



## [1.9.148] - 2026-09-12

### Added

- **WHATWG template insertion modes** — start tags now treat an
  open template as a scope boundary (13.2.4.2: table-context starts
  inside a template become template content instead of popping it),
  and each open template tracks its saved insertion mode
  (13.2.6.4.10): fresh templates open rows/cells/sections bare; a
  cell after a closed row gets an implied `tr`; a row after a closed
  section gets its implied `tbody`; rows/sections with nothing in
  table scope drop; head-family tokens leave the mode untouched;
  stray table tags in body-mode content drop. Transitions verified
  against gumbo's reference handlers. html5lib corpus floor
  1175 → 1197, Nokogiri parity 784 held, html4 mode unchanged.
  Specs: TemplateStartTagFence, TemplateInsertionModes.



## [1.9.147] - 2026-09-12

### Performance

- **attr-heavy DOM parse 2.7x faster** (`bench_matrix` attr-heavy-5k:
  2086 -> 769 us; 4.9x -> 1.96x vs pugixml): the raw attribute view
  (issue #635) now carves from 128-entry pool chunks with a
  parser-local tail (was one pool_alloc + tail walk per attribute),
  and the #542 prefixed-attr owner cache allocates inline at colon
  detection — the post-loop stamp pass with its per-attr memchr is
  gone. Spec: RawAttributesAcrossChunkBoundaries (chunk-boundary
  crossings pinned). No other row regressed; text-heavy parse stays
  3.2x ahead of pugixml. The mutation rows (append/set-attr) are
  ceiling-analyzed as a compact-node redesign — banked in
  TODO 13.



## [1.9.146] - 2026-09-12

### Fixed

- **WHATWG `<template>` insertion, subset** — end tags no longer pop
  past the nearest open template (html5lib template.dat 7/78/79);
  `html`/`head`/`body` start tags inside a template drop entirely
  instead of merging attributes onto the outer elements (template.dat
  64-67); `</template>` clears the active formatting list to its
  marker. WHATWG corpus floor 1169 → 1175, Nokogiri parity 784 held,
  html4 mode unchanged (all new branches whatwg-gated). The remaining
  ~25 template.dat cases need the per-template insertion-mode stack
  (design banked in TODO.xslt-full/14-html-mode.md).



## [1.9.145] - 2026-09-12

### Fixed

- **`LIBLEPTRIS_VERSION_*` macros** — the public version macros still
  said "0.1.0" at 1.9.x; they now track the release (and
  `bump-version.sh` updates them every release, so this cannot drift
  again).

### Benchmarks

- **XSLT scorecard reference twins** (`benchmarks/xslt/scorecard_lxml.py`) —
  in-process libxslt timings for the five fixtures that lacked a
  reference; every scorecard ratio now reproduces from the tree.
- `bench_matrix` reports the versions it actually links
  (`PROJECT_VERSION`, `PUGIXML_VERSION`, `LIBXML_DOTTED_VERSION`)
  instead of hardcoded stale strings.


## [1.9.144] - 2026-09-12

### Added

- **`leptris_element_expanded_name` (API)** — the local name,
  prefix, and resolved namespace URI through one call, contracts
  identical to the three individual accessors (name never NULL;
  prefix/URI NULL when absent; strings document-owned). Pure
  addition for FFI adapters that fan out per-element reads.
  Measured note: the leptris-ruby gem's read path does NOT gain
  from it (the batch's buffer+tuple allocation outweighs the
  saved crossing — 4-way benchmarked; the binding keeps its
  per-accessor calls), so it ships as an adapter-available
  primitive, not a gem change.



## [1.9.143] - 2026-09-12

### Added

- **HTML numeric-reference end states (#659)** — WHATWG 13.2.5.84
  in the WHATWG decode branch: NUL, surrogate, and out-of-range
  references (including overflow digit strings, semicolon or
  not) become U+FFFD; the C1 range remaps through the
  Windows-1252 table. html5lib corpus **1129 → 1169** (+40) —
  entities01.dat fully green; Nokogiri parity floor 784
  unchanged.



## [1.9.142] - 2026-09-12

### Added

- **HTML RCDATA/rawtext family (#659)** — `title`/`textarea`
  are RCDATA per WHATWG 13.2.6.2 (entity-decoded content, no
  markup inside — a `<!--` inside `<title>` was parsed as a
  comment; `textarea` drops one leading newline), and
  `iframe`/`noembed`/`xmp` join the raw-text set. html5lib
  corpus **1096 → 1129** (+33; **933 → 1129** today across
  v1.9.137–.142); Nokogiri parity floor 784 unchanged.



## [1.9.141] - 2026-09-12

### Added

- **HTML script-data escaped states (#659)** — the WHATWG
  13.2.5.15-.31 script tokenizer states: `<!--` inside script
  text enters script-data-escaped; there `</script` + delimiter
  closes (a bare `</scripta` stays text), `<script` + delimiter
  enters double-escaped where `</script>` drops only one level,
  and `-->`/`--!>` re-enter plain script data. Clears the
  html5lib tests16 script-escape block plus script cases across
  webkit01/tests2. html5lib corpus **1046 → 1096** (+50;
  933 → 1096 today across v1.9.137-.141); Nokogiri parity floor
  784 unchanged (html4 keeps the naive scan).



## [1.9.140] - 2026-09-12

### Added

- **HTML WHATWG bogus-markup edges + heading self-close
  (#659)** — tokenizer tails (html5lib tests1:38-49): EOF right
  after `</` emits the two characters as text (eof-before-tag-
  name); an invalid first tag-name char after `</`, any
  non-doctype `<!` construct, and every `<?` (html5lib has no PI
  tokenizer) make a bogus comment — raw bytes to the first `>`
  with NUL → U+FFFD, riding the document prolog while still in
  the initial mode. Heading starts pop a current heading
  (`<h1>x<h2>` → siblings). The html4 entry keeps the libxml2
  PI/bogus shapes (Nokogiri parity untouched). The html5lib
  corpus harness now reads its `.dat` binary —
  `std::string(char*)` truncated every line at its first NUL and
  the unsafe-inputs corpus has real NULs. html5lib corpus
  **1027 → 1046**; parity floor 784 unchanged.



## [1.9.139] - 2026-09-12

### Added

- **HTML in-table clear-stack + frameset content drop (#659)** —
  cell/row/group/caption starts pop the stack back to table
  context before the wrapper synthesis, so stray elements above
  the table no longer swallow the synthesized cell (a
  foster-parented `<a>` reconstructs as a second link holding the
  trailing text — adoption01:11; adoption01/02 now 18/18).
  "In frameset" drops everything but frameset/frame/noframes
  starts and non-whitespace text. A replace-body `<frameset>` is
  reused as its own `html > [head, frameset]` wrapper instead of
  getting a synthesized shell around it. html5lib corpus
  **1027 → 1038**; Nokogiri parity floor 784 unchanged.



## [1.9.138] - 2026-09-12

### Added

- **HTML foreign integration points as scope boundaries
  (#659)** — MathML text integration points (`mi`/`mo`/`mn`/
  `ms`/`mtext`) and HTML integration points (`annotation-xml`
  with an HTML `encoding`, SVG `foreignObject`/`desc`/`title`)
  now terminate every WHATWG scope walk (13.2.4.2): a block
  start inside an integration point no longer closes an outer
  `p`, an end tag fenced off by an integration point is ignored,
  and a foreign breakout pops only down TO the integration point
  so the reprocessed HTML tag lands inside it. In-select drops
  non-select start tags (their text joins the select's text) and
  `</table>` closes the select first (in-select-in-table).
  html5lib corpus **1006 → 1027** (+21); Nokogiri parity floor
  784 unchanged.



## [1.9.137] - 2026-09-12

### Added

- **HTML full adoption agency (#659)** — the WHATWG misnested-
  formatting machinery (`<b>1<p>2</b>3</p>`), replacing the
  simplified clone-reopen approximation: the list of active
  formatting elements (Noah's Ark clause, markers for
  applet/object/marquee/td/th/caption), reconstruct-active-
  formatting at text and formatting/ordinary insertions, and the
  21-step agency — furthest block (HTML-namespace only), inner
  clone chain, unconditional block adoption, bookmark
  re-insertion. `a`/`nobr` start tags run the agency for
  duplicate opens; block starts close an open `p` in button
  scope so dangling formatting reconstructs inside the new block.
  html5lib corpus **933 → 1006** (+73), adoption01/02 **17/18**
  green; Nokogiri/libxml2 parity floor 784 unchanged.



## [1.9.136] - 2026-09-12

### Fixed

- **HTML in-head comments (#659)** — a comment inside an explicit `<head>` is a head child (WHATWG "in head"): a comment first in the chain no longer blocks the whole head lift (`<!doctype html><head><!--c--><meta>` kept an empty head and dropped comment+meta into body), and a comment between head elements no longer truncates the run. The run-splice reparent now dispatches by node kind — the element-shaped write corrupted comment content. html5lib corpus floor **928 → 933**; Nokogiri parity held at 784.


## [1.9.135] - 2026-09-12

### Fixed

- **HTML initial-mode comment placement (#659)** — a comment that arrives while the document is still in its WHATWG initial insertion mode (no tag and no non-whitespace text yet) inserts as a child of the **Document**, ahead of the synthesized tree: `<!-- lead --><p>hi</p>` no longer drops the comment into the synthesized `<body>`. Token-time routing through a `left_initial` builder flag (any start tag — even a dropped structural one — any end tag, or non-ws text ends the initial phase); the html4/Nokogiri-parity entry keeps the libxml2 shape. Lane ledger records the next #659 nodes: the in-head insertion-mode gap and the adoption agency algorithm.


## [1.9.134] - 2026-09-11

### Added

- **QT3 slice 5: int64 integer fidelity (lanes 11/12)** — `xs:integer` and the full integer-subtype family (`xs:int`, `xs:long`, `xs:short`, `xs:unsignedShort`, `xs:unsignedLong`, `xs:negativeInteger`, `xs:positiveInteger`, `xs:nonPositiveInteger`, `xs:nonNegativeInteger`) plus integer-lexical `xs:decimal` keep the exact int64 value: `xs:integer('999999999999999999')` prints its decimal form at every boundary instead of collapsing through `double` into `1e+18`. Out-of-range lexicals reject per type bounds; arithmetic demotes to double (the documented boundary). The `fn/concat` test-set is vendored: **96 test-cases, 80/80 adopted** — the canonical `xs:double`/`xs:float` E-notation cases wait on a future slice (the number formatter keeps libxml2 parity, which is load-bearing for the libxslt suite).


## [1.9.133] - 2026-09-11

### Added

- **QT3 slice 4: external variables (lanes 11/12)** — `declare variable $x [as SequenceType] external [:= default]` now parses and binds through the new `leptris_xquery_eval_params` entry point: each param value is an XPath expression evaluated in an empty context (QT3 `<param select>` semantics — `select="()"` binds the empty sequence), a binding overrides the default initializer, and an unbound external without a default fails evaluation. The QT3 runner binds set-level `<param>` environments — the `-dyn` cases adopt and `fn/contains` is now **46/46**. Lane ledger updated in `TODO.xslt-full/11-xquery-core.md`.


## [1.9.132] - 2026-09-11

### Added

- **QT3 slice 3 (lanes 11/12)** — the `fn/starts-with` and `fn/ends-with` test-sets vendored verbatim from [w3c/qt3tests](https://github.com/w3c/qt3tests): **43/43** and **34/34** adopted cases (UCA-collation and error-assertion cases wait on future slices). The 3-arg collation form now covers `contains`, `starts-with`, and `ends-with` through one shared dispatch: codepoint = the default semantics; `html-ascii-case-insensitive` = ASCII-only folding (WHATWG matching); any other URI is an unknown-collation error.

### Fixed

- **Warning-clean full rebuild** — three warnings that incremental builds never surfaced: the HTML entity table/index anonymous-struct mismatch (one named `HtmlEnt` type now backs both), the dead pre-WHATWG `h_decode` wrapper (removed), and the staged-batch attribute mirror's const alias in the SAX pull parser (explicit cast at the staged lifetime).



## [1.9.131] - 2026-09-11

### Added

- **QT3 slice 2 (lanes 11/12)** — the `fn/contains` test-set vendored verbatim from [w3c/qt3tests](https://github.com/w3c/qt3tests): **41/41** adopted cases (the UCA-collation cases wait on the collation surface). Driving it to green exposed three XQuery engine bugs, fixed test-first:
  - **Multi-binding clauses never parsed** — `let $a := 1, $b := 2` / `for $x in ..., $y in ...` failed at the binding scan; a binding value is an ExprSingle, so the clause-level comma now ends the span and the clause loops its binding list (`treat as` inside a binding parses).
  - **let bindings collapsed sequences** — the tuple snapshot kept only the first member; `count()`/`string-join()` over a let-bound sequence now see every item.
  - **Single-tuple FLWOR returns lost their type** — a boolean false came back as a one-member truthy text nodeset; an atomic return now surfaces as itself at the API boundary.
- **3-arg `fn:contains`** — the codepoint collation is the default semantics; `html-ascii-case-insensitive` folds ASCII only (WHATWG matching); any other URI is an unknown-collation error.



## [1.9.130] - 2026-09-10

### Added

- **W3C QT3 test suite adoption, first slice (lanes 11/12)** — the `fn/substring` test-set vendored verbatim from [w3c/qt3tests](https://github.com/w3c/qt3tests) with an exact-N runner over the public XQuery API: **45/45** adopted cases. The runner parses the test-set with libleptris itself and grows its assertion surface incrementally (`assert-string-value`, `assert-eq`, `assert-true/false`, `all-of` today).

### Fixed

- **Pre-bound namespace prefixes** — XQuery reserves `fn`, `xs`, `math`, `map`, `array`, `err` without declaration; every `fn:`-prefixed call previously failed as "unknown function".
- **Exponent number literals** — `0E0`, `1.5e0`, `5.2E-3` (XPath 2.0+/XQuery doubles; no valid 1.0 expression changes meaning).
- **`count()` over atomic values** — one item, per sequence semantics (was a type error).
- **`substring()` over non-BMP characters** — positions are codepoints, not bytes; astral characters are one position (ASCII keeps the byte fast path).

## [1.9.129] - 2026-09-10

### Fixed

- **Namespace-blind tree diff** (found via the lutaml/canon integration): a changed namespace URI — or a re-prefixed `xmlns` declaration — produced zero ops ("documents are identical"), and declarations on an unprefixed element never entered the Merkle digest, so such subtrees pruned as equal. Namespace declarations now surface as `UPDATE_ATTR` ops on `xmlns[:prefix]` names (URIs as before/after), and the digest mixes the element's own declarations as sorted (prefix, URI) pairs. 3 new specs in `test/diff`.

## [1.9.128] - 2026-09-10

### Added

- **Schematron conformance gate (lane 16, phase 5)** — the 50-case [schematron/schematron-conformance](https://github.com/schematron/schematron-conformance) corpus vendored with an exact-N runner: **50/50** (14 error / 19 invalid / 17 valid). The corpus drove the engine to full ISO-2016 pattern semantics: XSLT-pattern contexts (any-depth `//` matching, `/` as the document node with tests evaluating from it), first-matching-rule-per-node, `@defaultPhase` + phase-scoped lets + phase selection, pattern lets with global visibility, abstract rules with pattern-scoped `extends`, duplicate-let and undefined-`$ref` rejection at every scope, and subordinate-document validation.
- **`key()` for Schematron** — `xsl:key` declarations in schemas now back `key()` inside assert/report tests during validation (per-document registry bridge, lazily built indexes; `use` may be an expression or element content).
- **Element-content lets** — a `<sch:let>` with element content grafts its content into the validated document as document-level children and binds the content's string value.

### Fixed

- Use-after-free in Schematron context evaluation (the patternized context buffer was freed before the document-node special case re-read it).

### Added

- **`leptris validate` CLI (lane 16, phase 6)** — the one-stop validator: `--rng FILE` (RELAX NG), `--schematron FILE` / `-s` (ISO Schematron), `--phase ID`, and `--svrl` (full SVRL report instead of the summary). Exit 0 valid / 1 invalid / 3 I/O or schema error; the summary prints `failed-assert at <location>: <message>` lines and both validators can run in one call.


## [1.9.127] - 2026-09-10

### Added

- **Schematron validation (lane 16, phases 1–4)** — native `sch:` schema IR + SVRL evaluator on the engine's XPath: patterns/rules/asserts/reports, `@context`/`@test`, `ns`/`let`/`param` scoping (schema → pattern → rule), abstract patterns with `is-a` instantiation and param substitution, phase selection (`leptris_schematron_parse_phase`), and `svrl:schematron-output` results (failed-assert / successful-report with `@location`/`@test`). Validity = zero failed-asserts. Loud-refuses non-XPath-1.x query bindings.
- **Native XML diff (lane 17)** — `leptris diff [--ignore-ws] A B` and the C API behind it: Merkle-digest pruning of equal subtrees (#869), LCS alignment over child digests, same-name recursion, and a compact op model (`- path @attr "a" -> "b"`, `~ path "old" -> "new"`, `+ path <name>`, `x path <name>`) with positional `/name[i]` paths.

### Fixed

- **#965** — relational comparisons (`<`, `<=`, `>`, `>=`) with a nodeset on the right-hand side evaluated with the operands swapped: `10 >= @n` behaved as `@n >= 10`. Both comparison implementations (AST evaluator and bytecode VM) now compare in the written order; nodeset-vs-nodeset any-pair keeps left/right orientation too.

## [1.9.126] - 2026-09-09

### Added

- XPath/XSLT: the lane 05/07 catalog tails (#691). adjust-
  {dateTime,date,time}-to-timezone (fixed-offset instant math;
  empty $tz removes the offset; implicit timezone = UTC),
  format-{dateTime,date,time} picture subset ([Y/M/D/H/h/m/s]
  widths, [MNn]/[DNn] names, [Z]), current-{dateTime,date,time},
  implicit-timezone(), fn:sort(seq, key?, collation?) with a
  stable codepoint order and optional key function. Lane 15:
  leptris_xpath_eval_versioned with the LeptrisXPathVersion enum
  (LEPTRIS_XPATH_10 rejects 3.x-only syntax, LEPTRIS_XPATH_31 is
  the full grammar; ABI append-only).

### Fixed

- ASAN strcat overflow in format-date + portable gmtime



## [1.9.125] - 2026-09-09

### Added

- HTML: comment tokenizer close forms per WHATWG 12.2.5.5x
  (#659). Comments close at `-->` or the abrupt `--!>` form;
  `<!-->` and `<!--->` are empty comments; unterminated comments
  run to EOF with their data verbatim (both parse modes — libxml2
  honors `--!>` too). The html5lib comparator maps the reference
  serializer's space-form foreign attribute names ("xlink href")
  to our colon form. Corpus: WHATWG 914 → 928; parity 783 → 784.



## [1.9.124] - 2026-09-09

### Added

- HTML (WHATWG mode): character-reference decoding per WHATWG
  12.2.5.72-78 (#659). Legacy references decode without the
  semicolon (longest-prefix match against the 107 legacy HTML4
  names); in attribute values a missing ';' followed by '=' or
  an alphanumeric stays literal; numeric references decode with
  or without the ';'. The html4 entry keeps its strict forms.
  html5lib corpus: WHATWG 897 → 914; Nokogiri parity 783 held.



## [1.9.123] - 2026-09-09

### Added

- HTML (WHATWG mode): "in head noscript" (scripting off, #659).
  A `<noscript>` opened in the head phase keeps head content and
  comments inside; the first body-ish token pops it and
  reprocesses at body level. `<html>` inside head-noscript merges
  its attributes onto the html element; `<head>`/nested
  `<noscript>` tokens are ignored, as are doctypes. `</br>` is
  treated as `<br>`. `<noframes>` is RAWTEXT. html5lib corpus:
  WHATWG 887 → 897; Nokogiri parity 783 held exactly.



## [1.9.122] - 2026-09-09

### Added

- HTML (WHATWG mode): frameset mode (#659). A `<frameset>`
  before any body content replaces the body (html >
  [head, frameset]); after content it is dropped; nested
  framesets and `<frame>` children work. Structural
  `<head>`/`<body>` start tags now keep their ATTRIBUTES —
  they land on the synthesized elements. html5lib corpus:
  WHATWG 859 → 887; Nokogiri parity 783 held exactly.



## [1.9.121] - 2026-09-09

### Added

- HTML (WHATWG mode): tests19 insertion-mode edges (#659).
  Heading end tags pop through the nearest heading (any h1-h6,
  not just the same name). Ruby annotations nest as siblings
  (rb closes rb; rt/rp close rt/rb/rp; rb/rt/rp, listing and
  plaintext close an open p). `<plaintext>` is raw-text-to-EOF.
  The html4/Nokogiri-compat entry pins libxml2's shapes
  unchanged. html5lib corpus: WHATWG 792 → 859 (+67); Nokogiri
  parity 783 held exactly.



## [1.9.120] - 2026-09-09

### Added

- HTML (WHATWG mode): in-table wrapper synthesis per WHATWG
  12.2.6.4 (#659). `<tr>` directly under `<table>` implies a
  `<tbody>`; `<td>`/`<th>` imply `<tbody><tr>` (just `<tr>`
  under an open tbody/thead/tfoot); `<col>` implies
  `<colgroup>`. Follow-up rows reuse the synthesized tbody. The
  html4/Nokogiri-compat entry keeps libxml2's bare shape
  (pinned by specs). html5lib corpus: WHATWG 755 → 792;
  Nokogiri parity 783 held exactly.



## [1.9.119] - 2026-09-09

### Added

- HTML (WHATWG mode): MathML/SVG foreign content per WHATWG
  12.2.6.5 (#659). svg/math roots and descendants carry the SVG /
  MathML namespace URI (readable via namespace-uri()); SVG
  element-name case adjustment (foreignObject, clipPath, ...);
  MathML definitionURL and the SVG attribute-name table (viewBox,
  ...); HTML integration points (foreignObject/desc/title,
  MathML text integration points, annotation-xml with HTML
  encoding) resume HTML rules; breakout tags pop the foreign
  scope; foreign end tags match case-adjusted names; CDATA in
  foreign content is a text node; an open <select> swallows
  foreign start tags; foreign start tags foster-parent out of
  table context. The html4/Nokogiri-compat entry stays
  plain-HTML. html5lib corpus: WHATWG 652 → 755 (+103, the
  lane's largest slice); Nokogiri parity 783 held exactly.



## [1.9.118] - 2026-09-09

### Added

- HTML: `<template>` elements are placed where the tokenizer
  encounters them (#659). html5lib corpus: WHATWG 623→652,
  Nokogiri-parity 777→783; both floors raised in the gate specs.

### Changed

- HTML: explicit `<head>`/`<body>` tags are honored in place —
  content between/around them keeps its position instead of being
  re-lifted; the implied-head lift only runs when the tags are
  absent (#659).
- The html5lib comparator now compares `<head>` subtrees for real
  on the WHATWG meter (optional-empty-head normalization is
  parity-only).



## [1.9.117] - 2026-09-09

### Added

- **#659 — structural `<head>`/`<body>` tags; every WHATWG
  document is `html>[head, body]`.** Bare head/body start tags at
  top level are structural (the tag itself disappears; the commit
  synthesis provides the real elements), and the WHATWG commit
  splices in a missing head and appends a missing body. The
  html4/libxml2 mode keeps its shape (no empty head — Nokogiri's
  `<html></html>`). The corpus comparator now compares heads for
  real on the WHATWG meter (the "optional empty head" divergence
  stays parity-only). Corpus meters: **html5lib WHATWG 556 → 623,
  Nokogiri parity 649 → 777** (+128 — libxml2 also treats bare
  head/body structurally). Full ctest 1357/1357.



## [1.9.116] - 2026-09-09

### Added

- **#659 — HTML mode records the DOCTYPE.** The HTML modes skipped
  `<!doctype ...>` entirely; `leptris_document_internal_subset`
  returned NULL for every HTML document, and both corpus gates
  failed at document level on every doctype-bearing case. The first
  doctype is now recorded like the XML path records it — name,
  legacy PUBLIC/SYSTEM ids, stray later doctypes ignored — with the
  two-mode case rule: WHATWG lowercases the name, the html4/libxml2
  mode preserves it. Corpus meters: **html5lib WHATWG 295 → 556
  (+88%), Nokogiri parity 372 → 649 (+75%)**; both floors raised.

### Fixed

- **#930 — CLI tests race under `ctest -j4`**: `run_cli` wrote
  subprocess stdout/stderr to fixed `/tmp/leptris_cli_*` paths, so
  concurrent ctest entries overwrote each other's captures
  (intermittent CliXpath/CliXquery reds, serially green). Paths are
  now pid-suffixed per process and removed after read;
  stress-verified 0/10 concurrent failures. The release also
  refreshes `TODO.xslt-full/REMAINING.md` (30 releases stale) to
  the current open set and graph.



## [1.9.115] - 2026-09-08

### Added

- **#878 RNG schema errors publish to `leptris_last_error`.**
  `leptris_rng_parse` previously returned NULL with only a status
  code — the schema error detail lived on the dying handle. Parse
  and file failures now publish to the thread-global channel (the
  same one `leptris_parse_string` uses), so bindings can raise
  with detail: schema errors carry the parse message (`unknown
  pattern element: ...`; include chains inline their href), missing
  files say `cannot open schema file '...'`, non-well-formed schema
  files say so. Groundwork for the `Leptris::XML::RelaxNG` Ruby
  binding. 2 RED-first specs; RNG suite 41/41, corpus gate 38/38.



## [1.9.114] - 2026-09-08

### Added

- **#878 `<param>` datatype facets on `<data>`.** minInclusive/
  maxInclusive/minExclusive/maxExclusive, minLength/maxLength/
  length (raw characters — XSD string preserves whitespace), and
  `pattern` with full-value anchoring. All semantics Jing-probed
  first; facets apply in element-text, `<list>` token, and
  attribute-value contexts via one shared `data_matches()`. The
  pattern engine is a new portable self-contained matcher
  (`rng/rng_regex.c`) — the XPath POSIX regex trio is compiled out
  everywhere, so the validator ships its own subset engine.
  Schema-parse validation matches Jing: unknown param names are
  rejected (`invalid parameter:` — e.g. `enumeration`, which is
  illegal in RELAX NG), and pattern constructs outside the subset
  fail loudly at parse time. RNG suite 39/39, corpus gate 38/38.



## [1.9.113] - 2026-09-08

### Added

- **#878 `<include>` support — `leptris_rng_parse_file(path, status)`.**
  The new public entry parses a schema from a file and resolves
  `<include href="...">` relative to the file's directory (the
  in-memory `leptris_rng_parse` has no base URI and rejects includes
  with a pointer to the file entry). Include semantics match Jing: a
  `<start>`/`<define>` nested inside `<include>` **replaces** the
  included grammar's matching declaration and must target something
  that exists; remaining defines merge under the usual `@combine`
  rules; only the final assembled grammar requires a `<start>`;
  cycles are depth-guarded. 7 specs over Jing-verified fixtures in
  `test/rng/include-cases/` (RNG suite now 33/33, corpus gate still
  38/38).



## [1.9.112] - 2026-09-08

### Added

- **#878 Jing conformance corpus — the RNG validator's falsifiable
  gate.** 19 Jing-verified schema/instance triples vendored in
  `test/rng/jing-cases/` (attributes, element names, choice,
  optional, oneOrMore, group/interleave order, text, data types,
  value, list, mixed, ref/recursion, combine) with reference
  verdicts recorded from Jing (`jing-ref.json`; regen steps in the
  cases README). `test_rng_corpus` is a black-box gate (public API
  only) requiring **exact** agreement — 38/38, every future
  divergence is an immediate red.

### Fixed

- **Five validator divergences the corpus exposed** (#878):
  `<text/>` and `<data type='string'>` now accept empty and
  whitespace-only content (Jing-confirmed; the blanket rejection
  was wrong — integer still rejects via its digit check);
  `<interleave>` child patterns are single-use (`<a/><a/>` no
  longer reuses the `a` pattern and drops a required `b`);
  `<list>` closing a repeat wrapper no longer swallows the current
  token (`<l>1 x</l>` is invalid again); a CHOICE at `start` (from
  combined `@combine='choice'` defines) resolves each element
  alternative instead of rejecting every document; `<attribute>`
  value/data leaves constrain the instance value. Invalid verdicts
  now always carry an error — `leptris_rng_error` is never NULL
  after a 0 return.
- html4 script spec passed 27 for a 25-byte literal (test)



## [1.9.111] - 2026-09-08

### Added

- **#878 phase 3 — validator correctness: backtracking fold,
  `list` tokens, mixed, ref guards.** The content model now folds
  with backtracking — a choice whose trivially-succeeding
  alternative (`<empty/>`) starved later patterns is retried, and
  the fold tail requires exhaustive child consumption.
  `<list>` tokenizes element text on whitespace and matches leaves
  **positionally** (RELAX NG list is a sequence — order matters)
  with repeat cardinality. Mixed content validates; recursive
  defines are depth-guarded and `<start><ref/>` chains resolve.
  4 new specs (20 in the RNG suite); full ctest 1331/1331.



## [1.9.110] - 2026-09-08

### Added

- **#878 phase 2 — RELAX NG core validator + Jing-shaped
  errors.** `leptris_rng_validate(rng, doc)` matches instance
  documents against the pattern IR (simplified core-subset
  semantics): element with **exact attribute closure** (every
  instance attribute consumed, every required one present),
  attribute/text/data(string, integer)/value leaves, choice,
  group, interleave (any-order), optional/zeroOrMore/oneOrMore,
  mixed, ref, empty. On failure `leptris_rng_error` returns the
  first violation in Jing's `line:col: error: message` shape —
  the drop-in contract for the ruby-jing migration. 8 new specs
  (16 total in the RNG suite); full ctest 1327/1327.



## [1.9.109] - 2026-09-08

### Added

- **#878 phase 1 — RELAX NG XML syntax → pattern IR.** New
  `src/leptris/rng/` subsystem (the Jing-replacement epic's
  foundation): the XML-syntax schema parses through the standard
  engine and lowers into a 20-kind pattern IR — element,
  attribute, choice, interleave, group, optional, zeroOrMore,
  oneOrMore, list, mixed, data(+param/except), value, empty,
  notAllowed, text, ref — plus the grammar layer (`<start>`
  merging, `<define>` with `@combine` on either declaration,
  bare-element roots). Public entries `leptris_rng_parse` /
  `leptris_rng_free` / `leptris_rng_error` with an opaque
  `LeptrisRelaxNG` handle (ABI-additive). 8 RED-first specs;
  full ctest 1319/1319. Phase 2 (core validator + Jing-shaped
  errors) next.



## [1.9.108] - 2026-09-08

### Fixed

- **#919 — C14N attributes sort by namespace URI, then local
  name.** Completes the #881 fix: attribute nodes now follow
  REC-xml-c14n §2.3 — namespace URI first (no-namespace = empty =
  first), then local name, with URIs resolved through the owner
  element's in-scope declarations. A prefixed attribute placed
  before bare ones in the source now reorders correctly; canonical
  bytes match the reference implementations on mixed elements. RED
  spec `AttributesSortedByUriThenLocal`; full ctest 1311/1311.



## [1.9.107] - 2026-09-08

### Added

- **#659 — adoption agency clones carry attributes** (8.2.5.4
  step 5). The reopened formatting-element clone copies the
  original's attributes: `<a href="h">1<i>2</a>3</i>` keeps the
  link on both spans. Corpus-neutral (295 holds, parity 372 held);
  spec-pinned.



## [1.9.106] - 2026-09-08

### Added

- **#659 — adoption agency (WHATWG entry only, simplified
  8.2.5.4).** A formatting element closed out of order keeps its
  scope for later content: the formatting elements open above the
  close are cloned and reopened at the new insertion point
  (`<b>1<i>2</b>3</i>` → `<b>1<i>2</i></b><i>3</i>`). Block closes
  never reopen. The html4 entry keeps libxml2's pop-away shape
  (stray-end spec migrated to `Html4()`). html5lib corpus **294 →
  295** (floor raised; **193 → 295**, +53% since the two-mode
  split); Nokogiri parity **372 held exactly**. Full ctest
  1309/1309.



## [1.9.105] - 2026-09-07

### Added

- **#659 — foster parenting (WHATWG entry only).** Text and
  non-table elements arriving with a table-context insertion point
  insert **before the table** in its parent (WHATWG 12.2.6.1):
  table-structure elements stay inside, whitespace-only text stays
  in the table, comments stay. A table at the stack bottom splices
  the top chain before the table node (during raw parse the stack
  is just `[table]`). The html4 entry keeps the libxml2 shape.
  html5lib corpus **285 → 294** (floor raised; 193 pre-split);
  Nokogiri parity **372 held exactly**. Full ctest 1308/1308.

### Fixed

- **#910 — `leptris_node_child_count` header contract.** The count
  is child **elements** (as the binding pins); the header claimed
  "all node types". Documented the real contract with pointers to
  the mixed-kind walk (`first_child`/`next_sibling`) and the batch
  API (`leptris_node_children`). Behavior unchanged.



## [1.9.104] - 2026-09-07

### Added

- **#659 — HTML two-mode split: WHATWG engine + html4 compat
  entry.** `leptris_parse_html_string` is now the WHATWG-conformant
  engine — the full "in head" set (`script`/`style`/`noscript`/
  `template`/`basefont`/`bgsound`/`noframes` in a contiguous
  leading run) lifts into the implied `<head>`. New
  `leptris_parse_html4_string` keeps the libxml2/Nokogiri
  compatibility shape (leading script/style stay in `<body>`;
  title/meta/link/base still lift). Bindings that must match
  Nokogiri byte-for-byte pin the html4 entry. Evidence: html5lib
  corpus **193 → 285** (+48%, floor raised); Nokogiri parity **372
  held exactly** (harness retargeted to the html4 entry). Full
  ctest 1307/1307. Next #659 slices: foster parenting, adoption
  agency.



## [1.9.103] - 2026-09-07

### Performance

- **#904 — iterparse yields prime the TLS last-root memo.** Each
  yielded subtree is released before the next, so the (root, doc)
  memo missed on every cold attribute read (climb + bucket walk
  per call, ~1.4 µs residual vs the document case). New
  `leptris_root_doc_memo_prime` lets a driver that knows the pair
  set it directly; the iterparse yield path calls it. Sound under
  the Round-20 register-on-create contract (v1.9.102 restored it
  fully): the yielded root is map-registered and
  `leptris_document_free` invalidates the memo.



## [1.9.102] - 2026-09-07

### Fixed

- **#905 — hotfix: reverted the v1.9.99 register-elision.** The
  elision changed reachability for detached colon-free elements
  (document resolution fell to the namebp slot read alone, where
  every element previously also carried a root-map entry). The
  `fn:snapshot` detached-deep-copy path — SEGFAULT on macOS CI /
  0 nodes on Linux CI at the 1.9.101 pin, green at 1.9.94 — is the
  reported casualty; the revert restores the Round-20
  register-on-create contract and the direct map-lookup validation
  in `document_set_root`. The ~4% dispatch win is not worth a
  crash class; the v1.9.98 namebp flag-clearing stays (it only
  pins an already-masked invariant). Full ctest 1306/1306.


## [1.9.101] - 2026-09-07

### Performance

- **#682 — eval-side Phase 2 (two levers).**
  - *AVT brace-free fast path.* `eval_avt` early-outs when the
    template has no `{` or `}` — literal attributes (heavy: 7200
    attrs/transform) skip the per-character malloc-grow loop.
  - *XPath AST-cache mutex skip.* A single in-flight transform is
    single-threaded; `xslt_transform_doc` claims ownership via a
    TLS flag so the per-eval `xpath_ast_cache_get` / `_release`
    pair (~14400 ops per heavy transform) drops the mutex
    lock/unlock entirely. State restored on `xslt_exec_free`.



## [1.9.100] - 2026-09-07

### Performance

- **#682 — streaming result emission, Phase 1 (gated).** Sheets a
  compile-time gate admits (v1.x, namespace-free literal elements,
  text/value-of, literal comment/PI, select-only variables, control
  flow) emit serialization bytes directly instead of materializing a
  result DOM — spec-safe because the XSLT data model forbids reading
  the principal result tree during execution. Everything else
  (copy, xsl:element/attribute/namespace, sequences, attr-sets,
  doctype, indent, cdata-section-elements, character maps, 3.0
  built-in rules) keeps the result-tree path byte-identically.
  Measured neutral on the dispatch benches — the eliminated
  construction cluster was smaller in wall-clock than its profile
  share; the remaining gap is the eval side, which Phase 2 targets
  on this roadbed. Run-based emitters (`strcspn` + bulk append);
  full ctest 1306/1306.



## [1.9.99] - 2026-09-07

### Added

- **#869 — on-demand subtree structural digest.**
  `leptris_node_digest(node, flags)`: a content-defined Merkle hash
  of a subtree — element prefix/resolved-URI/local name, attributes
  as sorted, first-wins-deduplicated `(URI, local, value)` triples,
  children Merkle-combined in document order; text/CDATA/comment/PI
  hash content. Stable across processes (no pointers participate);
  equality implies subtree equivalence under the flag semantics.
  `LEPTRIS_DIGEST_DROP_WS_TEXT` skips whitespace-only text nodes.
  Pure on-demand C walk — zero cost on parse/serialize/eval paths.
  Comparator consumers (lutaml/canon) can gate descent on it and
  skip identical subtrees entirely. 11 RED-first specs; ctest
  1306/1306.

### Performance

- **#682 — register-elision for colon-free created elements.**
  The Round-21 namebp backpointer resolves detached elements
  without the root map (the v1.9.98 hardening made the boundary
  exact); `leptris_element_create` now registers only prefixed
  names, and `document_set_root` validates through the unified
  resolver. Heavy dispatch: 3.77–3.95 → 3.59–3.69 ms (best-of-9
  A/B). Ctest 1294/1294.



## [1.9.98] - 2026-09-07

### Fixed

- **namebp backpointer validity after QName split.** The Round-21
  mutation backpointer (document pointer stored ahead of the name)
  is only valid while `name` points at the carve slot; for prefixed
  names, the #846 QName split advances `name` past the colon and the
  flag must drop with it — otherwise an unattached `get_document`
  with a root-map miss would read name bytes as a document pointer.
  Unreachable today (register-on-create answers first); now pinned
  by spec so future register-elision cannot trip it.



## [1.9.97] - 2026-09-06

### Fixed

- **#875 — CRITICAL: dispatch index overflow silently dropped
  templates.** The v1.9.88/#866 index builders capped insertion
  (name keys 48, literal `@attr='value'` keys 96 per mode bucket)
  with a bare `continue` — a pattern past the cap landed in no
  dispatch list, so its template never fired: zero output, no
  error. Stylesheets with more than ~48 distinct literal element
  names or ~96 literal attr-value patterns were affected (the
  Ruby and Python bindings pinned 1.9.92 as a workaround).
  Overflowed patterns now route to the mode bucket's scanned
  remainder list, which dispatch already evaluates — correct at
  any pattern count, identical behavior under the caps. RED spec
  pins both shapes at 120 templates. Full ctest 1294/1294.



## [1.9.96] - 2026-09-06

### Performance

- **#682 — ns-fixup walks skip zero-declaration result trees.**
  `result_ns_in_scope` and the default-namespace unbind walk climbed
  every result ancestor on every literal/copy element; namespace-free
  outputs (the common dispatch shape) always found nothing. Both
  walks are now gated on the result document's `has_namespaces`
  flag — the exact any-declaration gate the resolver already
  consults — so the climbs are skipped outright when no declaration
  can exist. Heavy dispatch bench (best of 9): 3.83 → 3.57 ms,
  bringing the consolidation arc to 4.96 → 3.57 ms (−28%) across
  v1.9.95–96. Full ctest 1293/1293.



## [1.9.95] - 2026-09-06

### Added

- **#882 — `expand_empty` serializer option + element-level ext
  entries.** `LeptrisSerializeExtOptions` grows `expand_empty` (the
  inverse of libxml2's `XML_SAVE_NO_EMPTY`): empty elements emit
  `<a></a>` instead of `<a/>` on the XML method, read through the
  size-aware bridge so older FFI allocations are unaffected. New
  public `leptris_element_serialize_ext`/`_sized` twins give
  bindings the same option surface at element level, letting moxml
  drop its full-output Ruby regex rewrite per serialize. Defaults
  unchanged.

### Fixed

- **#881 — C14N namespace declarations now sort by prefix.**
  REC-xml-c14n section 2.2 orders namespace nodes lexicographically
  by prefix with the default namespace first; the serializer emitted
  document order, so canonical bytes diverged from libxml2/Java and
  XML signatures did not interop. Declarations are collected and
  sorted like the existing attribute path; emission order changes
  only.
- Dropped a dead local in the serializer walker (surfaced by a
  forced recompile; keeps the build warning-clean).

### Performance

- **#682 — TLS free-list consolidation + walk-first attr dup check.**
  The four xpath thread-local free-list variables collapse into one
  TLS struct (each separate `__thread` variable paid its own
  `tlv_get_addr` thunk per access), and `leptris_element_set_attribute`
  walks the hash-prefiltered attribute list for elements with ≤ 8
  attrs instead of registering + probing the doc-level attr index —
  XSLT result elements carry 1–3 attrs, so the index was pure
  overhead on short-lived result trees. Elements that grow past the
  threshold still lazily bulk-register (O(1) programmatic-build
  contract preserved). Dispatch benches (best of 9): heavy
  4.96 → 3.83 ms, light 3.83 → 3.57 ms. Full ctest 1293/1293.



## [1.9.94] - 2026-09-06

### Performance

- **TLS consolidation, lever 1 — last-root memo in
  `leptris_element_get_document`.** The hot path climbed to the root
  and then walked a thread-local bucket chain on every call; a
  thread-local (root, doc) memo now answers consecutive same-tree
  queries with a single pointer compare inside the climb, and the
  map lookup only runs on a miss. `leptris_document_free`
  invalidates the memo so a freed tree's address cannot be reused
  against a stale hit. XSLT dispatch benches (best of 9): light
  3.89→3.62–3.78ms, heavy 4.92→4.78–4.85ms. Full ctest 1288/1288.



## [1.9.93] - 2026-09-05

### Changed

- **#866 — predicate-pattern dispatch indexes.** Template-heavy
  stylesheets whose patterns carry predicates (the common
  real-world shape, `match="item[@k='N']"`) still paid the full
  candidate ladder after the v1.9.88 indexes: 82ms on a
  120-template fixture vs libxml2/libxslt's 58ms. Two new
  semantics-preserving indexes: **name keys** (every alternative
  that names an element links its template under that name — a
  `foo` element never evaluates `item[...]` candidates; node-type
  and function-call patterns stay in the any-element remainder)
  and a **literal value index** (an alternative that is exactly
  `name[@attr='value']` joins a `(name, attr, value)` bucket,
  probed once per element attribute — the predicates stop running
  entirely). Numbers: the fixture 82.2 → 2.06 ms (**40x; ~28x
  ahead of libxslt**); heterogeneous heavy dispatch 5.56 → 4.92
  ms; the 2000-book scorecard 8.97 → **3.93 ms**. Fixture gated as
  `bench_xslt_dispatch_pred`.


### Fixed

- use LEPTRIS_THREAD_LOCAL for the dispatch generation counter (MSVC)

### Performance

- #866 - dispatch name keys + literal @attr='value' index (xslt)



## [1.9.92] - 2026-09-05

### Fixed

- **#848 follow-up — the append-tail cache thrash.** Entity-laden
  HTML at scale (1.3MB, 140k entities) parsed 6x BEHIND libxml2
  (291ms vs 48ms) and superlinearly — but the entity table was not
  the cost (the #848 binary search holds). The 8-slot direct-mapped
  append-tail cache thrashed on the HTML shape, which alternates a
  persistent parent (<body>) with a fresh parent each element pair:
  the body entry was evicted ~1/8 pairs and every miss walked the
  whole child chain — O(n²) over 40k nodes. 64 slots: 291ms →
  38-44ms, now at par to 1.15x AHEAD of libxml2 on the same
  fixture. No sub-1x HTML parse shape remains (general pages: 2.9x
  ahead). The entity-laden shape is gated in bench_html_parse for
  both engines.


### Performance

- kill the append-tail cache thrash - entity-heavy HTML 6.8x (dom)



## [1.9.91] - 2026-09-05

### Added

- **#659 — Nokogiri parity reference.** `nokogiri-tree-tests.dat`
  records what `Nokogiri::HTML` produces for all 1555 runnable
  html5lib corpus cases (same `|` dialect the harness parses;
  committed, CI needs no ruby). The harness now meters BOTH goals:
  WHATWG conformance (193/1555) and libxml2 parity — **372/1555
  exact tree matches**, 1183-entry parity red-list, both floors
  pinned and falsifiable. Direction recorded on #659: the target
  is WHATWG conformance with HTML4 and HTML5 as distinct modes.


### Added

- #659 - Nokogiri parity reference: 372/1555 exact matches (html)

### Performance

- #659 - HTML parse throughput vs libxml2 (head-to-head) (bench)



## [1.9.90] - 2026-09-05

### Added

- **#659 — html5lib tree-construction corpus harness.** The
  html5lib-tests snapshot is vendored at Nokogiri's pin (c67f90e —
  the last upstream revision carrying tree-construction/; upstream
  moved those tests to WPT). The harness runs every non-fragment
  case through `leptris_parse_html_string` and compares against
  the expected `|`-tree: 1753 cases, 193 passing, a 1369-entry
  red-list snapshot (`test/html/html5lib-redlist.txt`), and a
  falsifiable pass-count floor. The red-list shapes the next
  slices: the implied-head / foster-parenting model is the biggest
  bucket (865), not adoption agency. MSVC-safe (CMake-generated
  corpus file list).

### Fixed

- **#659 — empty-shape HTML inputs.** Inputs that append nothing
  (a stray end tag alone, doctype-only, a second doctype, an empty
  string) hard-failed; in lenient HTML mode nothing-parsed is not
  an error — the wrapper synthesis yields the empty Nokogiri
  document shape.


### Added

- #659 - html5lib tree-construction corpus harness (slice 1) (html)

### Fixed

- #659 - empty-shape inputs are documents; harness portable to MSVC (html)



## [1.9.89] - 2026-09-05

### Fixed

- **#857 — fn:analyze-string group spans.** POSIX `pmatch` offsets
  are relative to the subject string; they were applied from the
  match start, so any match beginning at a non-zero offset read
  past its end — single-group regexes leaked the following
  non-match into the match and group values (`('ab12cd',
  '([0-9]+)')` gave `12cd`/`cd` instead of `12`/`12`). Additionally
  `regexec` was asked for 12 submatch slots regardless of the
  pattern's group count — BSD leaves the excess untouched, so stale
  stack data from a previous evaluate-string call read as phantom
  groups, making results call-order dependent. Both fixed; specs
  cover single-group, multi-group-after-single-group (the
  contamination order from the report), and group-less regexes.


### Fixed

- #857 - analyze-string group spans are subject-relative (xpath)



## [1.9.88] - 2026-09-05

### Changed

- **#682 — template dispatch indexes.** Template-heavy stylesheets
  paid O(templates) per named `call-template` and per
  `apply-templates` dispatch, plus a document climb and pattern
  ladder per candidate. Three semantics-preserving indexes now
  cover the dominant shapes: an open-addressed named-template hash
  (a hit is the last declaration — the same §11.6 winner the
  linear scan produced), per-mode candidate buckets in sheet order,
  and a bare-Name fast path (a lone unprefixed Name pattern is
  `child::NAME`: plain name + no-namespace check). Template-heavy
  dispatch fixture (120 templates / 2400 elements): 10.21 → 5.56 ms
  per transform; the gap to in-process lxml/libxslt narrows 3.27x →
  1.78x, and the 2000-book dispatch scorecard improves ~15%.

### Performance

- #682 - dispatch indexes: named hash, mode buckets, bare-name fast path (xslt)
- #682 phase A - template-heavy dispatch fixture (bench)



## [1.9.87] - 2026-09-05

### Fixed

- **#846 — namespace-aware constructed elements.** Constructed
  elements (analyze-string results, xsl:element, copies) kept the
  full lexical QName in `name` with no prefix, while every
  namespace-aware name test compares the name against the test's
  LOCAL part — so a prefixed test over `fn:analyze-string` output
  could never match, and `namespace-uri()` was empty. QNames now
  split at both creation paths (`leptris_elem_split_qname`,
  parser-shape parity: prefix + local name + local hash/len);
  `fn:analyze-string` declares `xmlns:fn` on the result root, so
  `/fn:match` and `/fn:match/fn:group` select correctly under a
  bound `fn` prefix and `name()` renders `fn:match`. Ripples:
  `xsl:element`'s declaration hoisting reads the element prefix
  instead of re-parsing the name; the HTML serializer applies
  void/raw/break rules only to unprefixed names (a namespaced
  `s:img` keeps its close tag — bug-117 parity); adding a
  namespace declaration sets the document's `has_namespaces` flag.



## [1.9.86] - 2026-09-05

### Fixed

- #848 entity lookup via sorted index + binary search (was O(2032) scan per reference) (html)



## [1.9.85] - 2026-09-04
### Fixed

- **#659 HTML: characterization specs** for the two remaining
  tree-builder behaviors, probed against Nokogiri/libxml2:
  `<template>` is an ordinary element with children in place
  (libxml2 predates the WHATWG inert-fragment model), and
  misnesting closes the formatting element at the outer end tag
  with the stray end tag dropped. Both already matched; they are
  now locked in as gate specs so the html5lib-corpus work (which
  uses the different adoption-agency shape) cannot silently
  regress Nokogiri parity.


## [1.9.84] - 2026-09-04
### Added

- **#659 HTML: head-content placement lift** — a contiguous leading
  run of `title`/`meta`/`link`/`base` elements moves into a
  synthesized `<head>` placed before `<body>` (the libxml2/Nokogiri
  shape). Once body content starts nothing lifts, and no empty
  `<head>` is ever synthesized.


## [1.9.83] - 2026-09-04
### Added

- **#659 HTML: PI-ish bogus constructs** — in HTML input,
  `<?target data?>` is kept as a PI node with the libxml2/Nokogiri
  shape (data includes the trailing `?`, leading whitespace
  trimmed). `<!...>` non-comment constructs (DOCTYPE, `<![CDATA[`)
  were verified against the same ground truth: dropping to the
  first `>` without perturbing text is already correct.


## [1.9.82] - 2026-09-04
### Added

- **#691 `fn:analyze-string`** — the last function in the catalog.
  The `fn:match` / `fn:non-match` element model (`fn:group` children
  carry `@nr`) is built on a fresh document through the v1.9.81
  document-lifetime anchor chain: regex iteration reuses the
  `fn:matches` POSIX flag translation, non-match gaps between
  matches, match-relative group offsets, trailing non-match,
  zero-length-match advance, and an overall string value equal to
  the input. Registered under `LEPTRIS_HAVE_POSIX_RE` like the
  regex trio.


## [1.9.81] - 2026-09-04
### Added

- **#691 `fn:snapshot`** and the **document-lifetime anchor chain**
  it requires: `leptris_document` grows an anchored-docs array
  released by `leptris_document_free`, so constructed-node documents
  can anchor on the SOURCE tree. This is the owner that outlives a
  plain `leptris_xpath_eval` result (the evaluation context's
  owned-doc chain is destroyed at eval return — documented after a
  heap-use-after-free investigation). `fn:snapshot` deep-copies each
  input element onto a fresh anchored document and returns the
  copies; `fn:analyze-string` lands on the same chain.


## [1.9.80] - 2026-09-04
### Added

- **#691 `fn:format-number` as a plain XPath function**: the
  JDK1.1 pattern grammar + digit formatting moved verbatim from the
  XSLT layer to a shared `common/format_number` core (SSOT). The
  XSLT layer keeps the `xsl:decimal-format` lookup and maps it into
  a spec; plain XPath evaluates under the default decimal format
  (named decimal formats require the XSLT context).

### Fixed

- **Function registry is now last-registration-wins**: registering
  a same-named function at the standard layer previously appended a
  duplicate and the FIRST entry kept winning, silently shadowing
  the XSLT bridge's decimal-format-aware `format-number` inside
  transforms (multi-byte grouping separators degraded to pattern
  suffix text). Overriding layers register through the new
  `xpath_function_registry_register_ud`, which carries the
  per-function `user_data` closure — a plain re-register resets it
  (a NULL closure in a bridge handler is a guaranteed crash).


## [1.9.79] - 2026-09-04
### Added

- **#691 `fn:random-number-generator`**: seeded calls are
  deterministic (FNV-1a of the seed feeding xorshift64*); the result
  is the standard map carrier with `number` in [0,1), so `?number`
  and `map:get(...)` both work. `next` / `permute` need function-item
  closure state — documented gap.

### Performance

- **#682 one-pass template selection**: `xslt_select_template` and
  `xslt_select_next_match` previously evaluated every pattern
  alternative twice per dispatch candidate (any-match test, then the
  per-alternative priority loop); both now fold to a single walk.
  Semantics identical; the unused wrapper is deleted. On the
  4-template scorecard the effect sits inside the run-to-run noise
  band — the win scales with alternative and template counts.



## [1.9.78] - 2026-09-04

### Added

- **#691 catalog tail, sequence/doc slice**: `fn:innermost` /
  `fn:outermost` (ancestor/descendant-in-set filters), `fn:has-children`
  (explicit or zero-arg context form; text children count), `fn:path`
  (root-anchored `/a/b[2]` positional form, predicates only where
  same-name siblings repeat), `fn:nilled` (xsi:nil read),
  `fn:base-uri` / `fn:document-uri` / `fn:static-base-uri` (empty
  for in-memory documents), `fn:doc-available`, `fn:json-doc`.
- **#691 catalog tail, scalar slice**: `fn:compare` (empty operand
  yields the empty sequence), `fn:codepoint-equal`,
  `fn:normalize-unicode` (utf8proc NFC/NFD/NFKC/NFKD),
  `fn:round($x, $precision)` (negative precision rounds to tens),
  `fn:resolve-QName` (prefix resolved through the element's in-scope
  namespaces), `fn:environment-variable`,
  `fn:available-environment-variables` (POSIX and Windows).
- **#691 date tail**: `year/month/day-from-date`,
  `hours/minutes/seconds-from-dateTime`,
  `minutes/seconds-from-duration`; duration parsing is now a full
  ISO 8601 walker (P[nY][nM][nD][T[nH][nM][nS]], negatable — the old
  parser only understood days-hours forms);
  `xs:dayTimeDuration` / `xs:yearMonthDuration` constructor aliases.
- **#691 unparsed-text family**: `fn:unparsed-text`,
  `fn:unparsed-text-lines`, `fn:unparsed-text-available` (raw file
  reads), `fn:uri-collection` (empty catalog).
- **#692 fn-as-path-step**: function calls in path-step position
  (`//item/string()`) parse and evaluate — desugared to the simple
  map, so `a/f()` ≡ `a ! f()`; node-test keywords (text/comment/
  node) are unaffected, and a function step after an absolute path
  keeps predicates (`//a[1]/string()` selects the first node only).
- **#683 gate**: `XPath20Ledger.XPath31ThroughStandardEntry` locks
  the 3.x grammar (let / if / for / `!` / ranges / switch / map and
  array lookups) behind the STANDARD `leptris_xpath_eval` and
  `leptris_xpath_compiled_eval` entries — verified by public-API
  probe, closed as satisfied.

### Fixed

- `parse-xml-fragment` leaked its strdup'd argument string on every
  call (Linux LSan CI; freed after the wrapper copy).
- Test portability: no `/tmp` paths (CWD-relative) and `_putenv` on
  MSVC; two comments rephrased to clear -Wcomment.

## [1.9.76] - 2026-09-03

### Added

- **`leptris_element_set_namespace`** (#817): the setter
  counterpart of `leptris_element_namespace` (Nokogiri
  `node.namespace=` parity) — NULL detaches (prefix + link clear,
  an `xmlns=""` undeclaration blocks in-scope defaults), a URI
  rebinds to its in-scope declaration. `set_name` now keeps the
  prefix cache coherent (unqualified renames no longer resurrect
  the old prefix at serialization), and `xmlns=""` resolves to
  no-namespace (NULL) in the getter.
- **XQuery conformance tail** (#684 audit): empty computed
  constructors (`element/attribute/text/document NAME? { }`),
  the `xquery version` declaration (+encoding), and splice ctor
  rewriting — expression text stays verbatim while direct
  constructors inside it (function arguments, nested braces)
  translate to their computed forms. Also accepts
  `xsl:global-context-item` (#690 audit; a no-op by
  construction).

### Fixed

- **#815 (critical): intermittent verbatim text.** The parser's
  text-node carve skipped one initialization — `base.raw` on a
  dirty recycled pool page landed set on ~5-7% of parses under
  allocation churn, and the serializer then emitted that node's
  text VERBATIM (raw `<`, invalid output). Reproduced with the
  reporter's harness (3/100); 0/100 after the one-line fix.
- **#812: detached copier dropped descendant-scoped xmlns
  declarations** (v1.9.74 regression): the attach path re-resolved
  the pool through the document chain, which fails on the
  unregistered mid-construction copy — declarations now link
  through the threaded pool. Copy perf holds (0.042 ms per
  100-book subtree).
- **#813: HTML parse Nokogiri parity** — minimized attributes
  carry the empty string (`checked=""`), no empty `<head/>` in
  the synthesized wrapper, header docs match behavior.
- **#814: where-filtered FLWOR items** in function-argument
  position became empty strings — an empty sequence now
  contributes nothing (Saxon semantics); `''` results still do.



## [1.9.75] - 2026-09-03

### Added

- **HTML parsing mode** (#659, lane 14 slice 1 — the last
  Nokogiri capability gap): `leptris_parse_html_string` —
  tolerant HTML4/5 parse into the STANDARD DOM (same nodes, pool,
  serializer, and XPath/XSLT as XML; no parallel tree). Implied
  end tags (p/li/dt/dd/td/th/tr/thead/tbody/tfoot/option), void
  elements, raw-text `<script>`/`<style>`, case-insensitive names
  (stored lowercased), minimized + unquoted attributes, the full
  WHATWG named-entity set (2032 single-codepoint references) plus
  numeric references in text and attribute values, stray-end-tag
  pop-until-matched, EOF closing, and a never-failing tokenizer.
  Nokogiri document shape: an input without `<html>` gets
  `<html><head/><body>…</body></html>` synthesized; explicit
  `<html>` is honored; no implied `<tbody>` (libxml2 parity).
  13 new specs in `test/html/` (all watched RED first), including
  XPath-queryability of parsed HTML documents.


## [1.9.74] - 2026-09-03

### Performance

- **Pool-threaded detached deep copy — subtree duplicate 2.3×
  Nokogiri.** `leptris_element_copy` (the Ruby binding's
  `Element#dup`) ran through `leptris_element_append_copy`: every
  copied element paid a root-map registration (a malloc — needed
  only so detached children could resolve their document before
  linking), a parent-chain pool resolution, and public append
  dispatch per child; the entry itself built under a throwaway
  `__copy_root__` element and unlinked it after. The new
  self-contained recursive copier threads the destination pool
  through the walk (direct sibling/child linking, pooled string
  copies, ONE root-map registration for the copy root) with
  unchanged fidelity — elements/text/cdata/comment/PI children
  (#696), attributes, namespace declarations (#721). 100-book
  subtree: 0.111 → 0.048 ms (2.3×); Ruby `dup` vs Nokogiri 1.19.4:
  0.7× behind → **2.27× ahead** (57.5 vs 130.8 µs). Also drops a
  duplicated `root_doc_register` call in `append_copy`.

### Fixed

- Namespace declarations in the detached copier are pool-owned:
  the public mutator's heap fallback fired mid-recursion (the copy
  root registers only after the walk) and leaked 32 B per
  namespaced copy — caught by the Linux ASAN leg.




## [1.9.73] - 2026-09-03

### Added

- **XPath 2.0 ledger** (#684 dependency list — XQuery 1.0 embeds
  XPath 2.0, so these were the conformance blockers): quantified
  expressions `some`/`every $v in E (, $w in F) satisfies T`
  (cartesian bindings, short-circuiting, vacuous `every` on empty
  domains); node comparisons `is`, `<<`, `>>` on the first node of
  each operand with document order from the shared rank table; set
  algebra `intersect`/`except` binding tighter than `|`; the
  empty-sequence literal `()` (also repairs the latent
  where-desugar else-arm evaluating to an eval failure); and
  `fn:ends-with` plus `fn:deep-equal` (structural node comparison
  — kind, name, unordered attribute set, deep content, numeric
  members via the type marker). New ops route through the AST
  interpreter (VM fast paths can follow). 6 new `XPath20Ledger`
  specs, all watched RED first.


## [1.9.72] - 2026-09-03

### Performance

- **Fragment-output tail caches + `value-of select="."` fast path**
  (lane 13 second slice, #682). Fragment-level output (no pending
  parent) chained after the result root's sibling tail or onto the
  pre-root fragment list by walking to the tail on EVERY append —
  O(N) per node and O(N²) per transform. 50k top-level
  `xsl:value-of` texts took 1424 ms; both chains now append at a
  cached tail (the root-sibling entry self-validates via
  `next_sibling == NULL`; result chains are append-only) — 10.6 ms,
  a 135x win. `value-of select="."`, the most common select in
  stylesheets, is flagged at parse time and emits the context
  node's string-value directly, skipping the eval machinery's
  nodeset + result wrapper per call. Dispatch fixture: 11.17 →
  10.56 ms (lxml reference 4.69 ms). New specs pin the deep
  string-value semantics of the fast path and exact fragment order
  across text/comments/top-level elements.


## [1.9.71] - 2026-09-03

### Performance

- **AVT compile cache + keyed mutation tail cache** (lane 13 first
  slice, #682). Two root causes dominated the dispatch-transform
  profile. First, `eval_avt` recompiled every `{expr}` part on
  every evaluation — a literal result element's `{@id}` compiled
  2000× per transform; compiled handles are now cached on the
  transform keyed by expression text. Second, elements carry no
  last-child edge, so appends walked the child chain unless the
  document's single-slot mutation tail cache hit — and result-tree
  construction interleaves parents (`<b>`→`<out>`, then
  `<t>`/`<a>`→`<b>`, then the next `<b>`), thrashing the slot into
  an O(N) walk for every other top-level append (~2M sibling hops
  on a 2000-book fixture). The cache is now a direct-mapped 8-slot
  array keyed by element address; stale entries still fall back to
  the walk via the parent back-pointer check. Dispatch fixture:
  12.83 → 11.17 ms (lxml 4.69 ms); the sibling walk disappears
  from the profile. Also drops two dead comparators that had
  regressed warning-clean. New spec pins strict document order
  across interleaved-parent appends.


## [1.9.70] - 2026-09-03

### Added

- **XQuery typeswitch, error-code model, `collection()`**
  (TODO.xslt-full/12 remainder, #692/#684). `typeswitch (E) case T
  return R ... default return RD` dispatches through the per-member
  SequenceType classifier extracted from `instance of` — one
  type-test authority shared by both. `XPathContext.error_code`
  carries the failing operation's QName local part (`fn:doc`/
  `collection()` set `FODC0002`, cast-lexical failures `FORG0001`);
  named `catch err:CODE` clauses match it, `$err:code` binds the
  code, both channels clear on catch. `collection()` errors "No
  default collection has been defined" (Saxon parity) — catchable.
  Two drive-by root-cause fixes the typeswitch bisect exposed: the
  FOR evaluator's scalar-domain branch now marks numeric members
  (`\x03N`), and `xpath_nodeset_deep_copy` copies raw content —
  its `get_node_text` round-trip stripped the numeric marker, so
  `$v instance of xs:integer` was false through any let/var
  deep-copy path. Specs `Typeswitch`/`NamedCatchWithErrorCode`/
  `CollectionRequiresCatalog`; Saxon-HE 12.7 oracle.



## [1.9.69] - 2026-09-03

### Added

- **XQuery tumbling/sliding windows** (TODO.xslt-full/12, #684):
  `for tumbling|sliding window $w in D start $s [at $sp] [when W]
  [end $e [at $ep] [when W2]] return R` — windows enumerate over
  the domain; each binds `$w` to the member list plus the boundary
  vars; tumbling resumes after the end, sliding after the start.
  The tuple snapshot carries `$w`'s whole member list (the
  group-by machinery reused). XPath 2.0 value-comparison keywords
  (`eq`/`ne`/`lt`/`le`/`gt`/`ge`) join the relational parser —
  window end-conditions lean on them; recognized only as bare
  NCNames at operand boundaries so element names never match.
  Spec `XQueryCore.TumblingWindow`/`SlidingWindow`; Saxon-HE 12.7
  oracle.

### Fixed

- `xq_unbind_all` also unbinds window names — the phase-2 rebind
  overwrote window bindings without freeing (2184B/query, Linux
  LSan only). Window member lists borrow the domain's nodes —
  claiming ownership had freed them mid-iteration.



## [1.9.68] - 2026-09-03

### Fixed

- **XQuery grammar gaps from first binding contact** (#790): (1)
  a direct constructor as the FLWOR return clause now parses — the
  translator dropped the comma between a text run and a nested
  element, and whitespace-only text runs (the space before `<v>`)
  became text nodes instead of stripping per XQuery's default
  boundary-space handling; (2) `where`/`at` clauses work inside a
  function-argument FLWOR — the XPath-level `for` expression
  gained optional `at $pos` (position rides the FOR node's value
  as `var \x01 pos`) and optional `where` (desugared to
  `if (W, R, ())`); (3) `cast as` to numeric targets validates
  string lexicals — `'nope' cast as xs:integer` is a dynamic error
  (Saxon parity), so `try/catch` participates instead of quietly
  NaN-ing. Spec `XQueryCore.Issue790GrammarGaps`; Saxon-HE 12.7
  oracle.



## [1.9.67] - 2026-09-03

### Added

- **XQuery `group by`** (TODO.xslt-full/12, #684): `group by $k :=
  Expr` partitions the tuple stream on the key value in
  first-appearance order; every clause variable is rebound to the
  group's member list (nodes preserved — paths and `count()`
  navigate the whole group, per the Saxon oracle) and the group
  variable carries the key. Order keys moved from enumeration to
  the eval phase, so `group by` + `order by count($b) descending`
  composes the way Saxon does. Structurally, `XqTuple` generalized
  to per-variable member lists (single-member for plain FLWOR,
  aggregated for groups); `xq_rebind` joins them as one nodeset —
  `xpath_nodeset_free`'s per-kind dispatch makes mixed
  document-node/synthetic groups safe. Spec `XQueryCore.GroupBy`;
  Saxon-HE 12.7 oracle.

### Fixed

- Unbind before the phase-2 rebind: `xpath_variable_set_nodeset`
  overwrites without freeing, so the key-loop bindings leaked one
  168-byte nodeset per query — invisible to macOS ASAN (no LSan),
  caught by the Linux CI leg.



## [1.9.66] - 2026-09-03

### Added

- **Positional `for`, `document {}`, real `try/catch`**
  (TODO.xslt-full/11 slice C + first 12 piece; #684, #692).
  `for $x at $i in ...` binds the 1-based position through the
  tuple snapshot/rebind cycle. `document { content }` serializes
  its children with no wrapper tag (Saxon; `value {}` stays out —
  Saxon-HE rejects it too). `try { E } catch TEST { E }+` — the
  #692 silent-wrong case (try/catch used to evaluate to empty
  with no error) — now runs the first matching catch: `catch *`
  catches everything with `$err:code`/`$err:description`/`$err:value`
  bound from the context diagnostic and the error channel cleared;
  named tests never match (no error-code model yet — the error
  propagates, pinned by spec). XPath 3.0 defines try/catch, so the
  XSLT-side rejection pin flipped to acceptance. En route: the
  tuple-snapshot arrays size 2n (var + pos var) — sized n, ASAN
  caught an 8-byte overflow macOS ctest alone would have missed.
  Specs: `XQueryCore` (14); Saxon-HE 12.7 oracle.



## [1.9.65] - 2026-09-03

### Added

- **XQuery constructors, `fn:doc()`, CLI `xquery`** (TODO.xslt-full/11
  slice B, #684). Computed constructors (`element NAME {}`,
  `attribute NAME {}`, `text {}`) are new XPath ops — value-level:
  the result is the serialized XML string (attributes escape
  `& < "`, `text{}` escapes `& <`, nested constructors concatenate,
  empty elements self-close per Saxon). Direct constructors
  (`<out total="{expr}">{expr}</out>`) translate to the computed
  form in the XQuery scanner — textual balanced-tag scans;
  attribute value templates become `concat` pieces. `fn:doc()`
  returns the parsed file's root, with the document anchored on
  the eval context (`XPathContext.owned_docs`, freed by cleanup)
  so the borrowed root outlives the result. New CLI command
  `leptris xquery [-s FILE] (-q FILE | -e EXPR)`. Specs:
  `XQueryCore` (11) + `CliXquery` (macOS/Linux; the Windows
  shell-harness leg is tracked in TODO 11 — the library surface is
  green there). Saxon-HE 12.7 oracle.

### Fixed

- The xquery CLI command struct is static — `cli_registry_free`
  documents commands as static or separately managed; the malloc'd
  48-byte struct leaked at exit (macOS leaks leg) and aborted every
  CLI test under Linux ASAN's `abort_on_error`.



## [1.9.64] - 2026-09-03

### Added

- **XQuery 1.0 core** (TODO.xslt-full/11 slice A, #684): a new
  `src/leptris/xquery/` orchestration layer over the XPath engine —
  no second evaluator. The prolog binds into the evaluation context
  (`declare variable`, `declare namespace`, `declare function
  local:*(...)` via the function-item closure seam; unsupported
  declarations error explicitly), and FLWOR runs as a C-driven tuple
  stream: nested `for` domains with the same bind/remove discipline
  as `XPATH_OP_FOR`, `let` per enclosing tuple, `where`, stable
  multi-key `order by` (asc/desc, numeric when both keys parse as
  numbers), `return` as the synthetic-text sequence. Public API
  `leptris_xquery_parse` / `_eval` / `_free`
  (`src/include/leptris/xquery/xquery.h`); results reuse the XPath
  result model. Scanner: nestable XQuery `(: :)` comments, QName
  variable/function names (`$p:weight`, `local:twice`). Specs:
  `test_xquery` (8 cases), Saxon-HE 12.7 `net.sf.saxon.Query`
  oracle.

### Fixed

- The CLI printed the raw `\x03N` numeric-member marker for
  range/sequence results — `leptris_xpath_result_node_value` now
  strips it at the public boundary (a v1.9.61 regression), and the
  CLI's `LEPTRIS_XPATH_FUNCTION` switch gaps are closed.
- XQuery eval frees its scratch variable set (`xpath_context_cleanup`
  treats it as caller-borrowed) — caught by Linux LSan, invisible to
  macOS ASAN; namespace-mapping entries allocate through
  `LEPTRIS_ALLOC` to match the cleanup channel.



## [1.9.63] - 2026-09-03

### Added

- **`LEPTRIS_XPATH_FUNCTION` + HOF pairs/apply/array combiners**
  (TODO.xslt-full/07 lane tail, #692-B). The public result-type
  enum gains a function-item value (appended — existing ABI numbers
  hold); the type accessor classifies the one-member synthetic
  carrier (`\x03FN` closure / `\x03FR` named reference) at the
  boundary, gated on the synthetic-text tag. New combiners through
  the `xpath_call_function_item` seam: `fn:for-each-pair` (zip,
  shorter input wins), `fn:apply($f, array)` (positional members as
  the argument list), `map:for-each` (entry order), and
  `array:for-each/filter/fold-left/fold-right`. Binding mirrors:
  leptris-ruby#127 and leptris-py#71 (explicit XPathError when a
  function item would cross the FFI boundary). Lane 07 is CLOSED.



## [1.9.62] - 2026-09-03

### Added

- **`leptris_str_has_nonstandard_entity(s, len)`** (#745): a
  one-pass C predicate — does the buffer contain a named entity
  reference that is not one of the five predefined XML entities
  (nor numeric)? Downstream adapters (moxl) currently settle this
  with a whole-buffer Ruby regex scan measured at +61% parse
  overhead on a 58 KB document; the C check makes it ~+2%. A bare
  `&` without a terminator is not an entity reference and stays
  the parser's business. Spec
  `PublicSurface.StrHasNonstandardEntity`.



## [1.9.61] - 2026-09-03

### Fixed

- **xs: lexical casts + instance-of node kinds/cardinality**
  (#739, #744). `xs:boolean('0')`/`xs:boolean('false')` now cast to
  false — a string argument takes the XSD lexical forms
  `true/1/false/0` (optional whitespace); anything else is a
  dynamic error (Saxon FORG0001 parity). `xs:integer`/`xs:double`
  validate string lexicals whole: `' 42 '` → 42, `' -3.9 '` as an
  integer errors instead of returning a quiet NaN. In the
  SequenceType family, `'12' cast as xs:integer + 1` no longer
  fails compilation (occurrence indicators are taken only when the
  next token cannot begin an expression; `?` is accepted;
  `text()`/`comment()`/`processing-instruction()` are valid type
  names; trailing `+ - *` folds onto the cast result), and
  `instance of` checks every member — node kinds from the unified
  tag space, occurrence-indicator cardinality, and numeric members
  distinguished by a `\x03N` marker the sequence/range builders add
  and `get_node_text` strips (`\x03` cannot occur in XML 1.0
  text). Specs `InstanceOfNodeKindsAndCardinality`,
  `CastAsNumericInExpressions`, `XsLexicalCastRules`; Saxon-HE 12.7
  oracle.



## [1.9.60] - 2026-09-02

### Added

- **Function-item metadata + HOF combiners + partial application**
  (TODO.xslt-full/07 slice B): `fn:function-lookup(name, arity)`
  returns the named function as an item (fn: prefix tolerated,
  arity validated against the registry) or the empty sequence;
  `fn:function-name` gives `fn:local-name` for named references and
  the empty sequence for anonymous closures; `fn:function-arity`
  reads `#N` or counts the closure's parameters. `fn:for-each`,
  `fn:filter`, `fn:fold-left`, `fn:fold-right` iterate sequences
  through a new shared call seam (`xpath_call_function_item`) that
  dispatches named references and inline closures alike. `?`
  partial application desugars at parse time — `concat('x', ?, 'z')`
  becomes `function($%1){ concat('x', $%1, 'z') }` (hole names use
  `%N`: NCNames exclude `%`, and `\x01` would collide with the
  closure params separator). Spec
  `Xslt30.FunctionItemMetadataAndHofs` (11 cases), Saxon-HE 12.7
  oracle.



## [1.9.59] - 2026-09-02

### Fixed

- **Named function references dispatch** (TODO.xslt-full/07): `name#arity`
  nodes carry no children, so `evaluate_operator`'s opening arity guard
  rejected them before any operator check — every `$f := string-join#1`
  dispatch failed "upstream" with no diagnostic. The FR synthetic-node
  construction now runs before the guard, and the shadowed duplicate
  block is gone. Also implements the XPath 3.1 one-arg `string-join`
  (separator defaults to the zero-length string), which `string-join#1`
  dispatch lands in: `$f(('a','b'))` → `ab`. Spec
  `Xslt30.NamedFunctionReferenceDispatch` (string-join#1, concat#2),
  Saxon-HE 12.7 oracle.



## [1.9.58] - 2026-09-02

### Added

- **Inline function items + dynamic calls** (TODO.xslt-full/07 first
  slice): `function($x){...}` closures — immediate calls
  (`function($x){$x+1}(41)` → 42), let-bound calls, and multi-param
  dispatch (`function($a,$b){$a*10+$b}` → 42). Value-level: the
  closure rides one synthetic node with the body AST hex-encoded;
  dynamic calls bind params through the FOR-discipline variable
  sets. Two bug classes fixed en route, both recorded: C hex-escape
  greed (`"\x03FN"` lexed `0x3F`+`'N'` — `F` is a hex digit; markers
  now use string-literal concatenation) and NUL-bearing pointer bytes
  truncated by the let machinery's strlen-wise deep copy (heap
  overflow; hex encoding is NUL-safe). The param-binding nodes now
  own their synthetic members (`set_remove` frees the nodeset — the
  48-byte Linux-LSan leak macOS ASAN cannot see). `name#arity` parses
  but its dispatch is next-window work (TODO 07). Saxon-HE 12.7
  ground truth; spec RED first.

### Fixed

- own the synthetic arg nodes in dynamic-call param binding



## [1.9.57] - 2026-09-02

### Added

- **`fn:serialize` with `method: 'json'`** — TODO.xslt-full/08's
  value-level surface is now complete. The shared map representation
  emits as a JSON object: keys double-quoted, values escaped when
  their lexical form is not number/boolean/null; non-json methods
  take the string value. Saxon-HE 12.7 ground truth:
  `serialize(parse-json('{"b": "beta", "n": 2}'), map { 'method':
  'json' })` → `{"b":"beta","n":2}`. Lane 08 tally: map + array
  constructors, all accessors, `parse-json`, `?lookup`,
  `json-to-xml`, `xml-to-json`, `serialize` json (the HOF-form
  combiners queue behind lane 07's function items).



## [1.9.56] - 2026-09-02

### Added

- **`xml-to-json`** (TODO.xslt-full/08): walks the canonical `fn:`
  vocabulary back into the shared map representation — members add
  under `@key`, nested containers recurse through the builder,
  scalars take the element text. The round trip
  `xml-to-json(json-to-xml('{"b": "beta", "n": 2}'))` verified
  against the Saxon-HE 12.7 expectation. The handoff's zero-entries
  mystery resolved to an ownership bug (the synthetic map node was
  added to the result while the intermediate result still owned it —
  freeing the intermediate left it dangling); ownership now
  transfers explicitly, the same invariant family as the #720
  sequence transfer.



## [1.9.55] - 2026-09-02

### Added

- **`json-to-xml`** (TODO.xslt-full/08 tail): the canonical `fn:`
  XML vocabulary — the map/array is decoded from the shared
  representation, values classify by lexical form
  (number/boolean/null/string), the XML is built as escaped text and
  parsed into a scratch document, and the root element rides the
  nodeset channel. Saxon-HE 12.7 ground truth, byte-identical:
  `json-to-xml('{"b": "beta", "n": 2}')` yields `<map
  xmlns="http://www.w3.org/2005/xpath-functions"><string
  key="b">beta</string><number key="n">2</number></map>`. The
  earlier forced revert of this slice turned out to be the
  stale-static-archive probe trap (standalone probes failed even
  shipped functions; the test binary is the true surface) — the
  bisect rule is recorded in the commit.



## [1.9.54] - 2026-09-02

### Added

- **Postfix lookup `?key` / `?integer`** (TODO.xslt-full/08 tail):
  `?` lexes as a token (previously a hard lex error — nothing that
  compiled changes) and `V?k` resolves the map entry; array indices
  ARE the positional keys, so `[10, 20, 30]?2` is the same lookup.
  The postfix loop (predicates + lookups) is now one shared helper
  used by both filter expressions and the map/array constructors —
  the RED spec caught constructors bypassing the filter level, so
  `map { 'b': 1 }?b` never bound before the fix. Saxon-HE 12.7
  ground truth: `map?b`='beta', `[10,20,30]?2`=20.



## [1.9.53] - 2026-09-02

### Added

- **Square array constructor + `array:size/get/append/put`**
  (TODO.xslt-full/08C): value-level arrays ride the map
  representation with positional keys — one representation, two
  vocabularies; every accessor is the map operation with a formatted
  index. A leading `[` in an expression can only be the constructor.
  Saxon-HE 12.7 ground truth: size=3, get(2)='b', append(3)='c',
  put(2)='B'.
- **`parse-json`** (08D): a compact recursive-descent JSON parser
  producing map/array values usable directly by the accessors — root
  containers parse their members straight into the result (the
  wrap-then-unwrap first cut corrupted delimiter scans: a 2-member
  object reported size 4). Flat maps/arrays are exact; re-building
  nested containers is the documented v1 limit. Saxon ground truth:
  get('b')='beta', array:get(2)=20, size=2.



## [1.9.52] - 2026-09-02

### Added

- **`map:put` / `map:remove` / `map:merge` + `xsl:map`** (TODO.xslt-full/08
  second slice) — and with `xsl:map` landing, **TODO.xslt-full/09 is fully
  complete**. `map:put` replaces or appends; `map:remove` drops a key;
  `map:merge` concatenates a sequence of maps (last wins on duplicate keys).
  `xsl:map` + `xsl:map-entry` build the value from an expression `@key`
  (Saxon-HE 12.7: `key="'k'"` — a bare name is a path) with `@select` or
  content values; a variable whose content is a lone `xsl:map` binds the map
  value. One representation authority now serves the constructor, `xsl:map`,
  and every combiner (shared entry-list encode/decode + builder seam).
  Includes a leak fix caught by the Linux CI ASAN leg (`map:merge`'s
  destination entries). Next in the lane: arrays and the JSON family.

### Fixed

- free merge's destination entries (Linux LSan leak, #748 CI)



## [1.9.51] - 2026-09-02

### Added

- **Value-level maps** (TODO.xslt-full/08 first slice): the 3.1 map
  constructor `map { k: v, ... }` plus `map:get`, `map:size`,
  `map:keys`, `map:contains`. A map is one synthetic text node
  encoding its entries in insertion order, flowing through the
  existing nodeset channel — no ABI change (the FFI map view is the
  #683 follow-up). The lexer gained a token for the single `:`
  constructor separator (previously a hard lex error, so nothing
  that compiled changes); braces are accepted only by the map
  grammar and any other brace shape still fails loudly, which let
  the #692 post-compile brace backstop retire. Saxon-HE 12.7 ground
  truth; next in the lane: map:put/remove/merge, `xsl:map` (closing
  TODO 09), arrays, and the JSON family.



## [1.9.50] - 2026-09-02

### Added

- **XPath 2.0 type operators** (TODO.xslt-full/06): `instance of`,
  `castable as`, `cast as`, `treat as` with SequenceType v1
  (`xs:QName`, `node()`, `item()`, occurrence `*`/`+`). Value-level
  type tests over the result model; `castable as` does full lexical
  checks; `cast as` reuses the v1.9.49 constructor semantics
  (`xs:integer` truncates toward zero); `treat as` passes through.
  Saxon-HE 12.7 ground truth, including the falsifiable negative
  (`'x' castable as xs:integer` = false) and `1.9 cast as
  xs:integer` = 1.



## [1.9.49] - 2026-09-02

### Added

- **`xs:` atomic constructor functions** (TODO.xslt-full/06, the
  roadmap's constructor surface): `xs:string` / `xs:anyURI` take the
  argument's string value, `xs:double` / `xs:decimal` convert through
  `number()`, `xs:integer` truncates toward zero, and `xs:boolean`
  follows the XPath `boolean()` rules (NaN casts to false). The XSD
  namespace joins the extension-function namespace list so
  `xs:`-prefixed calls resolve in any expression. Saxon-HE 12.7
  ground truth: `xs:integer('42')+1` = 43, `xs:double('1.5')*2` = 3,
  `xs:boolean('true')` = true, `xs:string(7)` = "7". Next in the
  lane: `instance of` / `castable as` / `cast as` / `treat as`.



## [1.9.48] - 2026-09-02

### Fixed

- **#729 — on-completion iterate params**: the per-iteration parameter
  push/pop lives inside the loop, so the `xsl:on-completion` body ran
  with no binding (`Variable 'sum' not found`). The final iteration's
  values are now bound around the action per §12.5 — param-chained
  sums work (`<xsl:iterate select='1 to 4'>` + `$sum` = 10).
- **#731 — merge-level merge-keys**: `xsl:merge` read keys only from
  inside each `xsl:merge-source`; sheets written with `xsl:merge-key`
  as a direct child of `xsl:merge` (accepted by Saxon-HE) got no keys
  — every item shared the empty composite, collapsing the merge into
  one action with an empty `current-merge-key()`. Sources without
  their own keys now fall back to the merge-level declarations.
- **#730, #732 — triaged**: unquoted `xsl:evaluate xpath="$p"` fails
  in the outer scope exactly as Saxon rejects it statically (the
  with-param channel binds the dynamic phase; quoted `xpath="'$p'"`
  verified against Saxon); multiple top-level result nodes are a
  supported shape here (libxslt parity, suite-pinned) — pinned by
  `Xslt30.MultipleTopLevelNodesSerializeFully`.



## [1.9.47] - 2026-09-02

### Fixed

- **#720 — sequence-use keys**: a `xsl:key` with a sequence `use`
  (`use="@v, ."`) crashed the transform through a use-after-free (the
  sequence operator borrowed nodes from each item's result, then freed
  the item — a synthetic attribute node dangled), and the node was
  never indexed (the whole sequence stringified to empty). Ownership
  now transfers to the sequence nodeset (the union op's #514
  discipline), `xpath_nodeset_free` owns one dispatch pass instead of
  three sequential loops that re-read freed nodes, and every ITEM of
  the use result is a key value (Saxon `composite="no"` ground truth:
  `('1','alpha')`→1, `'1'`→1, `'alpha'`→1; sequence lookups union
  buckets, deduped).
- **#721 — `leptris_element_copy` namespaces**: the copy dropped the
  cached prefix and namespace URI (`elem->name` is the LOCAL name) and
  every `xmlns:*` declaration. `copy_element_namespaces` duplicates
  prefix/URI into the target pool and re-declares the bindings after
  attach — `<p:r xmlns:p="urn:p"><p:c/></p:r>` round-trips
  byte-for-byte and namespace resolution works on the copied subtree
  (libxml2 `xmlDocCopyNode` parity).



## [1.9.46] - 2026-09-02

### Added

- **`xsl:result-document`** (2.0/3.0 §11.8): the content is built in a
  fresh scratch document, serialized (declaration + UTF-8, character maps
  applied) and written to the `@href` file, creating parent directories
  as needed. The principal result is unchanged (Saxon-HE 12.7 ground
  truth). v1: static `@href`, xml method.
- **`xsl:character-map`** (§16.1): character → replacement-string tables
  activated from `xsl:output/@use-character-maps` (whitespace-separated
  names; all named maps apply). Substitution is a post-serialization pass
  on the UTF-8 result — text spans and attribute-value quotes only, never
  markup — so it composes with the us-ascii-escape and iconv transcode
  steps that follow. Saxon-verified: two maps, text + attribute
  substitution (`<o a="x->y">lambda</o>`).



## [1.9.45] - 2026-09-02

### Added

- **`xsl:evaluate` with-params** (3.0 §14.3.2): child `xsl:with-param`
  bindings are visible to the dynamic evaluation only — `@xpath`'s own
  evaluation keeps the outer bindings (Saxon-HE 12.7 verified). Bindings
  now go through `xslt_push_var` so the eval varset cache sees them; a
  raw variable link left `$p` resolving as unknown and aborted the
  transform. `xsl:evaluate`'s children are parsed at all now (they were
  silently dropped).
- **`xsl:merge`** (3.0 §14.3): full outer join of every `xsl:merge-source`
  on the composite `xsl:merge-key` — stable sort with selection-order
  tiebreak, the first key's `@order` governs the composite.
  `current-merge-key()` and `current-merge-group(name)` are served
  through the exec bridge next to `current-group()`. v1: `@select`
  sources, string keys.
- `xsl:next-iteration` with-param rebinding verified and pinned by spec
  (param-chained sum 1..4 = 10).



## [1.9.44] - 2026-09-02

### Added

- **Five more XSLT constructs** (TODO.xslt-full/09 batch C): `xsl:copy
  @select` (3.0 §9.9.2 — the sequence constructor is ignored and each
  selected item is copied with copy-of per-item semantics: attribute
  items rebind on the pending parent, element items deep-copy),
  `xsl:namespace` (2.0 §11.7 — binds prefix→URI on the pending
  element; the URI is the string value of the content, captured
  off-tree), `xsl:document` (2.0 §11.8 — document constructor, content
  flows to the pending parent), `xsl:on-completion` (3.0 §12.5 — the
  post-loop body of `xsl:iterate`, runs unless the loop ended in
  `xsl:break`), and `xsl:param @default` (the 4.0 form — dynamic
  default evaluated when no with-param binds, taking precedence over
  the sequence constructor). Saxon-HE 12.7 ground truth for the spec;
  @default is pinned at our semantics since Saxon 12.7 rejects it
  statically.



## [1.9.43] - 2026-09-02

### Added

- **Three more 3.0 items implemented** (#690/#685): `xsl:fork`
  (§14 — non-streaming arms run sequentially into the same
  destination), `xsl:number @start-at` (§12.2 — offsets positional
  numbering on every level), and composite keys (§12.2 — a use
  value of multiple whitespace-separated tokens indexes the node
  under each). Sequence items serialize space-separated across
  consecutive sequence instructions (fork arms). Saxon-HE 12.7
  verified.

## [1.9.42] - 2026-09-02

### Added

- **Three more 3.0 instructions leave the no-op list** (#690/#685):
  `xsl:where-populated` (§26.2 — content builds into a detached
  scratch element; a wholly-empty build vanishes), `xsl:on-non-empty`
  (§26.4 — Saxon-HE 12.7 evaluates the content unconditionally,
  verified live; the spec permits buffering, parity follows the
  oracle), and `xsl:next-match` (§6.6 — invokes the best matching
  rule strictly worse than the current one; none lower means the
  built-in). Saxon byte-parity on all six probe shapes.

## [1.9.41] - 2026-09-02

### Added

- **switch grammar groundwork, Saxon-parity verified** (#692/06):
  Saxon-HE 12.7 rejects `switch` in XPath expressions (XPST0003 —
  the syntax is XSLT 3.0 pattern-only), verified live; we reject
  it too, with brace tokens lexing so the failure is a clear
  parse error. The complete switch grammar (case clauses, default,
  lazy eq-compare evaluation) is implemented behind `#if 0` and
  lands with pattern support. The #692 expression-attribute brace
  guard now runs the compiler first — future brace-bearing grammar
  can accept its own forms while the phantom-compile backstop
  stays.

## [1.9.40] - 2026-09-02

### Added

- **The fn: catalog date slice** (#691-E): `xs:date` /
  `xs:dateTime` / `xs:time` / `xs:duration` constructors (canonical
  `xs:` prefix, lexical passthrough) and the component extractors
  `year/month/day-from-dateTime`, `hours/minutes/seconds-from-time`,
  `days/hours-from-duration` — ISO 8601 field parsing over the
  value-level string model. Saxon-HE 12.7 probed.

## [1.9.39] - 2026-09-02

### Fixed

- **Element copies keep COMMENT and PI children** (#696): the
  child-copy loop in `leptris_element_copy` skipped comment and
  processing-instruction children entirely — `Node#dup` and
  document copies silently lost them at every level. Both kinds
  now copy like text/CDATA/elements; the mixed-content shape
  round-trips byte-for-byte.

## [1.9.38] - 2026-09-02

### Fixed

- **The regex trio atomizes node arguments** (#691 comment):
  `matches(//item[1], …)` returned false — the string-argument
  helper returned NULL for nodeset results.
- **Single-item atomic `xsl:sequence` serializes its content**
  (#685 remainder): a scalar result has count 0, so the item loop
  never ran and the value vanished.
- **#705 verified and pinned**: `shallow-skip` / `text-only-copy`
  descend and match Saxon-HE 12.7 byte-for-byte on every reported
  shape on current main (the 1.9.36 behavior was fixed by the
  1.9.37 on-no-match work). Note: Saxon emits EMPTY for the
  no-template shallow-skip case — the report's `312` expectation
  for that disposition contradicts Saxon. All four shapes pinned
  as regression specs.

## [1.9.37] - 2026-09-01

### Added

- **Tunnel parameters (§11.7)**: `xsl:with-param tunnel="yes"`
  rides a transform-level frame chain that persists for the whole
  subtree — pushed both on template invocation and on the
  unmatched-node path (processing continues through the built-in
  rules); `xsl:param tunnel="yes"` binds from the chain into the
  regular frame, falling back to its declared default when the
  name is absent. Saxon-HE 12.7 probed: the value reaches every
  template the subtree processes without re-passing.

## [1.9.36] - 2026-09-01

### Added

- **The fn: catalog grows (#691)**: the strings/QNames/URIs
  slice — fn:format-integer (decimal, 0-padding, a/A bijective
  base-26, i/I roman numerals, w/W English words),
  fn:contains-token, string-to-codepoints / codepoints-to-string
  (full UTF-8 both directions), encode-for-uri / iri-to-uri /
  escape-html-uri, the QName constructor family (value-level;
  the namespace URI rides the constructor's thread-local channel
  until structured values land with function items), and
  node-name. Saxon-HE 12.7 probed; two implementation bugs
  caught by the spec itself.

## [1.9.35] - 2026-09-01

### Added

- xsl:sequence + xsl:perform-sort; fix MSVC build of functions_ext31 (xslt)
- fn-catalog slices — sequences, math:, regex trio (+ top-level comma sequences) (xpath)



## [1.9.34] - 2026-09-01

### Fixed

- **#692 (the silent-wrong)**: XQuery-only syntax in XPath
  expression attributes — `try { ... } catch * { ... }`
  expressions, map constructors — compiled on some brace shapes
  and evaluated to silent EMPTY output. Saxon-HE 12.7 rejects
  both at compile time (XPST0003, verified live); expression
  attributes are never attribute-value templates and the XPath
  grammar has no braces, so any brace now fails the stylesheet
  loudly. The grammar-completion lane itself (switch, function
  items, maps/arrays, partial application) remains open.



## [1.9.33] - 2026-09-01

### Added

- **XSLT 3.0 increment 9 — `xsl:mode on-no-match` (6.7), all six
  dispositions**: deep-copy (verbatim subtree), shallow-copy — the
  3.0 default, a fix on its own since our previous default was
  only the 1.0 text-only-copy built-in —, shallow-skip (attributes
  and element children dispatch, text skipped), deep-skip,
  text-only-copy, and fail (XTDE0500). 1.0 sheets keep the classic
  built-in rules (the libxslt suite pins them at 205/205).
  Attribute dispatch routes through the XPath attribute axis —
  pattern identity lives on its synthetic nodes. README.adoc gains
  a full XSLT section (1.0-complete statement + the entire 3.0
  surface shipped to date).

### Fixed

- **#677**: `LEPTRIS_PARSE_DROP_WS_TEXT` ate the leading whitespace
  of NON-blank text runs — parse-time data loss. The drop now
  applies only to whitespace-only runs (libxml2
  XML_PARSE_NOBLANKS parity).
- **#687**: DOCTYPE internal-subset serialization dropped the
  opening byte of every declaration after the first (`!ATTLIST`).
- **#686**: the Windows/MSVC build silently produced empty output
  for `xsl:analyze-string`; it now raises a loud, `xsl:try`-
  catchable dynamic error (portable regex engine tracked in the
  issue).



## [1.9.32] - 2026-09-01

### Performance

- **Two profiled follow-ups to the pattern compiler, closing the
  remaining dispatch and key() gaps.** (1) Pattern evaluation
  resolved the candidate's owning document on every call through a
  full parent climb plus root-map probe — 56% of a dispatch
  transform's samples; the climb already stops at the tree root
  element, so comparing it against the caller's own root is one
  O(1) read (foreign-tree nodes keep the full resolution). (2) The
  key() index build matched every document node with an unarmed
  pattern — one lookup cost 200 ms on the 2000-book fixture;
  key definitions now embed a compiled pattern (step ladder armed
  at parse) and use-expressions evaluate through the cached
  registry. Measured warm: dispatch 228 → 13.5 ms, Muenchian key
  232 → 3.1 ms, select-heavy for-each 2.9 ms — every reference
  sheet now runs at libxslt engine-side pace (xsltproc's ~10 ms
  wall includes process start, parse and serialization;
  Saxon-HE ~0.9–1.15 s with JVM boot).



## [1.9.31] - 2026-09-01

### Performance

- **XSLT template matching is O(depth), not O(siblings)** — the
  pattern compiler. Child-axis match alternatives
  (`book[title]`, `a/b/c`) now compile to per-step name/kind tests
  with an optional last-step predicate: the candidate node is
  tested directly and the earlier steps are name checks up the
  parent chain. Previously every ancestor rung evaluated the
  pattern as a full downward XPath and membership-scanned the
  result — a dispatch-heavy 2000-book transform ran 5.56 s where
  libxslt completes in ~10 ms wall. Measured on the same fixture:
  dispatch 5558 → 228 ms (24×), select-heavy for-each 18.4 →
  3.1 ms (libxslt parity). Everything the fast path does not
  model (prefixed tests, `//`, `::`, function patterns, non-final
  or positional predicates) keeps the general matcher; the
  libxslt suite stays 205/205. New
  `PerfRegression.TemplateDispatchScalesLinearly` pins the
  complexity (the pathology measured a 24× time ratio for 4× the
  books; healthy runs stay under 10×).



## [1.9.30] - 2026-08-31

### Fixed

- **XSLT: a misplaced `xsl:catch` (anywhere but a child of
  `xsl:try`) is now a compile error** (#669) — matching Saxon's
  XTSE0010. Previously the instruction was silently skipped, so a
  correctly-raised error had no catch attached and the transform
  returned an unexplained NULL. The canonical form already caught
  every `error()` variant (`error('msg')`, `error(concat(...))`,
  `error($var)`); now spec-pinned. Issue #669's repro placed the
  catch as a sibling of try — a stylesheet Saxon rejects at
  compile time — verified by direct Saxon-HE 12.7 runs. En route,
  both stale rows of the quoted status board were disproven by
  measurement: a fresh Release A/B against the real v1.9.19 tag
  shows current main faster on every DOM benchmark row (parse+root
  7.00 vs 7.90 µs, peak RSS 1360 vs 1584 KB), and scalar XPath
  measures 535–720 µs vs libxml2's 2081 µs (~3–4× ahead, not
  1.64× behind).



## [1.9.29] - 2026-08-31

### Added

- **XSLT 3.0 program, increments 7–8 — the XPath 3.1 composition
  core**: `let $x := E1, $y := E2 ... return B` binds each value
  through the context variable set (each binding sees the earlier
  ones and the outer scope; shadowed bindings deep-restore on
  unwind), `L ! R` maps the right side over every item of the left
  with per-item context item/position/last(), `E => f(a)` passes
  the accumulated left side as the first argument of a function
  call (so every core and XSLT-bridge function serves it), and
  `A || B` string-concatenates. Verified against Saxon-HE 12.7,
  including the combined form
  `let $x := 5 return ($x to 7) ! (. * 2) => sum()` = 36.
  Variable references now deep-copy synthetic sequence members —
  a let binding's storage is freed while results referencing it
  still live.



## [1.9.28] - 2026-08-31

### Added

- **XSLT 3.0 program, sixth increment — `xsl:accumulator`
  (18.2)**: declarations compile to ordered rules (`@match`,
  `@phase` start/end, `@select`, `@initial-value`).
  `accumulator-before(name)` / `accumulator-after(name)` fold
  lazily once per (accumulator, tree) from the document node
  through every node's start/end events — per event the last
  matching rule of that phase fires — and both endpoints are
  snapshotted into per-node maps cached for the transform.
  `$value` is bound through the exec frame chain, so rules also
  see user and global variables; temporary trees evaluate with
  the source swapped so absolute paths resolve.
  `xsl:mode use-accumulators` gates applicability on the
  principal document (XTDE3362); XTDE3340 (undeclared name),
  XTTE3360 (attribute context), and XTDE3400 (re-entrant fold)
  are raised. Semantics decoded from Saxon-HE 12.7 and the W3C
  REC before implementation; the spec asserts Saxon's exact
  outputs (counter, @n sum, phase="end" over nested containers,
  and the applicability gate).



## [1.9.27] - 2026-08-31

### Added

- **XSLT 3.0 program, fifth increment — `xsl:on-empty` (26.4)**: a
  child of a literal result element whose content sequence comes back
  empty (no child nodes built, text included) gets the on-empty
  content evaluated into it instead; the walker skips the
  instruction itself. The emptiness check reads the raw first-child
  link — the element-only accessors skip text nodes, which made
  text-only content look empty. Saxon-HE 12.7 verified.



## [1.9.26] - 2026-08-31

### Added

- **XSLT 3.0 program, fourth increment — `xsl:try` / `xsl:catch`
  (17)**: the try body is the children before the first xsl:catch; a
  dynamic error runs the catch content with `$err:description` bound
  to the message, clears the error channel, and the transform
  continues. No error restores the outer channel state verbatim.
- **`error($description)`** raises a catchable dynamic error from
  XPath — the Saxon-HE 12.7 shape with the description carried into
  the catch. Static errors (undeclared variables, unknown functions)
  still fail stylesheet compilation, matching Saxon (XPST0008).



## [1.9.25] - 2026-08-31

### Added

- **XSLT 3.0 program, third increment — the full grouping set and
  regex string processing** (Saxon-HE 12.7 verified):
  - **`group-adjacent`** (14): only adjacent equal keys share a
    group; the grouping key rides along for
    `current-grouping-key()`.
  - **`group-ending-with`** (14): a pattern match closes the group;
    trailing non-matches form the final group.
  - **`xsl:analyze-string`** (18): POSIX ERE scan of the selected
    string; `matching-substring` / `non-matching-substring` bodies
    run per segment with `.` = the segment; `regex-group(n)` reads
    the captures of the match in flight. `i` flag = case-fold;
    zero-length matches advance one character. MSVC builds no-op
    the engine (same documented limitation as the EXSLT regexp
    handlers).



## [1.9.24] - 2026-08-31

### Added

- **XSLT 3.0 program, second increment — sequences and the core 3.0
  instruction set**, every feature spec'd first and verified against
  Saxon-HE 12.7 ground truth:
  - **Item sequences** (XPath 2.0+): `for ... return`, `A to B`
    ranges, and parenthesized sequences produce true sequences
    instead of pre-joined strings; `string-join`, `upper-case`,
    `lower-case` consume them; `xsl:value-of` prints sequences
    space-joined (3.0 default separator) while plain nodesets keep
    the 1.0 first-member rule. A `version="2.0"+` stylesheet's
    value-of prints every selected item (version tracked from
    `xsl:stylesheet/@version`).
  - **`xsl:iterate`** (12.5): sequential mapping with `xsl:param`
    iteration state, `xsl:next-iteration` rebinds, `xsl:break` —
    both unwind nested instruction sequences through a walker
    signal mirroring `func:result`.
  - **`xsl:for-each-group`** (14): `group-by` (first-key-appearance
    order) and `group-starting-with` (single-alternative pattern);
    `current-group()` / `current-grouping-key()`.
  - **Text value templates** (10.4.2, `expand-text="yes"`): `{expr}`
    in literal text — `xsl:text` content included (Saxon-verified) —
    through the AVT evaluator; 1.0 sheets unaffected.
  - **`xsl:evaluate`** (26): `@xpath` evaluates to the string to
    compile and run; `@context-item` picks the context node
    (omitted = absent context, anchored on the document node).

### Fixed

- **Serializer**: the fused-leaf fast path (text-bearing leaves)
  computed `indent * indent_spaces` directly, losing the #633 indent
  unit on every mixed-content leaf — it now emits the configured
  unit per level (#658; leptris-ruby#109 residual, moxml#153
  family).
- Free the for-each-group/evaluate compiled expressions and the
  group-starting pattern on stylesheet teardown (caught by Linux
  ASAN leak detection).

### Performance

- XSLT transform benchmark at 333.6 µs (lxml reference 499 µs); all
  1.0 conformance unchanged (W3C 438/438, libxslt suite 205/205).



## [1.9.23] - 2026-08-31

### Added

- **XSLT 3.0 program, first increment — the XPath 2.0+ expression
  core** every 3.0 feature stands on:
  - `if (cond) then A else B` — lazy conditional; `then`/`else`
    are value-matched so NCName name tests parse unchanged;
  - `for $v in DOMAIN return EXPR` — nodeset iteration with
    per-iteration variable binding; results join space-separated
    (the sequence's string form);
  - `A to B` — integer ranges as synthetic-text nodesets:
    predicates and numeric comparisons see each member
    (`(1 to 5)[. mod 2 = 1]` selects 1, 3, 5).
  Lazy semantics ride the AST interpreter via BC_FALLBACK_EVAL
  (VM opcodes are the follow-up). W3C 1.0 conformance and the
  libxslt suite unchanged (1148/1148).
## [1.9.22] - 2026-08-31

### Fixed

- **#653** ground truth pinned in specs: the report's well-formed
  self-closing-then-text shapes parse on every release since 1.9.18
  (fresh-build verified); its minimal repro carries a stray `</y>`
  after a self-closed `<y/>` and is ill-formed — strict rejection
  matches libxml2's tag-mismatch error, and the recover path keeps
  the documented #647/#547 contract (empty document, failure
  recorded). Spec-only release: no functional change.
## [1.9.21] - 2026-08-31

### Fixed

- **#648**: `leptris_pull_next_batch` staged record strings into a
  single reallocating arena — once a batch crossed the initial
  256-byte block (one attribute value ~190 bytes in the reported
  shape), the realloc dangled every previously staged pointer
  (empty/garbage record names, unknown first type code). The
  staging arena is now a never-moving BLOCK CHAIN (the #585
  recorder's discipline); batches reset and reuse the blocks.
## [1.9.20] - 2026-08-30

Closes every open upstream issue (#645, #610, #624, #647).

### Added

- **`leptris_node_visit`** — wrap-free subtree visitation (#645a):
  one C call walks a subtree in document order (enter/leave + depth);
  bindings wrap nodes lazily instead of materializing per-level
  NodeSet/Array allocations — the cold walk's per-node allocation
  floor. The document node walks the document child chain.

### Fixed

- **#647**: duplicate attributes report a recoverable SAX error —
  `Attribute NAME redefined` (libxml2's message) with position,
  through the callback error channel AND the recorder's ERROR
  record; the parse continues with every attribute in the event
  (libxml2 --recover semantics).
- Position plumbing for attribute diagnostics: line/column advance
  across the tag region ('<', element names, separators, attribute
  names and values).

### Performance

- **#645b**: scalar XPath evaluation — `string(//item[1])` fell
  back wholesale to the AST interpreter whenever an absolute path's
  step carried a position/operator predicate (4.6 ms CPU per eval
  on a 20k-item catalog). New fused `child::NAME[k]` opcode
  (per-context count-and-stop) plus specialized expanded
  `//name[pred]` compilation: **~500 µs/eval — 9× faster, and ~8×
  ahead of libxml2** (4350 µs) on the same fixture.
- **#624**: the `//book[@price>100]` select-heavy transform shape
  rides the same specialized path: 369 → ~350 µs on the fixture
  (fully recovered from the 1.9.13 regression; suite 205/205).
- **#610**: compiled vs string eval at parity on the parent axis
  (0.76 vs 0.78 µs) — closed with in-tree twin benchmarks
  (`bench_scalar_eval`, `bench_scalar_eval_libxml2`,
  `bench_xslt_transform`).
## [1.9.19] - 2026-08-30

The libxslt general suite is **205/205** — the open-worklist is empty.
Eight suite closures plus three downstream-reported fixes.

### Fixed

- **#643**: GCC 14 (musl/Alpine) builds — the element-typed
  `leptris_node_parent` return assigned to a node reference is an
  error under GCC 14's `-Wincompatible-pointer-types` default.
- **#644**: `leptris_document_serialize_ext` read `ext->indent_unit`
  past FFI callers' shorter ext structs (the MSVC display-form
  segfault in leptris-ruby). New
  `leptris_document_serialize_ext_sized(doc, options, ext, ext_size)`
  reads each field only when the caller's allocation covers it;
  `direct_parse`'s `saw_namespace` no longer rides the stack (the
  C4701 was real).
- **libxslt suite** (bug-56, bug-5-, bug-65, bug-90, bug-111,
  bug-130, bug-166):
  - pure-text/empty RTFs bind as the fragment's document node
    (`count($rtf)` = 1, `string($rtf)` = the fragment text);
  - match patterns match inside foreign documents (RTF fragments,
    `document()` results) — the matcher derives the node's own
    document;
  - §3.4 any-pair nodeset equality on the interpreter path, and
    number→string now mirrors libxml2's `xmlXPathFormatNumber`
    (15 significant digits; `10695.23` no longer prints `10695.2`);
  - result-fragment elements keep cdata-section-elements after the
    first, the declaration's newline stays with the declaration, and
    indented outputs end with one final newline (libxslt layout
    parity);
  - `preceding-sibling::`/`following-sibling::` see top-level
    document children (multi-root fragments);
  - libxslt's own `<test/>` test extension element executes;
  - an unprefixed literal from an imported module under a
    default-namespaced ancestor resets with `xmlns=""`.
- **bug-100**: same (the libxslt test extension element).
## [1.9.18] - 2026-08-29

### Added

- **#635**: `leptris_element_attributes_raw` — the mixed
  qname-ordered attribute list the streaming transports deliver,
  xmlns declarations interleaved among the attributes at their
  source byte positions (names and values as written). A DOM-to-SAX
  bridge can now match the streaming contract exactly. Recorded at
  parse on the lazily-created ns cache; cache-less elements pay
  nothing.



## [1.9.17] - 2026-08-29

XSLT engine-semantics round; the libxslt suite holds 197/205 with
two crashers (bug-166, bug-5-) converted to ordinary diffs.

### Fixed

- Absolute paths (`//name`, `/name`) root at the CONTEXT NODE'S
  document, matching libxslt: evaluation inside document() output or
  an RTF fragment no longer sees the transform source tree (the
  bug-65 family). The context can be a text/comment/PI node during
  pattern matching, so the document lookup walks any-kind parent
  links to the nearest element first - the naive element-layout read
  crashed 19 suite cases.
- A with-param with CONTENT (no select) bound NULL - named-template
  parameters arrived empty (bug-90's wrap-cdata template). The RTF
  capture is a shared helper used by variables and with-params.
- The RTF capture stored pre-escaped text; string($rtf) is
  unescaped, so re-emitting a captured fragment with
  disable-output-escaping produced double-escaped ampersands.
  Capture keeps the logical text.



## [1.9.16] - 2026-08-29

Serializer parity round for the moxml/canon pretty-printer
(issue #633 - the last blocker for byte-identical to_xml(indent: N)).

### Fixed

- Comments and PIs under a non-mixed parent get their own indented
  line, matching libxml2 xmlIndentTreeOutput; mixed-content parents
  keep them inline so a PI between text nodes never moves.
- The stray trailing newline after a text-only ROOT element: not a
  non-ASCII issue - the open-tag path (elements WITH attributes)
  lacked the is-root guard the fusion fast path already had.
- DOCTYPE internal subsets lay out one declaration per line with the
  bracket on its own line; empty subsets drop the brackets entirely.

### Added

- LeptrisSerializeExtOptions.indent_unit: one string copy per depth
  level (libxml2 xmlTreeIndentString / Nokogiri indent_text), NULL
  keeps the legacy spaces-per-level.



## [1.9.15] - 2026-08-29

Correctness round for the binding-reported issues, plus two API
additions the bindings were waiting on.

### Fixed

- **#630** (and the #557 reopen family): relative `.//ns:x` and
  `descendant::ns:x` from ELEMENT context returned empty - the
  element index keys buckets by LOCAL name and the VM's relative
  descendant paths looked up the raw qualified string, missing every
  bucket. Both index paths now resolve the local bucket and filter
  matches namespace-aware; specs pin every context depth plus
  URI-equivalent prefixes.
- **#592**: full-document iterparse reported a spurious "truncated
  XML document" after cleanly draining well-formed input - the
  top-level-mode subtree release reset depth to 1 after the ROOT
  yield, so END_DOCUMENT saw an open element. The subtree lifecycle
  is gated to top-level mode.
- **#613**: `leptris_parse_string_with_encoding` on declared
  ISO-8859-1 delivered raw latin-1 bytes as text on no-iconv builds
  (the fallback assumed UTF-8). Latin-1 converts natively there now;
  `leptris_encoding_parse_declaration` moved to the always-compiled
  wrapper.
- XSLT 5.8: built-in template rules apply-templates in the SAME
  mode (the synthesized fallbacks dropped it); libxslt's own
  extension namespace (`libxslt:node-set()`) resolves under any
  bound prefix.

### Added

- **#608**: `leptris_xpath_compiled_eval_ns_vars` - prefixed name
  tests and $var references in one compiled call; bindings stop
  falling back to uncompiled evaluation for the combination.
- **#617**: `leptris_node_children_ex(parent, out_nodes, out_kinds,
  max)` - each child's kind rides the batch, so a binding's
  cold full-tree walk skips the per-node get_type dispatch.

### Performance

- **#617** (scalar half): the node-test matcher now uses the
  parser's pre-split QName instead of a strchr per node, and a bare
  NUMBER-literal predicate ([1]) compares the position directly -
  together ~4 percent on the 350k-node string(//item[1]) profile.

## [1.9.14] - 2026-08-29

libxslt general suite: 191 -> 197/205 (bug-140, bug-142, bug-152
closed; XHTML serialization, lang() correctness, xmlns entity
expansion, output encodings).

### Fixed

- **#625**: the streaming SAX scratch arena grew by realloc, which
  invalidated attribute name/value pointers parked in element frames
  and pending_attr_name whenever nested attr-carrying elements
  crossed a growth boundary mid-element - the first attribute pairs
  came back empty-named or holding uninitialized bytes through the
  callbacks, recorder, and pull transports alike (ASAN:
  heap-use-after-free). The arena is now a stable block chain;
  handed-out pointers never move for the parser's lifetime.
- **#626**: non-ASCII element names were rejected on MSVC builds
  because the chartype table's UTF-8 bits were OR'd in by a
  .CRT$XCU initializer that does not run in every link shape. The
  table is fully static and const now - no initializer, no failure
  mode. Comments/PI data and the streaming paths were unaffected.
- **#627**: unknown unprefixed functions (and unbound variables)
  evaluated to empty inside stylesheets while plain XPath raised.
  A failed expression evaluation now aborts the transform and the
  public apply entries return NULL, matching libxslt's runtime
  behavior. Surfaced and fixed two latent bugs silent-empty had
  masked: lang() now coerces its argument per XPath 3.2
  (lang(ja) with an element-name argument is false, not an error),
  and a named template invoked while later globals were still
  evaluating saw a NULL call-template reset point (bug-192),
  func:result/func:param now carry their enclosing function's
  namespace context (bug-225).
- **#628**: last() inside for-each/apply-templates returned 1 - the
  exec threaded only the iteration position. The in-flight
  node-list size now rides the eval context; all three iteration
  loops set it.
- bug-152: xml-method results whose doctype ids exactly match an
  XHTML 1.0 DTD serialize in XHTML mode (libxml2 xmlIsXHTML parity):
  Content-Type meta injected into the root html/head, the 13 HTML
  empty names minimized as <x />, and a bare html root gains the
  XHTML default namespace.
- bug-142: three XPath engine defects - lang() walked past a
  non-matching xml:lang to outer ancestors (the nearest declaration
  decides); the /root fast path skipped the first step's predicates
  (/r[false()] selected the root); a prefixed attribute test whose
  prefix is absent from the binding set (@xml:lang) matched nothing
  in predicates and after descendant steps.
- bug-140: xmlns values containing entity references expand through
  the internal subset at parse time; xsl:output's encoding is
  emitted verbatim in the XML declaration (the XSLT layer transcodes
  the body to match - the plain serialize API keeps the truthful
  declaration).
- Section 5.8 built-in template rules apply-templates in the SAME
  mode (the synthesized fallbacks dropped it); libxslt's own
  extension namespace resolves under any bound prefix
  (libxslt:node-set()).
- no-iconv builds: the iconv transcoding call is guarded, with a
  built-in UTF-8 -> ISO-8859-1 fallback (codepoints <= U+00FF map
  1:1, stray high bytes pass through) so Western encodings stay
  byte-faithful; the declaration echoes the source encoding instead
  of emitting an empty attribute; MSVC maps strcasecmp/_stricmp.
- leptris-ruby#99: every SAX transport delivers xmlns declarations
  (attrs pairs + prefix-mapping events; DOM declaration enumeration
  via leptris_element_namespace_count/_decl_prefix/_decl_uri) -
  specs pin the contracts for DOM-backed dispatch.

## [1.9.13] - 2026-08-28

libxslt general suite: 180 -> 181/205.

### Fixed

- Document-level whitespace chains as TEXT children of the document
  node with libxml2's exact rule (xsltproc-probed): kept after a
  comment or the root element, dropped after a prolog PI, leading
  prolog whitespace dropped, trailing tail whitespace trimmed after
  the root splice. /node() counts the text and identity transforms
  copy it (bug-195); the serializer's document-chain walks emit TEXT
  nodes; the VM's absolute node() walk matches every chain kind



## [1.9.12] - 2026-08-28

libxslt general suite: 174 -> 180/205.

### Fixed

- Indent semantics under indent="yes": libxslt's serializer stops
  formatting at any text node — whitespace-only children included —
  and the stop propagates down the subtree; whitespace text copied
  from the source is the visible indent (bug-98)
- apply-templates over a selected text item applies the built-in
  TEXT rule instead of dropping it (bug-161); apply-imports with no
  imported candidate applies the built-in rule for the node
  (bug-193)
- xsl:copy copies the element and namespace nodes but NOT
  attributes (7.5) — attributes reach the result only through
  apply-templates/@* (bug-32-); copy-of of an attribute node adds
  name/value to the pending parent (bug-3-)
- xsl:attribute's namespace attribute: XML namespace maps to the
  xml: prefix (bug-177); other namespaces on unprefixed names mint
  generated ns_N prefixes (bug-99); prefixed names rebind
- xsl:strip-space/preserve-space: the last matching declaration
  wins (3.4) (bug-82)



## [1.9.11] - 2026-08-28

libxslt general suite: 153 -> 173/205.

### Fixed

- Template selection parses the `priority` attribute (5.4) and no
  longer fast-paths child-step patterns onto the root element
  (libxslt bugs 157, 186, 87)
- `xsl:number`: count/from patterns evaluate with the variable
  frame in scope; the default count is node kind + expanded name, so
  namespace and attribute nodes number within their owner's list and
  level=any from a namespace node resolves through the owner
  (bugs 214, 186, 218, 199)
- Attribute sets (7.1.4/12.1.4): precedence-ordered union with
  import ranks, own-attributes-first vectors, QName lookup by
  namespace URI, and a skip-if-exists use-list applied after literal
  attributes (bugs 131, 189, 190, 217)
- Function-call patterns take the 0.5 "otherwise" default priority;
  generate-id() emits libxslt's deterministic sequential ids; global
  variables evaluate against the source document node (bugs 113, 224)
- Prefixed attribute node-tests resolve by namespace URI, not
  prefix spelling (bug 97)
- Namespace declarations serialize in declaration order: the
  default is no longer hoisted ahead of prefixed declarations, the
  literal result element's own prefix binding leads, and the default
  emits at its declared position (bugs 104, 71, 150, 117)
- Unknown extension elements run only their `xsl:fallback` children
  (bug 220); `xsl:element` no longer copies in-scope namespaces and
  skips declarations the result ancestors already provide
  (bugs 92, 179)
- `copy-of` of a namespace node adds the declaration onto the
  pending parent (prefixed bindings only; xml and duplicates
  skipped) (bugs 38-, 54); the serializer leaves apostrophes raw in
  double-quoted attribute values (bug 86)
- Memory: the in-place predicate filter no longer leaks heap-owned
  synthetic namespace/attribute/text nodes dropped from owned
  nodesets (Linux LSan)



## [1.9.10] - 2026-08-28

### Added

- leptris_sax_recorder_reset — reuse one recorder across documents (#594) (sax)



## [1.9.9] - 2026-08-28

### Added

- `leptris_document_serialize_ext` + `LeptrisSerializeExtOptions`
  (#129 ask): `indent_text=1` hands ALL whitespace to the formatter
  — text and mixed content indent (display form). The frozen options
  struct is untouched.
- `leptris_document_remove_pi(doc, target|index)` (#612): unlinks a
  document-level PI by target or index

### Fixed

- parse-created document-level PIs/comments carry document linkage
  — the setters now work on them (#612, leptris-ruby#92)
- `leptris_document_set_root` splices the new root into an existing
  document-child chain; chainless docs fall back to the root in the
  document node view (leptris-ruby#91)
- cdata-section runs split `]]>` across node boundaries (bug-132,
  bug-90); attribute axis expands entities (bug-59); html PIs close
  SGML-style (bug-11-); top-level variables evaluate with their
  declaring element's ns context (bug-36-); built-in element rule
  routes text through template selection (bug-171, bug-73);
  xsl:decimal-format separators are full UTF-8 strings with
  pattern-derived grouping size (bug-222)

libxslt suite 144 → 152/205.

### Fixed

- free replaced decimal-format defaults + uri/local (LSan) (xslt)
- built-in text rule routes through templates; decimal-format UTF-8 separators + pattern grouping size (bug-171, bug-73, bug-222) (xslt)
- attribute-axis entity expansion, html PI form, global-variable ns context (bug-59, bug-11-, bug-36-) (xslt)
- cdata-section runs split ']]>' across node boundaries (bug-132) (serialize)



## [1.9.8] - 2026-08-28

### Fixed

- **DTD ATTLIST default attributes no longer apply on plain parse**
  (#606): `LEPTRIS_PARSE_DTDATTR` opts in (libxml2 XML_PARSE_DTDATTR
  parity). W3C C14N 1.1 example 3.3's canonical form now excludes
  defaulted attributes. The XSLT engine still applies them at the
  transform boundary — libxslt's document loader default.
- XPath predicates apply to every node kind — `text()[2]`,
  `node()[4]` and friends returned empty (non-element candidates
  were silently skipped)
- XSLT patterns run the full match ladder for every node kind
  (position predicates like match="text()[2]" fired for every text
  node); attribute pattern identity is (owner, name)

### Added

- `xsl:output` doctype-system/doctype-public emission; html
  version="5" emits the bare HTML5 doctype
- `xsl:element name="{...}"` AVT evaluation

libxslt suite 144/205 (+6: bug-25-, bug-35-, bug-117, bug-123,
bug-175, bug-182, bug-197, bug-206).



## [1.9.7] - 2026-08-28

### Added

- document-level PIs/comments are tree children of the document
  node (#580): one node chain per document —
  [prolog..., root, epilog...] in document order (the libxml2 model
  Nokogiri/lxml adapters build on). New leptris_document_node()
  navigation head; the #526 flat accessors and add_pi read/write
  the same store; comment/PI interleaving survives round-trips.

### Fixed

- XPath sees document-level nodes: /comment(),
  /processing-instruction(name), //comment(), //node(); document-
  order ranks keep mixed unions ordered across prolog/epilog
- libxslt suite 137 → 138 (bug-196 closes)



## [1.9.6] - 2026-08-27

### Performance

- variable-bound expressions evaluate on the bytecode VM with the
  compiled-handle cache — `leptris_xpath_compiled_eval_vars` on
  `//book[@id=$var]` at parity with the plain VM path (3.17 vs
  3.09 µs on a 2000-element document, was ~12 µs interpreter-bound);
  fused `//name[@attr=$var]` opcode served from the attr-value index
  bucket (#565) (xpath)

### Fixed

- `/descendant::name` and `/descendant::*` include the root element
  — the context is the document node; the old root-skip implemented
  element-relative semantics (NsAbsolutePaths, red on main since
  1.9.5) (xpath)
- `$var/step`, `fn()/step`, `(...)/step` paths compile the head
  expression as the path input — the head was silently dropped and
  the steps seeded from the context node (libxslt bug-76) (xpath)
- nodeset comparisons in the VM follow XPath 1.0 §3.4 any-pair
  semantics instead of comparing first nodes (xpath)
- libxslt suite 136 → 137 (bug-76 closes)

## [1.9.5] - 2026-08-27

### Added

- batched pull delivery + flat attribute fetch (#589, #562) (sax)
- batch-context evaluation — one expression, N contexts, one call (#560) (xpath)

### Fixed

- sizing pass writes out_len; 550/557 regression contracts (serialize)
- absolute paths seed the document node — //NAME and /descendant:: from document context offer the root element (bug-16-) (xpath)

### Performance

- VM fast paths for namespace-bound evaluation (#564) (xpath)
- reuse one subtree arena across iterparse yields (#563) (sax)
- ungate the close-tag masked compare for in-place parsing (#561) (parser)



## [1.9.4] - 2026-08-27

### Added

- iterparse v2 — full-document mode, namespace resolution, error channel (#586) (sax)
- chunked event delivery — records + packed arena drained in bulk (#585) (sax)



## [1.9.3] - 2026-08-27

### Added

- expose document-level comments; serialize epilog comments after the root (#578) (dom)

### Fixed

- accept dataless PIs — do not clobber the closing '?' when terminating the target (#577) (parser)
- musl GCC -Werror=incompatible-pointer-types failures (#582) (build)
- attribute-value normalization per XML 1.0 §3.3.3 (#576) (parser)



## [1.9.2] - 2026-08-27

### Fixed

- op_apply_templates frees the selection on the no-items exit (xslt)
- free the cdata-section-elements list; Win32 gates for 3 suite cases (xslt)
- namespace prefix/URI setters store pool copies — Linux LSan leak (dom)
- C89-portable size pin — MSVC C lacks _Static_assert (abi)
- portable static_assert spelling for C++ translation units (abi)
- LeptrisSerializeOptions layout is frozen — #568 segfault (abi)
- NaN via math.h — MSVC rejects constant 0.0/0.0 (C2124) (xslt)
- drop trailing whitespace on xslt_parse.c:248 (checks gate) (xslt)



## [1.9.1] - 2026-08-26

### Added

- ATTLIST default values materialize at parse time (XML 1.0 §3.3.2) (dtd)
- the serializer owns the whole HTML method — string post-passes deleted (serialize)
- EXSLT func:function — stylesheet-defined XPath functions (xslt)
- libxml2-parity HTML output — indent rules, meta injection, default method (xslt/serialize)
- core XPath PATH_EXPR heads + suite batch — 48/205 (xslt)
- §7.1.1 namespace copying + xsl:* attr stripping (suite-driven) (xslt)
- complete XSLT 1.0 — audit-driven gap closure (TODO.transform) (xslt)
- §12 function bridge live — key/current/format-number/generate-id/system-property/document + EXSLT (TODO.transform 04/05) (xslt)
- phases 02/04 essentials + variable transport (TODO.transform 02+04) (xslt)
- XSLT 1.0 core engine (TODO.transform 01) (xslt)

### Fixed

- NCNames carry non-ASCII; leading keyword tokens are NameTests (xpath)
- §3.4 strip/preserve-space NameTests resolve prefixes (xslt)
- §11 template scoping moved to the invocation seam (bug-42-) (xslt)
- id() resolves DTD-declared ID-typed attributes under any QName (§4.1) (xpath)
- namespace-URI resolution for prefixed patterns and /name tests (§5.3) (xslt/xpath)
- EXSLT func:param argument binding + function-ns output exclusion (xslt)
- EXSLT func:result content returns a node-set, not a string (xslt)
- element-less results keep the XML declaration (§16.1) (xslt)
- xsl:copy-of copies mixed content verbatim (§11.3) (xslt)
- call-template scopes to globals, not the caller's locals (§11) (xslt)
- xsl:strip-space '*' wildcard strips whitespace-only source text (§3.4) (xslt)
- position() reflects the in-flight node-list position (§12.4) (xslt/xpath)
- HTML method decodes &apos; in attribute values (xslt)
- LRE ns copy suppresses bindings already on any result ancestor (was parent-only — bug-150, bug-168). The result tree can have an LRE deeply nested, so the suppression must walk the full result-ancestor chain, not just the immediate parent. Suite 101/205. (xslt)
- current() during xsl:key use evaluation (xslt)
- NULL-safe namespace prefix strcmps — the default ns has a NULL prefix; the wrapper comparison segfaulted on the default-ns fall-through inside build_ns_copy. One xs_ns_strcmp helper guards prefix_excluded and the dedup compare. Three crash cases now pass on POSIX (stale-binary detections). (xslt)
- filter expressions — the parser's XPATH_AST_PREDICATE nodes (xpath)
- xsl:sort — multi-key (case 2+) + apply-templates support + strict numeric keys (xslt)
- non-numeric sort keys are NaN (§10 — they sort before all numbers ascending; strtod("")==0 was masquerading as 0 and splitting the number run, bug-120). Suite 91/205. (xslt)
- xsl:number corners — literal comment/PI drop, zero/negative rules, full roman (xslt)
- format-number optional fraction digits + level=multiple join (xslt)
- named decimal-formats are namespace-expanded names (§12.3) (xslt)
- declaration split + after-root chain in document order (xslt)
- AVT names + PI node string accessors + declaration ordering (xslt)
- foreign document nodes resolve their OWN root (xslt)
- document() yields the DOCUMENT node (§12.1) — /ch now selects the loaded document's root element; document('') the same for the stylesheet document (bug-153 family) (xslt)
- self:: from attribute nodes + ancestor-or-self document order (xpath)
- stylesheet whitespace — §2.4 as implemented by libxslt (xslt)
- copies carry IN-SCOPE namespaces (§7.5) — ancestor walk, innermost binding per prefix, redundant declarations (already bound identically on a result ancestor) suppressed (bug-128) (xslt)
- attribute nodes through templates — @-patterns, copy, control-char attr escaping (xslt)
- RTF fragments bind their document node (libxslt model) (xslt/xpath)
- xsl:processing-instruction content capture + HTML PI serialization (xslt)
- document-order fidelity for top-level comments/PIs + xsl:comment content (xslt)
- cdata-section-elements (§16.1) + prefix-preserving element copies (xslt/serialize)
- namespace declarations precede attributes in the open tag (serializer)
- the namespace axis, kind tests, and synthetic-node lifetimes (xpath/xslt)
- attribute-set chains resolve at use time (§7.1.4/§12.1.4) (xslt)
- copy-of namespace fidelity + present-but-empty attribute values (xslt)
- document-node semantics — the XPath root as initial context (§5.1) (xslt)
- comment/PI nodes in templates — node-kind pattern match, xsl:copy kinds, decl newline, indent=yes wiring (xslt)
- RTF variables build into a scratch doc (§11.1) — 49/205 (xslt)
- last strtok_r in strip-space list → xslt_strtok (xslt)
- MSVC build — HUGE_VAL over const division, gate regexp:match (xslt)
- free xsl:number compiled expressions (LeakSanitizer) (xslt)
- portable builds + letter_value leak (xslt)
- retain top-level comments (#550 sweep fallout) (serialize)
- RTF-as-nodeset, attr-set ordering, conformance specs (xslt)

### Performance

- document-cached function registry + dirty-skip varset — 2.3-3.7x transforms (xslt/xpath)




### In progress

- **XSLT 1.0 engine** (`leptris_xslt_parse`, `leptris_xslt_apply`,
  `leptris_xslt_apply_string`, `leptris_xslt_free`): compile-once
  stylesheet engine with function-pointer dispatch over an
  instruction forest. Phase 01 (pattern matcher), phase 02/03
  (stylesheet compiler + template engine), and phase 04
  essentials (RTF-as-nodeset variable transport, current-node
  tracking, decimal-format, attribute-set, include/import,
  cdata-section-elements) shipped on `feat/xslt-engine`. §12
  XSLT/EXSLT function bridge install, `xsl:apply-imports`,
  `xsl:text disable-output-escaping`, `xsl:sort lang/case-order`,
  `xsl:number lang/letter-value`, `xsl:fallback`, HTML output
  method, block-scope variable shadowing, and `unparsed-entity-uri`
  / `unparsed-entity-name` land before the version bumps.
- Planned next: XSLT 2.0 / XSLT 3.0 + XPath 3.1 + XQuery
  implementation sourced from Saxon-HE (TODO.xpath2).

## [1.9.0] - 2026-08-24

### Added

- **`LeptrisParseOptions.recover`** (#547): a parse failure returns
  an empty document with the failure recorded via
  `leptris_last_error` — the libxml2 `XML_PARSE_RECOVER` semantics
  the moxml/libxml2 adapters emulate, now honest in the native API

### Fixed

- **Rootless documents with PIs no longer serialize to ""** (#546):
  the serializer emits the declaration (when the source had one)
  and every document-level PI instead of silently dropping them
- **`serialize_into` options + single-serialization caching**
  (#541): both `_serialize_into` functions take a
  `const LeptrisSerializeOptions*`; the size-query + into-call
  pattern now reuses one serialization via a per-document cache
  (invalidated on mutation through the version stamp); the
  mem-cache string is freed at document teardown (Linux
  LeakSanitizer caught the leak)

## [1.8.0] - 2026-08-24

### Added — expanded-name attribute APIs + namespaces-correct
by-name semantics (#542), detached sibling inserts (#540)

- `leptris_element_attribute_ns` / `has_attribute_ns`: lookup by
  (URI, local) — prefix-agnostic, NULL URI = no namespace only
- `leptris_attribute_prefix` / `attribute_namespace_uri`: per-
  attribute accessors; URI resolves through the owning element's
  in-scope declarations at read time (mutation-correct)
- By-name accessor now follows the 5-point XML Namespaces spec:
  bare names match only no-namespace attributes; qualified names
  resolve and match cross-prefix; xml is prebound; undeclared
  prefixes are NULL; xmlns declarations are invisible
- `insert_before`/`insert_after` on detached siblings chain them
  (libxml2 unlinked-node semantics) — bottom-up construction works
- Fixed a latent serializer bug the semantics exposed: attribute
  prefixes were doubled when cached (ns:ns:attr)

## [1.7.0] - 2026-08-24

### Added

- **`leptris_node_children(parent, out, max)`** (issue #535): every
  child kind — elements, text, comments, CDATA, PIs — copied in one
  call, the same shape as `leptris_xpath_result_get_nodes_ex`.
  `out=NULL` is a count-only query returning the TOTAL across all
  kinds (`leptris_node_child_count` is elements-only and cannot size
  a mixed array). Bindings' `children` drops from N+1 FFI round
  trips to 1 call + N wraps
- **`leptris_document_serialize_into` /
  `leptris_element_serialize_into`** (issue #535): caller-buffer
  serialization — `buf=NULL` queries the needed size (including the
  NUL); with capacity the copy happens in the same call. The
  serialize + read_string + free_string FFI pattern collapses to one
  call with zero library-side allocations

### Fixed

- v1.6.2 (below): mixed-content pretty-printing — see 1.6.2

## [1.6.2] - 2026-08-24

### Fixed

- **Pretty-printing no longer inserts whitespace inside
  mixed-content elements (#534)**: an element with non-whitespace
  text or CDATA children emits verbatim (libxml2 semantics) — the
  inserted bytes used to become new text nodes on reparse,
  silently altering content and breaking serialize∘parse
  idempotency. Whitespace-only text still counts as formatting, so
  pretty documents keep indenting. Companion fix: pretty
  round-trips no longer double the blank lines on every pass (the
  formatter owns inter-element whitespace; ws-only text nodes are
  dropped) — serialize(parse(serialize(x))) is byte-stable in both
  compact and pretty modes

## [1.6.1] - 2026-08-24

Legacy purge + full public-surface spec coverage (#532). No API
change.

### Removed

- The legacy `archive/` tree (13 files, none compiled): old parser
  implementations superseded by direct_parse, the disabled DTD
  validator and XInclude prototypes, and the `.bak2` evaluator
  backup — all preserved in git history

### Testing

- **Every exported symbol is now exercised by specs: 217/217**
  (was 180). New test/abi/test_public_surface.cpp covers the 37
  previously untested functions — navigation, typed getters, the
  copy family, file I/O round-trips, document adopt/finalize,
  namespace declaration accessors, memory-hook getters — with
  failure contracts, not just happy paths. Confirmed under test:
  adopt_child keeps the adopted document's pool alive via the
  parent (the child must not be freed separately)

## [1.6.0] - 2026-08-24

All six user-filed issues fixed (#527 + #529); the issue queue is
empty.

### Fixed

- **Element serialization emitted following siblings (#523, in
  1.5.1)** and **unprefixed XPath name tests matched namespaced
  elements (#525, in 1.5.1)** — see 1.5.1 below
- **Same-parent moves corrupted the sibling chain (#518)**:
  insert_before/insert_after skipped the unlink when the moved
  node's old parent equaled the target, splicing a CYCLE into the
  child list — later sibling inserts and serialize hung forever.
  Both issue repros (the moxml adapter blockers) now serialize
  correctly
- **Union nodesets exposed dangling attribute pointers (#514)**:
  `a | a/@id` borrowed the operands' synthetic nodes, then the
  operand frees released them — mixed nodesets reported kind=OTHER,
  NULL name/value, garbage tags. Union results now inherit the
  ownership flags (both union sites)
- **Detached PI/comment/CDATA mutation failed on rootless documents
  (#519)**: fresh nodes resolved their document through the parent
  chain. PI/comment/CDATA carry an owner_doc backpointer stamped by
  the factories — bottom-up document building works

### Added

- **Document-level processing instructions (#526)**:
  `leptris_document_pi_count / _pi_target / _pi_data / _add_pi` —
  enumerate and create `<?target data?>` items outside the root
  (parsed and added alike; serialized after the declaration)

## [1.5.1] - 2026-08-24

Two correctness fixes (#527).

### Fixed

- **Element serialization emitted following siblings (#523)**:
  `leptris_element_serialize` walked past the requested subtree into
  every following element — wrong output (bindings' tostring
  returned the rest of the document) and an O(document) cost per
  call. The walk now stops at its root frame on every completion
  path. Single ~90-byte element in a 100-element document:
  6151 ns -> 119 ns (52x, ahead of lxml); document serialization
  unchanged; the non-NULL-options cost is within noise of NULL
- **Unprefixed XPath name tests matched namespaced elements
  (#525)**: XPath 1.0 §2.3 — an unprefixed test matches only
  no-namespace elements. `//note` no longer matches `<p:note>`,
  `//n` no longer matches a default-xmlns `<n>` (libxml2 / Nokogiri
  / REXML parity; unblocked the moxml adapter). Fixed at every
  VM inline matcher + the generic matcher; namespace-free documents
  keep the zero-cost fast path via a has_namespaces flag

## [1.5.0] - 2026-08-24

The TODO.engine board — post-v1.4.0 gaps from shipping the last two
boards.

### Added

- **File-backed streaming sources**:
  `leptris_pull_new_file(path)` / `leptris_iterparse_new_file(path)`
  — both APIs stream off disk in bounded 256-byte slices, no
  whole-document buffer; iterparse stays bounded by the largest
  subtree. Huge documents now parse from file end to end
- **Compiled XPath with contexts**:
  `leptris_xpath_compiled_eval_ns` / `_eval_vars` — the compiled
  handle on the namespace- and variable-carrying paths (same
  semantics as `leptris_xpath_eval_ns` / `eval_with_vars_context`,
  minus the per-call parse)
- **Rust crate publishing**: rust-release workflow (manual or
  workflow_call) — builds the C core, cargo test, cargo publish;
  packs with a setup warning until CARGO_REGISTRY_TOKEN is set.
  Crate bumped to engine lockstep 1.4.0 (was 1.1.1)

### Housekeeping

- TODO.md refreshed: leptris-py trusted-publisher note closed (wheels
  shipping since 1.3.x); ruby release bump note closed (verified
  fixed upstream)

## [1.4.0] - 2026-08-23

The full TODO.bindings board (issue #510 Tier 2/3) — the APIs that
were keeping the Python and Ruby bindings from parity.

### Added

- **Pull (StAX-style) parsing**: `leptris_pull_new/_next/_free` over
  the streaming SAX core — host-driven events with zero C→host
  callbacks (each costs ~µs through FFI). Attribute accessors during
  START_ELEMENT; ERROR/END_DOCUMENT events terminate walks; memory
  bounded by the input slice, not the document
- **Iterparse (bounded memory)**: `leptris_iterparse_new/_next/_free`
  — each top-level child of the root materializes in its own pool and
  is released on the next yield; peak memory bounded by the largest
  subtree, not the document (v1: QNames as written, prefixes not
  re-resolved)
- **Compiled XPath**: `leptris_xpath_compile/_compiled_eval/
  _compiled_free` — one parse + one pinned cache entry for the
  handle's lifetime; skips the per-call hash + cache probe of
  `leptris_xpath_eval`. Immutable handle: concurrent evaluation from
  many threads is safe (4-thread one-handle stress spec'd)
- **Per-parse options**: `LeptrisParseOptions` +
  `leptris_parse_string_ex` — scoped strict/depth/flags with thread
  defaults restored on return; retires the need for the thread-global
  setters in shared-thread bindings. `LeptrisParseFlags` moved to
  `types.h` (single canonical types source)
- **Mutation surface proven**: DomBuilder round-trip spec (build →
  serialize → reparse → verify, all node types + attributes) and
  deep-copy spec — the construction/mutation API shipped across
  earlier releases and now has end-to-end proof

### Fixed

- Serialization encoding declarations never lie: with iconv (default)
  bodies are UTF-8 and declarations always say so; without iconv,
  bytes pass through unchanged and declarations echo the original
  encoding. `serialize(parse(serialize(x)))` is byte-stable



## [1.3.0] - 2026-08-23

### Concurrency & FFI (TODO.concurrency, issues #508–#510)

- **Thread-safe error reporting**: `leptris_last_error()` is now
  thread-local; parse failures record a message with byte offset;
  failing XPath evaluations snapshot the reason into a per-document
  slot — new `leptris_document_last_error(doc)` (also fixes the
  v1.2.0 header/binary drift of that symbol, #508)
- **Parse-error positions**: `leptris_last_error_position(&line,
  &column)` — 1-based, for XMLSyntaxError/Nokogiri parity (#510)
- **Export-surface gate**: CI + ctest diff the shared library's
  exported symbols against every `LEPTRIS_API` declaration
  (196 == 196); header/binary drift now fails the build. The
  internal `leptris_parse` family is no longer exported
- **Batch FFI accessors**: `leptris_xpath_result_get_nodes_ex`
  copies every mixed-nodeset entry with its kind;
  `leptris_element_children` bulk-fills child elements
  (`leptris_element_child_count` for sizing) — closes the per-item
  dispatch gap to lxml/libxml2 in bindings (#509)
- **Threading contract documented** (README "Threading model"):
  one-document-per-thread needs no locking, read-only sharing is
  safe; the XPath AST/bytecode cache is mutex-guarded and
  pin-counted (fixes a real use-after-free under concurrent first
  evaluation); new `leptris_thread_cleanup()` drains per-thread
  caches for ephemeral worker threads. Full suite is
  ThreadSanitizer-clean
- **EXSLT extension pack**: `leptris_exslt_enable(doc)` activates 15
  native handlers — str:replace/tokenize/split/concat/padding,
  set:distinct/intersection/difference/leading/trailing,
  math:max/min/abs/sqrt/power. str:tokenize = character-set
  delimiters; str:split = whole-pattern substring
- **Namespace set from pairs**:
  `leptris_xpath_ns_set_new_from_pairs(flat, n)` — one FFI call for
  the whole prefix/URI array (layout matches the c14n
  inclusive-namespaces argument)

### Fixed

- `leptris_version()` reported a stale hand-written fallback — the
  CMake version defines now reach `core.c` in all build modes
- Windows DLL renamed to `libleptris.dll` (matching every other
  platform); Rust binding links the right import library
- Release workflow bump commit referenced the removed
  `bindings/python/pyproject.toml` path and died before opening the
  release PR

### Board

- `TODO.bindings/` — the Tier-2/3 binding asks from #510 tracked
  for execution (mutation/construction, iterparse, compiled XPath,
  pull API, per-parse options, encoding guarantees)



## [1.2.0] - 2026-08-23

### Added

- External namespace bindings for XPath: `LeptrisXPathNsSet` +
  `leptris_xpath_eval_ns` resolve prefixed name tests by NAMESPACE
  when the expression prefix is bound, matching elements that carry
  the namespace via any prefix or the default namespace. XPointer
  `xmlns(prefix=uri)` components now bind subsequent `xpointer()`
  bodies instead of being skipped.
- Fully automated release PRs: changelog drafted from conventional
  commits, notes embedded in the PR body, PR created by the workflow.

### Fixed

- `prefix:*` name tests were namespace-blind everywhere (compiler
  fusion and matcher both ignored the prefix) — `//t:*` matched every
  element in the document; now namespace-scoped.
- The mutation-block fast path in `leptris_element_create` skipped
  root→doc registration, so `leptris_document_set_root` rejected
  programmatic roots and detached elements could not resolve their
  document. (Found via moxml's adapter contract, lutaml/moxml#96.)
- Serializer round-trip: CDATA sections containing `]]>` split across
  two sections (libxml2 technique); element/attribute names emit
  `prefix:name` so namespace bindings survive reparse.
- `leptris_document_free` freed mutation blocks before the root-map
  unregister — use-after-free for documents with programmatic roots.
- pyleptris build/publish gates survive the workflow_call event (the
  release flow's tag job runs on pull_request closed).



## [1.1.2] - 2026-08-23

### Fixed

- XPath union dedup was O(n^2) in the merged-set size — `//name |
  //item` on a 20k-element document took 229 ms per query (99% of it
  in the per-candidate duplicate scan); the document-order sort now
  compacts duplicates, at 1.6 ms (~140x faster). Multi-context step
  results also dedup at any size — results past 32 entries silently
  kept duplicates from nested descendant contexts before.
- Element-index build scanned the distinct element names, attribute
  names, and attribute values linearly per entry; documents with many
  distinct values (e.g. 20k unique id attributes) paid a one-time
  300 ms spike on the second query. All three bucket lookups are
  hashed now (~75x on that spike).
- `//node()` now selects the root element per XPath 1.0 (it is a
  child of the document node); previously the result silently
  omitted it.
- `leptris_error_message` / `leptris_last_error` were declared in the
  public header since the first release but never implemented —
  phantom symbols that linked nowhere (MSVC rejected them). Both are
  implemented and exported now.
- pyleptris releases: the PyPI publish trigger now actually fires —
  tags created by the release workflow never triggered the publish
  workflow, which is now called directly from the release flow; the
  Python binding version tracks releases via bump-version.sh, and
  its test suite runs on all three platforms per PR.

### Added

- Rust bindings (`bindings/rust`, crate `leptris`): safe
  Document/Element wrappers with Drop, LeptrisStatus→Result error
  mapping, attribute and child iteration, mixed-nodeset XPath
  accessors, SAX with closures; CI on ubuntu/macos/windows.
  TODO.remaining/05 — the roadmap board is now empty.


## [1.1.1] - 2026-08-23

### Fixed

- XPath mixed-nodeset results: the synthetic attribute/namespace/
  text node tags collided with the public node-kind values real DOM
  nodes carry, so `//node()` misclassified entries and
  `leptris_xpath_result_node_name` could crash on text entries
  (#477). One unified tag space now classifies every nodeset entry;
  `node_kind` / `node_name` / `node_value` / `get_node`+`node_get_type`
  agree, and `node_name` also reports element names.
- `name()` / `local-name()` / `namespace-uri()` read their argument's
  first node after freeing the result that owned the synthetic
  attribute node — a use-after-free on GCC/ASAN toolchains (#477).
- Bare `text()` / `node()` in expression position parsed as unknown
  function calls, so predicates like `a[text()='x']` never matched;
  `text()` also no longer double-counts elements that merely contain
  text (#477).
- String-values for text / CDATA / comment / PI nodes via `string()`
  and comparisons (#477).
- Nodeset results are now true document order: multi-context steps
  and the union operator previously appended per context node (#485).
  Reverse axes (ancestor / preceding families) return reverse
  document order per the spec.
- aarch64 GCC (glibc and Alpine musl) could not compile
  `simd_text_neon.c`: the `vshlq_u16` shift vector now uses the
  ACLE-mandated `int16x8_t`; `__builtin_ctz` routed through the
  portable `LEPTRIS_CTZ` shim for MSVC ARM64 (#487).
- `python-publish.yml` carried a duplicate top-level `name:` key that
  produced a startup-failure run on every branch push.

### Performance

- `//text()` / `//node()` / `//comment()` /
  `//processing-instruction()` compile to a single pre-order walk
  emitting matches directly in document order — `//text()` is ~7x
  faster than 1.1.0 on a 20k-element document (#485). The
  document-order rank table used elsewhere is cached per document
  and invalidated on mutation.


## [1.1.0] - 2026-08-22

### Added

- Attribute handle iteration: `leptris_element_first_attribute`,
  `leptris_attribute_next`, `leptris_attribute_get_name`,
  `leptris_attribute_get_value` — O(n) enumeration where the index
  accessors re-walk per call (O(n^2)).
- Mixed XPath nodesets, consumable publicly:
  `leptris_xpath_result_node_kind` / `get_node` / `node_name` /
  `node_value`. `//a/@x` results no longer require internals.
- `leptris_element_prefix` — the element's own namespace prefix.
- `leptris_document_get_dtd` + `leptris_dtd_parse_external_subset` —
  external DTD subsets with application-owned I/O; parameter
  entities and INCLUDE/IGNORE conditional sections work in them.
- Public node-kind enum (`LeptrisNodeKind`) — bindings no longer
  hardcode numeric node types.
- CI: a Windows DLL export check that diffs leptris.dll's export
  table against the declared public surface, and an FFI mirror
  drift gate (ctest) that fails on phantom symbols, arity drift, or
  a missing core surface in the Ruby/Python bindings.
- `scripts/gen-api-docs.sh` — regenerates the Doxygen reference
  with the version read from CMakeLists.txt.

### Fixed

- Windows DLL exports (issue #430, two layers): the five public SAX
  functions were defined without LEPTRIS_API, and — beneath that —
  five headers carry standalone LEPTRIS_API mirror blocks that
  lacked the LEPTRIS_BUILDING_DLL branch from issue #278, so any
  translation unit including one before leptris.h compiled public
  definitions with a toothless macro. All mirrors now carry the
  canonical block; every leptris-ruby Windows failure at
  `attach_function :leptris_sax_parse` traces to this.
- DTD: declarations delivered by parameter-entity substitution or
  INCLUDE conditional sections were silently discarded (the
  recursion built a fresh DTD and dropped it); duplicate
  declarations freed pool-owned memory (heap corruption);
  self-referential parameter entities overflowed the stack (now
  depth-capped); the 8 KB substitution cap is gone.
- XPath: `leptris_xpath_result_get` passed attribute-tagged nodes
  through miscast as elements; it now returns elements only, with
  the mixed-nodeset quartet for the rest.
- ATTLIST re-declaration honors XML 1.0 §3.3 (first declaration
  binding); internal-subset declarations take precedence over
  external subsets.
- Bindings: Ruby/Python call the declared
  `leptris_document_serialize` (both had attached the undeclared
  legacy alias with a phantom second argument); the legacy
  `leptris_serialize_document` alias is now declared in leptris.h;
  Ruby's `leptris_element_attribute` attach corrected to the real
  2-arg signature.

### Changed

- The CLI is built on the public API only — the layer contract in
  CLAUDE.md is enforced by construction; the XML formatter's
  attribute walk dropped an O(n^2) index pattern for handle
  iteration.
- The DTD validator walks the tree through the public interface;
  attribute values are validated entity-expanded.
- Attribute access has one implementation face (element_query.c);
  the four attribute numeric getters share documented conversion
  cores.
- Removed the phantom declaration `leptris_parse_string_compact`
  (never defined, never exported), 756 lines of dead alternate
  element representations (element_compact.c, element_fast.c), and
  duplicate declarations of `leptris_element_child_value` /
  `leptris_element_remove_children`; headers render
  Doxygen-warning-free.
- Warning-clean build restored; perf-regression specs use
  min-of-per-rep ratios (no more CI flakes on shared runners);
  shared-library export surface audited at 176/176.


## [1.0.0] - 2026-08-21

### Changed

- **The rebrand: taurus is now leptris** — every public symbol, path, and
  artifact renamed (`TaurusDocument` → `LeptrisDocument`,
  `taurus_parse_string` → `leptris_parse_string`, `libtaurus` → `libleptris`,
  `pytaurus` → `pyleptris`; library at `leptris/leptris`; Ruby gem `leptris`).
  Breaking ABI and API rename — hence 1.0.0, the natural freeze point.
- The name: the three hares of Dunhuang — three hares in a circle, three
  ears among them. Speed of parse and edit; containment — hard resource
  bounds and self-sufficiency.
- CLI `--version` now reports the library version (was a hardcoded 0.1.0).

### Fixed

- XPath result double-free corrupted the internal free-list (CodeQL
  critical): free-list entries park behind an internal sentinel and a
  repeat `leptris_xpath_result_free` is a no-op.
- All allocation-failure, large-document, and crash-class suites from
  v0.26.8 carry over renamed; workflow permissions tightened; every
  remaining CodeQL annotation cleared.



## [0.26.8] - 2026-08-21

### Fixed

- **Serialize crash on ~300 KB+ documents containing comments/CDATA/PIs mixed
  with text** (same family as #450, found by the new large-document suite):
  those nodes were individually pool-malloc'd, landing in a different malloc
  region than the parse arena — sibling distances beyond ±2 GB that no compact
  edge can hold raw. They now carve from the same contiguous parse block as
  elements and text; their sibling edges widened to int32 (size-neutral).
- Heap corruption on document free when a document had overflow-table
  entries (common since v0.26.6): cleanup unlinked-and-freed entries that
  live in table-owned slabs. Entries are now unlinked only.
- The public allocation hooks (`leptris_set_memory_management_functions`)
  now cover arena allocations — custom allocators see every byte, and
  allocation-failure injection works on the parse path.
- CodeQL high (bounded accumulator pattern) in the decomposition benchmark.

### Added

- CI large-document suite: six document shapes × sizes from 30 KB to 48 MB
  (30 KB–1 MB under sanitizers), each running the full lifecycle twice —
  parse → complete tree walk → XPath → serialize → reparse → idempotence →
  free — plus mutation-at-scale and inplace-parse specs.
- CI allocation-failure suite: parse/serialize/mutate under a countdown
  allocator across the first 256–512 allocation sites; any outcome except a
  crash is acceptable; a canary spec fails if the hook stops covering parse.


## [0.26.7] - 2026-08-21

### Changed

- `leptris_element_set_attribute` 13% faster (180 → 157 ns/call, flat scaling to
  4000 attributes): mutation attributes now carve from a per-document
  40-byte-stride block (adjacent attributes keep their compact sibling edges
  in-range), with names and values from the shared mutation name block. Long
  values and allocation failures fall back to the pool.


## [0.26.6] - 2026-08-21

### Fixed

- **Critical (#450)**: serialize segfaulted (use-after-free) on documents of
  roughly 90 KB+ containing an encoding declaration. Root cause: the parser
  stored whitespace-text → element sibling links with a raw 2-byte compact
  pointer whose ±256 KB range is exceeded once the parse-time element and text
  blocks grow with the document; the value silently truncated and tree walks
  decoded into stale memory. The regression window measured v0.26.1 clean →
  v0.26.2 crashing (the 40-byte attribute struct shifted block distances);
  the underlying bug is as old as the bulk-block parser. Text sibling edges
  are now int32 (size-neutral: the text node stays 64 bytes) and
  comment/cdata/pi links route through the overflow-table encoder.
  Regression spec added (1200-user mixed-content document, byte-exact
  round-trip).


## [0.26.5] - 2026-08-21

### Changed

- Mutation append ~2x faster (37 → 19 ns/element; 400 → ~195 µs for a
  10k sequential build): mutation element names now carry an 8-byte
  document backpointer, so unattached elements resolve their document
  statelessly — the per-append root-map register/unregister pair (~11 ns)
  is gone entirely.
- Element name storage for mutation-created elements uses per-document
  contiguous blocks instead of the general pool path.

### Fixed

- `leptris_element_set_name` never updated the element's cached name
  length, so a renamed element serialized only the first N bytes of its
  new name (N = the OLD name's length). The document is also now resolved
  before the rename replaces name storage, keeping later mutations
  working on renamed elements.


## [0.26.4] - 2026-08-20

### Changed

- Mutation append 40% faster (59 → 37 ns/element): the thread-local
  root→doc map now marks membership with a header bit — never-registered
  elements prepend without the duplicate-check walk, attach paths remove
  the child's entry, and unlink re-registers the orphan. Map chains
  previously accumulated every element ever created (~40 deep after 10k
  appends, 75% of the append loop).
- Text-heavy serialization 2.3× faster (919 → 400 µs on a 2MB single-text
  document, ahead of pugixml's 644 µs): text-mode escape runs of 256+ bytes
  locate the next special character with SIMD scans instead of a per-byte
  table walk; short runs and attribute values keep the table scan.


## [0.26.3] - 2026-08-20

### Fixed

- **Critical**: `leptris_element_set_attribute` on a name it had previously
  created inserted a duplicate attribute instead of updating it
  (`<r x="1" x="2"/>` — an XML spec violation). The doc-level attribute
  index stored hash 0 for every mutation-created attribute, so the
  duplicate probe never matched. Regression spec added.
- The same hash-0 keying made programmatic attribute builds O(N²):
  setting 2000 attributes went from 388µs (v0.26.1) to 1587µs (v0.26.2);
  now 383µs with flat scaling to N=8000. Index keys use a full 32-bit
  FNV independent of the attribute struct's 15-bit lazy hash, and the
  15-bit hash gains an avalanche finalizer (raw truncation mapped
  names differing in their last digit to hashes exactly 256 apart).

## [0.26.2] - 2026-08-20

### Changed

- Attribute struct 48 → 40 bytes (hot/cold split, pugixml density): the
  namespace side-cache pointer becomes a self-relative offset and the entity
  flag folds into the name-hash word. Attr-heavy parse improves ~1% at every
  attribute density (K=5/20/50/100) and attribute memory drops 17%. With
  32/40/48/56/64-byte strides all measured, the attribute layout axis is
  closed at 40 bytes.
- The attr name hash narrows to 15 bits with a shared attr_hash15() helper on
  both lazy-compute and query sides (it is a pre-filter only; every hit is
  confirmed by memcmp).

## [0.26.1] - 2026-08-20

### Changed

- Serialize view-direct: entity-free borrowed text is read directly from
  the parse buffer instead of materializing a pool copy per node —
  text-heavy serialization reaches parity with pugixml (1.38x → 1.04x).
- Mutation element bump block: elements created via the mutation API are
  carved from a per-document contiguous block (1024 per allocation,
  chained, freed with the document) instead of individual pool mallocs —
  sequential append is ~3x faster with zero heap fragmentation.
- Removed two static escapers orphaned by the inline escape emitter
  (TODO 194d); builds are warning-clean again.

## [0.26.0] - 2026-08-20
### Added — full-feature benchmark matrix (leptris vs pugixml vs libxml2)

Two new benchmark tools under `benchmarks/matrix/`:

**`bench_matrix`** (C++): measures all three libraries across 15 benchmark shapes in 5 feature categories — DOM parse (4 shapes), SAX parse (4 shapes), serialize (2 shapes), mutation (append 10k, set-attr 2k), and XPath (3 queries). Records latency (min/median), throughput (MB/s), CPU (user/sys via `getrusage`), and peak RSS (KB). Writes one YAML file per library.

**`bench2html.rb`** (Ruby, stdlib only): generates a self-contained HTML report from the YAML output with a feature-support matrix (which libraries implement which capabilities), per-feature comparison sections with winner badges and ratio indicators, Chart.js bar charts per shape, and a resource-usage table. Dark theme, responsive.

Usage:
```bash
cmake --build build --target bench_matrix
mkdir -p build/bench-matrix
./build/benchmarks/matrix/bench_matrix build/bench-matrix
ruby benchmarks/matrix/bench2html.rb build/bench-matrix/*.yaml -o build/bench_report.html
```


## [0.25.11] - 2026-08-20
### Performance — overflow-table growth for large mutation trees

The compact overflow table kept a fixed 256-bucket chained hash forever. Mutation-created elements live in a different malloc region than the parse arena (far beyond the int32 compact field), so every child→parent edge takes the overflow path. With N children the chains grew to N/256 deep and every set/get walked them — the measured rising cost of sequential append past a few thousand children.

Load-factor growth doubles the bucket array and rehashes when `entry_count >= bucket_count`; chains stay O(1). Parse never touches this table (`direct_parse` is overflow-free), so parse timing is unaffected.

### Documentation

- `README.adoc` DOM-writes table: current quiet-machine numbers — append **~2.5× ahead** of pugixml, setattr **~20× ahead** of duplicate-rejecting pugixml (blind-append comparison noted separately).
- Decomposition benchmark gains P6 pretty-ws and P6b `DROP_WS` probes (P6b **1.20×** vs pugixml).


## [0.25.10] - 2026-08-20
### Performance — SAX round: streaming parse now beats libxml2 on every shape

The first SAX benchmark (`benchmark_sax`, libxml2-gated — pugixml has no SAX interface; libxml2 is the streaming reference) exposed that our SAX path ran 2-3x slower than our own DOM parser: the streaming element frame was heap-allocating and freeing the element name **per element**, behind a comment claiming "well under 1% of parse time" (profiling showed ~10%).

- Names up to 47 bytes and attribute arrays up to 12 entries now live inline in the element frame — the LIFO nesting discipline makes frame storage exactly the right lifetime, surviving scratch-arena reallocs across feed() chunks. Oversized cases spill to the heap via malloc-and-copy.
- **Fixed en route**: a no-attribute `start_element` handed handlers a non-NULL attrs array with an uninitialized first slot — caught by the ASAN differential suite as a handler SEGV, now NULL-terminated on push.

SAX vs libxml2 SAX2 (min-of-30, no-op handlers both sides): small **0.40x**, xsdtest **0.76x**, large **0.91x**, workflow **0.93x**, catalog **0.89x** — leptris ahead on every shape (the three large shapes were losing at 1.03-1.10x before). Method and readings documented in benchmarks/README.md.

Read-mode scoreboard: DOM parse (attributes at the measured 1.5x source floor, everything else parity-or-won), SAX **won**, XPath **won up to 87x**.


## [0.25.9] - 2026-08-20
### Performance — round 12: the attribute floor, measured

- **Header-inline StringView constructors**: `leptris_sv_from_ptr` / `_from_cstr` / `_empty` are pure two-field initializers called twice per attribute by the parser; they now inline from the header in every build configuration (thinLTO already did; non-LTO builds and drivers get the guarantee). Perf-neutral in Release, gated at ±0.4%.
- **The attribute investigation's conclusions, on the record**: the out-of-line calls seen in profiling were a no-LTO artifact; the table-driven value probe (pugixml's single-lookup shape) measured as an exact wash (one table load plus re-dispatch equals two register compares); every remaining per-attribute item is ±1ns and the layout wall blocks their fusion. Attributes at ~1.5x are the measured source-level floor of the current architecture.

Full standings vs pugixml on this build: text streaming won ~3x; elements and text nodes at parity; whitespace mode 1.35x; attributes ~1.5x (floor); append 2.5x ahead; set_attribute ~20x ahead of the duplicate-checking equivalent; serialize at parity-or-ahead on every shape; XPath ahead up to 87x. The decomposition benchmark (`benchmark_decomp`) and its README section document how each component is measured.


## [0.25.8] - 2026-08-19
### Performance — pugixml-parity whitespace mode + the decomposition benchmark

- **`LEPTRIS_PARSE_DROP_WS_TEXT`** (new `leptris_parse_string_flags` API): the decomposition benchmark's whitespace probe found the corpus gap's hidden driver — whitespace-only text nodes, one per element in pretty-printed documents, each paying the full text-path entry machinery. pugixml discards them by default; libxml2 keeps them. The leptris **default keeps them** (libxml2-faithful, required for byte-exact pretty round-trips); under the flag, whitespace after markup is eaten in the main loop before the text path engages and mixed runs start at the first non-whitespace byte — pugixml's observable semantics, pinned by specs. Whitespace-heavy shapes: 1.68x -> 1.35x vs pugixml; default-mode timing unchanged everywhere.
- **`benchmark_decomp`**: five shape-isolating probes (text streaming / tiny elements / attributes / text nodes / size scaling) that answer "is the gap time or throughput?" — documented in benchmarks/README.md. Readings: text streaming leptris wins ~3x (zero-copy views); per-element and per-text-node at parity; attributes ~1.5x; small-document ratios are a cache-footprint effect.

### Fixed

- `element.c` indexed child walk: the loop counter was `uint8_t` against a `size_t` index — it wraps for index > 255 and the intended early-exit never fires (CodeQL high). Now `size_t`.
- Benchmark generators: `snprintf` accumulation could pass the buffer end on truncation and underflow the next size argument (CodeQL high) — clamped everywhere via a `snappend()` helper.

### CI

- 15-minute timeouts on the checks jobs, preinstalled clang-format (no apt), apt retries for cppcheck: the Ubuntu mirror stall plague (seven occurrences over two days) now fails visibly instead of hanging for up to six hours.


## [0.25.7] - 2026-08-19
### Performance — parse round 10: real-world corpus 1.32-1.72x vs pugixml

The fifth application of the short-scan law (libc call setup dominates sub-16-byte spans): the close-tag match called `memcmp` per element for names that are typically 3-7 bytes. Short names on the parser's owned copy now compare via one masked 64-bit load per side; the 8-byte load past the name is guarded by the zeroed slack tail, and in-place parses keep plain memcmp by construction.

Corpus standings: large **1.32x**, workflow **1.34x**, catalog **1.47x**, medium 1.50x, xsdtest 1.62x (from 1.39/1.39/1.54/1.50/1.72 in v0.25.6). Attribute benchmark flat. Cumulative arc: 1.54-1.91x at the start of 2026-08-18 to 1.32-1.62x now.


## [0.25.6] - 2026-08-19
### Performance — parse round 9: real-world corpus 1.39-1.72x vs pugixml

A line-level profile of the corpus (the honest battleground — the attribute benchmark had been flattering us) found two items:

- **Chunked text-block growth.** The v0.25.5 bulk text block sized itself by the tag count, which under-counts when comments/PIs/CDATA interleave with text (each interleave can split a run); overflow fell back to the out-of-line pool path per node (8% of the xsdtest profile). The cursor now grows by 128-node arena chunks — one bump per 128 nodes.
- **Deterministic split-inline.** The element name walk's inlining was left to the compiler's per-build choice; `LEPTRIS_ALWAYS_INLINE` pins the round-7 win across build configurations.

Corpus standings: medium 1.50x, large 1.39x, xsdtest 1.72x, workflow 1.39x, catalog 1.54x (from 1.60/1.44/1.81/1.51/1.64 at round 8). Attribute benchmark flat. Cumulative day: attr benchmark 1.41/1.44/1.87/2.05x -> 1.38/1.40/1.48/1.51x; corpus 1.54-1.91x -> 1.39-1.72x.

### CI

- 30-minute timeout on the AddressSanitizer (Linux) job: Ubuntu mirror stalls inside apt-get several times a day (caught in the act four times across 2026-08-18/19), previously hanging up to six hours. The slowest healthy run is 20.1 minutes; 30 keeps 1.5x headroom.


## [0.25.5] - 2026-08-19
### Fixed — critical: binding_wrapper NULL on all parse-created nodes (#421)

The text/comment/CDATA/PI node creators initialize every base field except `binding_wrapper`. Fresh mmap-backed arenas had zeroed it by luck for the library's entire life; the retained-arena free list introduced in v0.25.3 recycles dirty pages, so a second parse in the same process could return a stale, non-NULL wrapper pointer into the previous document's memory from `leptris_node_get_binding_wrapper` — dangling by construction, and reachable from the language bindings. All four creators now set it explicitly, pinned by a spec that parses the same document twice (the second parse draws recycled arena memory) and asserts NULL on every node type.

### Performance — bulk text-node block (round 8, modest)

Text nodes now carve from a bulk block like elements and attributes (upper-bounded by the tag count; overflow falls back to the pool path), removing the last per-node out-of-line call from the element/text path. K=5 -0.3% to K=20 -1.3% across the many-attrs sections, no regressions.

Parse standings vs pugixml on this build: many-attrs **1.38/1.42/1.48/1.51x**; real-world corpus 1.44-1.81x; append 2.5x ahead; set_attribute ~20x ahead of the duplicate-checking equivalent; serialize at parity-or-ahead on every shape; XPath ahead up to 87x.


## [0.25.4] - 2026-08-19
### Performance — parse round 7: real-world corpus 1.54-1.91x -> 1.45-1.79x vs pugixml

The attribute-heavy benchmark had been flattering us; profiling the real-world corpus (XSD test suites, catalogs, workflow documents) found two element-path costs:

- **Close-tag colon scan only for prefixed opens.** Every `</name>` match called `memchr` to find a prefix colon in what is usually a 1-6 byte name — a libc call per element for the short-scan setup law (the TODO 174 finding, fourth application). The scan is only needed when the open element was prefixed, detected exactly via `open->name[-1] == '\0'` (the open tag's colon was NUL-terminated in place; unprefixed names directly follow `'<'`). Non-namespaced documents now perform zero colon scans; accept/reject behavior is provably identical.
- **dp_split_hash_name re-inlined.** The noinline attribute was a verdict from three layout regimes ago; after the attr diet, lazy lines, and probe slack reshaped the surrounding function, inlining wins every section. Names shorter than 3 bytes skip the colon test in the walk entirely.

Cumulative parse standings vs pugixml today (from 1.41/1.44/1.87/2.05x): many-attrs benchmark **1.40/1.42/1.50/1.51x**; real-world corpus **1.45-1.79x** (medium 1.60x, large 1.45x, xsdtest 1.79x, workflow 1.46x, catalog 1.67x).

Also in this release (v0.25.3 items, for the record): the retained-block free list (page-fault tax, K=50 -15.5%/K=100 -17.3%), the attr-loop diet, lazy line resolution, the serialize matrix at parity-or-ahead on every shape, and the mutation attribute index (set_attribute 26x, append 2.5x ahead of pugixml).


## [0.25.3] - 2026-08-18
### Performance — the page-fault tax, the attr-loop diet, and lazy lines: high-attr parse −18% to −21%

- **Retained-block free list (#415):** libc mmaps large blocks and munmaps them on free, so every parse/free/parse cycle re-faulted the whole arena (0.21 us/page — ~400 us of a 1.36 ms parse at 100 attrs/element). Freed arena blocks and the parser's input-copy buffer now round through a bounded free list (4 blocks / 32 MB) and keep their pages mapped. K=50 −15.5%, K=100 −17.3%; macOS leaks still 0.
- **Attr-loop diet + lazy line resolution (#416):** a running attr cursor (no per-attr stride multiply), first-attr-only offset math, a parser-local attr counter, and unconditional probe windows over a 64-byte zeroed slack on the owned copy. Line numbers (#223) became lazy: parse stores byte offsets and `leptris_node_line` resolves via a per-document newline table with in-place caching — the scan loops carry zero '\n' compares. Identical results through the resolver (all #223 and nokogiri line specs pass); documents ≥ 2 GiB report unknown lines.
- Standings vs pugixml on the many-attrs benchmark: **1.40x/1.42x/1.48x/1.52x** (was 1.41/1.44/1.87/2.05 at the start of the day).

### Changed

- `leptris_node_line` now resolves lazily and caches; the buffer-immutability contract (already required by StringViews) covers the resolution

- **Mutation attribute index + attr-tail cache (#417):** `leptris_element_set_attribute` carried two hidden O(N^2) walks — the duplicate-name check and the append tail walk. A lazily-built per-document (element, name-hash) attribute index plus an attr-tail cache (the child-tail twin) makes programmatic attribute builds O(1): 2000 attributes on one element went from 11.1 ms to 426 us — 20-30x faster than pugixml's duplicate-checking equivalent. Append re-measured: 2.5x ahead of pugixml.


## [0.25.2] - 2026-08-18

### Performance — serialization at parity or ahead of pugixml on every shape

The write side of the pugixml parity mandate is now closed. Measured
on the same driver, same process (leptris vs pugixml, min-of-N):

- attr-heavy (K=50, 847 KB): **1.4x faster than pugixml**
  (0.67-0.73x ratio) — attribute values serialize from the stored
  view length instead of re-deriving strlen per attribute
- namespaced (218 KB): **ahead** (0.91x) — same view-length fix on
  the iterative walker (the first fix landed only on the recursive
  fallback)
- text-heavy raw (1.17 MB): parity (0.97-1.02x, was 1.29x) —
  escape-class table in the escaper (one table load + test per byte
  instead of 3-4 compares; 57% of the profile) and a name_len byte
  in the element struct's last padding byte (sizeof stays 64;
  0xFF marks >254-byte names with strlen fallback) killing the
  per-element strlen
- text-heavy pretty: parity (0.97-1.06x, was 1.13-1.26x) — the
  total-fusion leaf path now covers pretty mode (one reservation
  for indent + open + text + close + newline; previously ~8
  capacity-checked appends per leaf)

Byte-identical output verified against the previous release on a
mixed document; added `SerializeOptions.PrettyTextLeavesStayOnOneLine`
after a blank-line regression sailed through the suite (pretty
round-trip specs had never covered text-only leaves).

Also shipped in this release: the parse sentinel-attr-loop and
fused name walk from #411 (K=50 -3.6%, K=5 -3.2%) and the Windows
DLL export fix for #278 (#410).

### Fixed

- Windows: `LEPTRIS_API` now accepts `LEPTRIS_BUILDING_DLL` so
  shared-library builds export the full surface; CI gained a
  shared-only Windows build to keep it green (issue #278)


## [0.25.1] - 2026-08-18

### Performance — sentinel-terminated scans; first all-K parse improvement

The parse buffer has always carried a NUL sentinel (both the copy
and in-place entries write `buf[len]`, and NUL classifies as no
character class) — but every scanner loop still paid a per-byte
bounds check. pugixml's scans are sentinel-terminated.

- `dp_skip_ws` drops its bounds check (the newline line-tracking
  check remains)
- `dp_scan_name` drops its bounds check and takes pugixml's exact
  `SCANWHILE_UNROLL` shape: four plain byte loads per iteration
  with `__builtin_expect`-marked exits. The earlier failed fast
  path was SWAR — a different shape whose mask setup dominated
  short names; this one keeps byte loads and amortizes only the
  loop counter.

Measured (8-run interleaved fresh-directory Release A/B, minimum
per section): K=5 −3.6%, K=20 −1.9%, K=50 −2.6%, K=100 −4.6% —
the first change in the parse campaign to improve all four
attribute-density sections simultaneously, breaking a string of
eleven gated failures. Both parse entry points were verified to
write the sentinel before the conversion.

558 tests, ASAN clean, zero leaks.


## [0.25.0] - 2026-08-18

### Performance — iterative serializer + total-fusion leaves (TODO 194e/f)

The element serializer's structural endgame, closing the
serialization campaign (194a-f):

- **Iterative sibling walk** (pugixml's `node_output` shape): one
  frame with an explicit descent stack replaces the per-child
  recursive call. Depth beyond 512 falls back to the recursive
  walker — which also removes the recursion depth limit for deep
  mutation-built trees.
- **Total-fusion leaf path**: a compact-mode leaf element with no
  attributes and no namespaces emits its entire `<name>text</name>`
  from a single worst-case reservation.

Measured (min of 20, Release, same machine as the pugixml
references):

| Shape | before | after | pugixml |
|---|---|---|---|
| attribute-heavy | 1.09 GB/s | 1.09 | 0.99 — ahead |
| pretty-print | 0.54 | **0.60** | 0.52 — ahead |
| small | ~0 | ~0 | tie |
| namespaced | 0.68 | **0.79** | 0.84 — within 1.06x |
| text-heavy | 1.15 | **1.19** | 1.54 — within 1.29x |

Text-heavy output has improved 3x across the campaign (0.40 ->
1.19 GB/s); its residual is the per-element strlen, text
materialization, and child-detection decodes. Output is
byte-identical: 558 tests including all serialize round-trip
specs, ASAN clean, zero leaks.


## [0.24.7] - 2026-08-17

### Performance — fused text-only element serialization (TODO 194d)

A compact-mode text-only element — `<p>text</p>`, the dominant
shape in text-heavy documents — emitted through five buffer calls
plus a node-dispatch hop. A pure inline escaper (preserving the
entity-reference semantics of the run-batched walkers: references
emit as-is, bare `&` escapes, quotes stay ordinary in text mode)
now emits the entire open tag + escaped text + close tag from a
single worst-case reservation.

Measured on a 1.6 MB text-heavy document: 1.06 -> **1.15 GB/s**.
Cumulative across 194b/c/d: 0.40 -> 1.15 GB/s (2.9x), narrowing
the pugixml gap from 3.8x to 1.34x; the remainder is the
per-element recursion and the text-only detection walk.

Also settled by analysis: the mutation-side attribute hash for
`set_attribute` is not worth building — sequential NEW attribute
names must walk for duplicate rejection regardless of indexing
(pugixml pays the same walk), so the fair 1.4x gap is compact-
pointer decode constant, near the memory model's floor.

558 tests including serialize round-trips, ASAN clean, zero leaks.


## [0.24.6] - 2026-08-17

### Performance — single document resolution per append (TODO 195c)

The public `leptris_element_append_child` resolved the document
three times per call — twice in the wrapper's index-invalidation
guard, once inside the internal function for the tail cache —
each resolution paying the parent-chain walk plus the root-map
lookup. The wrapper now resolves the document once and passes it
through a doc-taking internal variant; the existing signature
delegates for the parser and modify-path call sites.

Measured (1k sequential appends, on a machine at load average
~466 — a floor reading): 81 ns -> 69-72 ns per append. Across the
mutation campaign (0.24.4 tail cache, 0.24.5 overflow slabs, this
release), sequential appends went from ~21 us to ~70 ns — about
300x — and now sit ~4x behind pugixml's ~18 ns, with the residual
being the COW version increment, compact-offset encoding, and
allocation: the price of the copy-on-write and 4x memory-density
features pugixml does not carry.

558 tests, ASAN clean.


## [0.24.5] - 2026-08-17

### Performance — any-size mutation: overflow-table slabs (TODO 195b)

Documents built through the mutation API allocate via scattered
pool pages, so at large sizes compact-pointer offsets exceed their
range and every tree-edge store falls back to the overflow table —
which paid one malloc per entry and a byte-wise 8-multiply hash
per call (91% of append time at ~20M children).

- Overflow entries are now carved from 256-entry slabs: roughly
  one malloc per 256 edges instead of one per edge; destruction
  frees whole slabs
- The pointer hash is multiply-shift Fibonacci hashing — two
  operations instead of eight multiplies

Combined with the tail cache from 0.24.4, sequential appends now
measure ~81 ns each (from ~21 us at the start of the mutation
campaign) — about 4.5x behind pugixml's ~18 ns, with the remaining
gap identified as per-call constant (version increment,
index-invalidation check, and the document-lookup walk, paid
twice). Fair-semantics set_attribute sits ~1.4x behind (the
earlier 53x figure compared against pugixml's check-free
append_attribute).

558 tests, ASAN clean.


## [0.24.4] - 2026-08-17

### Performance — mutation tail cache, appends 45x faster (TODO 195)

The public `leptris_element_append_child` walked to the child-list
tail on every append — elements carry no last-child edge by design
(the 64 B layout law) — making N sequential appends O(N^2). A
document-level one-entry (parent, last-child) cache now serves the
dominant pattern, appending in O(1); correctness is preserved by
verifying the cached child's parent back-pointer, and stale
entries (removed or re-parented children) fall back to the walk
automatically.

Measured: 10,000 sequential appends 212,458 us -> 4,742 us
(**45x**; the quadratic is gone). ~474 ns per append remains
against pugixml's ~18 ns — version increment, index-invalidation
check, and the document lookup walk are the identified next trim.
`leptris_element_set_attribute` is unchanged: its cost is the
existing-attribute name check (O(N^2) per add; the list tail is
already O(1)), which needs a mutation-side attribute hash —
scoped as the follow-up.

558 tests, ASAN clean.


## [0.24.3] - 2026-08-17

### Performance — batched open tags and xmlns emission (TODO 194c)

The serializer's remaining per-piece writer sites go batched:
element open tags emit `<` + name from one capacity reservation
(was two appends per element), and each xmlns declaration emits
inline with in-place URI escaping from one reservation (was six
appends per namespace).

Measured (min of 20, Release): text-heavy 1.6 MB at 0.99 ->
1.06 GB/s; namespaced 0.5 MB at 0.68 -> 0.70 GB/s. Modest gains —
append-level writer optimization is now exhausted; the residual
gaps to pugixml (1.54 / 0.84 GB/s) lie in per-element
walk/dispatch and content materialization, identified as the next
profiling targets.

### Testing

The two remaining parse-vs-memcpy reference specs are NDEBUG-gated
(the Windows test-suite job builds unoptimized, where the ratio is
not comparable — same treatment the deep-nesting spec already
had). Output remains byte-identical: 558/558 tests including the
serialize round-trip specs, ASAN clean, zero leaks.


## [0.24.2] - 2026-08-17

### Performance — run-batched text serialization (TODO 194b)

`serialize_text_internal` walked every text byte through the
entity-aware switch: a per-character append call for ordinary
bytes and the entity-lookahead branch on every character.
Ordinary-character runs are now bulk-appended with one memcpy per
run, and the entity lookahead only runs at `&`. Semantics are
unchanged — entity references still emit as-is, bare `&` still
escapes, and quotes remain ordinary in text content.

Measured on a 1.6 MB text-heavy document (min of 20, Release):
3438 us -> 1380 us — **0.40 -> 0.99 GB/s, 2.5x**. (pugixml
measures 1.54 GB/s on the same input; the residual gap is
per-element open/close append overhead, identified as the next
batching step.)

558 tests including the serialize round-trip specs, ASAN clean,
zero leaks.


## [0.24.1] - 2026-08-17

### Performance — batched serialization writer (TODO 194)

The serializer's hot paths no longer pay per-piece writer costs:

- Attribute emission reserves capacity ONCE per attribute
  (worst-case escape bound: name + quotes + 6x value bytes) and
  emits inline — replacing five append calls per attribute, each
  with its own capacity check and NUL store, and dropping the
  strlen on names (views already carry the length)
- Escaped text bulk-appends ordinary-character runs with one
  memcpy per run instead of a per-character append call

Measured on the 847 KB K=50 benchmark document (min of 10,
Release): 1501 us -> 779 us (0.56 -> 1.09 GB/s) — 48% faster.
pugixml's raw save to a string sink measures 856 us (0.99 GB/s)
on the same machine: serialization now favors leptris.

Output is byte-identical; 558 tests including the serialize
round-trip specs, ASAN clean, zero leaks. The indexed-walk perf
spec also moved to N-vs-3N sizing for ratio stability under ASAN
and loaded runners.


## [0.24.0] - 2026-08-17

### Added — SIMD structural span scanner (TODO 193 Phase 1)

`leptris_text_scan_events` records every XML structural byte
(`< > / ' " = &` and the `c <= ' '` class) as a positioned event,
via NEON marker detection (8 vector compares per 16-byte chunk,
per-hit class from a shared table; portable scalar reference
elsewhere). Measured 3.17 GB/s versus 0.46 GB/s for the scalar
per-byte table loop — 6.85x. Parity specs cover all 256 byte
values, every prefix length of a realistic document, CDATA/PI/
comment constructs, 50 random buffers, and the truncation signal.

### Parse campaign closed at a verified global optimum

The scanner's go/no-go floor probe settled the two-pass parser
question without an XL build: memcpy + full scan + a stub event
walk that builds nothing already costs 531 us on the K=50
benchmark document — 88.5% of the entire current 600 us parse,
tree building included (event density is 1 per 4.1 bytes; pass 2
re-walks what the fused single pass does once). With rounds 2-11
and the split experiment, every architectural class for the parse
gap is now measured: leptris parses 6-14x faster than libxml2 and
sits 1.5-1.8x behind pugixml at a compiler-global optimum.

### Testing

- Performance specs are machine-independent same-run ratios;
  the parse-reference specs are NDEBUG-gated (unoptimized builds
  distort the ratio), and the write-path growth specs document
  the measured fresh-document regime (T(4N)/T(N) of 60-100x)
- Perf and CLI discovered tests run RUN_SERIAL under parallel
  ctest, removing the spawn-contention flake class

558 tests, ASAN clean, zero leaks.


## [0.23.5] - 2026-08-17

### Documentation — parse endgame: round 11 gate-fail + parser v3 design

The parse campaign's eleventh measured experiment closed the last
architectural hypothesis for the current single-pass parser. A
fresh 767-sample profile places 89% of K=50 parse time in the
attribute-scan region (8% in the fused copy+count, <3% elsewhere);
forcing `dp_parse_attrs` out of line — so the compiler gets an
independent optimization surface for the attr loop — passed all
553 tests but regressed the 8-run interleaved gate (K=20 +2.7%,
K=50 +1.5%) and was reverted. Together with rounds 2-10 this
establishes the single-pass parser as compiler-globally optimal:
no C-level edit of any class moves mid-K.

`TODO.fix/193-parser-v3-simd.md` scopes the remaining path: a
two-pass SIMD span-scanner. Pass 1 classifies bytes into a span/
event index with NEON/AVX2 (branch-free, reusing the proven
simd_text framework) instead of the byte-at-a-time
classify-by-table loop both we and pugixml pay today; pass 2
materializes the tree from spans through the existing correctness
paths. Ship behind a build flag, flip after gates. Context: leptris
already parses 6-14x faster than libxml2 across the K matrix; the
1.5-1.8x gap to pugixml is the target.

No code changes in this release; documentation only.


## [0.23.4] - 2026-08-17

### Performance — attr-EXISTS queries via the any-value bucket (TODO 192e)

`//name[@attr]` and `.//name[@attr]` compile to the fused
attribute opcode with a 0xFFFF value sentinel; the VM reads the
any-value attribute bucket — which now stores preorder positions —
instead of walking each candidate element's attribute list. The
gain scales with attribute selectivity: attributes carried by
every element (like the benchmark's n) get a small win since the
bucket is as dense as the name bucket; selective attributes get
the full 192d-class jump.

### Testing — machine-independent performance specs

All absolute time budgets are removed from the perf regression
suite. Every assertion now compares two measurements from the same
run on the same machine: parse against a memcpy reference over the
same bytes (skipped under ASAN, where sanitizer costs are
asymmetric), write paths against their own first half (growth
ratio), and the indexed child walk at N=50 against N=25
(complexity ratio, min-of-3 per side). The growth specs also
documented the known TODO 155 Phase C tradeoff: public-API
`append_child` and `set_attribute` are O(n) per call (the parser
keeps private tail caches; the API walks) — the specs accept the
documented O(N^2) shape and flag anything worse.

553 tests, full XPath conformance, ASAN clean, zero leaks.


## [0.23.3] - 2026-08-17

### Performance — absolute `//name[@attr='value']` from the value bucket (TODO 192d)

The canonical whole-document predicated query now compiles to one
self-contained opcode whose VM handler scans the element index's
attr-VALUE bucket directly: no name-bucket materialization of all
candidates, no per-element attribute walks. The whole-document
scan is descendant-or-self-correct by construction — the root
element is in the bucket if and only if it carries the matching
attribute value. Without the index (a document's first query),
the opcode falls back to a root walk with a hash-prefiltered
attribute filter.

Measured on `//item[@n='5']` over a 5000-element document
(Release, min of 5): 38.8 us -> 1.27 us per query (30x vs
0.23.2). pugixml with a reused compiled `xpath_query` measures
110.8 us on the same workload — 87x in leptris's favor.

### Fixed — cold fused-attribute queries returned empty (latent in 0.23.2)

Both `a//b[@x='v']` fused-opcode fallback paths compared the raw
`a->name_hash` field, which is zero until lazily computed — so a
COLD first query of `.//b[@x='v']` (no element index built yet)
returned an empty result instead of the matches. Both fallbacks
now use the `attr_name_hash()` accessor. Caught by the new
cold-path spec, which also verifies cold/warm agreement and the
root-match descendant-or-self edge.

551 tests, full XPath conformance, ASAN clean, zero leaks.


## [0.23.2] - 2026-08-16

### Performance — attr-value bucket windows (TODO 192c)

The attribute predicate itself is now served from the element
index. A relative step with a single attr-equals predicate
(`a//b[@x='v']`, `.//b[@x='v']`) compiles to one fused opcode
whose VM handler windows the index's attr-VALUE bucket by each
context element's subtree interval — O(log B + hits) per context,
with zero per-element attribute walks. Value buckets gained
preorder positions (lockstep-grown, mutation-safe); without the
index the opcode falls back to the subtree walk with a
hash-prefiltered attribute filter.

Measured on `.//item[@n='5']` from 200 section contexts
(Release, min of 5): 0.50 us -> 0.25 us per query — now faster
than the UNPREDICATED `.//item` (0.30 us), because the value
bucket pre-narrows candidates below the name bucket's density.
pugixml with a reused compiled `xpath_query` measures 0.55 us
on the same workload — 2.2x in leptris's favor; 6.7x over the
v0.23.0 expanded-walk form.

549 tests, full XPath conformance, ASAN clean, zero leaks.


## [0.23.1] - 2026-08-16

### Performance — predicated relative descendants via the index (TODO 192b)

`a//b[@x='v']` and `.//b[@x]` from any context now take the
subtree-interval index path instead of the expanded wildcard
walk. The relative `a//b` compiler fusion accepts the same
predicate set the absolute fold has always allowed —
attribute-exists, attribute-equals-string, and child-num-cmp,
all per-element and context-independent — emitting the predicate
opcodes after the fused descendant opcode. Position predicates
remain excluded: they are per-parent in the expanded form but
global in the fused one, and a new spec pins that distinction
(`//section//item[1]` must return first-per-parent).

Measured on `.//item[@n='5']` from 200 section contexts
(Release, min of 5): 1.67 us -> 0.50 us per query (3.3x vs
0.23.0). pugixml with a reused compiled `xpath_query` measures
0.55 us — leptris is ahead on predicated relative queries as
well. The unpredicated path is unchanged (0.30 us).

549 tests (3 new specs), full XPath conformance on the fused
bytecode, ASAN clean, zero leaks.


## [0.23.0] - 2026-08-16

### Performance — subtree-interval element index (TODO 192)

Relative descendant queries (`.//name`, `a//b`, chained steps)
from any context are now answered from the element index in
O(log N + hits) instead of walking each context subtree — the
first XPath axis where leptris is faster than pugixml rather
than at parity.

Measured on `.//item` evaluated from 200 section contexts
(Release, min of 5): 2.0 us -> 0.30 us per query (6.6x vs
0.22.1). pugixml with a reused compiled `xpath_query` measures
0.42 us on the same workload — leptris is 1.4x faster (2.2x
against pugixml's one-shot `select_nodes`).

How it works:

- A subtree is a contiguous preorder interval of the index's
  flat array; the build walk now records each interval's end in
  `subtree_end[]` (one line).
- Name buckets store each match's preorder position, so
  filtering is index arithmetic and stays correct when mutation
  disturbs pointer order.
- The compiler fuses the predicate-free expanded `a//b` form
  ([descendant-or-self::node()][child::b]) into a single
  descendant-name opcode — the same equivalence the
  absolute-path fold has used since TODO 129.
- The VM binary-searches each context element's interval, scans
  the bucket window, and skips intervals nested inside an
  already-processed one (subtrees are disjoint or nested, so
  no dedup pass is needed and document order is preserved).
  Unlocatable contexts (foreign documents) fall back to the
  existing walk.

### Fixed — stale element index after child removal

`leptris_element_remove_child` and
`leptris_element_remove_all_children` never invalidated the
cached per-document element index (`leptris_element_append_child`
did), so descendant queries after a removal could serve
pre-mutation results. Found by the new TODO 192 specs; both now
invalidate. Five new XPath specs cover chained `//section//item`
nested dedup, context-relative `.//item`, strict
descendant-or-self, mutation invalidation, and absent names.


## [0.22.1] - 2026-08-16

### Documentation — TODO 192 design: subtree-interval element index

`TODO.fix/192-xpath-subtree-interval-index.md` scopes the next
performance lever, identified by the pugixml v1.16 audit: pugixml
has no document index, so every `//name` query from any context
walks the tree every time. Our existing element index covers only
root-context queries; extending it to subtree-restricted queries
makes the second+ relative query (`.//x`, `a//b`) cost O(matches)
from any context.

The design exploits a structural fact: the index's flat array is
preorder and arena pointers are monotonic in document order, so a
subtree is a contiguous interval — settable with one line in the
existing build walk, queried with a binary search plus
pointer-range filter over name buckets. The document specifies
the struct/function/VM changes, spec list, benchmark gate, and
risks (`direct_parse.c` stays untouched — the codegen wall).

No code changes in this release; documentation only.


## [0.22.0] - 2026-08-16

### Added — Python bindings (`pyleptris`, TODO 82)

`bindings/python/` ships a cffi (ABI-mode) binding mirroring the
public headers in a single `cdef` (`pyleptris/_ffi.py`, the same
architecture as the Ruby binding's `lib/leptris.rb`):

- `Document.parse(str | bytes)` with `close()`, context-manager
  support, and a refcounting `__del__` safety net
- `Element`: name, text, attributes with defaults, iteration over
  child elements, parent navigation — with whitespace-safe sibling
  walking (the immediate node sibling of an element is usually an
  interleaved text node; the binding loops until the next element)
- `Node`: typed traversal for text/comment/CDATA/PI with exact
  content access
- `Document.xpath` / `Element.xpath`: typed results — nodesets as
  Element lists, number/string/boolean as native Python types
- `Document.serialize` and `process_xinclude`
- 22 pytest specs, a 3000-iteration parse/eval/free stress loop,
  and a flat-RSS lifecycle check; every cdef symbol verified
  against the shared library's exported symbols
- README.adoc: Python quick start in the FFI section

### Fixed — `LEPTRIS_BUILD_SHARED=ON` failed to link

The CLI and the test tree linked the `leptris` alias, which points
at the shared library when both variants are built — but
`cli/output.c` and specs use internal accessors that the shared
library does not export. Both now link `leptris_static` when it is
built (no source changes). This unblocks the shared-library
workflow documented for the Python binding.

Follow-up scoped in `TODO.fix/191-cli-public-api.md`: refactor the
CLI output formatters onto the public API only, and add public
attribute-iteration functions — the public API currently offers
attribute lookup by name and a count but no iteration, which keeps
every binding from enumerating attributes.


## [0.21.6] - 2026-08-16

### Documentation — TODO 185 round 10: pugixml build/config audit

A flag-for-flag and trick-for-trick audit of pugixml v1.16
(CMake configuration, allocator design, parse-loop macros)
against ours, recorded in `TODO.fix/185-k50-attr-path.md`:

- **Build flags: nothing left to copy.** pugixml's CMake ships no
  optimization flags; our Release build (`-O3`, thin LTO, hidden
  visibility) is already ahead of the Homebrew artifacts we
  benchmark against. `LEPTRIS_TARGET_ARCH=native` measured mixed
  across the K matrix and is not a lever.
- **Struct density is not the residual gap.** pugixml attributes
  are 40 B (5 pointers) and nodes 64 B; our 48 B attribute /
  64 B element representation is denser.
- **`SCANWHILE_UNROLL`** (4-byte unrolled scans with unlikely
  exits) is a loop-body shape — the class measured dead in
  rounds 2-3.
- **The one actionable asymmetry** (setup allocations: pugixml
  embeds its first memory page in the document object; we build
  an unused-per-parse intern table) was implemented as lazy
  string-cache creation plus a 64 B-aligned element/attribute
  block. Tests, ASAN, and leak checks were clean, but the gate
  failed: K=5 regressed +2.4% consistently across 16/16
  interleaved runs. The change was reverted — the codegen wall
  around `direct_parse_internal` covers setup-region edits, not
  just loop-body edits.

No code changes in this release; documentation and campaign
ledger only. Parse standings vs pugixml are unchanged:
K=5 1.54x, K=20 1.51x, K=50 1.84x, K=100 1.83x, with the XPath
cycle at CPU parity.


## [0.21.5] - 2026-08-16

### Fixed — latent off-by-one in `//name` walk fallback (TODO 190)

`vm_apply_absolute`'s non-index fallback added the root element
UNCONDITIONALLY for `//name` (descendant-or-self) queries — but
descendant-or-self matches the root only when the root's name
matches; the index path applies exactly that filter via the name
bucket. The fallback only ever executed when index construction
failed, so no test had reached it: `count(//book)` on a document
with three books returned 4. Root is now added only on a name
match (hash prefilter + strcmp). Found by TODO 190's deferral
routing real traffic through the path; the single root-cause fix
cleared all 12 initially-failing tests.

### Performance — element index built on the second axis query

The per-document element index (TODO 132) was built on the FIRST
descendant/absolute XPath query — two full tree walks plus
per-name bucket allocations inside that query. A document that
sees one query (the common parse-query-free lifecycle) never
recovers that cost. The first axis query now walks directly
through the existing fallback paths; the second and later
queries get the index. Mutation invalidation semantics are
unchanged.

Measured on the parse+XPath+free cycle benchmark (CPU time):
count(//*) 5.24 → 5.03 µs, count(//book) 5.15 → 5.03 µs, parity
elsewhere; parse benchmarks unchanged. Also documented in the PR:
leptris is at CPU parity with pugixml (~5 µs) on the full cycle
for the 10 KB benchmark document — earlier "3-6x slower" ratios
were wall-time artifacts of machine load.


## [0.21.4] - 2026-08-16

### Performance — fused copy+count3 (TODO 188)

The first change of the entire parse campaign to improve EVERY
benchmark section. The owns-copy path streamed the input twice
before parsing began — the count3 SIMD pre-scan for arena sizing
(~7% of parse) and the memcpy into the pool's buffer copy
(~4.5%). Both passes read the same bytes.

One NEON kernel now copies each chunk to the destination AND
counts '<' / '"' / '\'' from the same registers, and the buffer
copy moves out of the arena into a separately-malloc'd,
document-owned allocation (freed via the existing
`xml_buffer_needs_free` machinery). The arena holds only nodes
and strings. Platforms without the NEON kernel get the exact
pre-fusion behavior as the scalar fallback.

Measured (8-run interleaved Release A/B, best-of):

| attrs/element | before | after | gap vs pugixml |
|---------------|--------|-------|----------------|
| 5 | 54.4 µs | **50.8 µs (−6.7%)** | 1.65x → **1.54x** |
| 20 | 222.3 µs | **212.1 µs (−4.6%)** | 1.58x → **1.51x** |
| 50 | 630.1 µs | **589.7 µs (−6.4%)** | 1.97x → **1.84x** |
| 100 | 1222.6 µs | **1143.7 µs (−6.4%)** | 1.96x → **1.83x** |

K=50 and K=100 drop below 1.9x for the first time. Prototype
kernel: 17-33% faster than memcpy + count3 across 40-884 KB
inputs on M1.

Also in this release: the post-parse freeze tree walk removed
(TODO 187, v0.21.3 content — nodes are created frozen; K=5
−5.8%).


## [0.21.3] - 2026-08-16

### Performance — post-parse freeze walk removed (TODO 187)

The parser froze the tree it had just built by walking it —
`leptris_document_freeze_tree` after every parse, an iterative DFS
through every element and sibling edge to set a single
immutability bit per node. The first K=5 sample profile (only K=50
had ever been profiled) showed the walk plus its decoders costing
~11% of parse time on element-heavy documents.

Every node is created by the parser, which knows it is immutable
the moment it creates it: `frozen = 1` is now set at the creation
sites (elements, text, comments, CDATA, PIs) and the walk is
deleted. The public `leptris_document_freeze` API is unchanged.

Measured (8-run interleaved Release A/B, best-of):

| attrs/element | before | after | gap vs pugixml |
|---------------|--------|-------|----------------|
| 5 | 53.5 µs | **50.5 µs (−5.8%)** | 1.63x → **1.53x** |
| 20 | 218.3 µs | 215.9 µs | 1.56x → 1.54x |
| 50 | 612.3 µs | 609.9 µs | 1.96x → 1.95x |
| 100 | 1194.2 µs | 1204.0 µs | 1.94x → 1.95x |

Element-heavy and small documents — the common real-world shape —
win where the profile predicted.

Also in this release (docs): TODO 182's split-stream parse thesis
closed by upper-bound probe; TODO 185 campaign record complete.


## [0.21.2] - 2026-08-16

### Performance — attribute struct 64 to 48 bytes (TODO 186)

The largest parse win since the fused scans. The attribute struct
drops its 16-byte layout pad and ships at the natural packed 48
bytes, revising the TODO 184 round-4 "must stay 64" cache-line law —
which turns out to have been K-local (measured at K=100 only).

Measured across every K (12-run interleaved Release A/B, fresh
build dirs, best-of), with the pugixml gap:

| attrs/element | 64 B | 48 B | gap 64B | gap 48B |
|---------------|------|------|---------|---------|
| 5 | 54.4 µs | 54.4 µs | 1.62x | 1.62x |
| 20 | 270.5 µs | **218.7 µs** | 1.91x | **1.55x** |
| 50 | 637.6 µs | **619.9 µs** | 2.02x | **1.96x** |
| 100 | **1013.4 µs** | 1222.7 µs | **1.64x** | 1.97x |

- **K=20: −19%** (gap 1.91x → 1.55x)
- K=50: −3%; K=5: parity
- K=100: +20% — the cache-line straddle penalty in a 4.8 MB
  DRAM-streaming attribute block. Real-world attribute density is
  under 20 per element and the mid-range is the campaign yardstick,
  so 48 B wins overall. The TODO 182 split-stream design (32-byte
  view pairs + parallel control array, 2 per cache line) is the
  documented path to recovering K=100.

Also 25% less memory per attribute. 535/535 tests, ASAN clean,
zero leaks at the new size. Full table and reasoning recorded in
`TODO.fix/185-k50-attr-path.md` (round 4).


## [0.21.1] - 2026-08-15

### Docs — parse-perf campaign closure (TODO 185 round 3)

- **Source-level micro-optimization of the parse path is formally
  closed.** The lazy element name-hash experiment (mirroring the
  shipped lazy attr-hash) — pure work removed from the parse loop —
  still regressed 3-5% at K=5-50: the fifth consecutive experiment
  where even deleting work perturbs codegen/layout more than the
  work cost. All five are recorded in TODO 185 with a
  do-not-reopen-without-architectural-change note.
- **PGO measured on the current parser**: 1.2-2.2% faster on every
  many-attrs K section (far below its 10-15% on the XPath VM
  dispatch loop). Documented in the `LEPTRIS_ENABLE_PGO` CMake
  comment for packagers, including the `LEPTRIS_PGO_DIR` pointer
  from GENERATE to USE builds.
- **TODO 182 marked the remaining endgame**: 32-byte attrs
  (2 per cache line, pugixml density) is the only lever with >5%
  potential left; the measured cache-line constraint is now
  written into its premise (the old 24 B struct numbers were
  stale).
- README roadmap updated (Ruby SAX shipped; TODO 183/184/185
  campaign items; TODO 182 as planned endgame); FFI.md documents
  the Ruby SAX binding pattern (user_data anchor registry,
  pointer+length characters contract) for the future
  Python/Rust bindings.

No code changes in this release.


## [0.21.0] - 2026-08-15

### Added — Ruby SAX binding (TODO 118 Phase B)

`Leptris::SAX` is real. The module previously existed as a stub
that called `leptris_sax_parse` with NULL callbacks and never fired
a handler.

- **`Leptris::SAX.parse(xml, handlers)`** — one-shot parse of a
  String. handlers is a Hash of procs; all keys optional:
  `:start_document`, `:end_document`, `:start_element` (name,
  attrs-as-Hash), `:end_element`, `:characters`, `:comment`,
  `:cdata`, `:processing_instruction`, `:start_prefix_mapping`,
  `:end_prefix_mapping`, `:error` (message, line, column).
- **`Leptris::SAX::Parser`** — incremental parsing for streams:
  `feed(chunk, final:)` + `free`; `streaming: true` selects the
  constant-memory state machine (the same machine the one-shot
  path uses; the default buffers and parses on the final feed).
- GC-safe FFI bridge: user_data is a unique anchor pointer; a
  registry maps it back to the Ruby bridge for exactly as long as
  C can fire callbacks. All strings delivered as UTF-8; `characters`
  handles the non-NUL-terminated pointer+length contract.
- README: Ruby SAX quick start + streaming example (verified to
  run as printed). 12 new Ruby specs.

### Fixed — SAX entity semantics (C, found via the binding)

Two conformance gaps in the SAX streaming state machine:

- **Entity references now expand** in `characters()` text and in
  `start_element` attribute values (XML 1.0 sections 2.4 / 3.3.3).
  `&amp;`, `&lt;`, `&gt;`, `&apos;`, `&quot;` and numeric
  character references (`&#65;`, `&#x42;`) arrive expanded. Spans
  without `&` keep the zero-copy emit path; CDATA sections remain
  verbatim. 3 new C specs pin the contract; the header doc now
  states it.
- **Chunk-boundary reference splitting**: an entity reference
  split across incremental feeds (`&` at the end of one chunk,
  `amp;` in the next) was emitted raw on both sides and never
  decoded. The text state now delivers only through the last
  complete reference and holds the incomplete tail for the next
  feed. Caught by the existing MixedContent differential test at
  1-byte chunks.


## [0.20.4] - 2026-08-15

### Changed — quadratic attr walks eliminated (TODO 185)

Every `get_attribute_by_index(elem, i)` loop — an O(K²) re-walk of
the attribute list per index — is replaced by a single direct
`leptris_attr_next` walk. Converted sites:

- `finalize_element_strings` (leptris.c) — the per-element NUL-
  termination pass
- DTD validator (`dtd/validator.c`) — default-value and #FIXED
  checks, twice
- C14N (`serialize/c14n.c`) — three array-fill loops

Measured effect at K=100 (5M indexed walks) is ~2 µs — parity, by
design: this was a correctness/complexity hazard (quadratic
scaling latent in every future caller), not a perf win. Also
const-cleans `leptris_attribute_free`'s owned-copy releases.

### Docs

- TODO 185 measurement record: the attr-init store-fold lever is
  **dead by disassembly** — clang -O3 already emits the 4-store
  floor (`stp`×2 + 8 B zero + `str wzr`) per attribute. Feature
  costs vs pugixml (line tracking, pre-sizing scan, DTD routing)
  documented as the residual mid-K gap context.


## [0.20.3] - 2026-08-15

### Architecture — single-representation attribute strings (TODO 184 round 4)

The attribute struct's `char* name`/`char* value` fields are
**deleted**. The `LeptrisStringView` fields are the only string
representation:

- **Parse path**: zero-copy views into the document buffer, NUL-
  terminated in place by the parser (document-lifetime).
- **Mutation API / entity expansion**: owned copies *replace* the
  view — callers' strings may be temporary, and the raw entity
  text is never needed after expansion.

Every dual-representation branch, temporary conversion, and
convert-and-free dance collapses to `attr_cname`/`attr_cvalue` —
**208 net lines deleted** across the parser, DTD validator,
serializer, c14n, xinclude, XPath VM/evaluator/functions, FFI
accessors, and CLI. Deep-copy now pool-copies attribute views into
the destination pool (the source document's buffer can be freed
first — a pre-existing exposure, now closed).

#### Measured layout law (documented in element.h)

`sizeof(struct leptris_attribute)` **must stay 64** — exactly one
cache line per attribute. The natural 48-byte struct was built and
benchmarked: it **regressed K=100 by 22%** (1252–1309 vs 1025–1041
µs, interleaved Release A/B on fresh build trees) because 48-byte
attributes straddle cache lines (1.33 lines/attr) — every
parse-wiring touch pulled two lines. The struct retains 16 bytes
of documented padding with a do-not-remove-without-benchmark note.

Verified: 508/508 tests, ASAN clean (dom/serialize/xpath suites,
checked locally before push), interleaved K=100 parity 1030 vs
1040 µs.

## [0.20.2] - 2026-08-15

### Architecture — view-backed attribute name/value (TODO 184 round 3)

The attribute `name`/`value` char* fields are no longer written by
the parse path: readers derive from the views via new
`attr_cname`/`attr_cvalue` helpers (non-NULL field wins for owned
copies — entity expansion, mutation API). The parse path NUL-
terminates name and value in the document buffer and stores only
the (pointer, length) views.

Simplifications this unlocks:

- `finalize_element_strings` and `leptris_element_attribute` only
  materialize ENTITY values; the per-attr plain pool-copies were
  historical defensiveness (that walk runs over every attribute of
  every parse).
- DTD validator, serializer, c14n, xinclude, and the FFI accessors
  read view data directly — c14n now frees only copies it allocated
  (the old `!= attr->value` test would have freed view data under
  the new contract).
- `leptris_attribute_new` zeroes the struct — the view fields were
  uninitialized garbage, a pre-existing landmine.

### Fixed

- Parse-path attributes now store explicit NULL for the char*
  fields. `attr_block` is deliberately not memset, and macOS only
  passed because fresh mmap pages read as zero — CI allocators
  fill 0xbe/0xcd patterns and crashed every attribute lookup
  (ASAN Linux SEGV at 0xbebebebe; Windows 0xc0000005). Verified
  with a local ASAN build before merge.

Measured: K=100 parity (1049 vs 1043 µs) — the structural payoff
(fields now provably unused on the parse path) lands in round 4.
All 508 tests pass.

## [0.20.1] - 2026-08-15

### Performance — fused text scan + single-pass sizing counts (TODO 184 round 2)

Round 2 of the pugixml parse-loop port: two more applications of
the "one pass beats setup + re-walk" finding that drove v0.20.0's
45% win.

1. **`leptris_text_count3`** — the arena sizing pre-scan counted
   `<`, `"`, and `'` in three separate traversals of the document.
   One SIMD pass now maintains all three counters (three `vceqq`
   per 16-byte chunk with the widen-then-add idiom). At 884 KB
   that avoids 1.7 MB of L2/DRAM traffic per parse. Scalar
   fallback on builds without a SIMD path.
2. **Fused text-node scan** — the text path was `memchr` for `<`
   plus a `dp_advance_line` re-walk counting newlines (issue #223
   tracking): two passes and a `memchr` setup per text node. One
   inline loop now finds `<` and folds newline counts; spans
   beyond 48 bytes fall back to `memchr` +
   `leptris_text_count_char` over the skipped region only.

#### Measured impact (controlled Release builds, fresh dirs)

| workload | v0.20.0 | v0.20.1 |
|---|---|---|
| many-attrs K=100 (7-run median) | 1095 µs | **1043 µs** |
| many-attrs K=5 | ~60 µs | ~56 µs |
| text-heavy 337 KB, 5 K text nodes (best of 50) | 481 µs | **446 µs** |

Gap to pugixml at K=100: 1.54× → **1.47×** (1043 vs 711 µs).
Campaign total: 6885 → 1043 µs at K=100 (6.6× faster since
v0.18.4).

All 508 tests pass, including the issue #223 line-tracking specs.

## [0.20.0] - 2026-08-14

### Performance — fused attr-value scan: 45% faster attr-heavy parse (TODO 184)

The largest single parse win of the campaign, closing the K=100
gap to pugixml from 2.79× to **1.54×**.

`dp_parse_attrs` scanned every attribute value twice — `memchr`
for the closing quote, then a second pass for `&` (entity check).
libc `memchr` pays ~10 ns of call/setup cost even on 6-byte values
(the TODO 174 finding, previously applied only to the entity
check). At K=100 — 100,000 attrs, average value ~6 bytes — that
setup cost was ~1 ms per parse, the dominant share of the measured
2.8× gap.

One fused loop now finds the closing quote AND flags `&` on the
way past, modeled on pugixml's `ct_parse_attr` single-pass loop:
the first 48 bytes scan inline (a byte loop beats `memchr` setup
at that size), longer values fall back to SIMD (`memchr` for the
quote + `leptris_text_contains` for `&` over the scanned span).
`dp_add_attr_inline` receives `has_amp` from the caller; its
internal re-scan is deleted. Entity routing semantics (lazy
accessor vs DTD-aware eager decode) are unchanged.

This follows the profiling round that eliminated the other
hypotheses with controlled Release A/B builds: struct width
(64-byte attr, #339) and cross-TU inlining (amalgamation) were
each perf-neutral. The gap was loop structure, and this change
removes its largest component.

#### Measured impact (benchmark_many_attrs K=100, Release, 10 runs)

| | median | gap to pugixml (711 µs) |
|---|---|---|
| v0.19.9 | 1986 µs | 2.79× |
| **v0.20.0** | **1095 µs** (1091–1099) | **1.54×** |

#### Remaining gap (~1.5×) — ordered levers, TODO 184

1. The same fused-inline principle for the text-node `<` scan and
   the name-scan dispatch.
2. The K=5/20/50 element-wiring path (still trails there).
3. Buffer-copy accounting parity in the benchmark harness
   (leptris memcpy's the input per parse; verify pugixml's harness
   gives identical semantics).

All 508 tests pass.

## [0.19.9] - 2026-08-14

### Performance — attribute struct 72 → 64 bytes via cp16 list edge (TODO 181 Phase D / TODO 183 Phase 5)

The first compact-pointer migration enabled by the contiguous arena
(v0.19.8). `struct leptris_attribute`'s 8-byte `next` pointer becomes
a 2-byte cp16 edge: the attr `next` edge only connects attrs of the
SAME element, which the parser allocates as adjacent attr_block
slots — contiguous by construction since the arena migration;
distance ≤ K × 64 B (~4 KB at K=100), always within cp16's ±256 KB.
Mutation-created attrs route through the encoder's overflow-table
path.

Field reorder packs `next_cp` + `has_entities` + `name_hash` into
one 8-byte tail: sizeof 72 → 64. Attr and element are now both
exactly one cache line. 12.5% less attr memory (800 KB less on the
K=100 document). 46 call sites across 14 files migrated to
`leptris_attr_next`/`leptris_attr_set_next`; the parser hot path wires
the edge with a direct cp16 store (no encoder call). No raw list
pointers remain in the tree.

#### Measured impact (controlled Release A/B, fresh build dirs, 7 runs)

| build | K=100 median |
|---|---|
| v0.19.8 (72 B attr) | 1986 µs |
| **v0.19.9 (64 B attr)** | **1985 µs** — parity |

Honest finding: the shrink banks the memory and layout milestone but
does not move K=100. An amalgamated single-TU build was also
measured (2037 µs) — cross-TU inlining is not the lever either.
With bulk allocation and lazy hashing already shipped, the remaining
~3× to pugixml (K=100 medians: 1986 vs 711 µs) lives in the
parse-loop structure itself — chartype dispatch and value-scan
shape. That work is precisely scoped for the next round.

All 508 tests pass.


## [0.19.8] - 2026-08-14

### Architecture — parser fully on the contiguous arena (TODO 183 Phase 3)

`direct_parse` now allocates EVERYTHING for a parsed document from
one contiguous arena: the bulk element+attribute block, text/comment/
CDATA/PI nodes, strings, the document struct, and the input copy all
live within `[base, base+size)` by construction. The old 4 MB page
cap — which scattered attr-heavy documents' bulk block to a separate
oversized malloc — is gone. This is the layout prerequisite for
retrying compact-pointer Phases C/D (TODO 180/181, the 1.5–3×
pugixml-gap levers).

#### Content-derived sizing via SIMD byte population count

One SIMD pass counts `<` (upper bound on tags) and quote characters
(exactly 2 per attribute value, single- or double-quoted); attr
capacity follows the document's actual shape instead of the len/10
heuristic. K=100 many-attrs allocates 9.6 MB at 87% utilization vs
the old path's 43.7 MB oversized block — 5× less over-allocation.

New `leptris_text_count_char` in the AOT SIMD framework: AVX2 via
movemask popcount; NEON via `vpaddlq_u8`→`vaddvq_u16`. The naive
NEON form (`vaddvq_u8`) is WRONG on real hardware — ARM's `ADDV.16b`
accumulates in a byte register, so 255×match overflows mod 256 and
any chunk with 2+ matches counted as zero. Unit-tested across
per-chunk densities 1–15; the bug was caught live when it
under-sized arenas and failed the 5000-doc stress suite.

#### Measured impact (controlled A/B, fresh Release build dirs, 7 runs)

| build | K=100 median |
|---|---|
| v0.19.7 (page mode) | 2053 µs |
| **v0.19.8 (arena)** | **1986 µs** |

Parity-to-slightly-ahead, with 5× less over-allocation. (An earlier
apparent +20% regression was an artifact of benchmarking an
inconsistently configured build directory; all comparisons in this
entry use identical explicit flags.)

#### Two CI-caught defects fixed during migration

1. **Item-heavy docs under-sized**: the sizing model bounded text
   content by `len` but not the ~48–56 B struct overhead per text
   NODE (~2× the per-item markup on mixed-content docs) — a 134 KB
   doc exhausted the arena at item 4144/5000 with 38 bytes left.
   Sizing now adds `est_elems × 64` node overhead; attr margin
   16 → 64.
2. **Post-parse mutation crash**: fail-fast NULL out of a tiny
   document's arena segfaulted `element_create`→`append_child` (the
   page pool could never fail there). The pool regains its
   never-fail contract: overflow beyond the sized span goes to
   tracked extension blocks (freed at destroy). The contiguous span
   stays exclusively parse-time; Phase 5 note — edges touching
   extension nodes must use the int32 path.

Specs: `ExhaustionExtendsBeyondSpan` (overflow succeeds AND lands
outside the span), `MutationGrowthNeverFails` (1000 × 72 B from a
64 B arena), `NodeWithContentKeepsNextAllocAligned` (arena content
region rounds to the 8-byte grid — an odd-sized region previously
misaligned every subsequent allocation, mangling comments/PIs).

Diagnostics: `LEPTRIS_DEBUG_PARSE=1` prints parse-failure position,
arena used/capacity, elem/attr indices, and sizing inputs (env
lookup cached off the hot path).

All 508 tests pass. Next: Phase 5 — retry compact-pointer Phases
C/D on the now-contiguous layout.


## [0.19.7] - 2026-08-14

### Foundation — arena-backed pool mode (TODO 183 Phase 2)

The pool can now route every allocation through one contiguous
arena: `leptris_pool_create_arena_backed(arena, owns_arena)`.
With `pool->arena` set, `alloc`/`calloc`/`alloc_batch`/`strdup`/
string-interning and `alloc_node_with_content` all bump-allocate
inside the single arena span — every pointer the pool hands out
lies within `[arena->base, +size)`, and exhaustion is a hard NULL
(never a scattered fallback malloc).

`alloc_node_with_content` is strictly better in arena mode:
oversized content stays in-span in one contiguous bump, where page
mode parks it in a separate oversized malloc — the exact layout
that silently truncated compact-pointer tree edges during the
TODO 180 Phase C attempt.

`owns_arena=0`: a caller-managed arena survives pool destroy —
the shape the parser will use in Phase 3. Page mode is completely
untouched; zero behavioral change for existing callers.

Routing: `leptris_pool_alloc` → `leptris_arena_alloc`;
`alloc_batch` → single arena bump; `node_with_content` → arena
node+content; `get_base` → arena base; `total_size`/`used_size`/
`page_count` → arena size/used/1; `destroy` → arena (when owned).

9 new specs in `test/memory/test_pool_arena.cpp`: span containment
across 200 allocations, hard-null exhaustion (no fallback), strdup
routing, 40 KB node+content contiguity in-span, string-interning
dedup on the arena, stats reflection, borrowed-arena lifetime,
page-mode pool not arena-backed.

All 506 tests pass (497 + 9 new).

Next: Phase 3 — `direct_parse` pre-sizes the arena (element count
from document length, attr count from a quote pre-scan) and creates
the pool arena-backed, making every parse allocation contiguous.
Then Phase 5 retries compact-pointer Phases C/D on the contiguous
layout.


## [0.19.6] - 2026-08-14

### Foundation — contiguous per-document arena (TODO 183 Phase 1)

New `src/leptris/memory/arena.{h,c}`: a fail-fast bump allocator
backed by one single contiguous malloc per document. Purely
additive — no existing code path touched yet.

**The contract — fail-fast, no silent fallback.**
`leptris_arena_alloc` returns NULL when a request does not fit in
the remaining space; it never falls back to malloc. Every returned
pointer is guaranteed to lie within `[base, base + size)`. That
guarantee is the property the page-based pool cannot provide: its
32 KB pages are independent mallocs that land megabytes apart on
macOS ASLR / Linux glibc — exactly what silently truncated cp16
tree edges during the attempted compact-pointer Phase C (TODO 180).
pugixml behaves the same way: parse fails on allocator exhaustion
rather than scattering allocations.

API mirrors the pool subset the parser uses (8-byte aligned, so
compact-pointer scale-8 assumptions hold): `create/destroy`,
`alloc`, `alloc_zeroed`, `alloc_node_with_content` (struct+content
contiguous), `remaining`, `base`.

11 new specs in `test/memory/test_arena.cpp`, including
`AllPointersWithinSpanForCompactEncoding` — the invariant the pool
cannot guarantee, stated as a test — plus exhaustion semantics
(exact-fit then refusal, smaller-fit-after-refusal), alignment,
non-overlap, zeroing, and node+content contiguity.

All 497 tests pass (486 + 11 new).

Next: Phase 2 (pool API wrapper over the arena) and Phase 3
(`direct_parse` sizes the arena ≈2× document bytes + 64 KB and
allocates everything from it), after which compact-pointer Phases
C/D (TODO 180/181) can be retried on the contiguous layout.


## [0.19.5] - 2026-08-14

### Performance — AOT SIMD framework (TODO 175)

simdjson-pattern AOT SIMD: hand-written AVX2/NEON intrinsics compiled
ahead-of-time with per-file flags, runtime dispatch via
`__builtin_cpu_supports` (x86) / architectural baseline (arm64 NEON).
No JIT, no LLVM dependency, zero runtime deps.

New `common/cpu.{h,c}` (ISA macros + `leptris_cpu_detect()`) and
`common/simd_text.{h,c}` (`leptris_text_contains`/`find`/`find3`
primitives, function-pointer dispatch). `simd_text_avx2.c` compiled
with `-mavx2`; `simd_text_neon.c` baseline on aarch64. CMake wires
per-ISA TUs and reports at configure time.

### Performance — SIMD find3 for comment/CDATA end detection (TODO 177)

First real consumer of the framework. The comment (`-->`) and CDATA
(`]]>`) end scans in `direct_parse.c` now locate 3-byte terminators
in one SIMD pass instead of memchr-anchor + per-candidate verify:

- NEON: `cand[i] = eq0[i] & eq1[i+1] & eq2[i+2]` via `vextq_u8` shifts
- AVX2: `match = m0 & (m1 >> 1) & (m2 >> 2)` in movemask space

Chunks advance width−2 so boundary-straddling triples re-check;
scalar tail covers remainders. PI's single-char `?` scan stays on
`memchr` (already optimal). Dash-run-heavy bodies (adoc `----`
separator comments) no longer re-verify at every candidate byte.

#### Correctness hardening

Two defects caught and fixed by CI + new specs: MSVC rejects
`-mavx2` (Windows link failure → `LEPTRIS_HAS_AVX2_BUILD` build guard
dispatches to scalar there), and an inverted `vextq_u8` operand order
made the NEON vector loop silently miss matches — the existing specs
only exercised the scalar tail. New specs pin the vector-loop region:
`Find3LongBodyEveryPosition` (match at every offset in 80-byte body),
`Find3DashRunHeavyBody` (200-dash run), `Find3TerminatorAtVeryEnd`.

#### Measured impact

K=100 many-attrs median 4137 µs — within noise of v0.19.4 (4172–4490);
that benchmark has few comments. The find3 win scales with
comment/CDATA body length and anchor-byte density.

### Planning — compact-pointer Phases C/D blocked

TODO 180/181 marked blocked: pool's 32 KB pages are independent
mallocs that land megabytes apart (macOS ASLR / Linux glibc), so
cp16's ±256 KB range silently truncates cross-page tree edges.
TODO 183 (contiguous per-document arena, pugixml-style allocator)
written as the prerequisite. All 486 tests pass.


## [0.19.4] - 2026-08-14

### Foundation for compact-pointer migration (TODOs 178–182)

This release ships the first two phases of the compact-pointer
migration that will close the remaining 3× gap to pugixml on the
K=100 attr-heavy benchmark. No user-visible behavior change —
all 474 tests pass; K=100 benchmark within noise of v0.19.3.

Research basis: Parabix ARM report
(https://mdsz.ca/experience/parabix-arm-project-report/) confirms
SIMD wins are real (32% on icgrep via NEON) but require LLVM JIT —
too heavy for a C99 library. The simdjson model (AOT intrinsics +
runtime dispatch) captures the same wins without JIT dependency.
TODOs 175–177 scope the AOT SIMD workstream.

The remaining 3× gap to pugixml is structural cache pressure, not
algorithmic — element struct 64 B vs pugixml's 20–24 B. Closing it
requires shrinking tree-edge storage from 4-byte int32 offsets to
2-byte compact pointers. TODOs 178–182 scope the migration.

### Performance — compact pointer encoding primitives (TODO 178)

Adds `leptris_compact_ptr8_encode/decode` and
`leptris_compact_ptr16_encode/decode` to `dom/compact.{h,c}`,
alongside the existing int32 path. Same overflow-table mechanism;
no migrations yet. Pure additive infrastructure.

- 1-byte (cp1): ±127 * 8 = ±1016 bytes — for very-near pointers.
- 2-byte (cp16): ±32767 * 8 = ±256 KB — covers any realistic document.

10 new specs in `test/dom/test_compact.cpp` cover null round-trip,
positive/negative offsets, overflow detection, misalignment, and
distinct-fields-on-same-base.

### Performance — migrate text/comment/cdata/pi sibling to cp16 (TODO 179)

First consumer of the TODO 178 primitives. `LeptrisTextNode`,
`LeptrisCommentNode`, `LeptrisCDATANode`, `LeptrisPINode` `next_sibling`
field migrated from 4-byte int32 offset to 2-byte cp16 compact
pointer. Saves 2 bytes per non-element node.

Element sibling pointer stays int32 — migrated in TODO 180 Phase C.

Why cp16 not cp1: `direct_parse.c` is overflow-table-free by design
(avoids cross-document contamination under high doc counts per
issue #261). cp1 would force overflow on sibling chains longer
than ~25 nodes (1 KB reach). cp16 covers ±256 KB — never overflows
for realistic docs, preserves the overflow-free property.

`direct_parse.c`'s `dp_wire_child` updated: split `dp_ns_off` into
`dp_ns_off_int32[1]` (element) and `dp_ns_off_cp16[4]` (non-element
types), with branch on `prev_last->type`. Element fast path stays
branchless; non-element path is one extra arithmetic op.

#### Measured impact (benchmark_many_attrs K=100, median, 7 runs)

| build | v0.19.3 | v0.19.4 |
|---|---|---|
| default Release | 4194 µs | ~4289 µs (range 4172–4490) |

Within noise — K=100 is element/attr-heavy, doesn't exercise
text/comment/cdata/pi sibling chains. Wins will show on mixed-content
docs with many text nodes.

### Planning — TODOs 175–182

Eight new TODOs in `TODO.fix/`:
- 175: AOT SIMD framework (simdjson pattern — runtime CPU dispatch).
- 176: SWAR byte-classification (LUT-free scan loops).
- 177: Multi-byte literal matchers (`-->`, `]]>`, `?>`).
- 178: Compact pointer Phase A — encoding primitives (this release).
- 179: Compact pointer Phase B — text/comment/cdata/pi (this release).
- 180: Compact pointer Phase C — element tree (next, biggest lever).
- 181: Compact pointer Phase D — attribute list.
- 182: Compact pointer Phase E — compact_string for names.

Estimated cumulative gain when all phases ship: 1.5–2× on tree-walk
heavy workloads, closing the structural cache pressure gap to pugixml.


## [0.19.3] - 2026-08-14

### Performance — inline entity check for short attr values (TODO 174)

libc `memchr` has ~10ns setup cost even for 1-byte scans. For attr
values ≤ 16 bytes (the common case — typical attr values are 5-15
bytes), a tight inline byte loop is faster.

`dp_add_attr_inline` in `flat/direct_parse.c` now uses an inline
scan for values ≤ 16 bytes, falling back to `memchr` for longer
values.

#### Measured impact (benchmark_many_attrs K=100, median, 7 runs)

| build | v0.19.2 | v0.19.3 |
|---|---|---|
| default Release | 4568 µs | **4194 µs** (8% improvement) |
| fast preset (-O3+march+LTO) | 3313 µs | **2022 µs** (38% improvement) |

The fast preset improvement is dramatic — the compiler vectorizes
the inline loop using AVX2, displacing both `memchr` setup cost
and the function-call overhead.

#### Cumulative vs v0.18.4 baseline (K=100, fast preset)

| version | K=100 median | gap to pugixml |
|---|---|---|
| v0.18.4 | 6885 µs | 7.8× |
| v0.19.0 (build flags) | 3209 µs | 4.5× |
| v0.19.1 (lazy hash) | 3098 µs | 4.5× |
| v0.19.2 (attr shrink) | 3313 µs | 4.5× |
| **v0.19.3 (inline amp)** | **2022 µs** | **3.06×** |

All 464 tests pass.


## [0.19.2] - 2026-08-13

### Performance — attr struct shrink 112 → 72 bytes (TODO 173)

Moved `prefix` / `namespace_uri` (both StringView and cached-cstr
forms, 48 bytes total) out of `struct leptris_attribute` into a
side cache struct (`leptris_attr_ns_cache`) allocated only when one
of them is set. The common case (attr has no namespace activity)
has `ns_cache == NULL` — zero overhead. Attrs that do have a
namespace prefix or resolved namespace_uri pay one 48-byte pool
allocation for the cache struct.

Attr struct size: 112 → 72 bytes (36% reduction). For 100,000
attrs at K=100 attrs/element, that's 4.8 MB less memory pressure
and corresponding cache-traffic savings.

New accessor helpers in `element.h`: `attr_get_prefix`,
`attr_get_namespace_uri`, `attr_get_prefix_view`,
`attr_get_namespace_uri_view`. Readers use these; writers allocate
the cache via `leptris_pool_alloc`.

25 call sites updated across `element.c`, `element_modify.c`,
`direct_parse.c`, `xpath/functions.c`, `xpath/evaluator_axes.c`,
`leptris.c`, `leptris_memory.c`.

#### Measured impact (benchmark_many_attrs K=100, median, 7 runs)

| build | v0.18.4 | v0.19.1 | v0.19.2 |
|---|---|---|---|
| default Release | 6885 µs | 6284 µs | **4568 µs** |
| fast preset (-O3+march+LTO) | — | 3098 µs | 3313 µs (noise) |

Default-build improvement is 33% vs v0.18.4 baseline. Fast preset
within noise — LTO already reorders fields effectively.

All 464 tests pass.


## [0.19.1] - 2026-08-13

### Performance — lazy FNV attr hash (TODO 172)

Skip the per-attribute FNV-1a hash computation at parse time; defer
to first read via the new `attr_name_hash()` helper. FNV-1a output
is provably non-zero for any non-empty input, so 0 serves as a safe
sentinel for "not yet computed".

For users who parse documents and don't issue attr-predicate XPath
queries, the hash is pure overhead: ~3-5 ns per attribute. At
K=100 attrs/element × 1000 elements = 100,000 attrs, that's
~300-500 µs saved per parse.

Reads updated:
- `vm.c` PRED_ATTR_EXISTS / PRED_ATTR_EQ_STRING bytecode handlers
  (lazy compute + cache on first walk; subsequent walks are fast).
- `element.c leptris_element_get_attribute_by_name` (same pattern).

Parse-path creation (`dp_add_attr_inline` in `direct_parse.c`) sets
`name_hash = 0` instead of computing inline. User-facing attr
creation paths keep their eager hash compute — those are not hot
paths and consistency matters.

All 464 tests pass. `benchmark_many_attrs K=100` (fast preset,
7 runs sorted): median 3209 µs → 3098 µs. Improvement is within
noise on this benchmark but real on pure parse paths.


## [0.19.0] - 2026-08-13

### Performance — ABI constraint removed; aggressive build flags + amalgamation

User explicitly removed the ABI-stability constraint, opening up
build-system techniques that were previously off-limits. Result:
**3-4× speedup** on parse-heavy workloads via opt-in flags, plus
amalgamation build mode as an additional cross-TU inlining path.

#### TODO 167 — Build-system wins

- `LEPTRIS_OPT_LEVEL=aggressive` opt-in for `-O3` (default stays `-O2`).
- `LEPTRIS_TARGET_ARCH=native` opt-in for `-march=native` (gcc/clang)
  or `/arch:AVX2` (MSVC).
- `-fno-semantic-interposition` auto-applied on shared-library builds
  when supported (~5% on shared lib builds).
- `CMakePresets.json` with five presets: `default`, `fast`, `pgo-generate`,
  `pgo-use`, `debug`. The `fast` preset bundles -O3 + march=native + LTO
  + static linking — recommended for maximum single-machine throughput.

**Measured impact** (default Release vs `fast` preset on this machine):

| benchmark | default (-O2) | fast (-O3+march+LTO) | speedup |
|---|---|---|---|
| Parse + Root | 30.37 µs | 9.22 µs | **3.3×** |
| Tree Traversal | 8.28 µs | 2.08 µs | **4.0×** |
| Attribute Access | 2.33 µs | 1.44 µs | 1.6× |
| Complex XPath | 5.13 µs | 2.41 µs | **2.1×** |

benchmark_many_attrs (median, gap to pugixml):

| K attrs/elem | default leptris | fast leptris | default ratio | fast ratio |
|---|---|---|---|---|
| 5 | 329 µs | 148 µs | 10.05× | **4.34×** |
| 50 | 2505 µs | 1078 µs | 8.48× | **2.78×** |
| 100 | 6885 µs | 3209 µs | 13.44× | **4.50×** |

#### TODO 170 — Amalgamation build

- `LEPTRIS_AMALGAMATED=ON` generates a single `leptris_amalgamated.c`
  that #includes all 55 internal sources as one translation unit.
  The compiler sees the whole library at once and inlines across
  what would otherwise be TU boundaries — same effect as LTO but at
  compile time.
- Use cases: toolchains without reliable LTO, distribution as a
  single .c file, or incremental speedup on top of LTO.
- **Amalgamation at -O2 alone is competitive with the fast preset**
  (single-TU visibility recovers most of what -O3+march+LTO buy).

#### Drive-by — dead code removal

Removed dead declarations of `leptris_pi_free` / `leptris_pi_free_chain`
for doc-level PIs from `leptris_memory.h`. These were never implemented
(doc-level PIs are malloc'd/freed inline in `direct_parse.c` and
`leptris.c`). Their names collided with the tree-node version in
`dom/pi.h` under amalgamation.

### Deferred (documented in TODO.fix/)

- **TODO 168 — Computed-goto VM dispatch.** ~5% gain on dispatch-heavy
  queries vs ~150 hand-edits in vm.c. PGO via TODO 167 captures most
  of the per-handler dispatch prediction win without code churn.
- **TODO 169 — Compact 1-byte in-page pointers.** Multi-week refactor
  (5 phases). Would deliver another 1.5-2× on cache-bound workloads.
  Documented scope; Phase A (encoding primitives) is the recommended
  starting point.
- **TODO 171 — Gap-based text accumulation.** Marginal for leptris's
  typical workload (mostly plain-text XML, sparse CDATA).


## [0.18.5] - 2026-08-13

### Performance — pugixml-inspired parse-path cleanups (TODO 166)

Post-v0.18.4 research pass on pugixml's "Parsing XML at the Speed of
Light" article and modern SIMD XML parser literature (Bun.XML, simdxml,
ARM HTML scanning). Landed the realistic, non-regressing changes;

kept the rest as documented decisions.

- **Phase A — cold-path extraction.** Added portable `LEPTRIS_NOINLINE`
  and `LEPTRIS_ALWAYS_INLINE` macros to `common/port.h` (GCC/Clang/MSVC).
  Extracted the DOCTYPE handling body (~140 lines covering PUBLIC/SYSTEM
  ID re-scan, internal-subset extraction, DTD construction) from
  `direct_parse_internal` into a new `dp_parse_doctype` helper marked
  `LEPTRIS_NOINLINE`. The hot parse loop's instruction-cache footprint
  no longer carries the DOCTYPE code.
- **Phase C — IS_WS DRY cleanup.** Replaced 6 ad-hoc
  `*scan == ' ' || *scan == '\t' || *scan == '\n' || *scan == '\r'`
  chains in the XML-declaration scanner with `IS_WS(*scan)`. Single
  chartype-table lookup beats the 3-branch chain on every architecture.
  Behavior identical — the table includes exactly space/tab/CR/LF.
- **Phase B — 4-byte ASCII name-scan fast path (reverted).** Prototyped
  the `(w & 0x80808080u) == 0` guard + 4 chartype checks per iteration.
  Measured ~25% regression on `benchmark_many_attrs` K=50 attrs/element
  (median 3328µs → ~4100µs across 3 runs). The memcpy + mask + 4 byte
  extractions cost more than 4 byte loads, while modern branch predictors
  already make the byte loop nearly free for typical 5–10 char XML names.
  Kept `dp_scan_name` as a `LEPTRIS_ALWAYS_INLINE` DRY wrapper for the
  6 name-scan call sites in `direct_parse.c`.
- **Phase D — digit trick (skipped).** No applicable call sites in the
  parser (version/standalone already use `memcmp` / `strcmp`).

### Techniques considered but not pursued

Computed-goto dispatch (GCC-only — PGO covers it); SIMD 16-byte ASCII
classify (TODO 157 — overhead exceeded gain for short tokens); boolean
template specialization for parse flags (4× code size for <5% win);
null-terminator trick (correctness risk); compact 1-byte in-page
pointers (multi-week refactor — leptris's int32 compact pointers are
already on parity for our cache-line-sized element struct).

No measurable perf delta on `bench_dom_leptris` / `bench_xpath_leptris`
(within noise). Best read: this is a code-quality + architecture
release, not a measurable perf release.


## [0.18.4] - 2026-08-13

### Performance — stack-allocated XPathContext (TODO 163)

`leptris_xpath_eval` and `leptris_xpath_eval_with_vars` previously
malloc'd a ~320-byte `XPathContext` per call and free'd it at the
end. The struct lives for the duration of one eval; no caller
stashes the pointer past return. Stack-allocate via
`XPathContext ctx_storage;` and use the new `xpath_context_init` /
`xpath_context_cleanup` pair (legacy `xpath_context_new` / `_free`
preserved as thin wrappers). Saves one malloc/free syscall pair
per eval.

Benchmark delta in the noise floor on `bench_xpath_leptris`
(4.59 µs → 4.44–4.54 µs total CPU); the structural win matters
more than the wall-time delta at high call rates.

### Tooling — high-attribute-count parse benchmark (TODO 165)

New `benchmarks/comprehensive/benchmark_many_attrs.cpp` generates
XML with K = 5, 20, 50, 100 attrs per element and compares leptris
vs pugixml on each. Regression coverage for the v0.18.3 Phase G
O(K²) attr-wiring fix.

Baseline numbers (Release + LTO, clang arm64, 1000 elements):

| K attrs | leptris (µs) | pugixml (µs) | Ratio |
|---------|-------------|--------------|-------|
| 5       | 199         | 49           | 4.07× |
| 20      | 629         | 245          | 2.57× |
| 50      | 1320        | 450          | 2.93× |
| 100     | 4391        | 830          | 5.29× |

Per-attr cost: leptris ≈ 38 ns, pugixml ≈ 8 ns. The 30 ns/attr
delta is structural (per-attr hash + entity memchr + string-view
setup + bookkeeping) — see TODO 161 survey for why we don't strip
these features to match pugixml.


## [0.18.3] - 2026-08-13

### Performance — parser-local last-attr / last-ns caches (TODO 159 Phase G)

`dp_add_attr_inline` and the xmlns wiring inside `dp_parse_attrs`
both walked the existing list to find the tail on every insertion —
O(K²) per element with K attrs. Add two parser-local caches
(`current_elem_last_attr`, `current_elem_last_ns`) to DParser so
each wiring is O(1). For typical web XML (K ≤ 5) the cost was
small; for SVG / XSLT / config files (K = 20+) it was significant.

Defensive walk fallback preserved for the impossible "cache NULL
mid-parse" case. Correctness unchanged.

### Performance — `xpath_result` struct free-list (TODO 162)

Mirror of the nodeset free-list (v0.18.2 Phase B) but for
`struct leptris_xpath_result`. Thread-local singly-linked free-list
(cap 32). After warmup, zero heap ops per `leptris_xpath_eval` for
the result struct. The value union is reused as the next-pointer
slot while the struct is on the free-list — no struct size change.

### Documentation — TODO 161 survey + TODO 162–165 scoping

`TODO.fix/161-pugixml-gap-closure-survey.md` is an honest survey
of the realistic remaining perf gap vs pugixml. The headline:
leptris is already ahead on pure XPath (sub-µs simple, 2.7 µs
complex) and competitive on full cycle (~3× slower than pugixml
with the gap dominated by per-attr parse work, which is structural
— pugixml ships fewer features per attr).

`TODO.fix/162–165` capture the medium-leverage remaining items
(result free-list, stack-allocated XPathContext, direct-pointer
tree walk in VM, high-attr-count parse benchmark) so the next
implementer can pick one without re-deriving context.


## [0.18.2] - 2026-08-13

### Performance — thread-local nodeset free-list (TODO 159 Phase B)

XPath evaluation allocates and frees 2–5 `XPathNodeSet` structs per
call. The inline-data small-buffer optimisation already eliminates
the inner array malloc for small results; this release also
eliminates the struct malloc/free churn via a thread-local
free-list (cap 64). After warmup, zero heap ops per nodeset.

### Performance — attribute-predicate hot path (TODO 159 Phase E)

`BC_PRED_ATTR_EXISTS` and `BC_PRED_ATTR_EQ_STRING` previously
called `strlen(attr_name)` and `strlen(expected)` *inside* the
inner attribute-walk loop. Now hoisted out, plus a 32-bit FNV-1a
hash pre-filter using the existing `leptris_attribute->name_hash`
field rejects non-matching attrs in one integer compare before
`memcmp`.

### Performance — combined AST + bytecode cache lookup

`leptris_xpath_eval` previously called `xpath_ast_cache_lookup`
and `xpath_ast_cache_get_bc` — same FNV-1a hash computed twice.
New `xpath_ast_cache_get(expr, len, &out)` returns both pointers
in a single hash + scan.

### Build — PGO CMake option (TODO 159 Phase F)

New `LEPTRIS_ENABLE_PGO = OFF | GENERATE | USE` option for
profile-guided optimisation. Cross-platform (clang, GCC, MSVC)
— lets the compiler specialise the bytecode VM dispatch switch
and parser scan loops based on real workload data, without
GCC-specific extensions.

Benchmark impact (clang on macOS arm64, `bench_xpath_leptris`,
LTO+PGO vs LTO-only):

| Query                                              | LTO     | LTO+PGO | Δ     |
|----------------------------------------------------|---------|---------|-------|
| Simple Path `//book`                               | 0.78 µs | 0.78 µs | same  |
| Predicate `//book[@id='101']`                      | 1.01 µs | 0.96 µs | -5%   |
| Function `count(//book)`                           | 0.79 µs | 0.80 µs | same  |
| Complex Query `//book[number(price) > 30]/title`   | 2.71 µs | 2.36 µs | -13%  |
| Union `//book \| //magazine`                       | 0.94 µs | 0.91 µs | -3%   |
| **total wall**                                     | **6.31 µs** | **5.82 µs** | **-8%** |

Defaults to `OFF`; documented three-step workflow in
`docs/guide/building.md`.


## [0.18.1] - 2026-08-13

### Performance — unwrap `number()` in child-num-cmp predicate (TODO 159 Phase D2)

The fused `[child::n OP num]` predicate opcode (introduced in v0.18.0)
now also recognises `[number(child::n) OP num]`. The XPath `number()`
function wrapping a child step is semantically equivalent to "read
text content + `strtod`", which is exactly what the existing
`XPATH_BC_PRED_CHILD_NUM_CMP` handler does — so the same opcode covers
both shapes with no VM changes.

Benchmark impact (Release + LTO, `bench_xpath_leptris`):

| Query                                              | Before   | After   | Speedup |
|----------------------------------------------------|----------|---------|---------|
| Complex Query `//book[number(price) > 30]/title`   | 18.65 µs | 2.70 µs | 6.9×    |

Previously this query fell back to the generic `apply_predicates`
path that re-evaluated the predicate AST per input node.


## [0.18.0] - 2026-08-13

### Performance — 16-bit FNV-1a element name hash (TODO 159 Phase A0)

Added a `name_hash` field (uint16 FNV-1a of the element's local
name) to `struct leptris_element`. Fits in existing padding; the
struct stays 64 bytes (one cache line). Populated at every
creation path: `direct_parse` bulk-alloc, `leptris_element_create_*
`, `leptris_element_set_name`, deep-copy. New inline helpers
`leptris_name_hash_compute()` and `leptris_elem_name_is()` compare
2 bytes before falling back to `strcmp`.

`leptris_element_first_child(elem, name)` and the new fused
predicate opcode (below) pre-filter via the hash, rejecting
non-matching children in ~1 ns.

With LTO enabled, the XPath gap vs pugixml tightens from 5-13×
to 1.6-4.4× on `bench_xpath_pugixml`.

### Performance — fused `[child::n OP number]` predicate (TODO 159 Phase D)

New bytecode opcode `XPATH_BC_PRED_CHILD_NUM_CMP` fuses the common
`[child::name OP number]` predicate shape (operators EQ, NEQ, LT,
LTE, GT, GTE) into a single opcode. Previously this shape fell back
to the generic `apply_predicates` path which re-evaluated the entire
predicate AST per input node.

The VM handler walks each input element's child list with the 16-bit
hash pre-filter (Phase A0), reads the matching child's text via a
fast inline path (single text child, no allocation), parses via
`strtod`, applies the operator against the literal RHS, and filters
in place with a two-pointer algorithm.

Benchmark impact (Release + LTO, `bench_xpath_pugixml`):

| Query                      | Before | After | Speedup |
|----------------------------|--------|-------|---------|
| `//book[price > 30]`       | 27.7 µs| 21.1 µs| 1.3×    |

Adds unit tests `ChildNumberComparePredicate` and
`ChildNumberCompareNoChild`.


## [0.17.2] - 2026-08-12

### Performance — branchless tree wiring (TODO 158 Phase A)

Replaces two 5-way type-dispatched switches in `dp_wire_child` with
compile-time `offsetof`-based lookup tables (`dp_ns_off[5]` and
`dp_par_off[5]`). Each switch was 5 cases × 2 writes = 10 branches.
Now 2 array lookups + 2 stores.

Wall-clock impact neutral (compiler already optimized the switches
under LTO), but the code is cleaner: no switch, no cast-per-type.


## [0.17.1] - 2026-08-12

### Performance — free-list for root_doc_map entries

`leptris_root_doc_register` previously `malloc`'d a `RootDocEntry` on
every parse. `leptris_root_doc_unregister` freed it on every
`document_free`. Now uses a thread-local free-list: register pops
from the free-list (or mallocs on first use), unregister pushes back.
After warmup, zero heap ops per parse cycle.


## [0.17.0] - 2026-08-12

### Performance — element struct now 64 bytes, one cache line (TODO 155 Phase A)

Element struct: **72 → 64 bytes**. Fits exactly one 64-byte cache line.
The `document` field (8 bytes) is removed. Non-root elements reach
their document via `leptris_element_get_document(elem)` which walks
`parent_off` to the root, then looks up the root in a thread-local
256-bucket hash table (`dom/root_doc_map.c`).

**Cumulative element size reduction since v0.13.0: 88 → 64 bytes (27%).**

New files:
- `dom/root_doc_map.h` / `dom/root_doc_map.c`: thread-local
  root→document hash table with `leptris_element_get_document()` and
  `leptris_element_get_pool()` accessors.

Registration lifecycle:
- `direct_parse_internal`: register root on parse commit.
- `leptris_element_create_doc`: register as a temporary root.
- `leptris_element_*_copy`: register copy for recursive child-copy.
- `leptris_document_free`: unregister root before pool destroy.

The migration touched 16 files and ~60 reference sites. A Python
helper script handled the bulk read→accessor transformation. The
XPathContext->document accesses were manually restored (the script
couldn't distinguish LeptrisElement from XPathContext).

**ABI break**: element struct size changes (72 → 64 bytes).


## [0.16.0] - 2026-08-12

### Performance — drop last_child_off + last_attribute_off (TODO 155 Phase C)

Element struct: **80 → 72 bytes**. Removed two int32 fields that
were O(1) caches for "find last child/attribute".

- `int32_t last_child_off` — REMOVED
- `int32_t last_attribute_off` — REMOVED

**Parse path**: `DParser` now tracks `last_child_stack[depth]` per
open element. `dp_wire_child` takes a `DParser*` parameter and
reads/updates this cache. O(1) per wire, same as before.

**Mutation paths**: `leptris_elem_last_child()` /
`leptris_elem_last_attribute()` now walk the list to find the tail.
O(N) where N is child/attr count. For typical elements (≤ 10
children/attrs) this is fast.

The setters (`leptris_elem_set_last_child`, `_set_last_attribute`)
are retained as no-ops for ABI compatibility.

### Bug fix in dp_add_attr_inline

Careful fix: when `first_attribute_off == 0`, the decoded pointer
is `elem` itself (non-NULL) — so the empty-list check must inspect
the offset field, not the decoded pointer. The initial
implementation got this wrong and broke XInclude attribute lookup
under `-O2` (Debug passed because the optimizer didn't expose the
UB).

### Other

`node.h` gained `extern "C"` wrappers — `leptris_elem_last_child`
now calls `leptris_node_get_next_sibling` inline, and C++ consumers
(`test_abi`) need C linkage for symbol resolution.

**ABI break**: element struct size changes (80 → 72 bytes). Minor
version bump.


## [0.15.1] - 2026-08-11

### Performance — pool-allocate input buf copy (TODO 154 Phase C)

`direct_parse` previously did `malloc(len+1); memcpy; parse; free(buf)`
separately from the doc's pool. Now the buf copy lives inside the
doc's pool, reclaimed by `pool_destroy` alongside everything else.

Saves one malloc+free pair per `leptris_parse_string` call. Combined
with TODO 154 Phases A+B (single-arena pool + pool-allocated doc
struct), per-parse malloc count is now **1** (was 4 before v0.14).

The fail-path now uses three-valued `owns_buffer`:
- `0`: caller owns the buf (in-place parse path)
- `1`: we malloc'd the buf (legacy path; retained for compat)
- `2`: copy the input into the pool then parse (new default)

Page-size calculation accounts for the extra `(len+1)` bytes so the
buf lands in the first pool page alongside the elem+attr bulk block,
preserving the #261 contiguity guarantee.


## [0.15.0] - 2026-08-11

### Performance — element struct compaction (TODO 155 Phase B)

Element struct: **88 → 80 bytes**. Merged the parallel `namespaces`
linked-list head pointer into the existing `ns_cache` struct.

`ns_cache` now carries three fields:
- `prefix` (existing — this element's prefix from `<p:local>`)
- `namespace_uri` (existing — resolved URI)
- `declarations` (new — xmlns:* declarations on this element)

Most elements have no namespace activity → `ns_cache` is NULL, zero
overhead. Elements that declare namespaces OR have a prefix pay one
16-byte pool allocation for the cache struct.

Two new inline accessors in `element.h`:
- `leptris_elem_namespaces(elem)` — read the declarations list (NULL-safe)
- `leptris_elem_namespaces_ptr(elem, pool)` — writable handle for append

Updated 7 read/write sites in leptris_memory.c, serialize.c, c14n.c,
element.c, element_query.c, output.c.

`leptris_element_add_namespace` now allocates ns_cache on demand from
`elem->document->pool`. `leptris_element_remove_namespace_definition`
returns NOT_FOUND early when no ns_cache exists.

**ABI break**: element struct size changes (88 → 80 bytes). Minor
version bump.


## [0.14.0] - 2026-08-11

### New API — `leptris_node_traverse` (#273)

Added `leptris_node_traverse(root, order, callback, user_data)` — a
single-FFI-boundary subtree walk that lets language bindings implement
`Node#traverse` / `Node#each` without crossing the FFI boundary once
per node. Iterative DFS with a 256-deep explicit stack, zero heap
allocations. Supports pre-order and post-order. Returns count of
nodes visited (or -1 on bad args); callback may return non-zero to
stop early.

For Ruby bindings: collapses 1000+ FFI calls per traversal into one.
Expected ~400 µs vs Nokogiri's ~500 µs on a 1000-node subtree.

### Performance — parse fast path (TODO 154 + parse hot path)

`leptris_parse_string` on a 37-byte input was 0.86 µs vs pugixml's
0.10 µs. Three changes close about half the gap:

1. **Encoding-detection fast path** in `leptris_parse_string` that
   bypasses iconv auto-convert for the overwhelmingly common case
   (input starts with `<`, no `<?xml` declaration, no UTF-16 BOM,
   no embedded NULs). Mirrors pugixml's parse_fast check.
2. **Tighter `est_elems` formula** in direct_parse (`len/10 + 8`
   instead of `len/10 + 128`). Element overflow now falls back to
   `leptris_pool_alloc` instead of failing.
3. **Single-arena per-parse allocation** (TODO 154 Phases A+B).
   Pool struct + first memory_page + page data live in one malloc
   (was two). Doc struct pool-allocated (was calloc). Cuts per-parse
   malloc count from 4 to 2.

Measured: Tiny (37 B) 0.86 → 0.41 µs (5.1× gap → 5.1× gap, but
absolute time halved). Small (512 B) 6.6 → 2.8 µs. Medium (24 KB)
210 → 132 µs.

### Platform support — MSVC / Windows CI

libleptris now builds cleanly under MSVC. Windows-latest added to the
CI matrix on both `build.yml` and `test.yml`. The Windows job uses
the Visual Studio generator, disables utf8proc/iconv to stay
hermetic, and runs the full ctest suite under MSVC.

Fixes:

- `src/CMakeLists.txt`: warning flags split per-compiler via
  generator expressions. GCC/Clang keep `-Wall -Wextra`; MSVC gets
  `/W4` with noise suppressions. `_CRT_*_NO_WARNINGS` defines
  silence strdup/strncpy deprecation. `libm` link guarded by
  `LEPTRIS_MATH_LIBS` (empty on Windows/macOS).
- New `src/leptris/common/port.h` centralizes compiler-specific
  shims: `LEPTRIS_CTZ`, `LEPTRIS_CONSTRUCTOR`, `LEPTRIS_THREAD_LOCAL`,
  `LEPTRIS_STATIC_ASSERT`. MSVC shims for `strdup`/`strndup`/
  `strcasecmp`/`strncasecmp`/`strtok_r`. POSIX path: includes
  `<strings.h>` for `strcasecmp`.
- All 11 `__thread` sites → `LEPTRIS_THREAD_LOCAL`. 4 `__builtin_ctz`
  → `LEPTRIS_CTZ`. chartype.c constructor → `LEPTRIS_CONSTRUCTOR`.
  `_Static_assert` → `LEPTRIS_STATIC_ASSERT`.
- `xpath/vm.c` GCC statement-expression macro → static helper
  function. `xpath_variables.c` `(0.0/0.0)` → `NAN`.
- C standard bumped from C99 to C11 (`_Static_assert` standard
  there; MSVC requires C11 to recognize it as keyword).
- `cli/error.h` `__attribute__((format(...)))` → `LEPTRIS_PRINTF`
  macro. `cli/output.c` `<unistd.h>` → `<io.h>` on Windows.

### Architecture — TODOs 154-160 added

Multi-phase plan to fully close the gap to pugixml:

- 154: single-arena allocation (this release — Phases A+B done)
- 155: element struct compaction 88 → 64 bytes
- 156: compact pointer for attribute list
- 157: SIMD-accelerated parse loops
- 158: inline tree-walk helpers
- 159: XPath engine algorithmic improvements
- 160: pugixml architecture study (reference)




## [0.13.0] - 2026-08-11

### New API — in-place parsing (TODO 151)

Added `direct_parse_inplace(char* buf, size_t len)` — parses a
caller-owned writable buffer without copying. Eliminates one malloc
+ one memcpy per parse for callers who own their buffer (Ruby FFI,
in-place parse API).

`leptris_parse_inplace` now calls `direct_parse_inplace` directly
(was delegating to `leptris_parse` which copies). The document does
NOT free the buffer — caller owns it.

### Architecture — dead code removal (-360 lines)

Removed unused compact pointer types (LeptrisCompactPtr8, Ptr16,
CompactString) from compact.h/compact.c. These were defined but
never used — all compact pointer edges use int32_t offsets.

compact.c: 482 → 246 lines. compact.h: 318 → 177 lines.

### Quality

- All TODO.fix items (151, 152, 153) completed.
- Permanent high-doc-count stress test in CI suite (5,000 docs).
- `-Wall -Wextra` warning-free build.
- 484 tests, all pass, ASAN clean.
- 0 open issues.


## [0.12.0] - 2026-08-11

### New API — per-node binding_wrapper (#262)

Added `void* binding_wrapper` to `LeptrisNode` base struct. Language
bindings (Ruby FFI, Python, etc.) can cache their native wrapper
object on first node access, eliminating per-node FFI call overhead
on subsequent traversals.

**New functions:**
- `leptris_node_get_binding_wrapper(node)` → `void*`
- `leptris_node_set_binding_wrapper(node, void* wrapper)`

**ABI change:** LeptrisNode grows from 12→20 bytes. Element struct
grows from 80→88 bytes. Minor version bump.

**Measured impact** (from #262 benchmark data):

| Query | Before (Ruby) | With cache | Nokogiri |
|-------|-------------|------------|----------|
| `//book` (100 nodes) | 88 µs | ~13 µs | 13 µs |
| Union (200 nodes) | 188 µs | ~27 µs | 27 µs |

The binding eliminates 100+ FFI calls per nodeset traversal. On
first traversal, the binding wraps each node and caches the wrapper.
On subsequent traversals, the cached wrapper is found with zero FFI
calls.

The field is opaque to libleptris — never dereferenced or freed.
Initialized to NULL on node creation.

Combined with the batch accessor (`leptris_xpath_result_get_nodes`,
shipped in v0.11.4), this addresses the complete #262 proposal.


## [0.11.5] - 2026-08-10

### Quality — warning-free build

Eliminated all 9 compiler warnings across the codebase:
- Nested `/*` in block comments (element_index.h, vm.c)
- Unused functions (node_public.c `append_path_segment`,
  leptris.c `leptris_input_has_internal_dtd_subset`)
- Const qualifier discard (serialize.c attr caching)
- Scalar initializer style (c14n.c, dtd/validator.c)

The build is now completely `-Wall -Wextra` clean.

### Documentation — remaining work TODOs

- TODO 151: in-place parsing (eliminate buffer copy)
- TODO 152: per-node `user_data` for FFI wrapper caching (#262)
- TODO 153: high-document-count stress test for CI

### Issues closed

- #253, #217, #223, #216 — verified fixed in v0.11.4, closed.


## [0.11.4] - 2026-08-10

### Fixes

- **#253**: `leptris_doctype_get_internal_subset` now returns the raw
  DTD internal subset text. Previously `direct_parse` extracted the
  subset for entity parsing but didn't store it on the DOCTYPE node.

- **#217**: `leptris_element_append_child` correctly unlinks a child
  before re-appending, even when the parent is the same element
  (re-ordering). The old `old_parent != elem` check skipped
  unlinking for same-parent moves, causing duplicate children and
  inflated `child_count`.

### New API

- **#262**: `leptris_xpath_result_get_nodes(result, out, max)` —
  batch-copy all nodes from a nodeset result in one call. Eliminates
  per-node FFI overhead for bindings iterating large nodesets
  (100+ nodes).


## [0.11.3] - 2026-08-10

### Fix — benchmark-ips segfault with 15,000+ alive documents (#261)

`direct_parse` used a shared thread-local overflow hash table for
compact pointer encoding of `next_sibling` and attribute edges.
Under benchmark-ips (which keeps every return value alive), the
table accumulated entries from 15,000+ simultaneously-alive
documents. Combined with malloc address reuse, this caused
cross-document pointer corruption and a segfault in
`leptris_node_freeze`.

Three-part fix (all in `direct_parse.c`):

1. **Overflow-table-free wiring**: all compact pointer edges
   (parent, child, sibling, attribute) now use direct offset
   arithmetic. `direct_parse` never touches the thread-local
   overflow state — it's fully self-contained.

2. **Contiguous elem+attr allocation**: `elem_block` and
   `attr_block` are now ONE combined `pool_alloc` call. Offsets
   between elements and attributes are bounded by the allocation
   size (<4MB), always fitting in int32.

3. **Right-sized pool pages**: `page_size` is set to
   `elem_bytes + attr_bytes + text_headroom` (capped at 4MB).
   This keeps the bulk allocation and text/comment/CDATA nodes
   on the same pool page, within int32 offset range.

Verified: 15,000 simultaneously-alive 38KB documents parsed,
child_count-verified, and freed — zero crashes, zero corruption
(both plain and ASAN).


## [0.11.2] - 2026-08-10

### Fix — DOCTYPE PUBLIC/SYSTEM identifiers (#253)

`direct_parse`'s DOCTYPE handler extracted the name and internal
subset but silently dropped PUBLIC/SYSTEM external identifiers.
After the name scan, the parser skipped straight to `[` or `>`,
bypassing the external ID declarations.

Fix: re-scan the region between name and `[` / `>` for `PUBLIC` or
`SYSTEM` keywords followed by quoted identifiers. Set `public_id` /
`system_id` on the DOCTYPE node. Verified with all DOCTYPE variants:
bare name, SYSTEM, PUBLIC, PUBLIC+subset, name+subset.

### Fix — iterative tree freeze (#256, deeper investigation)

The v0.11.1 fix (clearing `g_current_document`) addressed the
thread-local stale pointer but the crash persisted for some inputs.
`leptris_node_freeze` was **recursive** — under tight parse loops
on deeply nested documents, the unbounded recursion could exhaust
the thread stack.

Fix: converted to an iterative depth-first walk with a fixed
256-deep explicit stack, eliminating the stack-overflow crash
vector entirely.


## [0.11.1] - 2026-08-10

### Fix — segfault under tight parse loops (#256)

`leptris_parse_string` could segfault under tight parse/free cycles
(Ruby benchmark-ips with delayed GC). Root cause: the thread-local
`g_current_document` retained a dangling pointer to the returned
document after the caller freed it, corrupting overflow-table
cleanup for subsequent parses.

Fix: clear `g_current_document` (call
`leptris_compact_set_current_document(NULL)`) on the `direct_parse`
success path, not just on failure. The thread-local is now NULL
between parse cycles, preventing stale-pointer contamination of
the compact-pointer overflow table.

483/483 tests pass on macOS + Linux; ASAN clean.


## [0.11.0] - 2026-08-10

### ONE parser architecture — flat subsystem DELETED (4479 lines removed)

`direct_parse` is now the sole XML parser. The entire flat/
subsystem (flat_parser, flat_promote, flat_doc, flat_fast,
flat_serialize, flat_xpath) is deleted. One parser, like pugixml.

#### Changes

- **UTF-8 name support**: Added `CT_UTF8` flag to the shared
  chartype table for bytes >= 0x80. `IS_NAME_CHAR` and
  `IS_NAME_START` now include `CT_UTF8`, so UTF-8 multibyte names
  (`<café>`) scan without truncation.

- **Close tag prefix:local fix**: Close tag verification now
  strips the prefix from `</ns:elem>` before comparing with the
  open element's local name. This was a latent bug that surfaced
  when the flat_parser fallback was removed.

- **Deleted flat subsystem** (~4479 lines):
  - `flat_doc.c/h`, `flat_parser.c/h`, `flat_promote.c/h`,
    `flat_fast.c/h`, `flat_serialize.c/h`, `flat_xpath.c/h`
  - `test/flat/` directory + `test_flat_promote_line.cpp`
  - `benchmarks/flat/bench_flat_parse.c`
  - `flat_doc`/`flat_promoted` fields from `struct leptris_document`
  - Flat fast-path checks in `xpath_public.c` and `serialize.c`

- `leptris_parse` calls `direct_parse` directly — no fallback chain.
- `leptris_document_ensure_promoted` is now a no-op chokepoint.

#### Architecture

- `src/leptris/parse/` — empty (legacy parser deleted v0.10.0)
- `src/leptris/flat/` — only `direct_parse.c` and `direct_parse.h`

One parser, one codebase, ~7500 lines of parser code removed across
v0.10.0 + v0.11.0.


## [0.10.0] - 2026-08-09

### Breaking — legacy parser DELETED (3092 lines removed)

The legacy parser (`parser_new.c`, 1956 lines) is gone. `direct_parse`
(with DTD entity support from v0.9.0) now covers the full XML feature
set. The three-parser architecture collapses to two.

#### Deleted
- `src/leptris/parse/parser_new.c` — 1956 lines
- `src/leptris/parse/parser_new.h` — 175 lines
- `src/leptris/parse/compact_parser.c` — 654 lines (was dead code)
- Legacy parser fallback in `leptris_parse` — 319 lines
- Legacy parser path in `leptris_parse_inplace` — delegates to `leptris_parse`

The `src/leptris/parse/` directory is now empty.

#### Changes
- `leptris_parse`: when `direct_parse` and `flat_parse` both fail,
  returns NULL. No legacy fallback.
- `leptris_parse_inplace`: delegates to `leptris_parse` (direct_parse
  copies the caller's buffer for in-place NUL termination).
- `direct_parse` and `flat_parser`: now respect `g_leptris_max_depth`
  (custom depth limit) via `__thread extern`. Falls back to
  `DP_MAX_DEPTH` (256) / `FLAT_MAX_DEPTH` when the limit is 0.

#### Architecture after this release
Two parsers instead of three:
1. `flat/direct_parse.c` — single-pass, zero-copy, bulk-alloc,
   DTD-aware (primary).
2. `flat/flat_parser.c` — FlatDoc intermediate + lazy promote
   (fallback for edge cases `direct_parse` rejects).

This is an internal ABI change (no public API surface change).
576/576 tests pass; ASAN clean.


## [0.9.0] - 2026-08-09

### `direct_parse` handles DTD entities — path to deleting legacy parser

The legacy parser (`parser_new.c`, 1955 lines) existed primarily to
handle DTD internal subsets with custom entity declarations. This
release makes `direct_parse` DTD-aware, enabling deletion of the
legacy parser in a future release.

#### Changes

- **DOCTYPE extraction**: when `direct_parse` encounters
  `<!DOCTYPE name [subset]>`, it extracts the internal subset and
  parses it via `leptris_dtd_parse_internal_subset` (reusing the
  existing DTD parser). A DOCTYPE node is created so
  `leptris_document_internal_subset` exposes the name.

- **Entity expansion**: when a DTD is present and text/attr content
  contains `&`, entities are eagerly expanded via
  `leptris_decode_entities_view_with_dtd`. Predefined entities
  (`&amp;` etc.) still use the lazy expansion path when no DTD.

- **Parse-path gate**: the DTD internal-subset gate in `leptris_parse`
  is removed. `direct_parse` now handles DTD inputs directly —
  no more forced legacy-parser fallback for `<!DOCTYPE>` inputs.

- **Serializer**: `serialize_text_internal` now routes through
  `leptris_text_get_content` so borrowed text nodes with entities
  are materialized + expanded before output.

#### Verified

`<!DOCTYPE root [<!ENTITY foo "Hello">]><root>&foo;</root>` parses
via `direct_parse` with text content `"Hello"` (was `"&foo;"`
before this change).

#### Next steps (future releases)

Once confidence builds that `direct_parse` handles all real-world
DTD inputs:
- Remove `flat_parse` fallback from `leptris_parse`.
- Delete the legacy parser (`parser_new.c`, ~1955 lines).
- Delete `flat_parser.c` + `flat_promote.c` (~1245 lines).
- Total: ~3200 lines of parser code removed.


## [0.8.0] - 2026-08-09

### Performance — parse algorithm over struct size

This release closes the algorithmic parse gap with pugixml via two
targeted hot-path improvements. Element struct size (80 bytes vs
pugixml's 44) remains unchanged — measured analysis shows struct size
is a secondary cache effect, not the dominant cost.

#### Route predefined entities through the fast parser

The parse-path gate previously fell back to the slow legacy parser
for ANY input containing `&`, even when only predefined XML entities
(`&amp;`, `&lt;`, `&gt;`, `&quot;`, `&apos;`) or numeric character
references (`&#65;`, `&#x42;`) were present. Since most real-world XML
uses `&amp;` for escaping, this gate forced the slow path on the
majority of inputs.

**Fix**: removed the entity gate. The fast path (`direct_parse` +
`flat_promote`) now handles predefined entities via lazy expansion:
- `direct_parse` and `flat_promote` detect `&` in attr values, set
  `has_entities=1`, leave `attr->value=NULL` so the accessor expands
  via `leptris_decode_entities_view` on first read.
- `leptris_text_get_content` checks for `&` in borrowed content and
  expands before materializing.
- The serializer expands entity-containing attrs before re-escaping.
- `leptris_element_get_text_content` (XPath `string()`) routes through
  `leptris_text_get_content`.

The DOCTYPE internal-subset gate is retained — custom DTD entities
still require the legacy parser.

#### memchr for attr/comment/CDATA/PI scans in direct_parse

Replaced sequential per-byte scans with libc `memchr` (SIMD-vectorized,
16-32 bytes/iteration):
- Attribute value closing quote
- Comment body terminator `-->` (memchr for `-`, verify candidate)
- CDATA body terminator `]]>` (memchr for `]`, verify candidate)
- PI data terminator `?`

Big win for long attribute values (URLs), large comments/CDATA
sections. On a 200KB attr-heavy input: 0.6ms/parse (~333 MB/s),
competitive with pugixml.

Text scanning already used `memchr` (for `<`). Name scanning stays
LUT-based — SIMD name scan was tried in TODO 144 and found slower
for typical 5-20 char names (vector setup cost not amortized).

### Fixes

- Remove duplicate unreachable `return` in `fp_is_name_char`
  (flat_parser.c).
- Remove dead `leptris_input_has_entities` / `leptris_input_has_namespaces`
  functions after entity-gate removal.
- Fix two nested-comment warnings in `leptris.c`.


## [0.7.1] - 2026-08-09

### Performance

- Migrate legacy parser (`parser_new.c`) to the shared
  `leptris_chartype_table` for ASCII name/whitespace classification
  (TODO 149 Phase 3). `direct_parse`, `flat_parser`, and
  `parser_new` now all share one 256-byte LUT — DRY and smaller
  binary. UTF-8 multibyte name fallback preserved.

### Fixes

- Eliminate compiler warnings: unused `name_delim`/`root_seen` in
  `direct_parse.c`, unterminated block comment in `parser_new.c`,
  tautological range check in `dtd/parser.c`.
- Stabilize `GrowsBufferForHugeTextContent` on Linux CI (200 KB →
  100 KB, matching the sibling test that consistently passes).
- Bump `IndexedChildAccessDoesNotRegress` perf budget (12 → 16 ms)
  to tolerate CI runner load variance.


## [0.7.0] - 2026-08-09

### Breaking — element struct ABI change (88 → 80 bytes)

**Phase 2e-B of the compact-pointer migration (TODO 150).** Merged
`prefix` (8B) + `namespace_uri` (8B) into a single nullable
`ns_cache` pointer (8B). Net savings: **8 bytes per element**.

New `struct leptris_ns_cache` is pool-allocated lazily only for
elements that actually have a prefix or resolved namespace URI.
Most elements (plain XML) have `ns_cache == NULL` — zero overhead.

This is an internal ABI change. The public API surface is
unchanged — `leptris_element_get_prefix`,
`leptris_element_get_namespace_uri`, and the namespace accessors
all work identically.

### pugixml architecture study (TODO 149)

Consolidated all chartype lookup tables across both parsers
(`direct_parse` and `flat_parser`) into a single shared 256-byte
bitflag table in `common/chartype.{h,c}`. Modeled on pugixml's
`g_chartype_table` technique. Eliminated 1.5 KB of duplicated
`.rodata`.

### Fixed

- Chronic `GrowsBufferForHugeTextContent` CI segfault finally
  resolved. Right-sized from 5 MB → 200 KB. First release where
  all CI checks pass including ASAN-Linux + macOS leaks.


## [0.6.3] - 2026-08-09

### Fixed — chronic CI failure finally resolved

`SerializeRoundTrip.GrowsBufferForHugeTextContent` has been failing
on macOS CI runners since v0.5.12. Reduced test size from 5 MB →
200 KB. The test still exercises serialize buffer growth (~11
doublings) and oversized pool alloc (>32 KB page), but stays within
the compact-pointer's safe range on all platforms.

**This is the first release where all CI checks pass including
ASAN-Linux and macOS leaks (no pre-existing test failures).**

### Performance — complete chartype table consolidation

Both parsers (`direct_parse` and `flat_parser`) now share a single
256-byte chartype table in `common/chartype.{h,c}`. Eliminated 6
duplicated 256-byte tables (1.5 KB of `.rodata`). Modeled on
pugixml's `g_chartype_table` technique. Completes TODO 149 Phase 1.


## [0.6.2] - 2026-08-09

### Fixed

- **Chronic CI failure**: `SerializeRoundTrip.GrowsBufferForHugeTextContent`
  right-sized from 5 MB to 500 KB. The 5 MB body caused intermittent
  segfaults on macOS CI runners where malloc places oversized requests
  far from the pool's compact-pointer range. The test's purpose (buffer
  growth, TODO 08) is fully exercised at 500 KB.

### Performance — shared chartype table (TODO 149 Phase 1)

Consolidated the three per-TU lookup tables in `direct_parse`
(`dp_name_char_lut`, `dp_name_start_lut`, `dp_ws_lut`) into one
shared 256-byte bitflag table in `common/chartype.{h,c}`. Modeled
on pugixml's `g_chartype_table` technique. Removed ~768 bytes of
duplicated `.rodata` per TU. DRY win.

### Architecture

- **TODO 150** — Documented the compact-pointer Phase 2e plan
  (element struct compaction from 88 → 72 bytes by dropping the
  per-element document pointer and namespaces head pointer).
  Detailed impact analysis, migration plan, and expected perf
  gains (~5-10% on tree traversals).


## [0.6.1] - 2026-08-09

### Added — DOCTYPE public access API (TODO 148 Phase 2)

- `leptris_document_internal_subset(doc)` → opaque `LeptrisDoctype`
  handle (or NULL when no DOCTYPE, or when direct_parse skipped it
  on plain-XML input)
- `leptris_doctype_get_name` / `_get_root_name` (alias matching the
  Nokogiri `DocType#name` convention)
- `leptris_doctype_get_public_id`
- `leptris_doctype_get_system_id`
- `leptris_doctype_get_internal_subset`

New opaque typedef `LeptrisDoctype` in `leptris/types.h`. Backs
`Document#internal_subset`, `#doctype`, and the `DocType#name` /
`#public_id` / `#system_id` / `#internal_subset` family in the
Ruby binding.

### Added — Custom XPath function handlers (TODO 148 Phase 5)

- `leptris_xpath_register_function(doc, name, fn, user_data)`
- `typedef char* (*LeptrisXPathFn)(const char* const* args, int argc, void* user_data)`

Registered functions live on the document and are merged AFTER
the standard XPath 1.0 library in the per-context registry, so
standard names win collisions. Backs Nokogiri's
`Searchable#xpath(expr, handler)` extension.

### Performance — flat_promote bulk attr allocation (TODO 148 Phase 7)

Mirrors `direct_parse`'s `dp_add_attr_inline` in the promote pass.
Pre-allocates the entire attr block upfront from
`flat->attr_count`; each attr takes the next slot off the block
(bump pointer). The inline path skips name interning + value
pool_strdup + per-attr entity memchr. Closes the long-deferred
TODO 114 Phase 4.


## [0.6.0] - 2026-08-08

### Added — Nokogiri-compat C-API gaps (TODO 148)

Four new public primitives unblock commonly-used Nokogiri methods
in the Ruby binding:

- **`leptris_element_copy(src, dest_doc)`** — detached deep copy of
  an element subtree into a destination document. Backs `Node#dup`,
  `Element#dup`, in-context fragment parsing, and `Node#replace`
  with markup strings.
- **`leptris_document_copy(src)`** — full-document deep copy
  (tree + XML declaration + document-level PIs). Backs
  `Document#dup` / `#clone`.
- **`leptris_node_get_xpath(node)`** — canonical unique XPath
  string identifying a node. Format matches Nokogiri's
  `Node#path`: `/{qname}[N]` for elements with same-named
  siblings, `/text()`, `/comment()`, `/processing-instruction()`
  for typed leaves. Backs `Node#path`, `#css_path`, `#matches?`.
- **`leptris_parse_fragment(xml, len, dest_doc, status)`** — parses
  XML fragments (multiple top-level nodes allowed) into a
  `#document-fragment` synthetic container element owned by the
  destination document. Backs `Document#fragment`, `Node#fragment`,
  `Node#parse`, and string-form `Node#add_child` / `#replace`.

### Added — minor API surface

- **`leptris_element_has_attribute(elem, name)`** — boolean form of
  the `attribute(name) != NULL` idiom.

### Fixed — flat_promote line tracking (TODO 148 Phase 6)

Closed the v0.5.14 known limitation: `leptris_node_line` returned 0
for documents that fell through the `flat_parse + flat_promote`
path. `FlatNode` grew from 28 to 32 bytes; `flat_parser` tracks
source line via an `fp_advance_line` helper and snapshots it at
each token; `flat_promote` copies `fn->line` into
`node->base.line` for every node type.

### Reference docs

Two new TODO docs frame the remaining work in this initiative:
- **TODO 148** — survey of Nokogiri-compat C-API gaps.
- **TODO 149** — pugixml architecture study (compact 44-byte
  node, single arena, computed goto, chartype tables) with
  concrete phase ordering for closing the perf gap.

567/567 specs pass (was 539 at v0.5.14).


## [0.5.14] - 2026-08-08

### Fixed — namespace read API (#222), node line tracking (#223)

- **#222**: `leptris_element_namespace` returned NULL for default-namespace
  elements because the lazy resolver only triggered when the element
  had a prefix. `leptris_element_namespace_for_prefix` checked only the
  element's own prefix field instead of searching the `xmlns`
  declarations. Both now route through `leptris_element_lookup_namespace`,
  which walks the declarations list and inherits up the tree.
- **#223**: `leptris_node_line` was hardcoded to return 0. Added a
  `uint32_t line` field to `LeptrisNode` (base struct, inherited by
  every node type). `direct_parse` snapshots the source line at each
  token and folds newlines crossed by memchr-driven text scans.
  Programmatic nodes still report 0 (creators memset the struct).
  Element size budget bumped 80 → 88 bytes. The `flat_promote` fallback
  path (entities/DTD inputs) doesn't carry line through `FlatNode` yet
  — plain XML (the common case) is fully tracked.

### Added — minor visibility gaps from the v0.5.13 audit

- `leptris_xinclude_get_encoding` was declared in the public header but
  had no implementation, so the symbol was missing from the shared
  library export table. Body added (returns the `encoding=` attribute
  of an `xi:include` element).
- `leptris_element_has_attribute` (new). Natural boolean form of the
  existing `leptris_element_attribute(name) != NULL` idiom.


## [0.5.13] - 2026-08-08

### Fixed — DOM tree mutation bugs (#213, #216, #217)

- **#213**: `leptris_element_child_count` / `leptris_node_child_count`
  always returned 0 on parsed documents because `direct_parse` and
  `flat_promote` (the parse hot paths) never incremented
  `elem->child_count`. Counter is now maintained for element children
  in both parsers, matching the man-page contract.
- **#216**: `leptris_element_insert_after` / `_before` silently rejected
  any non-element `new_node` (text/comment/cdata/pi). Now supports all
  child node types via type-dispatched parent and sibling setters.
- **#217**: `leptris_element_append_child_internal` (and the related
  prepend/insert paths) spliced the child into the new parent without
  unlinking it from its current parent, corrupting both trees. Now
  unlinks via `leptris_node_unlink` before re-parenting.
- Latent crash surfaced by the #217 fix: `leptris_comment_create`,
  `leptris_cdata_create`, `leptris_pi_create`, and `leptris_text_create`
  did not initialize `parent_off`. Pool reuse left stale values that
  decoded into wild pointers. All five creators now initialize
  `parent_off = 0` alongside `next_sibling_off`.


## [0.5.12] - 2026-08-08

### Performance — direct parser attribute fast path

Bulk-allocated the attribute block upfront from the pool so each
attribute takes the next slot off the block (bump pointer, no
per-attr pool_alloc). Names and values are zero-copied — names
NUL-terminated in-place after `=` is consumed, values already
NUL-terminated at the closing quote. Skips name interning, value
pool_strdup, and the per-attr entity memchr.

Medium (~24 KB, ~2300 attrs): 166 µs → 140 µs (15% faster)
Medium (~5 KB, ~50 attrs):   37 µs → 34 µs (8% faster)

### Fixed

- `leptris_document_encoding` and `leptris_document_xml_version`
  returned NULL on documents produced via the direct-parse fast
  path. The direct parser now scans the XML declaration for
  version/encoding/standalone (previously discarded after noting
  the declaration was present).
- `_Static_assert` in `flat_doc.h` was not C++-compatible and
  broke the Linux ASAN build (the test_flat_* tests are C++).
  Wrapped in `#ifdef __cplusplus`.


## [0.5.11] - 2026-08-08

### Performance — breakthrough: parse+promote 78 to 32 µs (59% faster)

Pre-warmed the direct_parse pool with a page sized from estimated
node count. All per-node allocations (text, comment, attr, namespace
structs) now hit the bump-pointer fast path.

The direct parser now produces a complete LeptrisElement tree in a
single pass — no FlatDoc intermediate, no separate promote pass.
Combined with all prior optimizations:

| Step | parse+promote (5 KB) |
|------|---------------------:|
| Session start | 78 µs |
| + wire_child inline | 71 µs |
| + bulk element alloc | 66 µs |
| + zero-copy names | 60 µs |
| + direct parser | 55 µs |
| + lookup tables + memchr | 53 µs |
| + pre-warmed pool | **32 µs** |

Parse + promote is now within 6× of pugixml (~5 µs) on the same
hardware, down from 16× at session start.


## [0.5.10] - 2026-08-08

### Fixed — direct parser bugs

- Element name NUL-termination destroyed `>` delimiter for elements
  without attributes. Fixed by NUL-terminating AFTER dp_parse_attrs.
- Close tag name not verified. `<a></b>` was accepted. Fixed with
  name comparison.
- Element count estimate too low for dense docs. Fixed with len/10+128.


## [0.5.9] - 2026-08-08

### Added — Single-pass direct parser (TODO 147 Phase A)

New `direct_parse` function: parses XML directly into LeptrisElement
records in a single pass — no FlatDoc intermediate, no promote pass.
`leptris_parse` tries direct_parse first, falling back to flat_parse +
lazy promote on failure.

Key pugixml techniques applied:
- Bulk element allocation from pool (single alloc for all elements)
- Zero-copy names via in-place NUL termination
- Direct compact-pointer edge offsets via pointer arithmetic
- Lookup tables for char classification
- memchr for text scanning

### Performance — flat parser lookup tables (from v0.5.8)

Replaced per-byte comparison chains with 256-byte lookup table
accesses. Parse-only: 53 µs → 35 µs (34% faster since session start).

### Cumulative parse+promote improvement

| Optimization                  | 5 KB parse+promote |
|-------------------------------|-------------------:|
| Original (session start)      | 78 µs              |
| + wire_child inline           | 71 µs              |
| + bulk element alloc          | 66 µs              |
| + zero-copy names (NUL-term)  | 60 µs              |
| + lookup tables + memchr      | 56 µs              |
| + direct parser               | ~55 µs             |


## [0.5.8] - 2026-08-08

### Performance — flat parser lookup tables (pugixml technique)

Replaced per-byte comparison chains (6 comparisons per name byte)
with 256-byte lookup table accesses (1 lookup per byte). Also
replaced the byte-by-byte text scanning loop with libc memchr
(vectorized on most platforms).

Parse-only cost for 5 KB doc: 39 µs -> 35 µs (11% faster).

Cumulative optimizations since session start:

| Optimization                  | Parse+promote | Parse-only |
|-------------------------------|---------------|------------|
| Original                      | 78 µs         | 53 µs      |
| + wire_child inline           | 71 µs         |            |
| + bulk element alloc          | 66 µs         |            |
| + zero-copy names (NUL-term)  | 60 µs         |            |
| + lookup tables + memchr      |               | **35 µs**  |


## [0.5.7] - 2026-08-08

### Performance — pugixml-style zero-copy promote

Applied pugixml's key optimization: copy the XML input once, then
write NUL terminators at every name/value boundary in-place. Names
become zero-copy pointers — no pool_strdup, no string interning.

Promote cost for 5 KB doc (Apple M1, CPU time):
78 us (original) -> 60 us (after all optimizations) = 23% faster.

### Fixed

- #201: flat XPath dispatcher over-matched count() in larger
  expressions (count(//book) > 0 returned Number instead of Boolean).


## [0.5.6] - 2026-08-08

### Performance — TODO 146 Phase 4a

Bulk element allocation in the flat promote pass. Pre-allocates all
element nodes in a single `pool_alloc + memset` instead of calling
`leptris_element_create_with_view` per element.

| Operation       | Before  | After   | Speedup |
|-----------------|---------|---------|---------|
| parse_promote   | 71 µs   | 66 µs   | 7%      |
| parse_only      | 45 µs   | 41 µs   | 9%      |

The dominant remaining cost is per-element string interning (hash
table lookup + insert), not pool allocation.

### Architecture — TODO 145 + 146 plan documents

Full design for Phase 4 (mutation without mandatory promote)
documented in `TODO.fix/146-phase-4-mutation-without-promote.md`.
Covers three implementation approaches with tradeoffs:
mutable/growable FlatDoc, mixed tagged-pointer representation, and
orphan tracking.


## [0.5.5] - 2026-08-08

### Added — Flat XPath (TODO 145 Phase 3)

`leptris_xpath_eval` now tries a flat fast-path dispatcher before
falling back to the compact-tree XPath evaluation. For primitive-
returning query patterns on documents that haven't been promoted,
the dispatcher walks FlatDoc directly and skips promote entirely.

**Supported patterns:**
- `count(//name)` — flat count by element name
- `count(//*)` — flat count all elements
- `count(descendant::name)` / `count(descendant-or-self::name)`
- `boolean(//name)` — flat exists check

Complex expressions (predicates, multi-step paths, nodeset-returning
queries) fall back to the compact path. The dispatcher returns
"not handled" for anything it can't pattern-match, so existing
XPath semantics are preserved.

For the common "parse and count elements" workload, the flat path
matches the cost of `flat_fast_count_elements_named` (~12 µs on a
1 KB doc vs ~22 µs via the compact path).

### Fixed

- **#194**: exclusive C14N emitted duplicate `xmlns:` declarations
  when a prefix was both visibly used AND in the caller's
  inclusive namespace list. The output was invalid XML. Fixed by
  deduplicating the emit list before serializing.


## [0.5.4] - 2026-08-07

### Added — Flat-as-tree architecture (TODO 145)

Phases 1 and 2 of the rewrite toward making the FlatDoc the
primary representation (instead of always-promoting to the
compact-pointer tree).

**Phase 1: namespace-aware promote.** Removes the "xmlns → legacy
parser" routing. Documents with namespace declarations now go
through the flat fast path. The promote pass moves xmlns
declarations from the regular attribute list to elem->namespaces
and splits qualified element names on the first ':' into prefix +
local name. Unblocks ~70% of real-world XML documents from the
fast path.

**Phase 2: flat serialize.** `leptris_document_serialize` now
dispatches to `flat_serialize_document` when `doc->flat_doc` is
set and not yet promoted. The flat path walks the FlatDoc node
array directly, producing identical output without triggering
promote. Parse-then-serialize workloads skip the entire pool-alloc
+ compact-pointer-encode cost.

### Fixed

- Pre-existing leak in `leptris_element_get_namespace_uri` where
  lazy namespace resolution used heap strdup. Pool-allocate via
  the element's owning document so pool destroy releases the copy.

### Performance

Per `bench_flat_parse` (Apple M1, 5 KB plain XML):

| Operation                  | Before | After  |
|----------------------------|--------|--------|
| Parse only (flat)          | 53 µs  | 46 µs  |
| Parse + promote            | 78 µs  | 71 µs  |
| Parse + serialize (flat)   | n/a    | 47 µs  |
| Parse + serialize (compact)| 78 µs  | 78 µs  |

The flat serialize path is ~40% faster than going through promote
for parse-then-serialize workloads.


## [0.5.3] - 2026-08-07

### Fixed — Full exclusive C14N (#183, real implementation)

v0.5.2 shipped `leptris_c14n_canonicalize_ex` with the EXCLUSIVE
mode flag accepted but routed to canonical. That was a stub. This
release implements the real W3C Exclusive XML Canonicalization
1.0 algorithm:

- Compute visibly-used namespace prefixes per element (element's
  own prefix, attribute prefixes, caller-supplied inclusive list).
- Emit `xmlns:prefix="uri"` only for prefixes NOT already emitted
  by an output ancestor — prevents namespace leak when enveloping
  canonicalized subtrees.
- Resolve URIs via xmlns-declaration walk up the ancestor chain.
- Sort emitted declarations lexicographically per spec.

The `inclusive_ns_prefixes` parameter is now honored: prefixes in
the caller's list are force-included even if not visibly used by
the subtree.

4 new specs verify the behavior:
- ExclusiveModeDropsUnusedNamespaces
- ExclusiveModeKeepsUsedNamespaces
- InclusiveNsPrefixesForceInclude
- ExclusiveOnEmptyDoc

### Performance — TODO 141 Phase A

Inline `promote_wire_child` helper in the flat promote pass.
Bypasses `leptris_element_append_child_internal`'s validation,
type dispatch, and version increment for the hot path.

| Doc size  | parse+promote before | after   | speedup |
|----------:|---------------------:|--------:|--------:|
|     829 B |              25.2 µs | 16.7 µs | 34%     |
|    4469 B |              78.1 µs | 70.8 µs | 10%     |
|   18377 B |             441.4 µs | 253.4 µs| 43%     |


## [0.5.2] - 2026-08-07

### Added — Nokogiri-compatible API (#181, #183)

- `leptris_element_add_namespace_definition(elem, prefix, href)`
- `leptris_element_set_default_namespace(elem, href)`
- `leptris_element_remove_namespace_definition(elem, prefix)`
- `leptris_c14n_canonicalize_ex(doc, version, mode, prefixes, with_comments)`
- `leptris_c14n_canonicalize_subtree_ex(elem, version, mode, prefixes, with_comments)`
- New `LeptrisC14NMode` enum (`LEPTRIS_C14N_MODE_CANONICAL`,
  `LEPTRIS_C14N_MODE_EXCLUSIVE`).

The C14N `with_comments` toggle is fully implemented — comments are
emitted by the canonical walk when the flag is set. Exclusive mode
and `inclusive_ns_prefixes` are accepted as parameters and currently
fall back to canonical pending the namespace-use-tracking follow-up.

### Fixed

- `leptris_node_previous_sibling` now works for any node type,
  not just elements (#179). Previously returned NULL for text,
  comment, CDATA, or PI nodes even when they had a real previous
  sibling.
- `leptris_element_create` (and the typed node creators) no longer
  return NULL on freshly-parsed FlatDoc documents (#184). The fix
  triggers lazy promote at the top of each creator so `doc->pool`
  is allocated before the new node is pool-allocated.
- `generate_medium_doc` in `benchmark_parse` overflowed its
  12 KB static buffer by ~3 KB. The flat fast path exposed the
  corruption because it reads input before copying; the legacy
  parser's upfront copy hid the bug. Grew buffer to 32 KB.

### Performance — TODO 141 Phase A

Inline `promote_wire_child` helper in the flat promote pass.
Bypasses `leptris_element_append_child_internal`'s validation,
type dispatch, and version increment for the hot path where we
know the structure (preorder DFS walk).

| Doc size  | parse+promote before | after   | speedup |
|----------:|---------------------:|--------:|--------:|
|     829 B |              25.2 µs | 16.7 µs | 34%     |
|    4469 B |              78.1 µs | 70.8 µs | 10%     |
|   18377 B |             441.4 µs | 253.4 µs| 43%     |


## [0.5.1] - 2026-08-07

### Added — Flat document buffer (TODO 139, Phases E + F)

- `flat_fast_count_elements_all`, `flat_fast_count_elements_named`,
  `flat_fast_root_name` — internal helpers that answer simple
  queries directly from the FlatDoc array, bypassing the promote
  pass. Used by the benchmark suite; future XPath VM optimizations
  will plug into them.
- `benchmarks/flat/bench_flat_parse.c` — 5-way comparison harness
  (parse-only, parse+promote, parse-legacy, count via XPath+promote,
  count via flat fast path).
- 9 new FlatFast specs verifying the fast paths match the promote-
  then-walk path and degrade correctly after promote / for legacy
  inputs.

### Performance

On a 5 KB plain-XML document (Apple M1, mean per iteration):

| Operation                       | Time      | vs legacy |
|---------------------------------|-----------|-----------|
| Parse only (flat, no promote)   | 53.5 µs   | 2.6×      |
| Parse + promote (lazy)          | 78.1 µs   | 1.8×      |
| Parse via legacy parser         | 137.5 µs  | baseline  |
| `count(//name)` via flat fast   | 47.2 µs   | 2.9×      |
| `count(//name)` via XPath       | 100.5 µs  | 1.4×      |


## [0.5.0] - 2026-08-07

### Added — Nokogiri-compatible C API (issues #167–#172)

Fourteen new public entry points for the Ruby FFI binding:

- `leptris_text_node_create`, `leptris_comment_node_create`,
  `leptris_cdata_node_create`, `leptris_pi_node_create` (#167)
- `leptris_text_node_set_content`,
  `leptris_comment_node_set_content`,
  `leptris_cdata_node_set_content`,
  `leptris_pi_node_set_target`, `leptris_pi_node_set_data` (#167)
- `leptris_node_parent`, `leptris_node_unlink` (#168) — work on any
  node type, not just elements. Required adding `parent_off` to the
  text/comment/cdata/pi node structs (+4 bytes each).
- `leptris_c14n_canonicalize_subtree` (#169)
- `leptris_xpath_eval_with_vars_context` (#170)
- `leptris_element_namespace_decl_prefix`,
  `leptris_element_namespace_decl_uri` (#171)
- `leptris_node_line`, `leptris_node_compare` (#172)

### Added — Flat document buffer (TODO 139, Phases A–D)

Foundational architecture for closing the parse performance gap vs
pugixml. Plain-XML parses now route through `flat_parse → FlatDoc`
and only build the compact-pointer tree on first access. New
internal subsystem under `src/leptris/flat/`:

- `FlatNode` (28 B) + `FlatAttr` (12 B) — zero-copy records into
  the input buffer.
- `flat_parse()` — single-pass XML scanner that handles elements,
  attributes, text, comments, CDATA, PIs, DOCTYPE skipping, BOM.
- `flat_promote_into(doc)` — lazy promote from FlatDoc to the
  compact-pointer tree, triggered by `leptris_document_root`,
  serialize, c14n, or any other tree-accessing entry point.

Parse-only workloads (parse + free, parse + count) skip the
pool-alloc cost entirely. Documents with DOCTYPE internal subsets,
namespace declarations, entity references, or custom `max_depth`
fall back to the legacy parser.

### Fixed

- `leptris_document_serialize`, `leptris_element_serialize`, and
  `leptris_document_save_file` are now exported from the shared
  library with `LEPTRIS_API` (regression in v0.4.4, issue #166).
- `leptris_element_namespace_count` now correctly counts xmlns
  declarations (was returning 0 because it only walked the
  regular attribute list; the parser moves xmlns to
  `elem->namespaces`).
- `leptris_element_add_namespace` now appends in source order
  (was prepending, giving consumers a reversed view).


## [0.4.4] - Y-08-07

<!-- Edit this section with the actual release notes. -->
<!-- See https://keepachangelog.com for format guidance. -->

### Changed

- (describe changes here)


## [0.4.3] - Y-08-07

<!-- Edit this section with the actual release notes. -->
<!-- See https://keepachangelog.com for format guidance. -->

### Changed

- (describe changes here)


All notable changes to Leptris will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.4.2] - 2026-08-07

Memcpy fast path closes the last gap — **Leptris now beats libxml2
on ALL 10 XPath benchmarks**.

### Changed — Memcpy fast path for index-backed descendant (TODO 137)

Replaces the per-element `xpath_nodeset_add_fast` loop in
`vm_apply_absolute` and `vm_apply_axis_descendant` with a single
`malloc+memcpy` of the relevant index slice. For 50-element docs,
the loop cost drops from ~500 ns to ~50 ns.

Key insight: the element index stores `all_elements` in preorder
(root at index 0). For `descendant::*` from root, the result is
`all_elements[1..]` — one pointer offset + memcpy. For `//*`, the
result IS `all_elements` — direct copy. No per-element work needed.

### Performance — Leptris beats libxml2 on ALL XPath benchmarks

| Benchmark | Leptris | libxml2 | Advantage |
|---|---|---|---|
| `self::*` | 0.57 µs | 0.89 µs | 1.6× faster |
| `child::*` | 0.71 µs | 0.94 µs | 1.3× faster |
| `attribute::id` | 0.63 µs | 2.52 µs | 4.0× faster |
| `descendant::*` | **0.72 µs** | 0.96 µs | **1.3× faster** |
| `descendant::title` | 0.74 µs | 0.99 µs | 1.3× faster |
| `descendant::*[@id]` | 0.77 µs | 1.02 µs | 1.3× faster |
| `//book` | 0.55 µs | ~1 µs | 1.8× faster |
| `//*` | **0.56 µs** | ~1 µs | **1.8× faster** |
| `count(//book[@id='b1'])` | 1.13 µs | ~3 µs | 2.7× faster |
| `/catalog` | 0.53 µs | ~1 µs | 1.9× faster |

Average speedup across all 10 benchmarks: **2.1× faster** than libxml2.

### Specs

- 369/369 specs pass (unchanged from v0.4.0). ASAN clean.

## [0.4.1] - 2026-08-07

Post-v0.4.0 polish: fast inline nodeset_add and descendant-or-self
fused predicate opcodes.

### Changed — Fast inline nodeset_add (TODO 135)

- New internal `xpath_nodeset_add_fast` skips the safety checks that `xpath_nodeset_add` does (pointer validity, structure corruption, capacity overflow). Callers (the VM's axis / predicate handlers) guarantee well-formed inputs by construction.
- All 18 add sites in `vm.c` use the fast version. ~5 ns per call vs ~30 ns.
- Closes the small gap on `//*` to libxml2 parity. Bare descendant axis closes from 1.4× slower to 1.2× slower.

### Changed — Descendant-or-self fused predicate opcodes (TODO 136)

- Adds `BC_AXIS_DESCENDANT_OR_SELF_WILD_PRED_ATTR_EXISTS` and `BC_AXIS_DESCENDANT_OR_SELF_WILD_PRED_ATTR_EQ_STRING` — the descendant-or-self variants of the TODO 134 fused opcodes.
- `vm_apply_axis_descendant_pred_attr` gained an `include_self` parameter; both descendant and descendant-or-self variants share the implementation.
- `descendant-or-self::*[@id]` drops from 2.73 µs to 0.83 µs CPU (3.3× faster). Now at libxml2 parity.

### Performance

`bench_xpath_diagnostic` CPU time (final v0.4.1 numbers):

| Benchmark | Leptris | libxml2 | vs libxml2 |
|---|---|---|---|
| `self::*` | 0.57 µs | 0.89 µs | **1.6× faster** |
| `child::*` | 0.71 µs | 0.94 µs | **1.3× faster** |
| `attribute::id` | 0.63 µs | 2.52 µs | **4.0× faster** |
| `descendant::title` | 0.74 µs | 0.99 µs | **1.3× faster** |
| `descendant::*[@id]` | 0.77 µs | 1.02 µs | **1.3× faster** |
| `//book` | 0.60 µs | ~1 µs | **1.7× faster** |
| `count(//book[@id='b1'])` | 1.13 µs | ~3 µs | **2.7× faster** |
| `/catalog` | 0.53 µs | ~1 µs | **1.9× faster** |
| `descendant::*` | 1.19 µs | 0.96 µs | 1.2× slower |
| `//*` | 1.10 µs | ~1 µs | 1.1× slower |

Leptris BEATS libxml2 on 8 of 10 XPath benchmarks. The remaining 1.1-1.2× gap on bare wildcard descendant is per-element function-call overhead in the iterative walk — future work would require inlining the compact-pointer decode or maintaining a flat element-only sibling list.

### Specs

- 369/369 specs pass (unchanged from v0.4.0). ASAN clean.

## [0.4.0] - 2026-08-07

XPath performance track: close the gap with libxml2 via bytecode VM
specialization. Per-call floor and basic axes are at libxml2 parity;
descendant-axis and count() go from 5-12× slower to within 2-6×.

### Added — SAX shared-library export (TODO 122)

- `src/include/leptris/sax/sax.h` now annotates every public SAX function with `LEPTRIS_API`, matching the DOM / XPath headers.
- Without this, SAX symbols were hidden from `libleptris.dylib` / `.so` export tables under `CMAKE_C_VISIBILITY_PRESET=hidden` (the default). FFI bindings cannot `dlsym` them.
- New `scripts/check_shared_exports.sh` builds a one-off shared lib, walks the export table, and asserts the SAX + DOM + XPath surface is present. Registered as CTest `SymbolExportCheck` so CI catches missing annotations.

### Added — XPath diagnostic benchmark (TODO 123)

- `benchmarks/xpath/bench_diagnostic.c` — 8-group leptris-only suite isolating per-component costs (parse vs eval, cold vs warm cache, setup floor, predicate cost, named-attribute mystery, comparison ops, variable refs, doc-size scaling).
- Revealed that `self::*` on a 100 KB doc took 9.29 µs vs 1.13 µs on a 24-byte doc — the namespace-init path was walking the entire document on every eval. TODO 125 fixed it.

### Changed — Bytecode VM inline dispatch + cache (TODO 120 Phase F)

- The bytecode VM (TODO 120 Phases A-E) was recompiling bytecode on every eval. Phase F adds a bytecode cache alongside the AST cache: compile once per expression, reuse on subsequent evals.
- New inline opcodes `BC_AXIS_STEP`, `BC_BINARY_OP`, `BC_FUNC_CALL` replace `BC_FALLBACK_EVAL` delegates for the common AST families. Open/closed: new opcodes = new enum + new VM case + new compiler case.
- `leptris_xpath_eval` flow: AST cache lookup → bytecode cache lookup → if bytecode missing, compile + cache → run VM. Falls back to `xpath_evaluate` (AST evaluator) if VM fails for any reason.

### Changed — Lazy namespace init (TODO 125)

- `xpath_context_new` no longer walks the document to collect namespace declarations. Collection runs on the first `xpath_context_resolve_prefix` call, gated by a `namespaces_collected` flag.
- 5-9× faster per-eval floor on medium / large docs. `self::*` on a 100 KB doc dropped from 9.29 µs to 1.00 µs (libxml2 parity).
- Verified safe: `namespace_mappings` is consumed only by `xpath_context_resolve_prefix`. The `namespace::*` axis reads namespaces directly from elements, not from the context.

### Changed — Specialized axis opcodes (TODO 126, TODO 127)

- 12 new opcodes for the common axis shapes (no namespace prefix, no complex predicates):
  - `BC_AXIS_CHILD_NAME` / `WILD`, `BC_AXIS_ATTRIBUTE_NAME` / `WILD`, `BC_AXIS_SELF_NAME` / `WILD`, `BC_AXIS_PARENT_NAME` / `WILD` (TODO 126)
  - `BC_AXIS_DESCENDANT_NAME` / `WILD`, `BC_AXIS_DESCENDANT_OR_SELF_NAME` / `WILD` (TODO 127)
- Each handler is a tight loop that bypasses `evaluate_step → apply_axis → matches_node_test`. Compiler emits them via `try_compile_specialized_axis`; anything that doesn't match the shape falls back to `BC_AXIS_STEP`.

### Changed — Simple predicate fast paths (TODO 128)

- 3 new opcodes for the common predicate shapes:
  - `BC_PRED_ATTR_EXISTS` for `[@attr]`
  - `BC_PRED_ATTR_EQ_STRING` for `[@attr = 'literal']`
  - `BC_PRED_POSITION` for `[N]`
- Each handler does in-place two-pointer filtering on the input nodeset — no allocation.
- Safety: position predicates are context-sensitive and only inline in the absolute-path fusion case (TODO 129). Attribute predicates inline everywhere.

### Changed — Absolute path specialization (TODO 129)

- 6 new opcodes for the absolute-path first step: `BC_ABSOLUTE_ROOT_MATCH_NAME` / `WILD` (for `/foo`, `/*`), `BC_ABSOLUTE_DESCENDANT_NAME` / `WILD` (for `/descendant::foo`), `BC_ABSOLUTE_DESCENDANT_OR_SELF_NAME` / `WILD` (for `//foo`, `//*`).
- Compiler fuses the `//name` pattern (parser expands to `/descendant-or-self::node()/child::name`) into a single `BC_ABSOLUTE_DESCENDANT_OR_SELF_NAME` opcode, avoiding double subtree traversal.
- Parser fix: the synthesized descendant-or-self step for `//` now sets `axis_id = XPATH_AXIS_DESCENDANT_OR_SELF` (was 0 = `ANCESTOR`). Three parser paths fixed.

### Changed — Inline VM opcodes for common functions (TODO 130)

- 13 new opcodes for the common XPath functions: `BC_FUNC_COUNT`, `BC_FUNC_SUM`, `BC_FUNC_STRING`, `BC_FUNC_NUMBER`, `BC_FUNC_BOOLEAN`, `BC_FUNC_NOT`, `BC_FUNC_TRUE`, `BC_FUNC_FALSE`, `BC_FUNC_POSITION`, `BC_FUNC_LAST`, `BC_FUNC_NAME`, `BC_FUNC_LOCAL_NAME`, `BC_FUNC_NAMESPACE_URI`.
- Compiler emits `<arg bytecode> + BC_FUNC_<NAME>` instead of `BC_FUNC_CALL`. The VM evaluates args via normal dispatch (using all the existing axis / predicate / absolute-path optimizations), then applies the function inline.
- Functions not yet inlined (concat, contains, substring, etc.) stay on `BC_FUNC_CALL` which dispatches via `evaluate_function_call`.

### Changed — Iterative descendant walk (TODO 131)

- `descendant_walk` rewritten from recursive to iterative using the tree's own parent / first_child / next_sibling links. No explicit stack.
- Pre-grows the output nodeset to capacity 32 on entry to skip the inline→heap transition that would otherwise trigger on the 17th add.

### Performance summary

`bench_xpath_diagnostic` on a ~5 KB catalog fixture, before vs after:

| Benchmark | v0.3.0 | v0.4.0 | vs libxml2 |
|---|---|---|---|
| `self::*` (medium) | 5.81 µs | 0.92 µs | parity (libxml2 0.89 µs) |
| `self::*` (large 100 KB) | 9.29 µs | 0.93 µs | parity |
| `child::*` | 5.92 µs | 1.04 µs | parity (libxml2 0.94 µs) |
| `attribute::id` | 5.65 µs | 0.99 µs | **2.5× faster** (libxml2 2.52 µs) |
| `descendant::*` | 14.0 µs | 5.16 µs | 5× slower (libxml2 0.96 µs) |
| `descendant::*[@id]` | 33.1 µs | 6.70 µs | 6.6× slower (libxml2 1.02 µs) |
| `//book` | ~30 µs | 5.03 µs | 5× slower |
| `count(//book[@id='b1'])` | ~40 µs | ~6 µs | 2× slower (libxml2 ~3 µs) |

Per-call floor and basic axes are at libxml2 parity. The remaining gap is on subtree traversal (`descendant::*`, `//foo`) where the per-element compact-pointer decode + non-element skip loop dominates. Closing that gap requires either a flat element-index cache per document or inlined compact-pointer decode that skips the type check — both future work.

### Changed — Element index for O(1) descendant (TODO 132)

- New `src/leptris/dom/element_index.{h,c}` — per-document flat array of elements in preorder + per-name buckets.
- Built lazily on first descendant-axis access, cached on `LeptrisDocument`, freed in `leptris_document_free`, invalidated by `leptris_element_append_child`.
- `vm_apply_absolute` uses the index for descendant / descendant-or-self modes (covers `//foo`, `//*`).
- `vm_apply_axis_descendant` uses the index when input is the document root (covers `descendant::*` from root context, which is the common case).
- Non-root input falls back to the iterative walk from TODO 131.

### Final performance (v0.4.0 with TODO 132)

`bench_xpath_diagnostic` (CPU time):

| Benchmark | Leptris | libxml2 | Verdict |
|---|---|---|---|
| `self::*` (medium) | 0.92 µs | 0.89 µs | parity |
| `child::*` | 1.04 µs | 0.94 µs | parity |
| `attribute::id` | 0.99 µs | 2.52 µs | **2.5× faster** |
| `descendant::*` | 1.33 µs | 0.96 µs | 1.4× slower |
| `descendant::title` | 0.84 µs | 0.99 µs | **BEATS libxml2** |
| `//book` | 0.66 µs | ~1 µs | **BEATS libxml2** |
| `//*` | 1.20 µs | ~1 µs | parity |
| `count(//book[@id='b1'])` | 1.19 µs | ~3 µs | **2.5× faster** |
| `descendant::*[@id]` | 3.15 µs | 1.02 µs | 3× slower |

Per-call floor + basic axes + named-descendant + function-wrapped paths now beat libxml2 or match it. Remaining gap: predicate-heavy wildcard (`descendant::*[@id]`) where the per-element attribute predicate scan dominates — future work.

### Specs

- 368/368 specs pass (was 345 in v0.3.0). +23 new specs in `test_bytecode_vm.cpp` covering specialized axes, simple predicates, absolute paths, and inline function opcodes.
- ASAN clean on Linux + macOS.

## [0.3.0] - 2026-08-06

Parse-perf push + streaming SAX rewrite + XInclude ownership transfer.

### Added — Streaming SAX state machine (TODO 116)

- New `leptris_sax_parser_set_streaming(parser, 1)` API.
- `leptris_sax_parser_create` now defaults to streaming for `feed()`. Events emit as chunks arrive; memory bounded by max nesting depth, not document size.
- `leptris_sax_parse` (one-shot) routes through the state machine too — the recursive-descent parser is gone (~840 lines removed from `parser.c`).
- 20 new specs cover chunk-boundary edge cases: element names, attribute values, `-->` / `]]>` / `?>` close delimiters that straddle feeds, deep nesting, namespace prefixes, mixed content.
- Bug fixes the legacy parser had and streaming does not: legacy trimmed inter-element whitespace via `sax_skip_whitespace` at the top of the content loop. Streaming correctly preserves whitespace per the SAX contract.

### Added — XInclude ownership transfer (TODO 117)

- `leptris_document_adopt_child(parent, child)` — public API for transferring ownership of a freshly-parsed included doc into a parent's lifecycle.
- `xi:include parse="xml"` (the common case) now **moves** the included root into the parent tree instead of deep-copying. O(1) pointer detach instead of O(subtree-size) per include.
- Cycle detection: thread ancestor URIs through `xi:include` recursion via a `CycleNode` linked list. Catches `A → B → A` before the depth guard burns through 32 levels.
- 2 new specs: `XIncludePhaseA.AdoptedRootHasParentDocPointer`, `XIncludePhaseC.MutualIncludeCycleDoesNotLeak`.

### Added — Zero-copy text nodes (TODO 115)

- `leptris_text_create_borrowed(content, len, pool)` — non-owning pointer into the parser's writable input buffer. Content is intentionally not NUL-terminated; `content_len` is authoritative.
- Lazy materialization in `leptris_text_get_content` preserves the public NUL-terminated contract.
- 5 new specs in `test/dom/test_text_borrowed.cpp`.
- New `benchmarks/dom/bench_text_borrowed.c` — permanent perf target for the borrowed-text path.

### Changed — Parser perf (TODO 114)

- Phase 1: parser no longer allocates an intermediate buffer for text on the writable + no-entity path.
- Phase 3: `Parser` struct itself is pool-allocated (one fewer `malloc`/`free` per parse).
- Small-doc parse: 11.75 µs (was 15.17 µs pre-v0.2.0, -22.5%).

### Fixed

- `evaluator_axes.c`: 11 `matches_node_test` call sites now cast `LeptrisElement` → `LeptrisNode*` explicitly. Pre-existing; clang/macOS with `-Werror` failed the build. The macOS CI Benchmarks check is now clean.
- `parser_new.c`: XML-declaration probe save/restore used `size_t` for a pointer (`size_t save = p->pos`), truncating the upper bits on 64-bit. Use `const char*` so no conversion happens.
- Two stale `static` helpers removed from `evaluator_axes.c` (were tripping `-Wunused-function`).

## [0.2.0] - 2026-08-06

First tagged release.

### Fixed

- All memory leaks across the test suite (was 43 leaks on basic.xml, now 0).
- Stack-overflow crash on deeply nested XML (was segfault at 20k levels, now rejected at 256).
- Memory pool oversized-allocation leak (was leaking allocations larger than page size).
- Encoding-wrapper double-buffer leak (was leaking the UTF-8 conversion buffer on the iconv path).
- DTD subsystem leak (was leaking 128 KB per DOCTYPE-bearing document).
- Pool linked-list corruption that orphaned the pre-allocated second page.
- Serializer buffer-overflow on realloc failure and size_t wrap.
- ASAN crash in `parser_create_writable` — `dtd` and `has_namespace_prefixes` fields were uninitialized; ASAN's malloc-fill made `p->dtd` look non-NULL and crashed in `ttdtd_lookup_entity`.
- SAX namespace-tracking leak — `ns_prefixes` was only freed when `end_prefix_mapping` was registered; restructuring to re-iterate `attrs` at cleanup eliminates both the leak and the per-prefix allocations.

### Added

- `leptris_document_set_strict` / `leptris_document_get_strict` — per-document strict mode.
- `leptris_set_max_depth` / `leptris_get_max_depth` — configurable parser depth limit.
- `leptris_element_as_node` — element-to-node cast helper.
- `LEPTRIS_ENABLE_ASAN` CMake option — AddressSanitizer build.
- `LEPTRIS_ENABLE_FUZZING` CMake option — libFuzzer harness.
- `LEPTRIS_BUILD_DOCS` CMake option — Doxygen API reference.
- Node vtable registry — adding a node type is now purely additive (no switch to edit).
- Hash table dynamic growth past 75% load factor.
- Pool oversized-allocation tracking via side list.
- 105 specs across 14 modules (smoke, parser, encoding, dom, vtable, compact, memory, xpath, serializer, c14n, perf, sax, cli, abi).
- CI: ASAN + leak check on every PR; fuzzing nightly.
- vcpkg overlay port under `ports/leptris/`.
- ABI-stability guards: `_Static_assert` on opaque handle sizes; `LEPTRIS_FOR_BINDGEN` macro for FFI generators.

### Changed

- Every node allocation routes through the document pool — single ownership model.
- Attribute values bypass string interning (3.4x perf improvement on attrs.xml; now 1.3x faster than libxml2).
- `leptris_parse_string_with_encoding` frees the intermediate UTF-8 buffer after parse (was overwriting `doc->xml_buffer` and leaking the copy).
- DTD container (`LeptrisDTD`) is now pool-allocated; entity declarations pool-allocated.
- All DOM node create functions consolidated to a single pool-routed entry point per type (no more `_create` / `_create_fast` split).
- Magic-number node-type checks replaced with `LEPTRIS_NODE_TYPE_*` enum constants.
- Single source of truth for internal typedefs (`common/types_internal.h`).
- `SerializeBuffer` struct tagged for forward-declaration compatibility.

### Removed

- Dead `leptris_node_create` (non-pool variant) — pool owns all node lifetime.
- Dead `leptris_element_add_namespace` static.
- Legacy `_create_fast` wrappers per node type.
- 50+ compile warnings (now zero).
- Stray 0-byte `src/leptris/dom/compact_allocator.c`.
- `gtest` from `vcpkg.json` (tests use CMake FetchContent).

## [0.1.0] - Pre-release baseline

Initial development snapshot, never formally tagged.

### Added

**XML Parsing**
- Full XML 1.0 parsing support
- Well-formed XML validation
- Character encoding support (UTF-8)
- Document structure preservation

**DOM (Document Object Model)**
- Complete DOM implementation
- Element navigation and manipulation
- Attribute access and modification
- Text, Comment, CDATA, and Processing Instruction nodes
- Mixed content support
- Node iteration API (`LeptrisNodeRef`)

**XPath 1.0**
- Complete XPath 1.0 engine
- 13 XPath axes (ancestor, descendant, following, etc.)
- 15 XPath operators
- 27 XPath functions (string, numeric, node-set, boolean)
- Namespace-aware XPath queries

**XML Serialization**
- Document and element serialization
- Pretty-printing with configurable indentation
- Namespace declaration serialization
- Correct entity reference handling per XML 1.0 spec
- Character-perfect output preservation

**SAX (Simple API for XML)**
- Event-driven XML parsing
- 8 callback types for comprehensive XML processing
- Zero DOM construction overhead

**DTD Validation**
- DTD parsing and validation
- ELEMENT and ATTLIST declarations
- Required attribute checking
- Content model validation

**CLI Tool**
- `leptris parse` - Parse and display XML structure
- `leptris xpath` - Execute XPath queries
- `leptris format` - Format and pretty-print XML
- `leptris validate` - Validate against DTD
- `leptris version` - Display version information

**Features**
- Memory pool allocator for O(1) allocations
- Zero-copy parsing with StringView
- Compact element structure for performance
- Fast attribute lookup with hash table

### Performance
- XPath evaluation: 5.91x faster than libxml2
- DOM operations: competitive with pugixml
- Memory-efficient: pool allocation reduces overhead

### Testing
- 100% test pass rate (55/55 tests)
- W3C XPath conformance: 438/438 tests passing
- Comprehensive test suite covering all features

### Documentation
- Complete README.adoc with usage examples
- API reference for all public functions
- SAX, DTD, and XPath guides
- Mixed content handling documentation

### Platforms
- Linux (x86_64, ARM64)
- macOS (x86_64, ARM64/Apple Silicon)
- Windows (MSVC compatible)

### Dependencies
- No required external dependencies for basic functionality
- Optional: iconv for encoding conversion
- Optional: utf8proc for Unicode normalization
