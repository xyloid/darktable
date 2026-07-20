# darktable upstream divergence manifest

Date: 2026-07-19

Every deliberate divergence from upstream darktable files on this fork;
check each entry on every rebase. New fork-only files (`src/control/
remote_*.c`, `tools/mcp/`) are not divergences and are not listed —
this manifest tracks edits to files that also exist upstream, plus
fork-only code whose correctness depends on upstream data structures.

| upstream file | divergence | guard |
|---|---|---|
| `src/develop/blend.h` / `blend.c` | `dt_develop_blend_mode_section_t` + `dt_develop_blend_mode_sections()` extracted from blend_gui.c's combobox population | GUI-parity test `test_remote_blend` (frozen mode lists) |
| `src/develop/blend_gui.c` | mode-combobox population is a loop over the shared table | same parity test |
| `src/control/remote_blend.c` | hand-written field table over `dt_develop_blend_params_t` (not introspected upstream) | `DEVELOP_BLEND_VERSION` `#error` + offsetof asserts |

## Rehearsed procedure: upstream adds a blend mode

1. Add the new enumerator to `_mode_c_names` in
   `src/control/remote_blend.c` (reads report it by name even before it
   is writable).
2. Extend the matching row of the per-colorspace section table in
   `src/develop/blend.c` (or add a row, if upstream put the mode under a
   new section) so the shared table again matches upstream's combobox
   population.
3. Update the frozen parity arrays (`EXPECTED_RAW` / `EXPECTED_LAB` /
   `EXPECTED_RGB_DISPLAY` / `EXPECTED_RGB_SCENE`) in
   `src/tests/unittests/control/test_remote_blend.c` to the new
   expansion.
4. Rerun `ctest --test-dir build -R test_remote_blend` — the parity and
   mode-name tests must pass before the rebase is considered done.

A `DEVELOP_BLEND_VERSION` bump (struct shape change) additionally trips
the `#error` guard at the top of `remote_blend.c`: re-audit the field
table, the offsetof asserts, and the wire contract (protocol reference
§ Blend settings) before raising the guard.
