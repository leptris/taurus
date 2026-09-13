# 14 — HTML parsing mode (#659)

HTML4/5-tolerant parse into the standard DOM: implied end tags
(p/li/td/tr/...), attribute minimization, <script>/<style> raw
text, entities (named table), DOCTYPE legacy strings, optional
case-insensitive element names. API: LEPTRIS_PARSE_HTML flag +
leptris_parse_html_string; serializer method=html already ships.
Reference: libxml2 HTMLparser behaviors + the html5lib tree-
construction subset. Gate: html5lib tokenizer/tree tests subset +
Nokogiri parity corpus.

## Status 2026-09-03 — slice 1 SHIPPED (v1.9.75, PR #809)

`leptris_parse_html_string` (src/leptris/html/html_parse.c):
tokenizer + open-stack tree builder over the STANDARD DOM
(same nodes/pool/serializer/XPath as XML). Covered: implied ends
(p on blocks, li, dt/dd, td/th, tr, thead/tbody/tfoot,
option/optgroup), voids, raw script/style, lowercased names,
minimized + unquoted attrs, 2032-entry WHATWG entity table +
numeric in text and values, stray-end-tag pops, EOF closing,
never-failing tokenizer, Nokogiri document shape (synthesize
<html><head/><body> when absent; explicit <html> honored; no
implied tbody). 13 specs in test/html/ (watched RED first).

Remaining slices:
- head-content placement: <title>/<meta>/<link>/<base> before
  body content lift into <head> (libxml2 moves them); <template>
  contents as inert
- CDATA-section passthrough; processing-instruction-ish bogus
  comments (<?...> → comment)
- adoption-agency-shaped misnesting (current: natural nesting +
  stray-pop, libxml2-like)
- html5lib tokenizer/tree-construction corpus adoption (the gate)
- Nokogiri parity corpus: diff against nokogiri on a crawl sample
- bindings: expose the entry in leptris-ruby/py (their entries
  tracked on #683)



## Status 2026-09-05 (v1.9.83-85): slice 2 shipped

- PI-ish bogus constructs (v1.9.83): `<?target data?>` = PI node,
  data keeps trailing `?`, leading ws trimmed (libxml2 shape).
- Head-content lift (v1.9.84): contiguous leading title/meta/link/
  base run -> synthesized `<head>` before `<body>`; sibling chain
  severed at the boundary.
- Characterization (v1.9.85): `<template>` = ordinary element
  (libxml2 predates WHATWG inert-fragment — NO special handling is
  correct for parity); misnesting = close-at-outer-end-tag +
  stray-drop (our natural-nesting model). Both locked as gate
  specs. RE-SCOPE: adoption-agency is needed ONLY for the html5lib
  corpus gate (different tree shape), NOT for Nokogiri parity.

Remaining: html5lib tokenizer/tree corpus adoption (needs the
adoption-agency tree shape + a JSON test harness), Nokogiri parity
corpus on a crawl sample, bindings exposure of
leptris_parse_html_string.

## Design 2026-09-05 — html5lib corpus harness (the #659 slice)

Corpus: html5lib/html5lib-tests tree-construction .dat files
(Nokogiri vendors them as a git submodule; reference
~/src/external/nokogiri). Pin a snapshot in-repo under
test/html/html5lib-tests/ (vendored copy, not a submodule —
CI must not need network; keep only tree-construction + a
README crediting upstream + the pinned commit).

Harness shape (test/html/test_html5lib.cpp):
1. .dat scanner: split on blank-line-separated blocks; fields
   |#data| |#errors| |#document| |#script-on| (skip script-on
   variants initially). Multiple #data sections per case are
   fragments-after-document pairs (stage 2; skip initially).
2. Expected-tree parser: read the "| " indented lines into a
   node tree spec (element/name-ns, text, comment, DOCTYPE,
   template content). Namespaced names carry the URI in braces
   (svg/mathml foreign content).
3. Runner: leptris_parse_html_string per case; walk our tree in
   the same order; compare node kind, name, namespace URI (not
   prefix), attributes (order-insensitive), text content.
4. Mode-gate: the corpus REQUIRES adoption-agency for the
   formatting-element cases (~15% of cases) — mode-gated OFF in
   slice 1; the mode flips on when AA lands, keeping the
   Nokogiri-parity characterization specs green meanwhile.
5. Report: pass/skip counts by category; a red list file
   (like open_cases.txt) naming the failing cases that AA +
   later slices must close, so progress is falsifiable.

Estimated slices: (a) scanner+expected parser+runner skeleton
with pass-count spec ~ 1 PR; (b) fix fallout outside AA
(misnesting corners the characterization gate already shapes);
(c) AA implementation gated ON + red-list burn-down.

## Status 2026-09-05 (later) — html5lib harness SHIPPED (slice 1)

Vendored corpus (c67f90ea — Nokogiri's pin, the last upstream
revision with tree-construction/; upstream moved to WPT after) +
test_html5lib.cpp: .dat scanner (blank-line case separation — a
#data after #document with NO blank line above is html5lib's
fragment-pair continuation, skip), expected-tree parser (attr
continuation lines like `id="foo"` attach to the nearest open
element; html5lib's template `content` marker flattens — our
template is an ordinary element), comparator (strict on names/ns/
attrs/text/comments/doctype; documented divergences: expected
EMPTY <head> optional, adjacent text coalesced).

Baseline: total=1753, PASS=186, fail=1369, skip=198 (fragment
mode 190 + script-on 8). Floor pinned at 186 — falsifiable.
Red-list snapshot: test/html/html5lib-redlist.txt. Failure shape:
666 child-count under text-bearing elems + 199 under html
(implied-head/foster/misnesting class), 163 element-name, 69 text,
30 parse-failed, 13 kind. NEXT slices by category, NOT adoption
agency first: the child-count/html cluster is the implied-head +
foster-parenting model — the biggest bucket.

## Follow-up (same day): empty-shape inputs are documents (+7)

The harness's 30 parse-failed cases exposed that inputs appending
NOTHING (stray end tag only, doctype-only, second doctype, empty
string) hard-failed at the done-gate. Nothing-parsed is not an
error in lenient HTML: the wrapper synthesis handles the empty
chain (html>body empty). Floor 186 -> 193; spec
EmptyShapeInputsAreDocuments pins it. Remaining parse-fails (23)
are deeper tokenizer edges - next slice.

## Status 2026-09-05 (night) — Nokogiri PARITY floor (the real metric)

nokogiri-tree-tests.dat: 1555 Nokogiri::HTML reference trees for
every runnable corpus case (gen_nokogiri_reference.rb, same "| "
dialect the harness parses; libxml2's implicit doctype filtered to
input-doctyped cases only). The harness now reports BOTH meters:
html5lib conformance (193) and NOKOGIRI PARITY — 372/1555 exact
tree matches, 1183-entry parity red-list committed
(test/html/nokogiri-parity-redlist.txt), floor pinned.

Parity failure shape (first bucket): misnesting/adoption-adjacent
child-count diffs under b/a/body — the libxml2 tree-builder
behaviors (formatting-element handling) we have NOT replicated.
That is the #659 work list, ranked by the red-list.

Separate: HTML parse throughput gated - 90.2 vs 31.1 MB/s (2.9x
ahead of libxml2, byte-identical 450KB fixtures, PR #867).

## Update 2026-09-12: comment placement (initial mode) — shipped

`<!-- lead --><p>hi</p>` put the comment inside the synthesized
body; WHATWG initial mode inserts initial-phase comments as
DOCUMENT children. Fix is token-time routing: HBuilder tracks
left_initial (cleared by any start tag — even dropped structural
ones — any end tag, and any non-whitespace text via the h_append
choke point); comments tokenized while left_initial rides a
dedicated doc-prolog chain linked ahead of the tree at commit.
html4 entry unchanged (libxml2 shape). The Html() spec helper now
strips the wrapper around document-level prolog comments. Spec:
LeadingCommentsBelongToTheDocument (doc-level placement, two-
comment prolog, text-first in-body, html4 pinned).

Corpus floor unchanged (928): only 4 cases lead with comments and
they still fail on other axes — the known "in head" gap
(tests19:87 expects comment+meta inside an explicit <head>;
the dropped-structural design flattens them to body) and
adoption agency. NEXT #659 nodes: in-head insertion mode for
explicit <head> content, then full AFE, then bindings expose
html.
## Update 2026-09-12 (later): in-head comments — shipped (floor 933)

The tests19:87 class resolved narrower than modeled: elements
after an explicit <head> already lifted correctly — only COMMENTS
broke the head run (the lift scan walks elements only, so a
comment first in the chain blocked the whole lift, and one
between head elements truncated it). Fix: comments (and PI-ish
bogus comments) are NEUTRAL in the head-run scan, WHATWG-gated;
the run-splice reparent now dispatches set_parent by node kind
(an element-shaped write on a comment corrupted its content —
found by the spec, invisible before because comments never rode
the run). Corpus 928 → 933 (+5); Nokogiri parity 784 held.
Spec: HeadCommentsNestIntoHead. NEXT #659 node: full AFE (the
295-red block), then bindings expose html.

## Update 2026-09-12 (final): full AFE — shipped (floor 1006)

Replaced the simplified adoption block with the real WHATWG
machinery (13.2.4.3 + 13.2.6.4.7):

- HBuilder grows the LIST OF ACTIVE FORMATTING ELEMENTS (64
  entries; el + marker flag): formatting starts push with the
  Noah's Ark clause (4th same token identity drops the earliest);
  applet/object/marquee/td/th/caption push markers; marker-scope
  end tags clear to the marker; generic pops leave entries
  dangling by design (that is what reconstruction consumes).
- RECONSTRUCT runs at every text flush and before formatting/
  ordinary starts (blocklist = structural head set + block set +
  table family — the starts that close p instead): dangling
  entries re-open as clones at the insertion point and push on
  the stack. Raw-text containers are excluded ("text" insertion
  mode). This is what reopens formatting after `</b>` pops
  through an inner formatting element.
- The AGENCY itself: outer loop (8), step-2 current-node fast
  path, after-last-marker entry lookup, furthest block = first
  special HTML-ns element more recent than the entry (namespace
  matters: svg tr is not a furthest block), common ancestor
  (NULL = our top chain — html/body are synthesized at commit),
  snapshot-based inner loop (clone-and-replace of formatting
  intermediates, append lastNode chains), UNCONDITIONAL steps
  14-16 (the block itself is adopted out — skipping these when
  the inner loop broke at once was the first bug), new element
  takes the furthest block's children, bookmark insertion in the
  list, stack insert just more-recent than the block. leptris_
  node_unlink handles detach for the top-chain move (append_
  child_internal_doc already unlinks — a first draft that linked
  siblings without unlinking duplicated subtrees).
- a/nobr START tags run the agency first when an entry is open
  (duplicate-<a> close), then reconstruct, then open.
- Button-scope p close: block starts pop through an open p even
  with formatting in between (they stay dangling and reconstruct
  inside the new block) — `<p><b><b><b><b><p>x` now reconstructs
  exactly three b's (Noah dropped the fourth at push time).

Corpus 933 → 1006 (+73); adoption01/02 17/18 green; the last
adoption red (adoption01:11 `<table><a>1<td>...</td>3</table>`)
needs in-table "clear the stack back to table context" — table
mode slice, not AFE. Nokogiri parity 784 held. Spec:
AdoptionAgencyMisnest (6 shapes: both spec walkthroughs +
adoption01 1/2/3/5). NEXT #659 node: in-table dispatch (clear
stack back to table context + in-table text batching), then
bindings expose html → close #659.

## Update 2026-09-12 (b): foreign integration points are scope
## boundaries + in-select — shipped (floor 1027)

One root cause behind most of tests10's 15 reds: MathML text
integration points (mi/mo/mn/ms/mtext) and HTML integration
points (annotation-xml with HTML encoding, svg foreignObject/
desc/title) are on EVERY scope-walk boundary list (13.2.4.2).
h_is_int_point now terminates: the button-scope p-close scan, a
new generic-end-tag scope guard (an integration point between
the current node and the nearest name match = out of scope, the
tag is ignored — WHATWG-gated), and the foreign breakout pop
(pops only down TO the integration point, the reprocessed HTML
tag lands inside it). Plus in-select: non-select-set start tags
drop (text joins the select's text) and </table> closes the
select first (in-select-in-table). Corpus 1006 → 1027 (+21);
Nokogiri parity 784 held. Spec:
ForeignIntegrationPointsAreScopeBoundaries. NEXT #659 nodes:
frameset-mode content drop (tests10:21/22), annotation-xml
without encoding staying foreign (52-54 tails), in-table
dispatch (adoption01:11), then bindings expose html.

## Update 2026-09-12 (c): in-table clear-stack + frameset
## content drop — shipped (floor 1038)

Three insertion-mode gaps: (1) cell/row/group/caption starts
CLEAR THE STACK BACK TO TABLE CONTEXT before the wrapper
synthesis (stray elements above the table — a foster-parented
<a> — no longer swallow the synthesized cell; the trailing text
then fosters before the table inside a reconstructed formatting
clone: adoption01:11 green, AFE + foster + reconstruct
composing end to end); (2) "in frameset" drops everything but
frameset/frame/noframes starts (non-ws text too — html5lib
tests10:21/22); (3) the replace-body frameset no longer
double-wraps — h_split_head_body reuses the parsed frameset as
its own wrapper instead of synthesizing a shell around it.
Corpus 1027 → 1038 (+11); adoption01/02 now 18/18; parity 784
held. Spec: FramesetAndInTableClearStack. NEXT: template/
script-data/entity families by size, then bindings expose html.
## Update 2026-09-12 (d): bogus-markup edges + heading
## self-close (branch floor 1035, +8 on its 1027 base)

WHATWG tokenizer tails (tests1:38-49): EOF right after "</"
emits the two characters as text; an invalid first tag-name char
after "</" (and any non-doctype "<!" construct, and every "<?"
— html5lib has no PI tokenizer) makes a bogus comment with
NUL→U+FFFD substitution, routed to the document prolog while
still in the initial mode. Heading starts pop a current heading
(h_closes_ww). The ProcessingInstructionAndBogus spec flipped
to the bogus-comment truth; html4 keeps libxml2 PIs (parity
784 held). The corpus READER now reads the WHATWG meter's .dat
binary (std::string(char*) truncated every line at its first
NUL — the unsafe corpus has real NULs); the parity meter keeps
the truncating read (its reference trees were recorded from
that input shape; binary-preserve broke it 784→776).

OPEN MYSTERY (3 reds: plain-text-unsafe 12/13 + noscript01:1):
an exact C++ replication of the runner's read+parse produces
the byte-correct tree (FFFD comment in body; head>[noscript>/
[comment]]), lldb confirms the corpus binary itself runs the
FFFD h_bogus_comment with the right input bytes, comment_create
copies+NUL-terminates — yet the walker compares OUR comment as
''. Next session: instrument CollectDocument/Coalesce for
these cases (suspect the walker's body traversal vs prolog
routing interaction, or a second empty comment). +10 other
cases fixed; net +8.

## Update 2026-09-12 (e): script-data escaped states — shipped
## (floor 1088, +42)

The WHATWG 13.2.5.15-.31 script tokenizer states, as a scan
machine inside the rawtext branch (WHATWG + script only):
"<!--" enters script-data-escaped; there "</script"+delimiter
closes (a bare "</scripta" is text), "<script"+delimiter enters
double-escaped where "</script>" only drops one level, and
"-->"/"--!>" re-enter plain script data. Cleared the whole
tests16 38-48/64-72 block plus script cases across webkit01/
tests2 (1046 -> 1088, +42). Nokogiri parity 784 held (html4
keeps the naive scan). Spec: ScriptDataEscapedStates.

## Update 2026-09-12 (f): RCDATA/rawtext family — shipped
## (floor 1129, +33)

title/textarea are RCDATA (entity-decoded content, no markup
inside, textarea drops one leading newline) and iframe/noembed/
xmp join the whatwg raw set (13.2.6.2) — previously only
script/style/plaintext/noframes were raw, so title parsed its
content as markup (a comment inside the title) and iframe left
</noscript> to the generic end-tag path. html4 keeps libxml2
shapes (parity 784 held). Spec: RcdataAndRawtextFamily. NEXT:
explicit html/head shapes (tests1:7-13/101), entities01 (40),
template (29).

## Update 2026-09-12 (g): numeric-reference end states — shipped
## (floor 1169, +40)
WHATWG 13.2.5.84 in h_decode_ww's whatwg branch: NUL, surrogate,
and >0x10FFFF references (including strtol-saturated overflow
digit strings, semicolon or not) become U+FFFD; the C1 range
remaps through the Windows-1252 table (0x80 EUR ... 0x9F YD;
rows without entries stay literal). html4 keeps its strict
';'-required semantics (parity 784 held). Spec:
NumericReferenceEndStates. entities01 is now 100% green. NEXT:
template.dat (29), tests2/3 tails, the 3-case walker mystery.

## Triaged 2026-09-12 (h): template.dat (31 reds) — design banked

The runner keeps html5lib's literal `content` marker node (the
tail MARKER-splice in NormalizeExpected handles it — a FIRST
attempt to pre-splice content markers LOST 4 net cases, cause
not yet found; the tail splice alone is the current state).
ENGINE rules needed (expected trees confirmed):
- in-template depth counter; content = template's own children;
- td/th at template top synthesize a bare tr (NO tbody); tr is
  bare; block starts close open tr/td/tbody back to the template
  (case 45: div becomes a sibling of tr);
- end-tag fence: a non-template end tag whose nearest match is
  at/below the nearest template's stack index is IGNORED (7, 78,
  79); </template> pops + marker-clear (add template to the
  marker-clear set);
- structural html/head/body tokens inside template DROP entirely
  — attrs NOT merged (64-67);
- nested templates stack (68-70, 91).
RED spec drafted (TemplateInsertion, 4 shapes) — banked in the
git history of branch feat/html-template.

## Update 2026-09-12 (i): in-template fence + structural drop —
## shipped (floor 1175, +6)

WHATWG-gated: end tags never pop past the nearest template (the
fence — template.dat 7/78/79); structural html/head/body starts
inside template drop entirely, attrs NOT merged onto the outer
elements (64-67); </template> clears the AAFE to its marker.
Spec: TemplateInsertion. The row/cell synthesis HEURISTICS were
built twice (content-level rules v1/v2) and REVERTED both times
— net-negative: html5lib's template trees come from the
PER-TEMPLATE INSERTION-MODE STACK (each template saves its mode;
starts switch it: tr -> in-row, td -> in-cell, thead/tbody/
caption/col -> in-table..., EOF/reset restores). The remaining
~25 template reds need that mode-stack modeled properly —
next session's design, NOT more content heuristics.

## Update 2026-09-12 (j): in-template START-tag fence — shipped
## (floor 1187, +12)

13.2.4.2's template-as-scope-boundary, start-tag side: the
close-stack loop and the clear-back-to-table loop both stop at an
open template (WHATWG-gated). `<table><template><tr>` now nests the
row in template content instead of popping the template and
synthesizing at table level (template.dat 27-36/39/63 shapes).
Spec: TemplateStartTagFence. Nokogiri parity 784 held.

THE REMAINING ~24 template reds need the FAITHFUL per-template
insertion-mode stack — every simplified state model tried against
the expected trees has been falsified by a case pair: 46 vs 48
(post-</tr> td gets a tr-wrap; post-</td> td is bare), 52 vs 69
(post-row tbody DROPPED; post-section tfoot OPENED, tr gets a
tbody-wrap). The observable divergence demands the real mode
sequence (in-template -> in-table/in-row/in-cell pushes with their
own start/end handling), not a two-state flag. NEXT: mode enum +
per-template mode stack in HBuilder + the 13.2.6.4.10 transition
table verbatim; the 36-red extraction (trees for every failing
case) is in this file's git history and the session red-list.

## Update 2026-09-12 (k): per-template insertion-mode machine —
## shipped (floor 1197, +10)

HBuilder.tmpl_mode[]: the saved mode per open template
(TEMPLATE/IN_TABLE/IN_TBODY/IN_ROW/IN_CGROUP/IN_BODY), driven by
h_tmpl_content_start for table-context starts at template content +
mode restores at content-level closes + a section-popped-by-this-
token tracker (gumbo in-table-body 3775: pop the open section, go
in-table, reprocess). Fresh templates open rows/cells/sections
BARE; post-row cells get an implied tr (46); post-section rows get
an implied tbody (69); rows/sections with nothing in table scope
DROP (48/52/53); head-family tokens leave the mode untouched
(44/60-62); stray table tags in body-mode content drop (57).
Verified against gumbo's handle_in_template/in_table_body/in_row
(nokogiri's vendored copy) — the reference for the remaining
families. Corpus 1187 -> 1197, parity 784 held. Spec:
TemplateInsertionModes. REMAINING template.dat reds (~14): select-
in-table (22, 102), frameset/frame drops (41, 67, 93), misc rows
(71-76), nested/head shapes (91, 106, 108), foreignObject (100).

## Update 2026-09-13 (l): template frame/frameset drops — shipped
## (floor 1200, +3)

13.2.6.4.10 anything-else -> in-body: frame starts are ignored
outright and frameset tokens vanish inside a template
(template.dat 41/67/93 — content stays empty). WHATWG-gated in
h_tmpl_content_start. Spec: TemplateFrameAndFramesetDrop. Parity
784 held. REMAINING template.dat reds (~11): select-in-table (22,
102), misc rows (71-76), nested/head shapes (91, 106, 108),
foreignObject (100).
