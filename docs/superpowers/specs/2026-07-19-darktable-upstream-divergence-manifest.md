# darktable upstream divergence manifest

Date: 2026-07-19

Every deliberate divergence from upstream darktable files on this fork;
check each entry on every rebase. New fork-only files (`src/control/
remote_*.c`, `tools/mcp/`) are not divergences and are not listed —
this manifest tracks edits to files that also exist upstream, plus
fork-only code whose correctness depends on upstream data structures.

| upstream file | divergence | guard |
|---|---|---|
| `src/develop/blend.h` / `blend.c` | existing shared blend-mode section table; plus shared mask-display predicate used by both CPU and OpenCL gates, where export-pipe opt-in joins the unchanged focus/full-pipe condition | existing frozen mode-list parity + `test_mask_display_request_gate_truth_table` + direct mask integration gate |
| `src/develop/blend_gui.c` | mode-combobox population is a loop over the shared table | same parity test |
| `src/develop/pixelpipe_hb.h` / `pixelpipe_hb.c` | new `dt_dev_pixelpipe_t.mask_display_request`, initialized FALSE once and intentionally preserved across process/OpenCL restart | `test_mask_display_request_gate_truth_table` + direct mask integration gate |
| `src/imageio/imageio_common.h` / `imageio.c` | additive `dt_imageio_export_with_flags_and_mask`; existing export signature/callers unchanged; target piece enabled only in throwaway export state | disabled-target direct mask integration gate |
| `src/control/remote_blend.c` | existing hand-written table over `dt_develop_blend_params_t`; plus engine blendif channel tables mirroring GUI `Lab_channels[]`/`rgb_channels[]`/`rgbj_channels[]` | existing `DEVELOP_BLEND_VERSION`/`offsetof` guards + GUI-parity test `test_channels_parity_*` |
| `src/common/undo.h` / `undo.c` | additive paired recording guard plus lazy isolated-group scope with mutex-held begin/record/end, two-sided coalescing epochs, public-traversal sealing/lazy resumption of long-lived ambient segments, and thread-owned one-shot record suppression | isolated-scope, open-group public undo/redo, cross-thread suppression, and signal-backed newer-history/outer-group tests |
| `src/develop/develop.h` / `develop.c` | additive `dt_dev_add_new_masks_history_item`, sharing the existing masks-history wrapper while forcing a distinct, hard-isolated target-bypass production undo record; explicit in-memory `forms_history` sentinel distinguishes absent and empty masks snapshots during replay, with persisted empty final snapshots inferred only from `mask_manager` (the pre-existing arbitrary non-manager empty-snapshot reload limitation remains out of scope) | `test_new_mask_history_forces_distinct_snapshots` + signal-backed `dt_remote_undo` create boundary tests + empty final-delete replay |
| `src/develop/masks.h` / `masks.c` | `_group_create` renamed and exported as the three-argument, type-retaining `dt_masks_group_create_for_module`; additive cycle-safe mask graph queries, ownership-aware non-group full deletion with coherent staged module prefixes, explicit creation options, and extended creation helper; legacy removal API retained | `test_remote_masks` linkage test plus nested/cycle/shared-owner/full-delete/allforms/history tests, including two-module public-undo/redo prefixes, and `MASKS_REMOVE_CALLER_AUDIT` |
| `src/control/remote_transform.c` (NEW) | shared preview↔raw transform and preview-pipe freshness contract | `test_remote_transform` unit tests |
| `src/control/remote_masks.c` (NEW) | drawn-mask engine over `dev->forms` and group forms | `test_remote_masks` unit tests |

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

## Rehearsed procedure: upstream changes a masks point struct or adds a shape type

1. Update `remote_masks.c`'s per-type serializers and deserializers,
   validation ranges, and the affected `dt_masks_point_*` size checks to
   mirror the upstream storage shape.
2. For a new type, extend `dt_remote_masks_type_string` and
   `dt_remote_masks_type_from_string`, then decide whether the wire exposes it
   as editable or deferred.
3. Add a geometry round-trip row to `test_remote_masks` and rerun the focused
   masks and transform tests before accepting the rebase.

## Rehearsed procedure: upstream changes a blendif channel or boost offset

1. Update the corresponding row of the engine blendif channel table
   (`_channels_lab` / `_channels_rgb_display` / `_channels_rgb_scene` in
   `src/control/remote_blend.c`) — slot indices, `boost_supported`, and
   `boost_offset` must again mirror the GUI `Lab_channels[]` /
   `rgb_channels[]` / `rgbj_channels[]`.
2. Rerun `ctest --test-dir build -R test_remote_blend`. The GUI-parity
   tests (`test_channels_parity_*`) extern the GUI tables and assert them
   entry-for-entry against the engine table, so any silent upstream drift
   in a channel slot or boost offset becomes a build/test failure rather
   than a wrong wire contract.
