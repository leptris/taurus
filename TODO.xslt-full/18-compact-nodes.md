
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
