# g7-gamma-select: Gamma LUT address-space selection

Evidence tags: [V] verified in code/host tests, [I] inferred, [N] not verified.
Status: implemented [V], host-test-verified [V] (tests/gamma-select-unit.c, tests/gamma-diag-unit.c, tests/display-diag-gamma-owner-unit.c), device-verified: NONE [N] (the host has no Adreno; no on-device run of this change).

## Problem
`gd_region_read()` (src/protocol/gamma_diag.c) asked the owner task of the display mapping, and on the first successful read set `read_done` and stopped, even when the
descriptor checksum did not match. Device log [from the task report, not reproduced here]: owner task 2 reads fine but its checksum is 0x7a736167 (mismatch); task 0 holds the
table with checksum 0x48400000 (identity LUT, so no visible effect would follow either way).

## Behaviour now
* A read is ACCEPTED only if the region decodes (3 * count float32 words fit in len) AND the wrapping u32 word sum equals the descriptor's `sum` [V].
* Per region-read attempt the order is: preferred task (owner of the mapping, else last 0x44), the last 0x44 (if different), every other live task [V].
* The preferred task matching ends the attempt; the other tasks are not read (kept: "owner asked first and alone") [V]. Consequence [V by design]: ambiguity is not looked for
  when the preferred task already matches.
* Otherwise all candidates are read (<= LAGFX_MAX_TASKS + 1 reads per attempt). Exactly one match -> selected. Two or more matches -> AMBIGUOUS, nothing selected, no data offered [V].
* Attempts stay bounded by GD_MAX_REGION_ATTEMPTS (8, shared with failed reads) and stop once a table is accepted; no match / ambiguous attempts retry on later swaps up to the cap [V].
* Descriptor changes after acceptance do not re-run the selection (same one-shot behaviour as before) [V]; the record carries the off/len/count/sum it was validated against.

## Counters (lagfx_gd_stats_t, appended fields; the stats suffix in the display_diag stats line is unchanged)
`sel_accepted`, `sel_ambiguous`, `sel_no_match`, `sel_rejected_mismatch`, `sel_rejected_nofit`, `sel_rejected_unreadable`.
`read_done` = attempts where some task read OK; `sum_match` = attempts that ended with an accepted table; `sum_mismatch` = attempts with a readable region but no accepted table.
The invariants `read_attempts == read_done + unmapped + phys_read_fail` and `applied_ok == 0` still hold [V].

## Logs
The existing `g7-pvg-gamma: region task=.. via=.. ...` line is kept (it now shows the selected task, else the first readable read). New: ONE line per attempt (max 8 in total):
`g7-pvg-gamma: gamma-select attempt=N result=ACCEPTED|AMBIGUOUS|NO_MATCH [task=T|first_match=T] (owner=.. preferred=..) tried=.. readable=.. rejected[mismatch=.. nofit=.. unreadable=..]: task=2:sum=0x..!=0x.. ...`
plus one `gamma-select AMBIGUOUS` line when applicable. Existing `via=` strings: `owner`, `last44`, `last44(owner failed)` (owner unreadable), new `last44(owner not accepted)`, `scan(DIAGNOSTIC FALLBACK ...)`.

## Result API
`size_t lagfx_gd_select_get(lagfx_gd_select_t *o, uint8_t *copy, size_t cap)` (gamma_diag.h): state NONE / ACCEPTED / AMBIGUOUS, selected `task`, `preferred`, `va`, `len`, `count`, `sum`, `nwords`;
for ACCEPTED it copies 4 * nwords bytes of the checksum-validated table into `copy` (returns 0 and copies nothing otherwise; a return smaller than 4 * nwords means cap was too small).
Thread-safe against the op 0x08 thread (spin flag). Nothing consumes it yet; no visible gamma is applied [V: applied_ok is constant 0].

## What a consumer must still do [N, none of it implemented]
1. Interpret the layout: 3 planes x count float32, entry i = i/(count-1) for identity [I from older logs]; the plane order (R,G,B?) is unknown because all logged tables were identity [N].
   A checksum match validates the bytes only, not the semantics; a non-identity table has never been seen, and "sum = wrapping u32 sum of the words" is generalised from one example [I].
2. Decide when to (re-)read: the selection is one-shot at the first accepted table; a later descriptor change or a guest rewrite of the page is not tracked. The copy is a snapshot.
3. Resolve ambiguity itself if state is AMBIGUOUS (e.g. by which task owns the display surface); this module refuses to guess.
4. Apply the LUT in the display/present path (a shader or CPU pass on the final image) and decide colour-space / pixel-format ordering. Needs on-device verification on Adreno.
5. A hook that would live outside gamma_diag.c (e.g. display_diag.c calling the getter at present time) is deliberately not added here.
