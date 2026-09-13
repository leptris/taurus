# 18 — Compact nodes: the pugixml DOM race (user directive 2026-09-12)

User: "pugixml is the zero-copy C++ DOM champion — fix it! why are
we not zero-copy?" (asked 3x). Zero-copy is ALREADY true (parse is
in-place NUL-termination over one buffer copy — pugixml
load_buffer's own strategy; attr bookkeeping fixed in v1.9.147:
4.9x -> 1.96x). What remains is NODE SIZE + per-op bookkeeping.

## The measured floor (bench_matrix + /tmp/appendprof split)

| row | leptris | pugixml | state |
|---|---|---|---|
| parse attr-heavy-5k | 769us | 393us | 1.96x behind (was 4.9x) |
| parse text-heavy-2MB | 192 | 622 | 3.2x AHEAD — holds |
| mutation append 10k | 401 | 116 | 3.5x behind |
| mutation set-attr 2k | 475 | 204 | 2.3x behind |

appendprof split (10k ops): create-only 20.6ns, append-only
18.9ns vs pugi append_child 11.6ns TOTAL (44-byte nodes). Our
element is 64B struct + ns_cache side-allocs; per-create = pool
carve + memset + name pool-copy + FNV hash + split walk; per-append
= call chain + double parent decode + type-dispatch sibling setter
+ ~8 stores. Root-doc map registers ROOTS ONLY (3 call sites) —
NOT a per-create cost (verified; do not chase again).

## Phases (each gated: full suite + ABI + bench matrix)

P1 — namebp restructure (PREREQUISITE, unblocks P2/P3): the
name[-1] doc-backpointer must survive in-place QName splits and
cover unattached elements, so register-on-create/reachability can
be reworked. HAZARDS (banked incidents): #905 register-elision =
crash class on detached paths (v1.9.102 revert); split_qname must
clear/relocate the flag (v1.9.98). Gate spec: the v1.9.98 latent
tripwire + detached-construction suite.

P2 — element slimming 64B -> ~48B: lazy name_hash (attr pattern
already proves it — 2 bytes today), pack name_len/flags, evaluate
cp16 for first_child when blocks are contiguous, move attr_count
semantics fully to the chain (uint8_t wraps >255 — the chunk spec
showed the chain is the truth). ABI: struct is internal (opaque
handles) — no public ABI break; watch element-size spec (~96B
memory footprint target in validate.sh).

P3 — fast paths: create with no redundant zeroing (carve knows
which fields matter), append single-decode (validate mut_tail once,
direct element-typed sibling store), set-attr walk-first already
shipped (XSLT lane) — re-measure.

TARGETS: append row <= 116us (parity), attr-heavy <= 393us (with
the ns_cache carve re-attempt — root cause map in TODO 13's
measured-and-reverted entry: lazy get_namespace_uri -> declarations
walk -> slot cache-back; set_prefix creates the cache initing only
3 of 7 fields).

## Rules of engagement

- One phase per PR, full-suite + ASAN Linux legs green, bench
  matrix before/after in the PR body (fresh dirs, identical
  CMAKE_BUILD_TYPE — benchmark-discipline memory).
- NEVER re-apply the ns_cache carve without first fixing the
  prefix->URI mechanism (52/205 libxslt failures was the symptom).
- The XSLT dispatch rows (#682) stay out of this lane — separate
  scope call.

## Status 2026-09-13 morning (PR #1025): P0 shipped, two finds

- P0 SHIPPED: root-doc map entries chunk-carved (128-entry TLS
  chunks). The register malloc'd PER CREATED ELEMENT when the
  free-list was empty — 71ns, 78% of the public create path
  (10k-create harness 916 -> 192us). The matrix's rounds recycle
  via teardown, so ITS delta is small (append 401->390); the
  create-without-teardown shape is the beneficiary.
- Find 2 SHIPPED: elem->attr_count is uint8_t and WRAPS at 255 —
  the walk-vs-index threshold keyed on it oscillated, dropping
  high-attr elements back onto the O(N) walk (set-attr row 459 ->
  327us, 1.60x behind). Latching header bit 5
  (LEPTRIS_ATTR_INDEXED_FLAG); spec pins >255 dedup.
- REFINED MATH for "beat in all shapes": create 19.2 + append 18.9
  ~= 38ns vs pugi 11.6 TOTAL. P2 slimming (64->48B) floors create
  ~12-14ns; P3 append fast path ~8-10ns => 20-24ns, still ~2x.
  FULL parity needs P1 (register elision via namebp for known-doc
  elements — the #905 hazard restructure) + possibly dropping
  per-create hash/split to lazy. P1 IS the decisive slice; do it
  in a fresh window with the detached-construction suite as the
  gate.
