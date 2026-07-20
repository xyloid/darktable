# darktable MCP — Parametric Masks + Mask Render (Tier 2 / M-B) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Read and write parametric ("conditional") blendif masks — per-channel trapezoid ramps over module input/output values, with polarity, per-slot boost, and the mask-combine setting — as an extension of Tier 1's `blend` wire object behind a new `parametric_mask_params` capability; and add a read-only `mask_render` capability (`render_preview` gains `show_mask`) so an agent can see the mask it configured.

**Architecture:** Tier 2 extends the pure engine file `src/control/remote_blend.c` (M-A) with a family-scoped blendif channel table, pure bitfield/marker helpers, and new `combine`/`parametric` stages inside `dt_remote_blend_schema`/`_read`/`_patch_apply`. It writes only the caller's scratch `dt_develop_blend_params_t`, so M-A's single-history-item commit path is unchanged (zero new commit/threading/history surface). The mask render extends M-A/M-B's existing background export render path (`dt_remote_render_preview_execute`) with a guarded per-pipe display-mask opt-in, because the export pipe structurally cannot honor the live `request_mask_display` flag (see Design amendment 1).

**Tech Stack:** C (GLib, json-glib, cmocka), Python (FastMCP sidecar, pytest).

**Spec:** `docs/superpowers/specs/2026-07-19-darktable-mcp-parametric-masks-design.md`. The `mask_mode` transition appendix in `docs/superpowers/specs/2026-07-19-darktable-mcp-mask-support-design-candidates.md` is authoritative for `mask_mode` vocabulary and transitions (M-B adds the `parametric` write vocabulary; `CONDITIONAL` toggles never touch blendif storage). Builds on the M-A plan `docs/superpowers/plans/2026-07-19-darktable-mcp-blend-settings-tier1.md` as if fully implemented.

## Global Constraints

- **Depends on M-A fully implemented.** This plan consumes M-A interfaces verbatim: `src/control/remote_blend.c/.h` (`dt_remote_blend_patch_apply(module, JsonObject*, dt_develop_blend_params_t*, dt_remote_error_t**)`, `dt_remote_blend_schema/_read`, `dt_remote_blend_schema_for_ref/_read_for_ref`, the `_enum_name_t`/`_float_field_t` idioms, `dt_remote_blend_mask_mode_string/_from_string`, `dt_remote_blend_effective_colorspace`); `dt_remote_patch_t.blend` (borrowed `JsonObject*`); `dt_remote_mutation_result_t.blend_readback`; protocol calls-table members `.blend_schema`/`.blend_read`; `blend_params` capability; `set_module_params`'s `blend` sidecar argument; unit suite `src/tests/unittests/control/test_remote_blend.c`; integration file `tools/mcp/tests/integration/test_blend_tier1.py`.
- **New capabilities:** exactly `parametric_mask_params` and `mask_render`, both advertised in `hello`. `parametric_mask_params` implies `blend_params`. `mask_render` is independent.
- **Channel-name wire vocabulary, per effective colorspace family (verbatim, `<name>_in`/`<name>_out` per channel):**
  - Lab (`DEVELOP_BLEND_CS_LAB`, slot mask `DEVELOP_BLENDIF_Lab_MASK` = `0x3377`): `L a b C h`.
  - RGB display (`DEVELOP_BLEND_CS_RGB_DISPLAY`, slot mask `DEVELOP_BLENDIF_RGB_MASK` = `0x77FF`): `g R G B H S l`.
  - RGB scene (`DEVELOP_BLEND_CS_RGB_SCENE`, slot mask `0x77FF`): `g R G B Jz Cz hz`.
  - RAW blending (`DEVELOP_BLEND_CS_RAW`) and `DEVELOP_BLEND_CS_NONE`: **no** parametric-channel support. RAW does have CPU/OpenCL mask implementations (`blendif_raw.c`, `blendop_mask_RAW`), but they intentionally ignore the blendif channel arrays/`CONDITIONAL`, and `blend_gui.c:dt_iop_gui_update_blendif` asserts that RAW blendif is unsupported. Therefore there is no RAW channel table: schema omits `parametric`/`combine`, and such patches error `unsupported_field`.
- **Marker 4-tuple ordering (verbatim):** each slot stores `[m0, m1, m2, m3]` in a 0–1 stored domain, ascending `m0 ≤ m1 ≤ m2 ≤ m3`. The mask factor ramps 0→1 over `m0..m1`, is 1 over `m1..m2`, falls 1→0 over `m2..m3` (`dt_develop_blendif_process_parameters`, `blend.c`). Open ends: `m0,m1 ≤ 0` unbounded low; `m2,m3 ≥ 1` unbounded high. Degenerate equal markers are legal (hard edge). Domain is `[0.0, 1.0]` inclusive.
- **Derived-enable rule (verbatim, single source of truth):** a slot is **disabled** iff its markers are the full-span identity `m1 == 0.0 && m2 == 1.0` (i.e. identity markers `[0,0,1,1]`); enabled otherwise. Exactly the GUI rule at `blend_gui.c:_blendop_blendif_sliders_callback` (`if(parameters[1]==0.0f && parameters[2]==1.0f) bp->blendif &= ~(1<<ch); else bp->blendif |= (1<<ch);`). Enable is never sent on the wire; it is derived on every patch and only enabled slots appear in reads.
- **Polarity / `inverted` (verbatim rule, never resolved server-side):** `inverted` is the stored polarity bit (`blendif` bits 16–31). The pixel kernels compute effective inversion as `invert_mask = (blendif >> 16) XOR (mask_combine & DEVELOP_COMBINE_INCL ? family_mask : 0)` and use `1 − factor` for inverted channels (`data/kernels/blendop.cl:204`; C twins in `src/develop/blends/`). Documented rule for clients: `effective_inverted = inverted XOR (combine is inclusive)`. The wire always carries the stored bit.
- **Bit 31 (`DEVELOP_BLENDIF_active`) is a legacy flag** — never set by current code; every pack helper masks it off.
- **Boost factors (verbatim):** per **slot** (`blendif_boost_factors[16]`), a log2 exponent applied as `(stored − offset_ab) × 2^boost` where `offset_ab = 0.5` for Lab `a`/`b` slots and `0` otherwise. The **boost value** itself has a per-channel storage offset (the GUI's "zero"): `−6.64385619` for `Jz`/`Cz` (`_blend_init_blendif_boost_parameters`, `blend.c`), `0` for all other channels. Stored boost range = displayed `0..18` EV shifted by the channel offset: `[0.0, 18.0]` for offset-0 channels, `[-6.64385619, 11.35614381]` for `Jz`/`Cz`. **Boost is exp2 exponents and never rescales markers on the wire** — the GUI's threshold-preserving marker rescale (`_blendop_blendif_boost_factor_callback`) is deliberately NOT reproduced; each member is written verbatim. `offset_ab = 0.5` and the two boost offsets are distinct concepts (see Design amendment 2).
- **`mask_combine` mapping (verbatim):** wire enum ↔ storage, low bits only (`DEVELOP_COMBINE_MASKS_POS` = `0x04` is a Tier-3 drawn bit, preserved untouched): `exclusive` = `DEVELOP_COMBINE_NORM_EXCL` (`0x00`), `inclusive` = `DEVELOP_COMBINE_NORM_INCL` (`0x02`), `exclusive_inverted` = `DEVELOP_COMBINE_INV_EXCL` (`0x01`), `inclusive_inverted` = `DEVELOP_COMBINE_INV_INCL` (`0x03`).
- **`mask_mode` follows the authoritative transition appendix exactly.** The target parser recognizes `"off"`, `"uniform"`, `"parametric"`, `"drawn"`, and `"drawn+parametric"`; the transition helper may change only `ENABLED|CONDITIONAL`, requires the target `MASK` bit to equal the stored `MASK` bit, preserves every unowned bit, and rejects every stored raster state. Consequently `drawn ↔ drawn+parametric` is legal in a supported family (plus removal of a legacy unsupported `CONDITIONAL` state), while entering or leaving drawn state remains `drawn_via_attach_only`. Schema vocabulary is state-aware: non-drawn rows expose `off`/`uniform`/`parametric`, drawn rows expose `drawn`/`drawn+parametric`, and raster rows are non-writable, subject to the unsupported-family legacy rule below.
- **Unsupported-family legacy rule:** a pre-existing `CONDITIONAL` bit in RAW/NONE remains representable as the current `mask_mode`, may be left unchanged or removed, and still exposes no `parametric`/`combine` members. A patch may never newly introduce that unsupported state. Family availability is checked after the colorspace stage, against the final projected family, so a same-call RAW → RGB switch plus `mask_mode:"parametric"` is legal.
- **Validation order inside `blend`:** `mask_mode` → `colorspace` → strict `allow_inverted_combine` → `combine` → `parametric` (per channel: name → allowed members → markers → inverted → boost) → the existing Tier-1 mode/reverse/feathering/numeric stages. Blend keeps its Tier-1 position at the end of the class error precedence.
- **Foreign slots** (enabled usable slots 0–14 outside the effective family) are invisible in reads (surfaced only as `"foreign_channels": true`) and never touched by patches. Reserved slot 15 and legacy bit 31 are not channels and never contribute to that flag.
- **Struct tripwire:** M-A's `#if DEVELOP_BLEND_VERSION != 14 #error` and `offsetof` asserts stay; Tier 2 adds relative `offsetof`/size asserts for `blendif_parameters` and `blendif_boost_factors`.
- **Error vocabulary:** only existing codes (`unsupported_field` = `DT_REMOTE_ERR_UNSUPPORTED_FIELD`, `invalid_value` = `DT_REMOTE_ERR_INVALID_VALUE`); `details_json` always names `{"parameter":"blend.<path>"}` and adds `"constraint":"<slug>"` where the error table specifies one.
- **Verification stack:** `cmake --build build -j$(nproc)` → `ctest --test-dir build` → `cd tools/mcp && .venv/bin/pytest -q` → (final task only) `DARKTABLE_BIN=$PWD/build/bin/darktable tools/mcp/.venv/bin/pytest -q -m integration tools/mcp`.
- Every commit message ends with BOTH trailers:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_017UPe8bYYZLQ8aasRge4kBP`

## Design amendments (binding; applied to the design doc in Task 9)

1. **The mask render extends the export path with a persistent, guarded per-pipe opt-in.** The export pipe cannot satisfy the existing focus/full-pipe gate. Add `mask_display_request` to `dt_dev_pixelpipe_t` in `pixelpipe_hb.h`, initialize it only when the pipe is created, and deliberately do **not** clear it at the `pixelpipe_hb.c:3106` process/restart label. Both the CPU and OpenCL blend paths call one pure `dt_develop_blend_mask_display_request_is_valid()` predicate. A new `dt_imageio_export_with_flags_and_mask()` entry point owns the optional target; the existing `dt_imageio_export_with_flags()` signature and all existing callers remain unchanged. Inside the throwaway export pipe, the target piece is temporarily enabled even when its live module is disabled, its module requests `DT_DEV_PIXELPIPE_DISPLAY_MASK`, and a missing target fails the export instead of returning an ordinary image mislabeled as a mask. The throwaway `dt_develop_t` means no live GUI flag is changed or restored.
2. **`boost.offset` in the schema is the boost-value storage offset, distinct from the Lab a/b marker offset `offset_ab = 0.5`.** The engine channel table's `boost_offset` field mirrors the GUI table's `boost_factor_offset` (`−6.64385619` for `Jz`/`Cz`, `0` elsewhere — verified in `rgbj_channels[]` and `_blend_init_blendif_boost_parameters`). The `0.5` for Lab `a`/`b` is the separate `offset_ab` used only in the non-normative `display_hint` formula and in the GUI's boost-rescale (which the wire deliberately does not reproduce). No source contradiction; recorded so an implementer never conflates the two.
3. **The H4 `inverted`+`combine` conflict guard is server-side with an explicit override member.** A patch that both changes `combine` (to a value differing from stored) and sets any explicit `inverted: <bool>` inside `parametric` is refused `invalid_value`, `constraint: "inverted_and_combine_conflict"`, unless the `blend` patch also carries `"allow_inverted_combine": true`. Sidecar passes the member through; the engine is authoritative.
4. **`off`/`uniform` (no spatial-mask bit) mask render returns solid white at normal framing.** For a target whose stored `mask_mode` lacks `DEVELOP_MASK_CONDITIONAL`, `DEVELOP_MASK_MASK`, and `DEVELOP_MASK_RASTER`, the blend path emits no display mask, so the export would return the normal image. The engine instead renders normally (for correct framing) then fills the buffer white before JPEG-encoding. `mask_of.mask_mode` reports the true stored mode string.
5. **HSL "value" channel wire name is `l` (lowercase).** `rgb_channels[]` labels the HSL value channel `N_("L")` but its slot is `DEVELOP_BLENDIF_l_in`/`_out`; the wire name is `l` to disambiguate from Lab `L`, matching the candidates sketch's `g R G B H S l`.
6. **`mask_of` must cross the MCP boundary, not merely the raw wire.** `render_preview` remains image-only for ordinary previews. For `show_mask`, the sidecar returns mixed content: a JSON text block carrying `mime_type`, dimensions, revision, and `mask_of`, followed by the native JPEG image block. This mirrors the existing `get_scopes` mixed-content idiom and makes the documented provenance observable to the model.
7. **The visual gate is deterministic and has one declared dependency.** Task 8 adds `Pillow>=10.0,<13` to the `dev` extra, uses no NumPy, pins `img/DSC07350.ARW`, `max_px=512`, exact normalized bright/dark ROIs, marker values, and numeric thresholds.

## File structure

| File | Responsibility |
|---|---|
| `src/control/remote_blend.h` / `.c` | blendif channel table + pure helpers/private transition parser (Task 1); `combine`+`parametric` in schema (Task 2), read (Task 3), patch-apply and public full `mask_mode` parser (Task 4) |
| `src/tests/unittests/control/test_remote_blend.c` | parity + pack/unpack + validation + schema/read/patch tests (Tasks 1–4) |
| `src/develop/pixelpipe_hb.h` (`dt_dev_pixelpipe_t`) / `pixelpipe_hb.c` | `mask_display_request` field + one-time initialization; restart preservation (Task 5) |
| `src/develop/blend.h` / `.c` | shared CPU/OpenCL mask-request predicate and both gate call sites (Task 5) |
| `src/imageio/imageio_common.h` / `imageio.c` | additive mask-aware export entry point; existing export API remains unchanged (Task 5) |
| `src/control/remote_edit.h` / `.c` | preview request/result mask fields + execute/white path (Task 5); expanded prepare target capture (Task 6) |
| `src/control/remote_protocol.h` / `.c` | `parametric_mask_params`+`mask_render` capabilities; `show_mask` parse/thread; `mask_of` serialize (Task 6) |
| `src/tests/unittests/control/test_remote_protocol.c` + `fixtures/*.json` | protocol tests + shared fixtures (Task 6) |
| `tools/mcp/src/darktable_mcp/server.py` + `tools/mcp/tests/test_tools.py` | capability gates + `show_mask` passthrough + docstrings/worked examples (Task 7) |
| `tools/mcp/pyproject.toml`, `tools/mcp/tests/integration/test_blend_tier1.py`, `test_parametric_masks_tier2.py` (NEW) | Pillow test dependency + cumulative Tier-1 expectation update + deterministic live `show_mask` assertions (Task 8) |
| docs (protocol reference, design status, divergence manifest, README) | Task 9 |

---

### Task 1: Engine blendif channel table + pure bitfield/marker helpers + transition matrix

**Files:**
- Modify: `src/control/remote_blend.h` (append declarations)
- Modify: `src/control/remote_blend.c` (append table, helpers, and private full-target parser)
- Test: `src/tests/unittests/control/test_remote_blend.c` (append)

**Interfaces:**
- Consumes: M-A's `dt_develop_blend_colorspace_t`, `dt_develop_blendif_channels_t`, the externed GUI tables `Lab_channels[]`/`rgb_channels[]`/`rgbj_channels[]` (blend_gui.c, non-`static const dt_iop_gui_blendif_channel_t[]`).
- Produces (for Tasks 2–4):
  - `typedef struct dt_remote_blendif_channel_t { const char *name; dt_develop_blendif_channels_t slot_in, slot_out; gboolean boost_supported; float boost_offset; float marker_offset; float display_factor; const char *display_unit; } dt_remote_blendif_channel_t;`
  - `const dt_remote_blendif_channel_t *dt_remote_blendif_channels(dt_develop_blend_colorspace_t csp);` (NULL-terminated table; NULL when the family has no parametric support)
  - `gboolean dt_remote_blendif_slot_enabled(uint32_t blendif, int slot);`
  - `gboolean dt_remote_blendif_slot_inverted(uint32_t blendif, int slot);`
  - `uint32_t dt_remote_blendif_slot_pack(uint32_t blendif, int slot, gboolean enabled, gboolean inverted);`
  - `gboolean dt_remote_blendif_markers_enable(const float m[4]);`
  - A file-static full-target parser recognizes all five non-raster targets
    for the transition helper. The public M-A
    `dt_remote_blend_mask_mode_from_string` deliberately remains limited to
    `off`/`uniform` through Tasks 1–3, so each intermediate commit keeps the
    already-shipped M-A patch behavior; Task 4 expands it atomically with the
    new patch stage.
  - `gboolean dt_remote_blend_mask_mode_transition(uint32_t stored, const char *target, uint32_t *out, const char **constraint);` applies the authoritative table without changing the stored `MASK` bit.

- [ ] **Step 1: Write the failing tests**

Append to `test_remote_blend.c` (add `#include "develop/blend_gui.h"` is NOT needed; the GUI tables are declared here via `extern`). Add near the top includes, after the existing `#include "develop/blend.h"`:

```c
// The GUI blendif channel tables are non-static `const` (blend_gui.c) and
// directly linkable; extern them for the GUI-parity binding below.
extern const dt_iop_gui_blendif_channel_t Lab_channels[];
extern const dt_iop_gui_blendif_channel_t rgb_channels[];
extern const dt_iop_gui_blendif_channel_t rgbj_channels[];
```

Then the tests:

```c
/* ------------------------------------------------------------------ */
/* Task 1: blendif channel table + pure helpers (Tier 2 / M-B)         */
/* ------------------------------------------------------------------ */

static const dt_remote_blendif_channel_t *_find_channel(dt_develop_blend_colorspace_t csp,
                                                        const char *name)
{
  for(const dt_remote_blendif_channel_t *c = dt_remote_blendif_channels(csp); c && c->name; c++)
    if(!strcmp(c->name, name)) return c;
  return NULL;
}

// Bind the engine table to the externed GUI table entry-for-entry: same
// slots, same boost enablement, same boost storage offset. Drift in the
// GUI tables becomes a failure here with zero GUI changes (design decision 6).
static void _assert_parity(dt_develop_blend_colorspace_t csp,
                           const dt_iop_gui_blendif_channel_t *gui,
                           const char *const *wire_names)
{
  const dt_remote_blendif_channel_t *eng = dt_remote_blendif_channels(csp);
  assert_non_null(eng);
  guint i = 0;
  for(; gui[i].label; i++)   // GUI tables terminate with a {NULL} entry
  {
    assert_non_null(eng[i].name);
    assert_string_equal(eng[i].name, wire_names[i]);
    assert_int_equal(eng[i].slot_in, gui[i].param_channels[0]);
    assert_int_equal(eng[i].slot_out, gui[i].param_channels[1]);
    assert_int_equal(eng[i].boost_supported, gui[i].boost_factor_enabled);
    assert_float_equal(eng[i].boost_offset, gui[i].boost_factor_offset, 1e-6);
  }
  // engine table ends exactly where the GUI table ends
  assert_null(eng[i].name);
}

static void test_channels_parity_lab(void **state)
{
  (void)state;
  static const char *const names[] = { "L", "a", "b", "C", "h" };
  _assert_parity(DEVELOP_BLEND_CS_LAB, Lab_channels, names);
}
static void test_channels_parity_rgb_display(void **state)
{
  (void)state;
  static const char *const names[] = { "g", "R", "G", "B", "H", "S", "l" };
  _assert_parity(DEVELOP_BLEND_CS_RGB_DISPLAY, rgb_channels, names);
}
static void test_channels_parity_rgb_scene(void **state)
{
  (void)state;
  static const char *const names[] = { "g", "R", "G", "B", "Jz", "Cz", "hz" };
  _assert_parity(DEVELOP_BLEND_CS_RGB_SCENE, rgbj_channels, names);
}
static void test_channels_none_and_raw_unsupported(void **state)
{
  (void)state;
  assert_null(dt_remote_blendif_channels(DEVELOP_BLEND_CS_NONE));
  assert_null(dt_remote_blendif_channels(DEVELOP_BLEND_CS_RAW));
}

static void test_boost_offsets_and_ranges(void **state)
{
  (void)state;
  // Jz/Cz carry the -6.64385619 storage offset; everything else 0.
  assert_float_equal(_find_channel(DEVELOP_BLEND_CS_RGB_SCENE, "Jz")->boost_offset,
                     -6.64385619f, 1e-6);
  assert_float_equal(_find_channel(DEVELOP_BLEND_CS_RGB_SCENE, "Cz")->boost_offset,
                     -6.64385619f, 1e-6);
  assert_float_equal(_find_channel(DEVELOP_BLEND_CS_LAB, "L")->boost_offset, 0.0f, 1e-6);
  // hue channels have no boost; g/R/G/B/L do.
  assert_false(_find_channel(DEVELOP_BLEND_CS_LAB, "h")->boost_supported);
  assert_true(_find_channel(DEVELOP_BLEND_CS_LAB, "L")->boost_supported);
  assert_false(_find_channel(DEVELOP_BLEND_CS_RGB_SCENE, "hz")->boost_supported);
  // Lab a/b marker offset is 0.5; others 0.
  assert_float_equal(_find_channel(DEVELOP_BLEND_CS_LAB, "a")->marker_offset, 0.5f, 1e-6);
  assert_float_equal(_find_channel(DEVELOP_BLEND_CS_LAB, "L")->marker_offset, 0.0f, 1e-6);

  // Bind each display-hint category to the GUI print functions: normalized
  // channels are percentages, a/b are centered 256-scale values, and hues
  // are degrees. Empty string means deliberately unitless.
  const dt_remote_blendif_channel_t *lab_a =
    _find_channel(DEVELOP_BLEND_CS_LAB, "a");
  const dt_remote_blendif_channel_t *rgb_g =
    _find_channel(DEVELOP_BLEND_CS_RGB_DISPLAY, "g");
  const dt_remote_blendif_channel_t *rgb_h =
    _find_channel(DEVELOP_BLEND_CS_RGB_DISPLAY, "H");
  assert_float_equal(lab_a->display_factor, 256.0f, 1e-6);
  assert_string_equal(lab_a->display_unit, "");
  assert_float_equal(rgb_g->display_factor, 100.0f, 1e-6);
  assert_string_equal(rgb_g->display_unit, "%");
  assert_float_equal(rgb_h->display_factor, 360.0f, 1e-6);
  assert_string_equal(rgb_h->display_unit, "\xc2\xb0");
}

static void test_slot_enabled_inverted_pack(void **state)
{
  (void)state;
  // Exercise every in/out slot in every supported family, not one sample.
  const dt_develop_blend_colorspace_t families[] = {
    DEVELOP_BLEND_CS_LAB, DEVELOP_BLEND_CS_RGB_DISPLAY, DEVELOP_BLEND_CS_RGB_SCENE
  };
  for(guint f = 0; f < G_N_ELEMENTS(families); f++)
    for(const dt_remote_blendif_channel_t *c = dt_remote_blendif_channels(families[f]);
        c && c->name; c++)
    {
      const int slots[] = { c->slot_in, c->slot_out };
      for(guint i = 0; i < G_N_ELEMENTS(slots); i++)
      {
        const int slot = slots[i];
        uint32_t bf = dt_remote_blendif_slot_pack(0u, slot, TRUE, TRUE);
        assert_true(dt_remote_blendif_slot_enabled(bf, slot));
        assert_true(dt_remote_blendif_slot_inverted(bf, slot));
        assert_int_equal(bf, (1u << slot) | (1u << (16 + slot)));
        bf = dt_remote_blendif_slot_pack(bf, slot, FALSE, FALSE);
        assert_false(dt_remote_blendif_slot_enabled(bf, slot));
        assert_false(dt_remote_blendif_slot_inverted(bf, slot));
        assert_int_equal(bf, 0u);
      }
    }

  // the legacy DEVELOP_BLENDIF_active bit (31) is always stripped by pack
  uint32_t bf = dt_remote_blendif_slot_pack(1u << 31, 0, TRUE, FALSE);
  assert_int_equal(bf & (1u << 31), 0u);
}

static void test_markers_enable_rule(void **state)
{
  (void)state;
  const float full_span[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
  const float half[4] = { 0.0f, 0.0f, 0.55f, 0.65f };
  const float hard[4] = { 0.3f, 0.3f, 0.3f, 0.3f };
  assert_false(dt_remote_blendif_markers_enable(full_span)); // identity -> disabled
  assert_true(dt_remote_blendif_markers_enable(half));
  assert_true(dt_remote_blendif_markers_enable(hard));
}

static void test_mask_mode_targets_and_transition_matrix(void **state)
{
  (void)state;
  typedef struct { const char *name; uint32_t bits; } target_t;
  static const target_t targets[] = {
    { "off", DEVELOP_MASK_DISABLED },
    { "uniform", DEVELOP_MASK_ENABLED },
    { "parametric", DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL },
    { "drawn", DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK },
    { "drawn+parametric", DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK_CONDITIONAL },
  };
  static const uint32_t rows[] = {
    DEVELOP_MASK_DISABLED,
    DEVELOP_MASK_ENABLED,
    DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL,
    DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK,
    DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK_CONDITIONAL,
  };

  // Every target is exercised through the transition helper below. The
  // public M-A parser remains off/uniform until Task 4 so Tasks 1-3 are
  // independently green and do not broaden shipped patch behavior early.
  uint32_t ignored = 0;

  // Every non-raster cell in the authoritative appendix: success exactly
  // when stored and target MASK ownership agree.
  for(guint r = 0; r < G_N_ELEMENTS(rows); r++)
    for(guint t = 0; t < G_N_ELEMENTS(targets); t++)
    {
      uint32_t projected = UINT32_MAX;
      const char *constraint = NULL;
      const gboolean same_drawn =
        ((rows[r] & DEVELOP_MASK_MASK) != 0)
        == ((targets[t].bits & DEVELOP_MASK_MASK) != 0);
      assert_int_equal(dt_remote_blend_mask_mode_transition(
                         rows[r], targets[t].name, &projected, &constraint),
                       same_drawn);
      if(same_drawn)
      {
        assert_null(constraint);
        assert_int_equal(projected, targets[t].bits);
      }
      else
        assert_string_equal(constraint, "drawn_via_attach_only");
    }

  // The helper owns only ENABLED|CONDITIONAL; a future/unowned bit is
  // preserved exactly.
  const uint32_t future_bit = 1u << 17;
  uint32_t projected = 0;
  const char *constraint = NULL;
  assert_true(dt_remote_blend_mask_mode_transition(
    DEVELOP_MASK_ENABLED | future_bit, "parametric", &projected, &constraint));
  assert_int_equal(projected,
                   DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL | future_bit);

  assert_false(dt_remote_blend_mask_mode_transition(
    DEVELOP_MASK_ENABLED | DEVELOP_MASK_RASTER, "off", &ignored, &constraint));
  assert_string_equal(constraint, "raster_unsupported");
  assert_false(dt_remote_blend_mask_mode_transition(
    DEVELOP_MASK_DISABLED, "bogus", &ignored, &constraint));
  assert_string_equal(constraint, "unknown_value");
}
```

Register the seven parity/helper tests plus `test_mask_mode_targets_and_transition_matrix` in `main()`'s array.

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build -j$(nproc) 2>&1 | tail -5`
Expected: compile FAILURE — `dt_remote_blendif_channel_t` / `dt_remote_blendif_channels` undeclared.

- [ ] **Step 3: Declare the table + helpers in `remote_blend.h`**

Append inside the `G_BEGIN_DECLS` block (after M-A's declarations):

```c
/* ---- Tier 2 / M-B: parametric (blendif) masks ---- */

/** one wire channel of a blend colorspace family: a stable ASCII name
 * (never a translated GUI label) plus its `_in`/`_out` blendif slots and
 * boost metadata. `boost_offset` is the STORAGE offset of the boost
 * value's GUI zero (-6.64385619 for Jz/Cz, 0 else); `marker_offset` is
 * the separate Lab a/b centering (0.5) used only in the non-normative
 * display hint. See the parametric-masks design SS Boost. */
typedef struct dt_remote_blendif_channel_t
{
  const char *name;
  dt_develop_blendif_channels_t slot_in;
  dt_develop_blendif_channels_t slot_out;
  gboolean boost_supported;
  float boost_offset;
  float marker_offset;
  float display_factor;   // non-normative display hint
  const char *display_unit;
} dt_remote_blendif_channel_t;

/** the NULL-terminated channel table for `csp`, or NULL when the family
 * has no parametric support (RAW / NONE). The order matches the GUI
 * tables entry-for-entry (bound by a parity test). */
const dt_remote_blendif_channel_t *
dt_remote_blendif_channels(dt_develop_blend_colorspace_t csp);

/** bitfield <-> per-slot views. `slot` is a usable storage slot 0..14;
 * slot 15 is DEVELOP_BLENDIF_unused, whose polarity bit aliases the legacy
 * DEVELOP_BLENDIF_active bit (31), and is rejected. `slot_pack` sets/clears
 * the enable bit (slot) and polarity bit (16+slot) and always strips bit 31. */
gboolean dt_remote_blendif_slot_enabled(uint32_t blendif, int slot);
gboolean dt_remote_blendif_slot_inverted(uint32_t blendif, int slot);
uint32_t dt_remote_blendif_slot_pack(uint32_t blendif, int slot,
                                     gboolean enabled, gboolean inverted);

/** the derived-enable rule (single source of truth): a slot is DISABLED
 * iff its markers are the full-span identity (m[1]==0 && m[2]==1). */
gboolean dt_remote_blendif_markers_enable(const float m[4]);

/** Apply the authoritative mask-mode transition table. Only ENABLED and
 * CONDITIONAL may change; MASK and every unknown bit are preserved.
 * On FALSE, `*constraint` is one of the static strings unknown_value,
 * drawn_via_attach_only, or raster_unsupported. */
gboolean dt_remote_blend_mask_mode_transition(uint32_t stored,
                                              const char *target,
                                              uint32_t *out,
                                              const char **constraint);
```

- [ ] **Step 4: Implement the table + helpers in `remote_blend.c`**

Update the top comments in `remote_blend.h` and
`test_remote_blend.c` so they describe the Tier-1 base plus Tier-2
extensions rather than calling the completed surface Tier-1-only.

Add, next to M-A's `offsetof` asserts, two relative tripwires for the blendif arrays:

```c
G_STATIC_ASSERT(sizeof(((dt_develop_blend_params_t *)0)->blendif_parameters)
                == 4 * DEVELOP_BLENDIF_SIZE * sizeof(float));
G_STATIC_ASSERT(sizeof(((dt_develop_blend_params_t *)0)->blendif_boost_factors)
                == DEVELOP_BLENDIF_SIZE * sizeof(float));
G_STATIC_ASSERT(offsetof(dt_develop_blend_params_t, blendif_boost_factors)
                == offsetof(dt_develop_blend_params_t, blendif_parameters)
                   + 4 * DEVELOP_BLENDIF_SIZE * sizeof(float));
```

Append the table + helpers:

```c
/* ------------------------------------------------------------------ */
/* Tier 2: blendif channel tables (wire names are stable ASCII)        */
/* ------------------------------------------------------------------ */

// display_factor/display_unit are NON-NORMATIVE metadata (clients that
// want GUI-style numbers): display ~= (stored - marker_offset) * 2^boost
// * display_factor. The wire itself always carries stored 0..1 values.
static const dt_remote_blendif_channel_t _channels_lab[] = {
  { "L", DEVELOP_BLENDIF_L_in, DEVELOP_BLENDIF_L_out, TRUE,  0.0f, 0.0f, 100.0f, "%" },
  { "a", DEVELOP_BLENDIF_A_in, DEVELOP_BLENDIF_A_out, TRUE,  0.0f, 0.5f, 256.0f, "" },
  { "b", DEVELOP_BLENDIF_B_in, DEVELOP_BLENDIF_B_out, TRUE,  0.0f, 0.5f, 256.0f, "" },
  { "C", DEVELOP_BLENDIF_C_in, DEVELOP_BLENDIF_C_out, TRUE,  0.0f, 0.0f, 100.0f, "%" },
  { "h", DEVELOP_BLENDIF_h_in, DEVELOP_BLENDIF_h_out, FALSE, 0.0f, 0.0f, 360.0f, "\xc2\xb0" },
  { NULL, 0, 0, FALSE, 0.0f, 0.0f, 0.0f, NULL } };

static const dt_remote_blendif_channel_t _channels_rgb_display[] = {
  { "g", DEVELOP_BLENDIF_GRAY_in,  DEVELOP_BLENDIF_GRAY_out,  TRUE,  0.0f, 0.0f, 100.0f, "%" },
  { "R", DEVELOP_BLENDIF_RED_in,   DEVELOP_BLENDIF_RED_out,   TRUE,  0.0f, 0.0f, 100.0f, "%" },
  { "G", DEVELOP_BLENDIF_GREEN_in, DEVELOP_BLENDIF_GREEN_out, TRUE,  0.0f, 0.0f, 100.0f, "%" },
  { "B", DEVELOP_BLENDIF_BLUE_in,  DEVELOP_BLENDIF_BLUE_out,  TRUE,  0.0f, 0.0f, 100.0f, "%" },
  { "H", DEVELOP_BLENDIF_H_in,     DEVELOP_BLENDIF_H_out,     FALSE, 0.0f, 0.0f, 360.0f, "\xc2\xb0" },
  { "S", DEVELOP_BLENDIF_S_in,     DEVELOP_BLENDIF_S_out,     FALSE, 0.0f, 0.0f, 100.0f, "%" },
  { "l", DEVELOP_BLENDIF_l_in,     DEVELOP_BLENDIF_l_out,     FALSE, 0.0f, 0.0f, 100.0f, "%" },
  { NULL, 0, 0, FALSE, 0.0f, 0.0f, 0.0f, NULL } };

static const dt_remote_blendif_channel_t _channels_rgb_scene[] = {
  { "g",  DEVELOP_BLENDIF_GRAY_in,  DEVELOP_BLENDIF_GRAY_out,  TRUE,  0.0f,         0.0f, 100.0f, "%" },
  { "R",  DEVELOP_BLENDIF_RED_in,   DEVELOP_BLENDIF_RED_out,   TRUE,  0.0f,         0.0f, 100.0f, "%" },
  { "G",  DEVELOP_BLENDIF_GREEN_in, DEVELOP_BLENDIF_GREEN_out, TRUE,  0.0f,         0.0f, 100.0f, "%" },
  { "B",  DEVELOP_BLENDIF_BLUE_in,  DEVELOP_BLENDIF_BLUE_out,  TRUE,  0.0f,         0.0f, 100.0f, "%" },
  { "Jz", DEVELOP_BLENDIF_Jz_in,    DEVELOP_BLENDIF_Jz_out,    TRUE,  -6.64385619f, 0.0f, 100.0f, "%" },
  { "Cz", DEVELOP_BLENDIF_Cz_in,    DEVELOP_BLENDIF_Cz_out,    TRUE,  -6.64385619f, 0.0f, 100.0f, "%" },
  { "hz", DEVELOP_BLENDIF_hz_in,    DEVELOP_BLENDIF_hz_out,    FALSE, 0.0f,         0.0f, 360.0f, "\xc2\xb0" },
  { NULL, 0, 0, FALSE, 0.0f, 0.0f, 0.0f, NULL } };

const dt_remote_blendif_channel_t *
dt_remote_blendif_channels(dt_develop_blend_colorspace_t csp)
{
  switch(csp)
  {
    case DEVELOP_BLEND_CS_LAB:         return _channels_lab;
    case DEVELOP_BLEND_CS_RGB_DISPLAY: return _channels_rgb_display;
    case DEVELOP_BLEND_CS_RGB_SCENE:   return _channels_rgb_scene;
    default:                           return NULL;  // RAW/NONE: no parametric channel table
  }
}

gboolean dt_remote_blendif_slot_enabled(uint32_t blendif, int slot)
{
  g_return_val_if_fail(slot >= 0 && slot < DEVELOP_BLENDIF_unused, FALSE);
  return (blendif & (1u << slot)) != 0;
}

gboolean dt_remote_blendif_slot_inverted(uint32_t blendif, int slot)
{
  g_return_val_if_fail(slot >= 0 && slot < DEVELOP_BLENDIF_unused, FALSE);
  return (blendif & (1u << (16 + slot))) != 0;
}

uint32_t dt_remote_blendif_slot_pack(uint32_t blendif, int slot,
                                     gboolean enabled, gboolean inverted)
{
  blendif &= ~(1u << 31);              // strip legacy DEVELOP_BLENDIF_active
  g_return_val_if_fail(slot >= 0 && slot < DEVELOP_BLENDIF_unused, blendif);
  if(enabled)  blendif |=  (1u << slot);        else blendif &= ~(1u << slot);
  if(inverted) blendif |=  (1u << (16 + slot)); else blendif &= ~(1u << (16 + slot));
  return blendif;
}

gboolean dt_remote_blendif_markers_enable(const float m[4])
{
  return !(m[1] == 0.0f && m[2] == 1.0f);
}
```

Add a private parser for every non-raster target. Refactor the existing
public parser through it while retaining M-A's `off`/`uniform` projection:

```c
static gboolean _mask_mode_target_from_string(const char *s, uint32_t *out)
{
  if(!s || !out) return FALSE;
  if(!strcmp(s, "off")) { *out = DEVELOP_MASK_DISABLED; return TRUE; }
  if(!strcmp(s, "uniform")) { *out = DEVELOP_MASK_ENABLED; return TRUE; }
  if(!strcmp(s, "parametric"))
  {
    *out = DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL;
    return TRUE;
  }
  if(!strcmp(s, "drawn"))
  {
    *out = DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK;
    return TRUE;
  }
  if(!strcmp(s, "drawn+parametric"))
  {
    *out = DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK_CONDITIONAL;
    return TRUE;
  }
  return FALSE;
}

gboolean dt_remote_blend_mask_mode_from_string(const char *s, uint32_t *out)
{
  if(!out) return FALSE;
  uint32_t target = 0;
  if(!_mask_mode_target_from_string(s, &target)
     || (target & (DEVELOP_MASK_MASK | DEVELOP_MASK_CONDITIONAL)))
    return FALSE;
  *out = target;
  return TRUE;
}
```

Then add the helper, calling the private parser rather than the temporarily
M-A-limited public wrapper:

```c
gboolean dt_remote_blend_mask_mode_transition(uint32_t stored,
                                              const char *target_name,
                                              uint32_t *out,
                                              const char **constraint)
{
  if(constraint) *constraint = NULL;
  if(stored & DEVELOP_MASK_RASTER)
  {
    if(constraint) *constraint = "raster_unsupported";
    return FALSE;
  }

  uint32_t target = 0;
  if(!_mask_mode_target_from_string(target_name, &target))
  {
    if(constraint) *constraint = "unknown_value";
    return FALSE;
  }
  if((stored & DEVELOP_MASK_MASK) != (target & DEVELOP_MASK_MASK))
  {
    if(constraint) *constraint = "drawn_via_attach_only";
    return FALSE;
  }

  *out = (stored & ~(DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL))
         | (target & (DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL));
  return TRUE;
}
```

- [ ] **Step 5: Build and run**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R test_remote_blend --output-on-failure`
Expected: PASS (M-A parser/patch behavior is unchanged; the new pure helper
already covers the final transition vocabulary).

- [ ] **Step 6: Commit**

```bash
git add src/control/remote_blend.h src/control/remote_blend.c \
        src/tests/unittests/control/test_remote_blend.c
git commit -m "feat: blendif channel table + pure slot/marker helpers (Tier 2 / M-B)

Family-scoped wire channel tables (Lab/RGB-display/RGB-scene) bound to the
GUI tables by a parity test; pure enable/polarity pack/unpack and the
derived-enable rule; a private full-target parser powers the pure
state-preserving transition helper while the public M-A parser remains
off/uniform until the atomic Task-4 patch expansion."
```
(with both Global Constraints trailers)

---

### Task 2: `combine` + `parametric` schema

**Files:**
- Modify: `src/control/remote_blend.c` (`dt_remote_blend_schema`)
- Test: `src/tests/unittests/control/test_remote_blend.c` (append)

**Interfaces:**
- Consumes: Task 1's channel table; M-A's `blend_fixture_t` harness and `_node_object` helper.
- Produces: `dt_remote_blend_schema()` emits state-aware `mask_mode.values`/`writable`, `combine`, `parametric.channels`, and the write-only `allow_inverted_combine` confirmation member for Lab/RGB families only.

- [ ] **Step 1: Write the failing tests**

Append to `test_remote_blend.c`:

```c
/* ------------------------------------------------------------------ */
/* Task 2: parametric + combine schema                                 */
/* ------------------------------------------------------------------ */

static void test_schema_parametric_for_rgb_module(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  JsonNode *node = dt_remote_blend_schema(fx->module);
  JsonObject *schema = _node_object(node);

  // Non-drawn state: the exact writable vocabulary is off/uniform/parametric.
  JsonObject *mm_schema = json_object_get_object_member(schema, "mask_mode");
  JsonArray *mm = json_object_get_array_member(mm_schema, "values");
  assert_int_equal(json_array_get_length(mm), 3);
  assert_string_equal(json_array_get_string_element(mm, 0), "off");
  assert_string_equal(json_array_get_string_element(mm, 1), "uniform");
  assert_string_equal(json_array_get_string_element(mm, 2), "parametric");
  assert_true(json_object_get_boolean_member(mm_schema, "writable"));

  // combine enum, four values, writable
  JsonObject *combine = json_object_get_object_member(schema, "combine");
  assert_true(json_object_get_boolean_member(combine, "writable"));
  JsonArray *cv = json_object_get_array_member(combine, "values");
  assert_int_equal(json_array_get_length(cv), 4);
  assert_string_equal(json_array_get_string_element(cv, 0), "exclusive");
  assert_string_equal(json_array_get_string_element(cv, 1), "inclusive");
  assert_string_equal(json_array_get_string_element(cv, 2), "exclusive_inverted");
  assert_string_equal(json_array_get_string_element(cv, 3), "inclusive_inverted");

  JsonObject *override = json_object_get_object_member(schema, "allow_inverted_combine");
  assert_string_equal(json_object_get_string_member(override, "type"), "bool");
  assert_true(json_object_get_boolean_member(override, "writable"));
  assert_true(json_object_get_boolean_member(override, "write_only"));

  // parametric.channels lists exactly the effective family's slots
  const dt_develop_blend_colorspace_t eff =
    dt_remote_blend_effective_colorspace(fx->module, fx->module->blend_params->blend_cst);
  JsonObject *channels = json_object_get_object_member(
    json_object_get_object_member(schema, "parametric"), "channels");
  guint expected = 0;
  for(const dt_remote_blendif_channel_t *c = dt_remote_blendif_channels(eff); c && c->name; c++)
    expected += 2;  // _in and _out
  assert_int_equal((guint)json_object_get_size(channels), expected);

  // a hue slot has boost:null; Jz_in carries the shifted range
  if(eff == DEVELOP_BLEND_CS_RGB_SCENE)
  {
    JsonObject *jz = json_object_get_object_member(channels, "Jz_in");
    JsonObject *boost = json_object_get_object_member(jz, "boost");
    JsonArray *range = json_object_get_array_member(boost, "range");
    assert_float_equal(json_array_get_double_element(range, 0), -6.64385619, 1e-5);
    assert_float_equal(json_array_get_double_element(range, 1), 11.35614381, 1e-5);
    assert_true(json_object_has_member(channels, "hz_in"));
    JsonObject *hz = json_object_get_object_member(channels, "hz_in");
    assert_true(json_object_get_null_member(hz, "boost"));
  }
  JsonArray *md = json_object_get_array_member(
    json_object_get_object_member(channels,
      eff == DEVELOP_BLEND_CS_LAB ? "L_in" : "g_in"), "markers_domain");
  assert_float_equal(json_array_get_double_element(md, 0), 0.0, 1e-6);
  assert_float_equal(json_array_get_double_element(md, 1), 1.0, 1e-6);

  json_node_unref(node);
  blend_fixture_free(fx);
}

static void _assert_mask_mode_schema(dt_iop_module_t *module,
                                     uint32_t stored,
                                     const char *const *expected,
                                     guint expected_len,
                                     gboolean writable)
{
  module->blend_params->mask_mode = stored;
  JsonNode *node = dt_remote_blend_schema(module);
  JsonObject *mm = json_object_get_object_member(_node_object(node), "mask_mode");
  JsonArray *values = json_object_get_array_member(mm, "values");
  assert_int_equal(json_array_get_length(values), expected_len);
  for(guint i = 0; i < expected_len; i++)
    assert_string_equal(json_array_get_string_element(values, i), expected[i]);
  assert_int_equal(json_object_get_boolean_member(mm, "writable"), writable);
  json_node_unref(node);
}

static void test_schema_mask_mode_state_aware(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  fx->module->blend_params->blend_cst = DEVELOP_BLEND_CS_RGB_SCENE;
  static const char *const plain[] = { "off", "uniform", "parametric" };
  static const char *const drawn[] = { "drawn", "drawn+parametric" };
  static const char *const raster[] = { "raster" };

  _assert_mask_mode_schema(fx->module, DEVELOP_MASK_DISABLED, plain, 3, TRUE);
  _assert_mask_mode_schema(fx->module,
                           DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL,
                           plain, 3, TRUE);
  _assert_mask_mode_schema(fx->module,
                           DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK,
                           drawn, 2, TRUE);
  _assert_mask_mode_schema(fx->module,
                           DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK_CONDITIONAL,
                           drawn, 2, TRUE);
  _assert_mask_mode_schema(fx->module,
                           DEVELOP_MASK_ENABLED | DEVELOP_MASK_RASTER,
                           raster, 1, FALSE);
  blend_fixture_free(fx);
}

static void test_schema_no_parametric_for_raw_module(void **state)
{
  (void)state;
  // Force the blending exposure fixture into RAW to exercise the family
  // gate without depending on a non-blending module's NULL schema.
  blend_fixture_t *fx = blend_fixture_new("exposure");
  fx->module->blend_params->blend_cst = DEVELOP_BLEND_CS_RAW;
  JsonNode *node = dt_remote_blend_schema(fx->module);
  JsonObject *schema = _node_object(node);
  assert_false(json_object_has_member(schema, "parametric"));
  assert_false(json_object_has_member(schema, "combine"));
  json_node_unref(node);

  // A legacy conditional bit remains representable/removable, but RAW
  // still does not expose writable channel/combine members.
  fx->module->blend_params->mask_mode =
    DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL;
  node = dt_remote_blend_schema(fx->module);
  schema = _node_object(node);
  JsonObject *mm = json_object_get_object_member(schema, "mask_mode");
  JsonArray *values = json_object_get_array_member(mm, "values");
  assert_int_equal(json_array_get_length(values), 3);
  assert_string_equal(json_array_get_string_element(values, 2), "parametric");
  assert_true(json_object_get_boolean_member(mm, "writable"));
  assert_false(json_object_has_member(schema, "parametric"));
  assert_false(json_object_has_member(schema, "combine"));
  json_node_unref(node);
  blend_fixture_free(fx);
}
```

Register all three tests in `main()`. Update Tier 1's
`test_schema_shape_for_rgb_module` by changing the `mask_mode.values`
length assertion from 2 to 3 and adding an element-2 assertion for
`"parametric"`. Replace
`test_schema_mask_mode_not_writable_with_extra_bits` (and rename its
registration to match) with this metadata regression:

```c
static void test_schema_mask_mode_extra_bits_metadata(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  fx->module->blend_params->mask_mode =
    DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL;
  JsonNode *node = dt_remote_blend_schema(fx->module);
  JsonObject *mm = json_object_get_object_member(_node_object(node), "mask_mode");
  assert_true(json_object_get_boolean_member(mm, "writable"));
  assert_true(json_object_get_boolean_member(mm, "current_extra_bits"));
  json_node_unref(node);

  fx->module->blend_params->mask_mode =
    DEVELOP_MASK_ENABLED | DEVELOP_MASK_RASTER;
  node = dt_remote_blend_schema(fx->module);
  mm = json_object_get_object_member(_node_object(node), "mask_mode");
  assert_false(json_object_get_boolean_member(mm, "writable"));
  assert_true(json_object_get_boolean_member(mm, "current_extra_bits"));
  json_node_unref(node);
  blend_fixture_free(fx);
}
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R test_remote_blend --output-on-failure 2>&1 | tail -20`
Expected: FAIL — schema has no `combine`/`parametric` members / `mask_mode.values` lacks `"parametric"`.

- [ ] **Step 3: Implement the schema additions**

In `dt_remote_blend_schema`, compute parametric support before building
`mask_mode`, then replace Tier 1's fixed two-value schema block with the
state-aware appendix projection:

```c
  const dt_develop_blend_colorspace_t eff_mm =
    dt_remote_blend_effective_colorspace(module, module->blend_params->blend_cst);
  const gboolean parametric_supported = dt_remote_blendif_channels(eff_mm) != NULL;
  const gboolean raster = (bp->mask_mode & DEVELOP_MASK_RASTER) != 0;
  const gboolean drawn = (bp->mask_mode & DEVELOP_MASK_MASK) != 0;
  const gboolean conditional = (bp->mask_mode & DEVELOP_MASK_CONDITIONAL) != 0;

  json_builder_set_member_name(b, "mask_mode");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "type");
  json_builder_add_string_value(b, "enum");
  json_builder_set_member_name(b, "values");
  json_builder_begin_array(b);
  if(raster)
    json_builder_add_string_value(b, "raster");
  else if(drawn)
  {
    json_builder_add_string_value(b, "drawn");
    // Keep a legacy unsupported current value representable so it can be
    // removed, even though it cannot be newly added in this family.
    if(parametric_supported || conditional)
      json_builder_add_string_value(b, "drawn+parametric");
  }
  else
  {
    json_builder_add_string_value(b, "off");
    json_builder_add_string_value(b, "uniform");
    if(parametric_supported || conditional)
      json_builder_add_string_value(b, "parametric");
  }
  json_builder_end_array(b);
  json_builder_set_member_name(b, "writable");
  json_builder_add_boolean_value(
    b, !raster && (!drawn || parametric_supported || conditional));
  json_builder_set_member_name(b, "current_extra_bits");
  json_builder_add_boolean_value(b, extra_bits);
  json_builder_end_object(b);
```

After the M-A float-field loop, before `json_builder_end_object(b)`, add:

```c
  // Tier 2: combine + parametric (Lab / RGB families only; absent for RAW)
  if(parametric_supported)
  {
    json_builder_set_member_name(b, "allow_inverted_combine");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "bool");
    json_builder_set_member_name(b, "writable");
    json_builder_add_boolean_value(b, TRUE);
    json_builder_set_member_name(b, "write_only");
    json_builder_add_boolean_value(b, TRUE);
    json_builder_end_object(b);

    json_builder_set_member_name(b, "combine");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "enum");
    json_builder_set_member_name(b, "values");
    json_builder_begin_array(b);
    json_builder_add_string_value(b, "exclusive");
    json_builder_add_string_value(b, "inclusive");
    json_builder_add_string_value(b, "exclusive_inverted");
    json_builder_add_string_value(b, "inclusive_inverted");
    json_builder_end_array(b);
    json_builder_set_member_name(b, "writable");
    json_builder_add_boolean_value(b, TRUE);
    json_builder_end_object(b);

    json_builder_set_member_name(b, "parametric");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "channels");
    json_builder_begin_object(b);
    for(const dt_remote_blendif_channel_t *c = dt_remote_blendif_channels(eff_mm); c && c->name; c++)
    {
      for(int io = 0; io < 2; io++)
      {
        gchar *slot_name = g_strdup_printf("%s_%s", c->name, io ? "out" : "in");
        json_builder_set_member_name(b, slot_name);
        g_free(slot_name);
        json_builder_begin_object(b);

        json_builder_set_member_name(b, "markers_domain");
        json_builder_begin_array(b);
        json_builder_add_double_value(b, 0.0);
        json_builder_add_double_value(b, 1.0);
        json_builder_end_array(b);

        json_builder_set_member_name(b, "boost");
        if(c->boost_supported)
        {
          json_builder_begin_object(b);
          json_builder_set_member_name(b, "writable");
          json_builder_add_boolean_value(b, TRUE);
          json_builder_set_member_name(b, "offset");
          json_builder_add_double_value(b, (double)c->boost_offset);
          json_builder_set_member_name(b, "range");
          json_builder_begin_array(b);
          json_builder_add_double_value(b, (double)c->boost_offset);
          json_builder_add_double_value(b, (double)c->boost_offset + 18.0);
          json_builder_end_array(b);
          json_builder_end_object(b);
        }
        else
          json_builder_add_null_value(b);

        json_builder_set_member_name(b, "display_hint");
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "factor");
        json_builder_add_double_value(b, (double)c->display_factor);
        json_builder_set_member_name(b, "offset");
        json_builder_add_double_value(b, (double)c->marker_offset);
        json_builder_set_member_name(b, "unit");
        json_builder_add_string_value(b, c->display_unit);
        json_builder_set_member_name(b, "boost_scales");
        json_builder_add_boolean_value(b, c->boost_supported);
        json_builder_end_object(b);

        json_builder_end_object(b);
      }
    }
    json_builder_end_object(b);  // channels
    json_builder_end_object(b);  // parametric
  }
```

- [ ] **Step 4: Build and run**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R test_remote_blend --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/control/remote_blend.c src/tests/unittests/control/test_remote_blend.c
git commit -m "feat: parametric + combine blend schema (Tier 2 / M-B)

Live-instance schema gains combine (four enum values) and parametric.channels
(the effective family's _in/_out slots with markers_domain, boost range/offset
or null, and a non-normative display_hint); mask_mode values/writability are
state-aware for plain, drawn, and raster rows; the conflict override is
advertised write-only. Parametric members remain absent for RAW/NONE."
```
(plus trailers)

---

### Task 3: `combine` + `parametric` read

**Files:**
- Modify: `src/control/remote_blend.c` (`dt_remote_blend_read`)
- Test: `src/tests/unittests/control/test_remote_blend.c` (append)

**Interfaces:**
- Consumes: Task 1 helpers; M-A read serializer.
- Produces: `dt_remote_blend_read()` emits `combine`, `parametric` (enabled in-family slots only, each `{markers, inverted, boost}`), and `foreign_channels` boolean.

- [ ] **Step 1: Write the failing tests**

Append:

```c
/* ------------------------------------------------------------------ */
/* Task 3: parametric + combine read                                   */
/* ------------------------------------------------------------------ */

// helper: set a slot's markers + enable + polarity + boost directly
static void _set_slot(dt_develop_blend_params_t *bp, int slot,
                      float m0, float m1, float m2, float m3,
                      gboolean inverted, float boost)
{
  float *p = &bp->blendif_parameters[4 * slot];
  p[0] = m0; p[1] = m1; p[2] = m2; p[3] = m3;
  bp->blendif = dt_remote_blendif_slot_pack(bp->blendif, slot,
                                            dt_remote_blendif_markers_enable(p), inverted);
  bp->blendif_boost_factors[slot] = boost;
}

static void test_read_parametric_enabled_only(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  dt_develop_blend_params_t *bp = fx->module->blend_params;
  bp->blend_cst = DEVELOP_BLEND_CS_RGB_SCENE;
  bp->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL;
  bp->mask_combine = DEVELOP_COMBINE_NORM_EXCL;
  // g_in enabled (non-identity), inverted; R_in left full-span => disabled
  _set_slot(bp, DEVELOP_BLENDIF_GRAY_in, 0.1f, 0.2f, 0.6f, 0.7f, TRUE, 0.0f);
  _set_slot(bp, DEVELOP_BLENDIF_RED_in, 0.0f, 0.0f, 1.0f, 1.0f, FALSE, 0.0f);

  JsonNode *node = dt_remote_blend_read(fx->module);
  JsonObject *o = _node_object(node);
  assert_string_equal(json_object_get_string_member(o, "mask_mode"), "parametric");
  assert_string_equal(json_object_get_string_member(o, "combine"), "exclusive");
  assert_false(json_object_has_member(o, "allow_inverted_combine")); // write-only

  JsonObject *param = json_object_get_object_member(o, "parametric");
  assert_true(json_object_has_member(param, "g_in"));
  assert_false(json_object_has_member(param, "R_in"));   // full-span -> disabled -> absent
  JsonObject *g = json_object_get_object_member(param, "g_in");
  JsonArray *markers = json_object_get_array_member(g, "markers");
  assert_float_equal(json_array_get_double_element(markers, 0), 0.1, 1e-6);
  assert_true(json_object_get_boolean_member(g, "inverted"));
  assert_float_equal(json_object_get_double_member(g, "boost"), 0.0, 1e-6);
  assert_false(json_object_get_boolean_member(o, "foreign_channels"));

  json_node_unref(node);
  blend_fixture_free(fx);
}

static void test_read_foreign_channels_flag(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  dt_develop_blend_params_t *bp = fx->module->blend_params;
  bp->blend_cst = DEVELOP_BLEND_CS_RGB_SCENE;   // family = 0x77FF
  bp->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL;
  // enable a Lab-only slot (bit 8 = C_in, in 0x3377 but the scene family
  // mask 0x77FF also includes bit 8; choose an out-of-0x77FF slot instead:
  // bit 15 DEVELOP_BLENDIF_unused is outside both). Use raw bit 11, which
  // is set in neither Lab_MASK (0x3377) nor RGB_MASK (0x77FF).
  _set_slot(bp, 11, 0.1f, 0.2f, 0.6f, 0.7f, FALSE, 0.0f);

  JsonNode *node = dt_remote_blend_read(fx->module);
  JsonObject *o = _node_object(node);
  assert_true(json_object_get_boolean_member(o, "foreign_channels"));
  json_node_unref(node);

  // Reserved slot 15 is not a foreign channel. Its polarity twin is the
  // legacy active bit 31; neither half may make the read flag true.
  bp->blendif = (1u << DEVELOP_BLENDIF_unused) | (1u << DEVELOP_BLENDIF_active);
  node = dt_remote_blend_read(fx->module);
  o = _node_object(node);
  assert_false(json_object_get_boolean_member(o, "foreign_channels"));
  json_node_unref(node);
  blend_fixture_free(fx);
}

static void test_read_non_finite_parametric_storage_serializes_null(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  dt_develop_blend_params_t *bp = fx->module->blend_params;
  bp->blend_cst = DEVELOP_BLEND_CS_RGB_SCENE;
  bp->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL;
  _set_slot(bp, DEVELOP_BLENDIF_Jz_in, NAN, 0.2f, 0.6f, 0.7f,
            FALSE, INFINITY);

  JsonNode *node = dt_remote_blend_read(fx->module);
  JsonObject *entry = json_object_get_object_member(
    json_object_get_object_member(_node_object(node), "parametric"), "Jz_in");
  JsonArray *markers = json_object_get_array_member(entry, "markers");
  assert_true(json_node_is_null(json_array_get_element(markers, 0)));
  assert_true(json_object_get_null_member(entry, "boost"));
  json_node_unref(node);
  blend_fixture_free(fx);
}
```

Register all three.

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R test_remote_blend --output-on-failure 2>&1 | tail -20`
Expected: FAIL — read lacks `combine`/`parametric`/`foreign_channels`.

- [ ] **Step 3: Implement the read additions**

Add the read-direction `combine` mapping helper near the top of
`remote_blend.c` (Task 4 adds the write direction when it first has a
caller, keeping this intermediate commit warning-free):

```c
static const char *_combine_to_string(uint32_t mask_combine)
{
  switch(mask_combine & (DEVELOP_COMBINE_INV | DEVELOP_COMBINE_INCL))
  {
    case DEVELOP_COMBINE_NORM_EXCL: return "exclusive";
    case DEVELOP_COMBINE_NORM_INCL: return "inclusive";
    case DEVELOP_COMBINE_INV_EXCL:  return "exclusive_inverted";
    case DEVELOP_COMBINE_INV_INCL:  return "inclusive_inverted";
    default:                        return "exclusive";
  }
}

```

In `dt_remote_blend_read`, after the M-A float-field loop, before `json_builder_end_object(b)`:

```c
  const dt_develop_blend_colorspace_t eff_r =
    dt_remote_blend_effective_colorspace(module, bp->blend_cst);
  const dt_remote_blendif_channel_t *table = dt_remote_blendif_channels(eff_r);
  if(table)
  {
    json_builder_set_member_name(b, "combine");
    json_builder_add_string_value(b, _combine_to_string(bp->mask_combine));

    json_builder_set_member_name(b, "parametric");
    json_builder_begin_object(b);
    uint32_t in_family = 0u;
    for(const dt_remote_blendif_channel_t *c = table; c->name; c++)
    {
      const int slots[2] = { (int)c->slot_in, (int)c->slot_out };
      for(int io = 0; io < 2; io++)
      {
        const int slot = slots[io];
        in_family |= (1u << slot);
        if(!dt_remote_blendif_slot_enabled(bp->blendif, slot)) continue;
        gchar *slot_name = g_strdup_printf("%s_%s", c->name, io ? "out" : "in");
        json_builder_set_member_name(b, slot_name);
        g_free(slot_name);
        json_builder_begin_object(b);
        const float *p = &bp->blendif_parameters[4 * slot];
        json_builder_set_member_name(b, "markers");
        json_builder_begin_array(b);
        for(int k = 0; k < 4; k++)
        {
          if(isfinite(p[k])) json_builder_add_double_value(b, (double)p[k]);
          else json_builder_add_null_value(b); // corrupt legacy storage: keep JSON strict
        }
        json_builder_end_array(b);
        json_builder_set_member_name(b, "inverted");
        json_builder_add_boolean_value(b, dt_remote_blendif_slot_inverted(bp->blendif, slot));
        _add_float_member(b, "boost", bp->blendif_boost_factors[slot]);
        json_builder_end_object(b);
      }
    }
    json_builder_end_object(b);

    // foreign: any enabled slot outside the effective family (legacy edits)
    const uint32_t usable_slot_mask = (1u << DEVELOP_BLENDIF_unused) - 1u;
    const uint32_t enabled_slots = bp->blendif & usable_slot_mask;
    json_builder_set_member_name(b, "foreign_channels");
    json_builder_add_boolean_value(b, (enabled_slots & ~in_family) != 0);
  }
```

- [ ] **Step 4: Build and run**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R test_remote_blend --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/control/remote_blend.c src/tests/unittests/control/test_remote_blend.c
git commit -m "feat: parametric + combine blend read (Tier 2 / M-B)

Reads emit combine (stored INV|INCL bits, MASKS_POS ignored), only
enabled in-family slots as {markers, inverted, boost}, and a
foreign_channels boolean when out-of-family slots are enabled. Corrupt
non-finite marker/boost storage serializes as JSON null."
```
(plus trailers)

---

### Task 4: `combine` + `parametric` patch apply

**Files:**
- Modify: `src/control/remote_blend.h` (public parser contract)
- Modify: `src/control/remote_blend.c` (`dt_remote_blend_mask_mode_from_string`, `dt_remote_blend_patch_apply`)
- Test: `src/tests/unittests/control/test_remote_blend.c` (append)

**Interfaces:**
- Consumes: Tasks 1–3 helpers; M-A `dt_remote_blend_patch_apply` (extended in place); `dt_develop_blend_init_blendif_parameters()`.
- Produces: the public mask-mode parser gains all five non-raster targets,
  and `dt_remote_blend_patch_apply` handles their transitions plus `combine`
  + `parametric` per the validation order, with the error table below.

**Error table this task implements (constraint slugs normative):**

| condition | code | constraint |
|---|---|---|
| `parametric`/`combine` on a RAW/NONE family (no parametric channel table/semantics) | UNSUPPORTED_FIELD | (`parameter: blend.parametric`/`blend.combine`) |
| projected state would create unsupported `CONDITIONAL` (newly introduced in RAW/NONE, or moved from a supported family into RAW/NONE) | UNSUPPORTED_FIELD | (`parameter: blend.mask_mode` / `blend.colorspace`) |
| `combine` value unknown | INVALID_VALUE | `unknown_value` |
| `allow_inverted_combine` present but not boolean | INVALID_VALUE | `wrong_type` |
| `parametric` not an object, or a channel entry neither object nor null | INVALID_VALUE | `wrong_type` |
| channel name not in the effective family | UNSUPPORTED_FIELD | (`parameter: blend.parametric.<name>`) |
| unknown member inside a channel entry | UNSUPPORTED_FIELD | (`parameter: blend.parametric.<name>.<member>`) |
| `markers` missing / not an array of exactly 4 numbers | INVALID_VALUE | `markers` |
| markers non-finite, outside [0,1], or not ascending | INVALID_VALUE | `markers` |
| `inverted` present but not a boolean | INVALID_VALUE | `wrong_type` |
| `boost` on a `boost:null` (hue) channel | INVALID_VALUE | `boost` |
| `boost` non-number, non-finite, or out of `[offset, offset+18]` | INVALID_VALUE | `boost` |
| `parametric` while projected `mask_mode` lacks `CONDITIONAL` | INVALID_VALUE | `requires_parametric_mask_mode` |
| `inverted` change together with a `combine` change, no override | INVALID_VALUE | `inverted_and_combine_conflict` |
| `mask_mode` target changes stored `MASK`, or stored row is raster | INVALID_VALUE | `drawn_via_attach_only` / `raster_unsupported` |

- [ ] **Step 1: Write the failing tests**

Append (reuse M-A's `_patch_from_string`, `_assert_patch_fails`, `blend_fixture_t`):

First replace the compound-mode portion of the existing
`test_mask_mode_from_string` with the final public-parser expectations:

```c
  assert_true(dt_remote_blend_mask_mode_from_string("parametric", &v));
  assert_int_equal(v, DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL);
  assert_true(dt_remote_blend_mask_mode_from_string("drawn", &v));
  assert_int_equal(v, DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK);
  assert_true(dt_remote_blend_mask_mode_from_string("drawn+parametric", &v));
  assert_int_equal(v, DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK_CONDITIONAL);
  assert_false(dt_remote_blend_mask_mode_from_string("raster", &v));
  assert_false(dt_remote_blend_mask_mode_from_string("", &v));
  assert_false(dt_remote_blend_mask_mode_from_string(NULL, &v));
  assert_false(dt_remote_blend_mask_mode_from_string("off", NULL));
```

```c
/* ------------------------------------------------------------------ */
/* Task 4: parametric + combine patch apply                            */
/* ------------------------------------------------------------------ */

static void test_patch_parametric_sets_and_derives_enable(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  fx->module->blend_params->blend_cst = DEVELOP_BLEND_CS_RGB_SCENE;
  JsonObject *patch = _patch_from_string(
    "{\"mask_mode\":\"parametric\","
    " \"parametric\":{\"Jz_in\":{\"markers\":[0.0,0.0,0.5,0.6],\"inverted\":true}}}");
  dt_develop_blend_params_t dst = *fx->module->blend_params;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  assert_null(error);
  assert_int_equal(dst.mask_mode, DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL);
  const int slot = DEVELOP_BLENDIF_Jz_in;
  assert_true(dt_remote_blendif_slot_enabled(dst.blendif, slot));   // non-identity -> enabled
  assert_true(dt_remote_blendif_slot_inverted(dst.blendif, slot));
  assert_float_equal(dst.blendif_parameters[4 * slot + 2], 0.5f, 1e-6);
  // boost defaults to the channel offset (GUI zero) = -6.64385619
  assert_float_equal(dst.blendif_boost_factors[slot], -6.64385619f, 1e-5);
  json_object_unref(patch);
  blend_fixture_free(fx);
}

static void test_patch_parametric_null_resets_and_disables(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  dt_develop_blend_params_t *bp = fx->module->blend_params;
  bp->blend_cst = DEVELOP_BLEND_CS_RGB_SCENE;
  bp->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL;
  _set_slot(bp, DEVELOP_BLENDIF_Jz_in, 0.1f, 0.2f, 0.6f, 0.7f, TRUE, -6.0f);
  JsonObject *patch = _patch_from_string("{\"parametric\":{\"Jz_in\":null}}");
  dt_develop_blend_params_t dst = *bp;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  const int slot = DEVELOP_BLENDIF_Jz_in;
  assert_false(dt_remote_blendif_slot_enabled(dst.blendif, slot));     // reset -> disabled
  assert_false(dt_remote_blendif_slot_inverted(dst.blendif, slot));
  assert_float_equal(dst.blendif_parameters[4 * slot + 0], 0.0f, 1e-6);
  assert_float_equal(dst.blendif_parameters[4 * slot + 3], 1.0f, 1e-6);
  assert_float_equal(dst.blendif_boost_factors[slot], -6.64385619f, 1e-5); // channel offset
  json_object_unref(patch);
  blend_fixture_free(fx);
}

static void test_patch_parametric_preserves_foreign_and_unlisted(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  dt_develop_blend_params_t *bp = fx->module->blend_params;
  bp->blend_cst = DEVELOP_BLEND_CS_RGB_SCENE;
  bp->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL;
  _set_slot(bp, 11, 0.1f, 0.2f, 0.6f, 0.7f, FALSE, 0.0f);  // foreign slot
  _set_slot(bp, DEVELOP_BLENDIF_RED_in, 0.2f, 0.3f, 0.7f, 0.8f, FALSE, 0.0f); // unlisted
  JsonObject *patch = _patch_from_string(
    "{\"parametric\":{\"Jz_in\":{\"markers\":[0.0,0.0,0.5,0.6]}}}");
  dt_develop_blend_params_t dst = *bp;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  assert_true(dt_remote_blendif_slot_enabled(dst.blendif, 11));               // foreign survives
  assert_true(dt_remote_blendif_slot_enabled(dst.blendif, DEVELOP_BLENDIF_RED_in)); // unlisted survives
  json_object_unref(patch);
  blend_fixture_free(fx);
}

static void test_patch_combine_and_colorspace_layering(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  fx->module->blend_params->blend_cst = DEVELOP_BLEND_CS_RGB_SCENE;
  // combine writes the INV|INCL bits, preserving MASKS_POS
  fx->module->blend_params->mask_combine = DEVELOP_COMBINE_MASKS_POS;
  JsonObject *patch = _patch_from_string("{\"combine\":\"inclusive\"}");
  dt_develop_blend_params_t dst = *fx->module->blend_params;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  assert_int_equal(dst.mask_combine & (DEVELOP_COMBINE_INV | DEVELOP_COMBINE_INCL),
                   DEVELOP_COMBINE_NORM_INCL);
  assert_true((dst.mask_combine & DEVELOP_COMBINE_MASKS_POS) != 0);  // preserved
  json_object_unref(patch);

  // colorspace switch wipes blendif; a same-call parametric member then
  // validates against the NEW family and applies on top
  patch = _patch_from_string(
    "{\"colorspace\":\"DEVELOP_BLEND_CS_RGB_DISPLAY\","
    " \"mask_mode\":\"parametric\","
    " \"parametric\":{\"H_in\":{\"markers\":[0.1,0.2,0.6,0.7]}}}");
  dst = *fx->module->blend_params;
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  assert_int_equal(dst.blend_cst, DEVELOP_BLEND_CS_RGB_DISPLAY);
  assert_true(dt_remote_blendif_slot_enabled(dst.blendif, DEVELOP_BLENDIF_H_in));
  json_object_unref(patch);

  // Availability is checked against the FINAL projected family, not the
  // forced legacy RAW value present before this same-call colorspace switch.
  fx->module->blend_params->blend_cst = DEVELOP_BLEND_CS_RAW;
  fx->module->blend_params->mask_mode = DEVELOP_MASK_DISABLED;
  patch = _patch_from_string(
    "{\"mask_mode\":\"parametric\","
    " \"colorspace\":\"DEVELOP_BLEND_CS_RGB_SCENE\","
    " \"parametric\":{\"Jz_in\":{\"markers\":[0.1,0.2,0.6,0.7]}}}");
  dst = *fx->module->blend_params;
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  assert_int_equal(dst.blend_cst, DEVELOP_BLEND_CS_RGB_SCENE);
  assert_int_equal(dst.mask_mode, DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL);
  assert_true(dt_remote_blendif_slot_enabled(dst.blendif, DEVELOP_BLENDIF_Jz_in));
  json_object_unref(patch);
  blend_fixture_free(fx);
}

static void test_patch_parametric_rejections(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  fx->module->blend_params->blend_cst = DEVELOP_BLEND_CS_RGB_SCENE;
  fx->module->blend_params->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL;
  // Lab channel name against an RGB-scene module
  _assert_patch_fails(fx, "{\"parametric\":{\"L_in\":{\"markers\":[0.1,0.2,0.6,0.7]}}}",
                      DT_REMOTE_ERR_UNSUPPORTED_FIELD, "blend.parametric.L_in");
  // wrong marker count / order / domain
  _assert_patch_fails(fx, "{\"parametric\":{\"Jz_in\":{\"markers\":[0.1,0.2,0.6]}}}",
                      DT_REMOTE_ERR_INVALID_VALUE, "markers");
  _assert_patch_fails(fx, "{\"parametric\":{\"Jz_in\":{\"markers\":[0.5,0.2,0.6,0.7]}}}",
                      DT_REMOTE_ERR_INVALID_VALUE, "markers");
  _assert_patch_fails(fx, "{\"parametric\":{\"Jz_in\":{\"markers\":[0.0,0.0,0.6,1.5]}}}",
                      DT_REMOTE_ERR_INVALID_VALUE, "markers");
  // boost on a hue channel
  _assert_patch_fails(fx, "{\"parametric\":{\"hz_in\":{\"markers\":[0.1,0.2,0.6,0.7],\"boost\":1.0}}}",
                      DT_REMOTE_ERR_INVALID_VALUE, "boost");
  // boost out of range for Jz (offset -6.64.. => max 11.356..)
  _assert_patch_fails(fx, "{\"parametric\":{\"Jz_in\":{\"markers\":[0.1,0.2,0.6,0.7],\"boost\":20.0}}}",
                      DT_REMOTE_ERR_INVALID_VALUE, "boost");
  // combine unknown value
  _assert_patch_fails(fx, "{\"combine\":\"weird\"}",
                      DT_REMOTE_ERR_INVALID_VALUE, "unknown_value");
  blend_fixture_free(fx);
}

static void test_patch_parametric_strict_validation_and_edges(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  fx->module->blend_params->blend_cst = DEVELOP_BLEND_CS_RGB_SCENE;
  fx->module->blend_params->mask_mode =
    DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL;

  _assert_patch_fails(fx, "{\"parametric\":[]}",
                      DT_REMOTE_ERR_INVALID_VALUE, "wrong_type");
  _assert_patch_fails(fx, "{\"parametric\":{\"Jz_in\":1}}",
                      DT_REMOTE_ERR_INVALID_VALUE, "wrong_type");
  _assert_patch_fails(fx, "{\"parametric\":{\"Jz_in\":{}}}",
                      DT_REMOTE_ERR_INVALID_VALUE, "markers");
  _assert_patch_fails(fx, "{\"allow_inverted_combine\":1}",
                      DT_REMOTE_ERR_INVALID_VALUE, "wrong_type");
  _assert_patch_fails(fx,
    "{\"parametric\":{\"Jz_in\":{\"markers\":[0.1,0.2,0.6,0.7],\"invert\":true}}}",
    DT_REMOTE_ERR_UNSUPPORTED_FIELD, "blend.parametric.Jz_in.invert");
  _assert_patch_fails(fx,
    "{\"parametric\":{\"Jz_in\":{\"markers\":[0.1,true,0.6,0.7]}}}",
    DT_REMOTE_ERR_INVALID_VALUE, "markers");
  _assert_patch_fails(fx,
    "{\"parametric\":{\"Jz_in\":{\"markers\":[0.0,0.1,0.6,1e400]}}}",
    DT_REMOTE_ERR_INVALID_VALUE, "markers");
  // These two doubles are outside the domain before float narrowing and
  // must not round to legal 0/1 float endpoints.
  _assert_patch_fails(fx,
    "{\"parametric\":{\"Jz_in\":{\"markers\":[-1e-50,0.1,0.6,0.7]}}}",
    DT_REMOTE_ERR_INVALID_VALUE, "markers");
  _assert_patch_fails(fx,
    "{\"parametric\":{\"Jz_in\":{\"markers\":[0.1,0.2,0.6,1.0000000001]}}}",
    DT_REMOTE_ERR_INVALID_VALUE, "markers");
  _assert_patch_fails(fx,
    "{\"parametric\":{\"Jz_in\":{\"markers\":[0.1,0.2,0.6,0.7],\"inverted\":1}}}",
    DT_REMOTE_ERR_INVALID_VALUE, "wrong_type");
  _assert_patch_fails(fx,
    "{\"parametric\":{\"Jz_in\":{\"markers\":[0.1,0.2,0.6,0.7],\"boost\":1e400}}}",
    DT_REMOTE_ERR_INVALID_VALUE, "boost");
  _assert_patch_fails(fx,
    "{\"parametric\":{\"Jz_in\":{\"markers\":[0.1,0.2,0.6,0.7],\"boost\":\"high\"}}}",
    DT_REMOTE_ERR_INVALID_VALUE, "boost");
  const double min_boost =
    (double)_find_channel(DEVELOP_BLEND_CS_RGB_SCENE, "Jz")->boost_offset;
  const double max_boost = min_boost + 18.0;
  gchar *below = g_strdup_printf(
    "{\"parametric\":{\"Jz_in\":{\"markers\":[0.1,0.2,0.6,0.7],\"boost\":%.17g}}}",
    min_boost - 1e-6);
  gchar *above = g_strdup_printf(
    "{\"parametric\":{\"Jz_in\":{\"markers\":[0.1,0.2,0.6,0.7],\"boost\":%.17g}}}",
    max_boost + 1e-6);
  _assert_patch_fails(fx, below, DT_REMOTE_ERR_INVALID_VALUE, "boost");
  _assert_patch_fails(fx, above, DT_REMOTE_ERR_INVALID_VALUE, "boost");
  g_free(below);
  g_free(above);

  // Degenerate markers and both inclusive boost endpoints are legal.
  const double legal_boosts[] = { min_boost, max_boost };
  for(guint i = 0; i < G_N_ELEMENTS(legal_boosts); i++)
  {
    gchar *json = g_strdup_printf(
      "{\"parametric\":{\"Jz_in\":{\"markers\":[0.3,0.3,0.3,0.3],\"boost\":%.17g}}}",
      legal_boosts[i]);
    JsonObject *patch = _patch_from_string(json);
    g_free(json);
    dt_develop_blend_params_t dst = *fx->module->blend_params;
    dt_remote_error_t *error = NULL;
    assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
    assert_null(error);
    json_object_unref(patch);
  }
  JsonObject *empty = _patch_from_string("{\"parametric\":{}}");
  dt_develop_blend_params_t dst = *fx->module->blend_params;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_blend_patch_apply(fx->module, empty, &dst, &error));
  assert_null(error); // empty object is a legal no-op channel patch
  json_object_unref(empty);
  blend_fixture_free(fx);
}

static void test_patch_parametric_requires_parametric_mask_mode(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  fx->module->blend_params->blend_cst = DEVELOP_BLEND_CS_RGB_SCENE;
  fx->module->blend_params->mask_mode = DEVELOP_MASK_ENABLED;  // uniform, no CONDITIONAL
  _assert_patch_fails(fx, "{\"parametric\":{\"Jz_in\":{\"markers\":[0.1,0.2,0.6,0.7]}}}",
                      DT_REMOTE_ERR_INVALID_VALUE, "requires_parametric_mask_mode");
  blend_fixture_free(fx);
}

static void test_patch_inverted_combine_conflict_and_override(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  dt_develop_blend_params_t *bp = fx->module->blend_params;
  bp->blend_cst = DEVELOP_BLEND_CS_RGB_SCENE;
  bp->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL;
  bp->mask_combine = DEVELOP_COMBINE_NORM_EXCL;
  // changing combine AND setting inverted in one call, no override -> refused
  _assert_patch_fails(fx,
    "{\"combine\":\"inclusive\",\"parametric\":{\"Jz_in\":"
    "{\"markers\":[0.1,0.2,0.6,0.7],\"inverted\":true}}}",
    DT_REMOTE_ERR_INVALID_VALUE, "inverted_and_combine_conflict");
  // Explicit inverted is safe when combine is present but unchanged.
  JsonObject *patch = _patch_from_string(
    "{\"combine\":\"exclusive\",\"parametric\":{\"Jz_in\":"
    "{\"markers\":[0.1,0.2,0.6,0.7],\"inverted\":true}}}");
  dt_develop_blend_params_t dst = *bp;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  assert_null(error);
  json_object_unref(patch);
  // with the override flag it succeeds
  patch = _patch_from_string(
    "{\"allow_inverted_combine\":true,\"combine\":\"inclusive\","
    "\"parametric\":{\"Jz_in\":{\"markers\":[0.1,0.2,0.6,0.7],\"inverted\":true}}}");
  dst = *bp;
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  json_object_unref(patch);
  blend_fixture_free(fx);
}

static void test_patch_mask_mode_parametric_transitions(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  fx->module->blend_params->blend_cst = DEVELOP_BLEND_CS_RGB_SCENE;
  // off -> parametric: legal
  fx->module->blend_params->mask_mode = DEVELOP_MASK_DISABLED;
  JsonObject *patch = _patch_from_string("{\"mask_mode\":\"parametric\"}");
  dt_develop_blend_params_t dst = *fx->module->blend_params;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  assert_int_equal(dst.mask_mode, DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL);
  json_object_unref(patch);

  // drawn -> drawn+parametric adds only CONDITIONAL and preserves MASK.
  fx->module->blend_params->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK;
  patch = _patch_from_string("{\"mask_mode\":\"drawn+parametric\"}");
  dst = *fx->module->blend_params;
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  assert_int_equal(dst.mask_mode, DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK_CONDITIONAL);
  json_object_unref(patch);

  // drawn+parametric -> drawn drops only CONDITIONAL; channel storage survives.
  fx->module->blend_params->mask_mode =
    DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK_CONDITIONAL;
  _set_slot(fx->module->blend_params, DEVELOP_BLENDIF_Jz_in,
            0.1f, 0.2f, 0.6f, 0.7f, FALSE, -6.64385619f);
  const float before = fx->module->blend_params
                         ->blendif_parameters[4 * DEVELOP_BLENDIF_Jz_in + 1];
  patch = _patch_from_string("{\"mask_mode\":\"drawn\"}");
  dst = *fx->module->blend_params;
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  assert_int_equal(dst.mask_mode, DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK);
  assert_float_equal(dst.blendif_parameters[4 * DEVELOP_BLENDIF_Jz_in + 1], before, 1e-6);
  json_object_unref(patch);

  // Crossing MASK ownership remains forbidden in both directions.
  fx->module->blend_params->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK;
  _assert_patch_fails(fx, "{\"mask_mode\":\"parametric\"}",
                      DT_REMOTE_ERR_INVALID_VALUE, "drawn_via_attach_only");
  fx->module->blend_params->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL;
  _assert_patch_fails(fx, "{\"mask_mode\":\"drawn+parametric\"}",
                      DT_REMOTE_ERR_INVALID_VALUE, "drawn_via_attach_only");
  fx->module->blend_params->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_RASTER;
  _assert_patch_fails(fx, "{\"mask_mode\":\"off\"}",
                      DT_REMOTE_ERR_INVALID_VALUE, "raster_unsupported");

  // A conditional bit cannot be newly introduced while the final family
  // remains RAW. A legacy RAW conditional value is nevertheless a legal
  // no-op target and can be removed, matching the state-aware schema.
  fx->module->blend_params->blend_cst = DEVELOP_BLEND_CS_RAW;
  fx->module->blend_params->mask_mode = DEVELOP_MASK_DISABLED;
  _assert_patch_fails(fx, "{\"mask_mode\":\"parametric\"}",
                      DT_REMOTE_ERR_UNSUPPORTED_FIELD, "blend.mask_mode");
  fx->module->blend_params->mask_mode =
    DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL;
  patch = _patch_from_string("{\"mask_mode\":\"parametric\"}");
  dst = *fx->module->blend_params;
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  assert_int_equal(dst.mask_mode, DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL);
  json_object_unref(patch);
  patch = _patch_from_string("{\"mask_mode\":\"uniform\"}");
  dst = *fx->module->blend_params;
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  assert_int_equal(dst.mask_mode, DEVELOP_MASK_ENABLED);
  json_object_unref(patch);
  blend_fixture_free(fx);
}
```

Register all in `main()`.

Delete Tier 1's `test_patch_mask_mode_transition_table` and its `main()`
registration. Its M-A-only `mask_configuration_present` expectations are
obsolete; Task 1's exhaustive pure 25-cell matrix plus
`test_patch_mask_mode_parametric_transitions` above replace it without
duplicating the same matrix twice.

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R test_remote_blend --output-on-failure 2>&1 | tail -20`
Expected: FAIL (combine/parametric stages not implemented; several asserts fail).

- [ ] **Step 3: Extend `dt_remote_blend_patch_apply`**

Add the write-direction combine helper beside Task 3's serializer:

```c
static gboolean _combine_from_string(const char *s,
                                     uint32_t *bits /* INV|INCL only */)
{
  if(!s || !bits) return FALSE;
  if(!strcmp(s, "exclusive"))
    { *bits = DEVELOP_COMBINE_NORM_EXCL; return TRUE; }
  if(!strcmp(s, "inclusive"))
    { *bits = DEVELOP_COMBINE_NORM_INCL; return TRUE; }
  if(!strcmp(s, "exclusive_inverted"))
    { *bits = DEVELOP_COMBINE_INV_EXCL; return TRUE; }
  if(!strcmp(s, "inclusive_inverted"))
    { *bits = DEVELOP_COMBINE_INV_INCL; return TRUE; }
  return FALSE;
}
```

First, extend M-A's `allowed[]` member list with the Tier-2 members:
```c
    "combine", "parametric", "allow_inverted_combine",
```

Expand the public wrapper atomically with patch support; it now exposes the
private full-target parser introduced in Task 1:

```c
gboolean dt_remote_blend_mask_mode_from_string(const char *s, uint32_t *out)
{
  return _mask_mode_target_from_string(s, out);
}
```

Update its `remote_blend.h` comment from "Tier-1 writable vocabulary
only" to: all five non-raster transition targets are accepted; `"raster"`,
unknown strings, NULL input, or NULL output return FALSE without writing
`*out`.

Before the `mask_mode` stage, capture whether a conditional bit was
already a supported state or an unsupported legacy state:

```c
  const gboolean stored_conditional =
    (dst->mask_mode & DEVELOP_MASK_CONDITIONAL) != 0;
  const dt_develop_blend_colorspace_t stored_eff_cs =
    dt_remote_blend_effective_colorspace(module, dst->blend_cst);
  const gboolean stored_parametric_supported =
    dt_remote_blendif_channels(stored_eff_cs) != NULL;
```

Then replace M-A's `mask_mode` stage with the Task-1 transition helper.
This stage projects bits only; family availability is deliberately checked
after the colorspace stage so a same-call RAW -> RGB switch works:

```c
  // 1. mask_mode -- authoritative appendix; only ENABLED|CONDITIONAL
  // may change, and MASK ownership must remain identical.
  if(json_object_has_member(patch, "mask_mode"))
  {
    const char *constraint = NULL;
    uint32_t projected = 0;
    if(!dt_remote_blend_mask_mode_transition(
         dst->mask_mode, _json_member_string(patch, "mask_mode"),
         &projected, &constraint))
    {
      const char *message = !strcmp(constraint, "raster_unsupported")
        ? _("raster masks are not writable")
        : !strcmp(constraint, "drawn_via_attach_only")
          ? _("drawn masks are entered and left through attach/detach")
          : _("unknown mask_mode target");
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE,
                                      "mask_mode", constraint, "%s", message);
      return FALSE;
    }
    dst->mask_mode = projected;
  }
```

Immediately after M-A's `colorspace` stage, derive the final table and
enforce the projected-family invariant:

```c
  const dt_develop_blend_colorspace_t eff =
    dt_remote_blend_effective_colorspace(module, dst->blend_cst);
  const dt_remote_blendif_channel_t *table = dt_remote_blendif_channels(eff);
  if((dst->mask_mode & DEVELOP_MASK_CONDITIONAL) && !table
     && (!stored_conditional || stored_parametric_supported))
  {
    const char *field = stored_conditional ? "colorspace" : "mask_mode";
    if(error) *error = _blend_error(
      DT_REMOTE_ERR_UNSUPPORTED_FIELD, field, NULL,
      _("parametric masks are unavailable in the projected blend colorspace"));
    return FALSE;
  }
```

The last condition permits an already-stored unsupported legacy
`CONDITIONAL` state to remain representable or be removed, but never lets
a patch create such a state or move one from a supported family into an
unsupported family. Then, before M-A's `mode` stage, insert the strict
override, `combine`, and `parametric` stages. This makes the exact order
`mask_mode → colorspace (including projected-family validation) →
allow_inverted_combine → combine → parametric → mode → reverse →
feathering_guide → numeric fields`.

```c
  gboolean combine_changed = FALSE;
  gboolean allow_inv_combine = FALSE;
  if(json_object_has_member(patch, "allow_inverted_combine"))
  {
    JsonNode *node = json_object_get_member(patch, "allow_inverted_combine");
    if(!node || !JSON_NODE_HOLDS_VALUE(node)
       || json_node_get_value_type(node) != G_TYPE_BOOLEAN)
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE,
                                      "allow_inverted_combine", "wrong_type",
                                      _("'allow_inverted_combine' must be a boolean"));
      return FALSE;
    }
    allow_inv_combine = json_node_get_boolean(node);
  }

  // 3b. combine
  if(json_object_has_member(patch, "combine"))
  {
    if(!table)
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_UNSUPPORTED_FIELD, "combine", NULL,
                                      _("combine is not available for this blend colorspace"));
      return FALSE;
    }
    uint32_t bits = 0;
    if(!_combine_from_string(_json_member_string(patch, "combine"), &bits))
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "combine", "unknown_value",
                                      _("unknown combine value"));
      return FALSE;
    }
    const uint32_t before = dst->mask_combine & (DEVELOP_COMBINE_INV | DEVELOP_COMBINE_INCL);
    combine_changed = (before != bits);
    dst->mask_combine =
      (dst->mask_combine & ~(DEVELOP_COMBINE_INV | DEVELOP_COMBINE_INCL)) | bits;  // MASKS_POS preserved
  }

  // 3c. parametric
  if(json_object_has_member(patch, "parametric"))
  {
    if(!table)
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_UNSUPPORTED_FIELD, "parametric", NULL,
                                      _("parametric masks are not available for this blend colorspace"));
      return FALSE;
    }
    if(!(dst->mask_mode & DEVELOP_MASK_CONDITIONAL))
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "parametric",
                                      "requires_parametric_mask_mode",
                                      _("set mask_mode to \"parametric\" to write parametric channels"));
      return FALSE;
    }
    JsonNode *pnode = json_object_get_member(patch, "parametric");
    if(!pnode || !JSON_NODE_HOLDS_OBJECT(pnode))
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "parametric", "wrong_type",
                                      _("'parametric' must be an object"));
      return FALSE;
    }
    JsonObject *pobj = json_node_get_object(pnode);
    GList *names = json_object_get_members(pobj);
    for(GList *n = names; n; n = n->next)
    {
      const char *slot_name = n->data;
      // resolve slot_name -> channel + in/out
      const dt_remote_blendif_channel_t *ch = NULL;
      int slot = -1;
      for(const dt_remote_blendif_channel_t *c = table; c->name && slot < 0; c++)
      {
        gchar *in_name = g_strdup_printf("%s_in", c->name);
        gchar *out_name = g_strdup_printf("%s_out", c->name);
        if(!strcmp(slot_name, in_name))  { ch = c; slot = (int)c->slot_in; }
        else if(!strcmp(slot_name, out_name)) { ch = c; slot = (int)c->slot_out; }
        g_free(in_name); g_free(out_name);
      }
      if(!ch)
      {
        if(error) *error = _blend_error(DT_REMOTE_ERR_UNSUPPORTED_FIELD, NULL, NULL,
                                        _("unknown parametric channel"));
        if(error && *error)
        {
          g_free((*error)->details_json);
          (*error)->details_json = g_strdup_printf(
            "{\"parameter\":\"blend.parametric.%s\"}", slot_name);
        }
        g_list_free(names);
        return FALSE;
      }
      JsonNode *entry = json_object_get_member(pobj, slot_name);
      float *p = &dst->blendif_parameters[4 * slot];
      if(json_node_is_null(entry))
      {
        // null resets: identity markers, polarity cleared, boost -> channel offset
        p[0] = 0.0f; p[1] = 0.0f; p[2] = 1.0f; p[3] = 1.0f;
        dst->blendif = dt_remote_blendif_slot_pack(dst->blendif, slot, FALSE, FALSE);
        dst->blendif_boost_factors[slot] = ch->boost_offset;
        continue;
      }
      if(!JSON_NODE_HOLDS_OBJECT(entry))
      {
        if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "parametric", "wrong_type",
                                        _("a parametric channel entry must be an object or null"));
        g_list_free(names);
        return FALSE;
      }
      JsonObject *e = json_node_get_object(entry);
      static const char *const entry_allowed[] = { "markers", "inverted", "boost", NULL };
      GList *entry_members = json_object_get_members(e);
      for(GList *em = entry_members; em; em = em->next)
      {
        gboolean known = FALSE;
        for(const char *const *a = entry_allowed; *a && !known; a++)
          known = !strcmp(*a, em->data);
        if(!known)
        {
          if(error)
          {
            *error = _blend_error(DT_REMOTE_ERR_UNSUPPORTED_FIELD, NULL, NULL,
                                  _("unknown parametric channel member"));
            g_free((*error)->details_json);
            (*error)->details_json = g_strdup_printf(
              "{\"parameter\":\"blend.parametric.%s.%s\"}",
              slot_name, (const char *)em->data);
          }
          g_list_free(entry_members);
          g_list_free(names);
          return FALSE;
        }
      }
      g_list_free(entry_members);

      // markers: exactly 4 finite ascending in [0,1]
      JsonNode *mnode = json_object_get_member(e, "markers");
      if(!mnode || !JSON_NODE_HOLDS_ARRAY(mnode)
         || json_array_get_length(json_node_get_array(mnode)) != 4)
      {
        if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "parametric", "markers",
                                        _("'markers' must be an array of exactly 4 numbers"));
        g_list_free(names);
        return FALSE;
      }
      JsonArray *marr = json_node_get_array(mnode);
      float m[4];
      double prev = -G_MAXDOUBLE;
      for(int k = 0; k < 4; k++)
      {
        JsonNode *mk = json_array_get_element(marr, k);
        if(!JSON_NODE_HOLDS_VALUE(mk)
           || (json_node_get_value_type(mk) != G_TYPE_DOUBLE
               && json_node_get_value_type(mk) != G_TYPE_INT64))
        {
          if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "parametric", "markers",
                                          _("markers must be numbers"));
          g_list_free(names);
          return FALSE;
        }
        const double value = json_node_get_double(mk);
        if(!isfinite(value) || value < 0.0 || value > 1.0 || value < prev)
        {
          if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "parametric", "markers",
                                          _("markers must be finite, ascending, within [0,1]"));
          g_list_free(names);
          return FALSE;
        }
        prev = value;
        m[k] = (float)value;  // narrow only after double-domain validation
      }
      // inverted (optional bool, default false)
      gboolean inverted = FALSE, has_inverted = FALSE;
      if(json_object_has_member(e, "inverted"))
      {
        JsonNode *inode = json_object_get_member(e, "inverted");
        if(!JSON_NODE_HOLDS_VALUE(inode) || json_node_get_value_type(inode) != G_TYPE_BOOLEAN)
        {
          if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "parametric", "wrong_type",
                                          _("'inverted' must be a boolean"));
          g_list_free(names);
          return FALSE;
        }
        inverted = json_node_get_boolean(inode);
        has_inverted = TRUE;
      }
      // H4 guard: inverted change together with a combine change
      if(has_inverted && combine_changed && !allow_inv_combine)
      {
        if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "parametric",
                                        "inverted_and_combine_conflict",
                                        _("changing 'inverted' and 'combine' together is "
                                          "error-prone; pass allow_inverted_combine:true to confirm"));
        g_list_free(names);
        return FALSE;
      }
      // boost (optional; default = channel offset i.e. GUI zero)
      float boost = ch->boost_offset;
      if(json_object_has_member(e, "boost"))
      {
        if(!ch->boost_supported)
        {
          if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "parametric", "boost",
                                          _("this channel does not support boost"));
          g_list_free(names);
          return FALSE;
        }
        double bv = 0.0;
        JsonNode *bnode = json_object_get_member(e, "boost");
        if(!JSON_NODE_HOLDS_VALUE(bnode)
           || (json_node_get_value_type(bnode) != G_TYPE_DOUBLE
               && json_node_get_value_type(bnode) != G_TYPE_INT64))
        {
          if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "parametric", "boost",
                                          _("'boost' must be a number"));
          g_list_free(names);
          return FALSE;
        }
        bv = json_node_get_double(bnode);
        if(!isfinite(bv) || bv < (double)ch->boost_offset || bv > (double)ch->boost_offset + 18.0)
        {
          if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "parametric", "boost",
                                          _("'boost' out of range for this channel"));
          g_list_free(names);
          return FALSE;
        }
        boost = (float)bv;
      }
      // write verbatim; derive enable from markers (never rescale markers)
      for(int k = 0; k < 4; k++) p[k] = m[k];
      dst->blendif = dt_remote_blendif_slot_pack(dst->blendif, slot,
                                                 dt_remote_blendif_markers_enable(m), inverted);
      dst->blendif_boost_factors[slot] = boost;
    }
    g_list_free(names);
  }
```

Place this block after M-A's `colorspace` stage and before its `mode`
stage. Remove M-A's later `const ... eff` declaration and let the existing
mode-validation code reuse the projected `eff` declared above.

- [ ] **Step 4: Build and run**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R test_remote_blend --output-on-failure`
Expected: PASS. Then full `ctest --test-dir build`.

- [ ] **Step 5: Commit**

```bash
git add src/control/remote_blend.h src/control/remote_blend.c \
        src/tests/unittests/control/test_remote_blend.c
git commit -m "feat: parametric + combine blend patch apply (Tier 2 / M-B)

Validation order mask_mode -> colorspace -> strict override -> combine ->
parametric -> existing Tier-1 stages; derived enable, verbatim markers, null reset,
foreign-slot preservation, family gating, the transition-appendix mask_mode
rows, and the H4 inverted+combine conflict guard with an explicit override."
```
(plus trailers)

---

### Task 5: Mask render engine — persistent pipe opt-in, CPU/OpenCL parity, additive export entry point

**Files:**
- Modify: `src/develop/pixelpipe_hb.h` (`dt_dev_pixelpipe_t`)
- Modify: `src/develop/pixelpipe_hb.c` (one-time initialization only)
- Modify: `src/develop/blend.h`, `src/develop/blend.c` (shared predicate; CPU and OpenCL gates)
- Modify: `src/imageio/imageio_common.h`, `src/imageio/imageio.c` (additive mask-aware export entry point)
- Modify: `src/control/remote_edit.h`, `src/control/remote_edit.c` (request/result fields, ordinary prepare initialization, mask-aware execute/white path)
- Test: `src/tests/unittests/control/test_remote_blend.c`

**Interfaces:**
- Consumes: Tier 1 preview prepare/execute and the existing, unchanged `dt_imageio_export_with_flags()` API.
- Produces:
  - `dt_dev_pixelpipe_t.mask_display_request`.
  - `dt_develop_blend_mask_display_request_is_valid(has_focus, is_full_pipe, pipe_opt_in)` used by both CPU and OpenCL.
  - Additive `dt_imageio_export_with_flags_and_mask(..., history_end, mask_target)`; every pre-Tier-2 caller continues to call `dt_imageio_export_with_flags(...)` unchanged.
  - Mask provenance fields on the preview request/result structs and a
    mask-aware execute path. The existing two-argument prepare signature
    remains intact in this task so the intermediate commit builds; Task 6
    expands it atomically with the protocol calls table and all stubs.

- [ ] **Step 1: Write the failing predicate test**

Add `#include "develop/blend.h"` and `#include "develop/pixelpipe_hb.h"` to
`test_remote_blend.c`, then append and register:

```c
static void test_mask_display_request_gate_truth_table(void **state)
{
  (void)state;
  assert_false(dt_develop_blend_mask_display_request_is_valid(FALSE, FALSE, FALSE));
  assert_false(dt_develop_blend_mask_display_request_is_valid(TRUE, FALSE, FALSE));
  assert_false(dt_develop_blend_mask_display_request_is_valid(FALSE, TRUE, FALSE));
  assert_true(dt_develop_blend_mask_display_request_is_valid(TRUE, TRUE, FALSE));
  assert_true(dt_develop_blend_mask_display_request_is_valid(FALSE, FALSE, TRUE));

  dt_dev_pixelpipe_t pipe = { 0 };
  pipe.mask_display_request = TRUE; // compile guard for the real struct field
  assert_true(pipe.mask_display_request);
}
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build -j$(nproc)`
Expected: compile failure for the missing predicate/pipe member.

- [ ] **Step 3: Add the field, one-time initialization, and shared gate**

In `src/develop/pixelpipe_hb.h`, immediately after `mask_display`:

```c
  /** Remote mask render: honor a module's request_mask_display on this
   * throwaway export pipe without GUI focus/full-pipe ownership. This is
   * request configuration, not per-run output state, so process restart
   * must preserve it. */
  gboolean mask_display_request;
```

In `dt_dev_pixelpipe_init()` at `pixelpipe_hb.c:289`, initialize it beside
`mask_display`:

```c
  pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
  pipe->mask_display_request = FALSE;
```

At the `restart:` block around `pixelpipe_hb.c:3106`, continue clearing
only `pipe->mask_display`. Do **not** clear `mask_display_request`; late
OpenCL fallback must retain the export request.

Declare in `src/develop/blend.h` and implement in `src/develop/blend.c`:

```c
gboolean dt_develop_blend_mask_display_request_is_valid(gboolean has_focus,
                                                        gboolean is_full_pipe,
                                                        gboolean pipe_opt_in);
```

```c
gboolean dt_develop_blend_mask_display_request_is_valid(gboolean has_focus,
                                                        gboolean is_full_pipe,
                                                        gboolean pipe_opt_in)
{
  return (has_focus && is_full_pipe) || pipe_opt_in;
}
```

Replace **both** `valid_request` definitions, the CPU one around
`blend.c:556` and the OpenCL one around `blend.c:957`, with:

```c
  const gboolean valid_request =
    dt_develop_blend_mask_display_request_is_valid(
      dt_iop_has_focus(self), piece->pipe == self->dev->full.pipe,
      piece->pipe->mask_display_request);
```

- [ ] **Step 4: Add a mask-aware export without changing existing callers**

Add to `src/imageio/imageio_common.h`:

```c
typedef struct dt_imageio_mask_display_t
{
  const char *op;
  int instance;
} dt_imageio_mask_display_t;

gboolean dt_imageio_export_with_flags_and_mask(
  const dt_imgid_t imgid, const char *filename,
  struct dt_imageio_module_format_t *format,
  struct dt_imageio_module_data_t *format_params,
  const gboolean ignore_exif, const gboolean display_byteorder,
  const gboolean high_quality, const gboolean upscale,
  const gboolean is_scaling, const double scale_factor,
  const gboolean thumbnail_export, const char *filter,
  const gboolean copy_metadata, const gboolean export_masks,
  dt_colorspaces_color_profile_type_t icc_type, const gchar *icc_filename,
  dt_iop_color_intent_t icc_intent,
  dt_imageio_module_storage_t *storage,
  dt_imageio_module_data_t *storage_params,
  int num, const int total, dt_export_metadata_t *metadata,
  const int history_end,
  const dt_imageio_mask_display_t *mask_target);
```

In `src/imageio/imageio.c`, rename the current implementation of
`dt_imageio_export_with_flags` to
`dt_imageio_export_with_flags_and_mask` and add the trailing target. Add
this wrapper with the original signature so all nine existing call
expressions remain source-compatible:

```c
gboolean dt_imageio_export_with_flags(
  const dt_imgid_t imgid, const char *filename,
  dt_imageio_module_format_t *format,
  dt_imageio_module_data_t *format_params,
  const gboolean ignore_exif, const gboolean display_byteorder,
  const gboolean high_quality, const gboolean upscale,
  const gboolean is_scaling, const double scale_factor,
  const gboolean thumbnail_export, const char *filter,
  const gboolean copy_metadata, const gboolean export_masks,
  const dt_colorspaces_color_profile_type_t icc_type,
  const gchar *icc_filename, const dt_iop_color_intent_t icc_intent,
  dt_imageio_module_storage_t *storage,
  dt_imageio_module_data_t *storage_params,
  int num, const int total, dt_export_metadata_t *metadata,
  const int history_end)
{
  return dt_imageio_export_with_flags_and_mask(
    imgid, filename, format, format_params, ignore_exif,
    display_byteorder, high_quality, upscale, is_scaling, scale_factor,
    thumbnail_export, filter, copy_metadata, export_masks, icc_type,
    icc_filename, icc_intent, storage, storage_params, num, total,
    metadata, history_end, NULL);
}
```

Immediately after `dt_dev_pixelpipe_create_nodes()` and
`dt_dev_pixelpipe_synch_all()` in the mask-aware implementation, add:

```c
  if(mask_target)
  {
    gboolean found = FALSE;
    for(GList *nodes = pipe.nodes; nodes; nodes = g_list_next(nodes))
    {
      dt_dev_pixelpipe_iop_t *piece = nodes->data;
      dt_iop_module_t *module = piece->module;
      if(!g_strcmp0(module->op, mask_target->op)
         && module->multi_priority == mask_target->instance)
      {
        // The export dev/pipe is throwaway. Enabling here permits read-only
        // mask inspection of a module disabled in live history without
        // changing live state or adding history.
        module->enabled = TRUE;
        piece->enabled = TRUE;
        module->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_MASK;
        pipe.mask_display_request = TRUE;
        found = TRUE;
        break;
      }
    }
    if(!found)
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[dt_imageio_export_with_flags_and_mask] target %s instance %d not found",
               mask_target->op, mask_target->instance);
      goto error;
    }
  }
```

No call in `mipmap_cache.c`, tethering, Lua AI, neural restore, control
jobs, or the ordinary imageio wrappers changes.

- [ ] **Step 5: Extend preview prepare/execute**

Add `#include "control/settings.h"` to `remote_edit.h`; this is the owning
header for `dt_dev_operation_t` and `remote_edit.h` did not previously need
it.

Replace the two structs in `src/control/remote_edit.h` with:

```c
typedef struct dt_remote_preview_request_t
{
  int32_t imgid;
  uint64_t revision;
  gboolean want_mask;
  gboolean force_white;
  dt_dev_operation_t mask_op;
  int mask_instance;
  uint32_t mask_mode_stored;
} dt_remote_preview_request_t;

typedef struct dt_remote_preview_t
{
  uint8_t *jpeg;
  size_t jpeg_len;
  int width, height;
  uint64_t revision;
  gboolean is_mask;
  dt_dev_operation_t mask_op;
  int mask_instance;
  uint32_t mask_mode_stored;
} dt_remote_preview_t;
```

Keep the existing
`dt_remote_render_preview_prepare(out, error)` declaration/definition in
this task. At the beginning of prepare, after the darkroom precondition
and before `dt_dev_write_history(dev)`, initialize the new fields for its
ordinary-preview-only caller:

```c
  out->want_mask = FALSE;
  out->force_white = FALSE;
  out->mask_op[0] = '\0';
  out->mask_instance = 0;
  out->mask_mode_stored = 0;
```

Task 6 replaces this initialization with target capture while updating
the protocol function pointer and stubs in the same commit. This avoids an
intermediate function-signature mismatch.

Update the adjacent `remote_edit.h` request/prepare comments to distinguish
ordinary requests (`want_mask == FALSE`) from the Task-6 mask target fields.

In execute, replace the ordinary export call with:

```c
  const dt_imageio_mask_display_t mask_target = {
    .op = req->mask_op, .instance = req->mask_instance
  };
  const gboolean export_failed = dt_imageio_export_with_flags_and_mask(
    (dt_imgid_t)req->imgid, "remote-preview", &format, &sink.head,
    TRUE, FALSE, FALSE, FALSE, FALSE, 1.0, FALSE, NULL, FALSE, FALSE,
    DT_COLORSPACE_SRGB, NULL, DT_INTENT_LAST, NULL, NULL, 1, 1, NULL, -1,
    req->want_mask ? &mask_target : NULL);
```

After the successful export/cancellation checks and before JPEG encode:

```c
  if(req->want_mask && req->force_white)
    memset(sink.buf, 0xff,
           sizeof(uint32_t) * (size_t)sink.width * (size_t)sink.height);
```

When constructing the result:

```c
  preview->is_mask = req->want_mask;
  if(req->want_mask)
  {
    g_strlcpy(preview->mask_op, req->mask_op, sizeof(preview->mask_op));
    preview->mask_instance = req->mask_instance;
    preview->mask_mode_stored = req->mask_mode_stored;
  }
```

- [ ] **Step 6: Build and run the C suite**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure`
Expected: all C tests pass. Task 8 supplies live CPU/pixel assertions;
both CPU and OpenCL now consume the unit-tested predicate.

- [ ] **Step 7: Commit**

```bash
git add src/develop/pixelpipe_hb.h src/develop/pixelpipe_hb.c \
        src/develop/blend.h src/develop/blend.c \
        src/imageio/imageio_common.h src/imageio/imageio.c \
        src/control/remote_edit.h src/control/remote_edit.c \
        src/tests/unittests/control/test_remote_blend.c
git commit -m "feat: add isolated mask-render export path (Tier 2 / M-B)"
```
(plus trailers)

---

### Task 6: Protocol layer — capabilities, strict `show_mask`, `mask_of`, shared fixtures

**Files:**
- Modify: `src/control/remote_edit.h`, `src/control/remote_edit.c` (expanded prepare signature + target capture)
- Modify: `src/control/remote_protocol.h`, `src/control/remote_protocol.c`
- Modify: `src/tests/unittests/control/test_remote_protocol.c`
- Modify: `src/tests/unittests/control/fixtures/hello_response.json`
- Create: `src/tests/unittests/control/fixtures/set_module_params_parametric_request.json`
- Create: `src/tests/unittests/control/fixtures/set_module_params_parametric_response.json`
- Create: `src/tests/unittests/control/fixtures/render_preview_show_mask_request.json`
- Create: `src/tests/unittests/control/fixtures/render_preview_show_mask_response.json`
- Delete: `src/tests/unittests/control/fixtures/set_module_params_error_blend_mask_configuration_request.json`
- Delete: `src/tests/unittests/control/fixtures/set_module_params_error_blend_mask_configuration_response.json`
- Create: `src/tests/unittests/control/fixtures/set_module_params_error_blend_raster_request.json`
- Create: `src/tests/unittests/control/fixtures/set_module_params_error_blend_raster_response.json`
- Modify: `tools/mcp/tests/test_protocol.py`

**Interfaces:**
- Consumes: Task 5's preview mask fields/mask-aware execute path and Tier
  1's borrowed `patch->blend`.
- Produces: the expanded preview prepare signature, two hello capabilities,
  strict `show_mask {op, instance}`, and raw-wire
  `mask_of {op, instance, mask_mode}`.

- [ ] **Step 1: Author the exact shared fixtures**

`set_module_params_parametric_request.json`:

```json
{
  "id": 43,
  "method": "set_module_params",
  "params": {
    "module": "exposure",
    "values": {},
    "blend": {
      "mask_mode": "parametric",
      "combine": "exclusive",
      "parametric": {
        "Jz_in": { "markers": [0.55, 0.65, 1.0, 1.0], "inverted": false }
      }
    }
  }
}
```

`set_module_params_parametric_response.json`:

```json
{
  "id": 43,
  "ok": true,
  "result": {
    "module": "exposure",
    "instance": 0,
    "enabled": true,
    "values": {},
    "blend": {
      "mask_mode": "parametric",
      "colorspace": "DEVELOP_BLEND_CS_NONE",
      "effective_colorspace": "DEVELOP_BLEND_CS_RGB_SCENE",
      "mode": "DEVELOP_BLEND_NORMAL2",
      "reverse": false,
      "fulcrum": 0.0,
      "opacity": 100.0,
      "feathering_radius": 0.0,
      "feathering_guide": "DEVELOP_MASK_GUIDE_IN_AFTER_BLUR",
      "blur_radius": 0.0,
      "contrast": 0.0,
      "brightness": 0.0,
      "details": 0.0,
      "combine": "exclusive",
      "parametric": {
        "Jz_in": {
          "markers": [0.55, 0.65, 1.0, 1.0],
          "inverted": false,
          "boost": -6.64385619
        }
      },
      "foreign_channels": false
    },
    "revision": 13
  }
}
```

`render_preview_show_mask_request.json`:

```json
{
  "id": 44,
  "method": "render_preview",
  "params": {
    "max_px": 512,
    "quality": 90,
    "show_mask": { "op": "exposure", "instance": 1 }
  }
}
```

`render_preview_show_mask_response.json` uses the existing stub JPEG bytes:

```json
{
  "id": 44,
  "ok": true,
  "result": {
    "mime_type": "image/jpeg",
    "width": 512,
    "height": 342,
    "revision": 35,
    "mask_of": { "op": "exposure", "instance": 1, "mask_mode": "parametric" },
    "data": "c3R1Yi1qcGVnLWJ5dGVzLWZvci1yZW5kZXItcHJldmlldy1maXh0dXJl"
  }
}
```

Update `hello_response.json` by inserting
`"parametric_mask_params"` and `"mask_render"` immediately after
`"blend_params"`.

Replace Tier 1's obsolete `mask_configuration_present` error fixture pair
with the still-reachable raster transition contract.
`set_module_params_error_blend_raster_request.json`:

```json
{
  "id": 42,
  "method": "set_module_params",
  "params": {
    "module": "exposure",
    "values": {},
    "blend": { "mask_mode": "uniform" }
  }
}
```

`set_module_params_error_blend_raster_response.json`:

```json
{
  "id": 42,
  "ok": false,
  "error": {
    "code": "invalid_value",
    "message": "raster masks are not writable",
    "details": {
      "parameter": "blend.mask_mode",
      "constraint": "raster_unsupported"
    },
    "retryable": false
  }
}
```

- [ ] **Step 2: Write the failing C tests**

Add `#include "develop/blend.h"` to `test_remote_protocol.c` for the
`DEVELOP_MASK_*` constants used by the new preview fixtures.

Update `test_hello_success` to expect this exact tail and count:

```c
  assert_int_equal(json_array_get_length(caps), 13);
  assert_string_equal(json_array_get_string_element(caps, 6), "blend_params");
  assert_string_equal(json_array_get_string_element(caps, 7), "parametric_mask_params");
  assert_string_equal(json_array_get_string_element(caps, 8), "mask_render");
  assert_string_equal(json_array_get_string_element(caps, 9), "instances");
  assert_string_equal(json_array_get_string_element(caps, 10), "history");
  assert_string_equal(json_array_get_string_element(caps, 11), "preview");
  assert_string_equal(json_array_get_string_element(caps, 12), "scopes");
```

Keep the existing assertions for indices 0–5. Add this exact parametric
passthrough stub and test next to the Tier-1 blend stubs:

```c
static gboolean stub_set_module_params_parametric_capture(
  const dt_remote_module_ref_t *ref, const dt_remote_patch_t *patch,
  const uint64_t *expected_revision, dt_remote_mutation_result_t **out,
  dt_remote_error_t **error)
{
  (void)expected_revision;
  (void)error;
  assert_non_null(patch);
  assert_non_null(patch->blend);
  assert_string_equal(json_object_get_string_member(patch->blend, "mask_mode"),
                      "parametric");
  JsonObject *parametric = json_object_get_object_member(patch->blend, "parametric");
  JsonObject *jz = json_object_get_object_member(parametric, "Jz_in");
  assert_false(json_object_get_boolean_member(jz, "inverted"));
  assert_int_equal(json_array_get_length(json_object_get_array_member(jz, "markers")), 4);

  dt_remote_mutation_result_t *result = g_malloc0(sizeof(*result));
  result->op = g_strdup(ref->op);
  result->instance = ref->instance;
  result->instance_name = g_strdup("");
  result->enabled = TRUE;
  result->values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);
  result->blend_readback = json_from_string(
    "{\"mask_mode\":\"parametric\","
    "\"colorspace\":\"DEVELOP_BLEND_CS_NONE\","
    "\"effective_colorspace\":\"DEVELOP_BLEND_CS_RGB_SCENE\","
    "\"mode\":\"DEVELOP_BLEND_NORMAL2\",\"reverse\":false,"
    "\"fulcrum\":0.0,\"opacity\":100.0,\"feathering_radius\":0.0,"
    "\"feathering_guide\":\"DEVELOP_MASK_GUIDE_IN_AFTER_BLUR\","
    "\"blur_radius\":0.0,\"contrast\":0.0,\"brightness\":0.0,\"details\":0.0,"
    "\"combine\":\"exclusive\",\"parametric\":{\"Jz_in\":{"
    "\"markers\":[0.55,0.65,1.0,1.0],\"inverted\":false,"
    "\"boost\":-6.64385619}},\"foreign_channels\":false}", NULL);
  result->revision = 13;
  *out = result;
  return TRUE;
}

static void test_set_module_params_parametric_fixture_round_trip(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_exposure_full,
    .set_module_params = stub_set_module_params_parametric_capture,
  };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("set_module_params_parametric_request.json",
                           "set_module_params_parametric_response.json");
  dt_remote_protocol_set_calls(NULL);
}
```

Register `test_set_module_params_parametric_fixture_round_trip` in
`main()`.

Change both existing preview-prepare stubs to the new signature. The
successful stub must capture the optional target:

```c
static dt_dev_operation_t g_prepare_mask_op;
static int g_prepare_mask_instance;

static gboolean stub_render_preview_prepare_ok(dt_remote_preview_request_t *out,
                                               const char *show_mask_op,
                                               int show_mask_instance,
                                               dt_remote_error_t **error)
{
  (void)error;
  out->imgid = 172;
  out->revision = 34;
  g_strlcpy(g_prepare_mask_op, show_mask_op ? show_mask_op : "",
            sizeof(g_prepare_mask_op));
  g_prepare_mask_instance = show_mask_instance;
  if(show_mask_op)
  {
    out->want_mask = TRUE;
    g_strlcpy(out->mask_op, show_mask_op, sizeof(out->mask_op));
    out->mask_instance = show_mask_instance;
    out->mask_mode_stored = DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL;
  }
  return TRUE;
}
```

Replace the failure stub with:

```c
static gboolean stub_render_preview_prepare_not_in_darkroom(
  dt_remote_preview_request_t *out, const char *show_mask_op,
  int show_mask_instance, dt_remote_error_t **error)
{
  (void)out;
  (void)show_mask_op;
  (void)show_mask_instance;
  if(error)
    *error = _make_error(DT_REMOTE_ERR_NOT_IN_DARKROOM,
                         g_strdup("no darkroom view is active"));
  return FALSE;
}
```

Add and register:

```c
static void test_render_preview_show_mask_parses_and_defers(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .render_preview_prepare = stub_render_preview_prepare_ok
  };
  dt_remote_protocol_set_calls(&calls);
  _install_fake_async(TRUE);
  JsonNode *request = _load_fixture("render_preview_show_mask_request.json");
  assert_null(dt_remote_protocol_dispatch(json_node_get_object(request), NULL));
  assert_string_equal(g_prepare_mask_op, "exposure");
  assert_int_equal(g_prepare_mask_instance, 1);
  assert_true(g_queued_req.want_mask);
  assert_string_equal(g_queued_req.mask_op, "exposure");
  assert_int_equal(g_queued_req.mask_instance, 1);
  assert_int_equal(g_queued_max_px, 512);
  assert_int_equal(g_queued_quality, 90);
  json_node_unref(request);
  dt_remote_protocol_set_async(NULL);
  dt_remote_protocol_set_calls(NULL);
}
```

Rename `stub_set_module_params_blend_mask_configuration` to
`stub_set_module_params_blend_raster` and replace its error body with:

```c
  if(error)
  {
    *error = _make_error(DT_REMOTE_ERR_INVALID_VALUE,
                         g_strdup("raster masks are not writable"));
    (*error)->details_json =
      g_strdup("{\"parameter\":\"blend.mask_mode\","
               "\"constraint\":\"raster_unsupported\"}");
  }
  return FALSE;
```

Rename `test_set_module_params_blend_error_fixture` to
`test_set_module_params_blend_raster_error_fixture`, update its calls-table
stub, its two fixture names, and its `main()` registration.

Extend `test_render_preview_error_bad_shapes` with these strict rows:

```c
  _assert_inline_error(
    "{\"id\":24,\"method\":\"render_preview\",\"params\":{\"show_mask\":true}}",
    "invalid_value");
  _assert_inline_error(
    "{\"id\":25,\"method\":\"render_preview\",\"params\":{\"show_mask\":{}}}",
    "invalid_value");
  _assert_inline_error(
    "{\"id\":26,\"method\":\"render_preview\",\"params\":{\"show_mask\":{\"op\":\"\"}}}",
    "invalid_value");
  _assert_inline_error(
    "{\"id\":27,\"method\":\"render_preview\",\"params\":{\"show_mask\":{\"op\":\"exposure\",\"instance\":-1}}}",
    "invalid_value");
  _assert_inline_error(
    "{\"id\":28,\"method\":\"render_preview\",\"params\":{\"show_mask\":{\"op\":\"exposure\",\"extra\":1}}}",
    "invalid_value");
  _assert_inline_error(
    "{\"id\":29,\"method\":\"render_preview\",\"params\":{\"show_mask\":{\"op\":1}}}",
    "invalid_value");
  _assert_inline_error(
    "{\"id\":30,\"method\":\"render_preview\",\"params\":{\"show_mask\":{\"op\":\"exposure\",\"instance\":1.5}}}",
    "invalid_value");
  _assert_inline_error(
    "{\"id\":31,\"method\":\"render_preview\",\"params\":{\"show_mask\":{\"op\":\"exposure\",\"instance\":2147483648}}}",
    "invalid_value");

  assert_int_equal(g_async_begin_calls, 12);
  assert_int_equal(g_async_abort_calls, 12);
  assert_int_equal(g_queue_preview_calls, 0);
```

Replace the existing final `4`/`4` counter assertions in that function;
do not leave both counter blocks.

Add and register a pure response test:

```c
static void test_build_preview_mask_response_matches_fixture(void **state)
{
  (void)state;
  dt_remote_preview_t preview = {
    .jpeg = (uint8_t *)RENDER_PREVIEW_STUB_BYTES,
    .jpeg_len = strlen(RENDER_PREVIEW_STUB_BYTES),
    .width = 512,
    .height = 342,
    .revision = 35,
    .is_mask = TRUE,
    .mask_instance = 1,
    .mask_mode_stored = DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL,
  };
  g_strlcpy(preview.mask_op, "exposure", sizeof(preview.mask_op));
  JsonNode *actual = dt_remote_protocol_build_preview_response(44, &preview, NULL);
  JsonNode *expected = _load_fixture("render_preview_show_mask_response.json");
  assert_true(_json_equal(actual, expected));
  json_node_unref(actual);
  json_node_unref(expected);
}
```

- [ ] **Step 3: Run to verify failure**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R test_remote_protocol --output-on-failure`
Expected: failure because capabilities, parsing, and serialization are absent.

- [ ] **Step 4: Implement strict parsing and serialization**

First expand the declaration and definition in `remote_edit.h/.c`:

```c
gboolean dt_remote_render_preview_prepare(dt_remote_preview_request_t *out,
                                          const char *show_mask_op,
                                          int show_mask_instance,
                                          dt_remote_error_t **error);
```

In prepare, replace Task 5's ordinary-only field initialization (after
the darkroom precondition and before `dt_dev_write_history(dev)`) with:

```c
  out->want_mask = FALSE;
  out->force_white = FALSE;
  out->mask_op[0] = '\0';
  out->mask_instance = 0;
  out->mask_mode_stored = 0;
  if(show_mask_op)
  {
    const dt_remote_module_ref_t ref = {
      .op = show_mask_op, .instance = show_mask_instance
    };
    dt_iop_module_t *module = dt_remote_find_module(dev, &ref, error);
    if(!module) return FALSE;
    if(!(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING))
    {
      if(error)
      {
        *error = dt_remote_error_new(DT_REMOTE_ERR_UNSUPPORTED_FIELD,
                                     _("module '%s' does not support blending"),
                                     show_mask_op);
        (*error)->details_json =
          g_strdup("{\"parameter\":\"show_mask.op\"}");
      }
      return FALSE;
    }
    out->want_mask = TRUE;
    g_strlcpy(out->mask_op, module->op, sizeof(out->mask_op));
    out->mask_instance = module->multi_priority;
    out->mask_mode_stored = module->blend_params->mask_mode;
    out->force_white = !(out->mask_mode_stored
      & (DEVELOP_MASK_CONDITIONAL | DEVELOP_MASK_MASK | DEVELOP_MASK_RASTER));
  }
```

In `remote_protocol.h`, change the calls-table member to:

```c
  gboolean (*render_preview_prepare)(dt_remote_preview_request_t *out,
                                     const char *show_mask_op,
                                     int show_mask_instance,
                                     dt_remote_error_t **error);
```

In hello, add the two capabilities after `blend_params` and extend the
adjacent capability-ownership comment to describe both. In
`remote_protocol.c`, define:

```c
static const char *const RENDER_PREVIEW_KEYS[] =
  { "max_px", "quality", "show_mask", NULL };
static const char *const SHOW_MASK_KEYS[] = { "op", "instance", NULL };
```

After quality clamping in `_handler_render_preview`, add:

```c
  const char *show_mask_op = NULL;
  gint64 show_mask_instance = 0;
  if(json_object_has_member(params, "show_mask"))
  {
    JsonNode *node = json_object_get_member(params, "show_mask");
    if(!node || !JSON_NODE_HOLDS_OBJECT(node))
      return _handler_fail(_error_new(DT_REMOTE_ERR_INVALID_VALUE,
                                      _("parameter 'show_mask' must be an object")));
    JsonObject *show_mask = json_node_get_object(node);
    if(!_check_known_keys(show_mask, SHOW_MASK_KEYS, &err)) return _handler_fail(err);
    if(!_require_string(show_mask, "op", &show_mask_op, &err)) return _handler_fail(err);
    if(!show_mask_op[0])
      return _handler_fail(_error_new(DT_REMOTE_ERR_INVALID_VALUE,
                                      _("parameter 'show_mask.op' must not be empty")));
    if(!_optional_int_default(show_mask, "instance", 0,
                              &show_mask_instance, &err))
      return _handler_fail(err);
    if(show_mask_instance < 0 || show_mask_instance > G_MAXINT)
      return _handler_fail(_error_new(DT_REMOTE_ERR_INVALID_VALUE,
                                      _("parameter 'show_mask.instance' is out of range")));
  }
```

Call prepare with the parsed values:

```c
  if(!s_calls.render_preview_prepare(&req, show_mask_op,
                                     (int)show_mask_instance, &err))
    return _handler_fail(err);
```

In `dt_remote_protocol_build_preview_response`, after revision and before
data, add:

```c
    if(preview->is_mask)
    {
      json_builder_set_member_name(b, "mask_of");
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "op");
      json_builder_add_string_value(b, preview->mask_op);
      json_builder_set_member_name(b, "instance");
      json_builder_add_int_value(b, preview->mask_instance);
      json_builder_set_member_name(b, "mask_mode");
      json_builder_add_string_value(
        b, dt_remote_blend_mask_mode_string(preview->mask_mode_stored));
      json_builder_end_object(b);
    }
```

Include `control/remote_blend.h` in `remote_protocol.c`.

- [ ] **Step 5: Extend Python fixture coverage and run both suites**

Add both the parametric set pair and the `render_preview` show-mask pair to
`test_success_responses_match_shared_fixtures`; the latter proves the raw
Python client preserves `mask_of` rather than reshaping the result. Add
these two asserts to `test_client_capabilities_populated_from_hello`:

```python
    assert "parametric_mask_params" in client.capabilities
    assert "mask_render" in client.capabilities
```

Run:

```bash
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
cd tools/mcp && .venv/bin/pytest -q tests/test_protocol.py
```

Expected: all commands pass.

- [ ] **Step 6: Commit**

```bash
git add src/control/remote_edit.h src/control/remote_edit.c \
        src/control/remote_protocol.h src/control/remote_protocol.c \
        src/tests/unittests/control/test_remote_protocol.c \
        src/tests/unittests/control/fixtures/ tools/mcp/tests/test_protocol.py
git commit -m "feat: add parametric-mask and mask-render wire contracts (Tier 2 / M-B)"
```
(plus trailers)

---

### Task 7: Sidecar — complete capability gates and observable mask provenance

**Files:**
- Modify: `tools/mcp/src/darktable_mcp/server.py`
- Test: `tools/mcp/tests/test_tools.py`

**Interfaces:**
- Ordinary `render_preview` remains one native image content block.
- `render_preview(show_mask=...)` returns a JSON text metadata block followed by the native JPEG block.

- [ ] **Step 1: Write the failing tests using the current fake-server API**

Add this helper near `_built_server` in `test_tools.py`:

```python
def _advertise_capabilities(server, capabilities: list[str]) -> None:
    fixture = load_fixture("hello_response.json")
    server.hello_override = lambda params, req_id: {
        **fixture,
        "id": req_id,
        "result": {**fixture["result"], "capabilities": capabilities},
    }
```

Append:

```python
@pytest.mark.parametrize(
    "blend",
    [
        {"parametric": {"Jz_in": {"markers": [0.55, 0.65, 1.0, 1.0]}}},
        {"combine": "inclusive"},
        {"allow_inverted_combine": True},
        {"mask_mode": "parametric"},
        {"mask_mode": "drawn"},
        {"mask_mode": "drawn+parametric"},
    ],
)
async def test_tier2_blend_members_require_parametric_capability(
    tmp_path, fake_server_factory, blend
):
    server = await fake_server_factory()
    _advertise_capabilities(server, ["params", "blend_params"])
    calls: list[dict] = []
    server.handle("set_module_params", lambda params: calls.append(params) or {})
    app = await _built_server(tmp_path, server)
    with pytest.raises(ToolError) as excinfo:
        await app.call_tool(
            "set_module_params",
            {"module": "exposure", "values": {}, "blend": blend},
        )
    assert "parametric_mask_params" in str(excinfo.value)
    assert calls == []


async def test_parametric_blend_passes_when_advertised(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    _advertise_capabilities(
        server, ["params", "blend_params", "parametric_mask_params"]
    )
    seen: list[dict] = []
    server.handle(
        "set_module_params",
        lambda params: seen.append(params)
        or {"module": "exposure", "instance": 0, "enabled": True,
            "values": {}, "revision": 5},
    )
    app = await _built_server(tmp_path, server)
    blend = {
        "mask_mode": "parametric",
        "combine": "exclusive",
        "parametric": {"Jz_in": {"markers": [0.55, 0.65, 1.0, 1.0]}},
    }
    await app.call_tool(
        "set_module_params",
        {"module": "exposure", "values": {}, "blend": blend},
    )
    assert seen[0]["blend"] == blend


async def test_show_mask_requires_mask_render_capability(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    _advertise_capabilities(server, ["params", "preview"])
    calls: list[dict] = []
    server.handle("render_preview", lambda params: calls.append(params) or {})
    app = await _built_server(tmp_path, server)
    with pytest.raises(ToolError) as excinfo:
        await app.call_tool("render_preview", {"show_mask": {"op": "exposure"}})
    assert "mask_render" in str(excinfo.value)
    assert calls == []


async def test_show_mask_returns_metadata_then_native_image(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    _advertise_capabilities(server, ["params", "preview", "mask_render"])
    fixture = load_fixture("render_preview_show_mask_response.json")
    seen: list[dict] = []
    server.handle(
        "render_preview", lambda params: seen.append(params) or fixture["result"]
    )
    app = await _built_server(tmp_path, server)
    result = await app.call_tool(
        "render_preview", {"show_mask": {"op": "exposure", "instance": 1}}
    )
    assert seen[0]["show_mask"] == {"op": "exposure", "instance": 1}
    assert isinstance(result, list)
    assert len(result) == 2
    assert isinstance(result[0], TextContent)
    metadata = json.loads(result[0].text)
    assert metadata["mask_of"] == {
        "op": "exposure", "instance": 1, "mask_mode": "parametric"
    }
    assert metadata["revision"] == 35
    assert "data" not in metadata
    assert isinstance(result[1], ImageContent)
    assert base64.b64decode(result[1].data) == base64.b64decode(
        fixture["result"]["data"]
    )
```

- [ ] **Step 2: Run to verify failure**

Run: `cd tools/mcp && .venv/bin/pytest -q tests/test_tools.py -k 'tier2_blend or parametric_blend or show_mask'`
Expected: failures for incomplete gating, missing argument, or missing metadata content.

- [ ] **Step 3: Implement complete Tier-2 capability detection**

Inside the existing `if blend is not None:` block, after the Tier-1 gate
and before assigning `params["blend"]`, add:

```python
            tier2_mask_mode = blend.get("mask_mode") in {
                "parametric", "drawn", "drawn+parametric"
            }
            needs_parametric = tier2_mask_mode or any(
                key in blend
                for key in ("parametric", "combine", "allow_inverted_combine")
            )
            if needs_parametric and "parametric_mask_params" not in client.capabilities:
                raise TransportError(
                    "this darktable does not advertise parametric_mask_params; "
                    "upgrade darktable to edit parametric masks"
                )
```

This explicitly covers the two transition-only writes (`drawn` and
`drawn+parametric`) and the conflict override, not only `parametric` and
`combine`.

- [ ] **Step 4: Return mask metadata as mixed MCP content**

Replace the render tool with the same ordinary-preview behavior plus the
new optional branch:

```python
    @app.tool(structured_output=False)
    async def render_preview(
        max_px: int = 1024,
        quality: int = 85,
        show_mask: dict[str, Any] | None = None,
    ) -> list[Any]:
        """Render the current darkroom image as a native JPEG content
        block. `max_px` is clamped to [64, 2048] and `quality` to [50, 95].

        `show_mask={"op": <module>, "instance": <n>}` instead renders that
        module's blend mask (white = full effect), with identical framing.
        It requires `mask_render`; off/uniform masks are solid white. Mask
        renders return a JSON metadata block (`mime_type`, dimensions,
        revision, `mask_of`) followed by the native JPEG block.
        """
        max_px = max(64, min(2048, max_px))
        quality = max(50, min(95, quality))
        params: dict[str, Any] = {"max_px": max_px, "quality": quality}
        client = await _client()
        if show_mask is not None:
            await client.ensure_connected()
            if "mask_render" not in client.capabilities:
                raise TransportError(
                    "this darktable does not advertise mask_render; "
                    "upgrade darktable to render masks"
                )
            params["show_mask"] = show_mask

        result = await client.call("render_preview", params)
        image = Image(data=base64.b64decode(result["data"]), format="jpeg")
        if show_mask is None:
            return [image]
        if not isinstance(result.get("mask_of"), dict):
            raise TransportError("darktable returned a mask image without mask_of metadata")
        metadata = {key: value for key, value in result.items() if key != "data"}
        return [json.dumps(metadata), image]
```

The existing `test_render_preview_returns_native_image_content` remains
unchanged and proves the ordinary path is still a one-image result.

- [ ] **Step 5: Expand the `set_module_params` docstring with corrected examples**

Extend `SERVER_INSTRUCTIONS`' preview sentence to say that agents can also
render an individual module's blend mask when `mask_render` is available.

First replace the Tier-1-only `mask_mode` sentence (`"off"/"uniform" --
drawn/parametric/raster configurations are read-only here`) with the final
contract: the schema gives the state-aware choice set;
`off`/`uniform`/`parametric` are writable in non-drawn supported families,
`drawn ↔ drawn+parametric` may toggle only `CONDITIONAL`, entering/leaving
drawn ownership is attach/detach-only, and raster remains read-only.

Then append this text to the blend section:

```text
`blend.parametric` (capability `parametric_mask_params`) maps effective-
colorspace channel names such as `Jz_in` to complete slot replacements:
`{markers:[m0,m1,m2,m3], inverted?, boost?}`. Markers are ascending in
[0,1]; [0,0,1,1] disables the slot, and null resets it. To select bright
sky/highlights deterministically, set
`colorspace:"DEVELOP_BLEND_CS_RGB_SCENE"`, `combine:"exclusive"`,
`mask_mode:"parametric"`, and `Jz_in:{markers:[0.55,0.65,1,1]}` with
omitted/false `inverted`. To select
shadows, use `Jz_in:{markers:[0,0,0.2,0.35]}`. Boost is an exp2 exponent
and does not rescale markers. Inclusive combine XORs effective polarity;
changing combine and explicit inverted together requires
`allow_inverted_combine:true`. Verify thresholds with `show_mask`.
```

- [ ] **Step 6: Run the full Python suite and commit**

Run: `cd tools/mcp && .venv/bin/pytest -q`
Expected: all tests pass.

```bash
git add tools/mcp/src/darktable_mcp/server.py tools/mcp/tests/test_tools.py
git commit -m "feat: expose Tier-2 mask controls through MCP"
```
(plus trailers)

---

### Task 8: Deterministic integration gates with direct mask-image assertions

**Files:**
- Modify: `tools/mcp/pyproject.toml` (`dev` extra)
- Modify: `tools/mcp/tests/integration/test_blend_tier1.py` (superseded schema expectation/wording)
- Create: `tools/mcp/tests/integration/test_parametric_masks_tier2.py`

**Fixture contract:** `harness.test_image()` is exactly
`img/DSC07350.ARW`. At `max_px=512` its landscape preview contains a dark
top-left background ROI and a bright central flamingo-head ROI. The test
forces `DEVELOP_BLEND_CS_RGB_SCENE` before using `Jz_in` and uses
normalized coordinates so the assertion remains valid if the JPEG's
integer height changes by one pixel.

- [ ] **Step 1: Write the failing integration file**

First update Tier 1's cumulative integration contract: change
`blend_schema["mask_mode"]["values"]` from `["off", "uniform"]` to
`["off", "uniform", "parametric"]`. Rename its section-6 heading to
"mask_mode drawn ownership + off/uniform round-trip" and rename
`test_mask_mode_rejections_and_roundtrip` to
`test_mask_mode_drawn_ownership_and_roundtrip`; the test body remains valid
because a plain-state → drawn transition still fails
`drawn_via_attach_only`.

Create the file with this complete content:

```python
"""Tier-2 parametric-mask and read-only mask-render live gates."""

from __future__ import annotations

import base64
import io

import pytest
from PIL import Image, ImageChops, ImageStat

from darktable_mcp.errors import ProtocolError

from . import harness

pytestmark = pytest.mark.integration

MASK_MAX_PX = 512
MASK_QUALITY = 90
HIGHLIGHT_MARKERS = [0.35, 0.50, 1.0, 1.0]

# Normalized boxes (left, top, right, bottom), pinned by visual inspection
# of img/DSC07350.ARW's default 512px render.
DARK_BACKGROUND_ROI = (0.00, 0.00, 0.10, 0.18)
BRIGHT_SUBJECT_ROI = (0.39, 0.34, 0.51, 0.48)


async def _module_params(client, module: str, instance: int = 0) -> dict:
    return await client.call(
        "get_module_params", {"module": module, "instance": instance}
    )


async def _render_mask(client, op: str = "exposure", instance: int = 0):
    await harness.wait_for_stable_revision(client)
    for attempt in (1, 2):
        try:
            result = await client.call(
                "render_preview",
                {
                    "max_px": MASK_MAX_PX,
                    "quality": MASK_QUALITY,
                    "show_mask": {"op": op, "instance": instance},
                },
            )
            with Image.open(io.BytesIO(base64.b64decode(result["data"]))) as encoded:
                mask = encoded.convert("L").copy()
            return result, mask
        except ProtocolError as exc:
            if exc.code != "preview_failed" or attempt == 2:
                raise
    raise AssertionError("unreachable")


def _mean_in_normalized_roi(image: Image.Image, roi: tuple[float, float, float, float]) -> float:
    width, height = image.size
    left, top, right, bottom = roi
    box = (
        int(round(left * width)),
        int(round(top * height)),
        int(round(right * width)),
        int(round(bottom * height)),
    )
    return ImageStat.Stat(image.crop(box)).mean[0]


async def _set_highlight_mask(client, *, enable: bool | None = True) -> dict:
    params = {
        "module": "exposure",
        "instance": 0,
        "values": {},
        "blend": {
            "colorspace": "DEVELOP_BLEND_CS_RGB_SCENE",
            "mask_mode": "parametric",
            "combine": "exclusive",
            "parametric": {
                "Jz_in": {"markers": HIGHLIGHT_MARKERS}
            },
        },
    }
    if enable is not None:
        params["enable"] = enable
    return await client.call("set_module_params", params)


async def _separator_edit(client) -> None:
    params = await _module_params(client, "temperature")
    await client.call(
        "set_module_params",
        {
            "module": "temperature",
            "instance": 0,
            "values": {"red": params["values"]["red"] * 1.01},
        },
    )
    await harness.wait_for_stable_revision(client)


async def test_hello_advertises_parametric_and_mask_render(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        info = client._conn.server_info
        assert info is not None
        assert "parametric_mask_params" in info["capabilities"]
        assert "mask_render" in info["capabilities"]


async def test_schema_carries_parametric_contract(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        await client.call(
            "set_module_params",
            {
                "module": "exposure",
                "instance": 0,
                "values": {},
                "blend": {"colorspace": "DEVELOP_BLEND_CS_RGB_SCENE"},
            },
        )
        schema = await client.call(
            "get_module_schema", {"module": "exposure", "instance": 0}
        )
        blend = schema["blend"]
        assert blend["mask_mode"]["values"] == ["off", "uniform", "parametric"]
        assert blend["mask_mode"]["writable"] is True
        assert blend["combine"]["values"] == [
            "exclusive",
            "inclusive",
            "exclusive_inverted",
            "inclusive_inverted",
        ]
        assert "Jz_in" in blend["parametric"]["channels"]
        assert blend["parametric"]["channels"]["Jz_in"]["display_hint"]["unit"] == "%"
        assert blend["allow_inverted_combine"]["write_only"] is True


async def test_disabled_target_mask_is_dark_low_bright_high(darktable_session):
    """The export temporarily enables a disabled target in its throwaway pipe."""
    async with harness.connected_client(darktable_session) as client:
        result = await _set_highlight_mask(client, enable=False)
        assert result["enabled"] is False
        response, mask = await _render_mask(client)
        assert response["mask_of"] == {
            "op": "exposure", "instance": 0, "mask_mode": "parametric"
        }
        assert mask.size[0] == MASK_MAX_PX
        dark = _mean_in_normalized_roi(mask, DARK_BACKGROUND_ROI)
        bright = _mean_in_normalized_roi(mask, BRIGHT_SUBJECT_ROI)
        assert bright > 100.0
        assert bright / max(dark, 1.0) > 3.0
        await client.call(
            "set_module_enabled",
            {"module": "exposure", "instance": 0, "enabled": True},
        )


async def test_combine_flip_changes_mask(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        await _set_highlight_mask(client)
        _, exclusive = await _render_mask(client)
        await client.call(
            "set_module_params",
            {
                "module": "exposure",
                "instance": 0,
                "values": {},
                "blend": {"combine": "inclusive"},
            },
        )
        _, inclusive = await _render_mask(client)
        assert ImageStat.Stat(ImageChops.difference(exclusive, inclusive)).mean[0] > 50.0


async def test_mask_mode_off_renders_full_white(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        await client.call(
            "set_module_params",
            {
                "module": "exposure",
                "instance": 0,
                "values": {},
                "blend": {"mask_mode": "off"},
            },
        )
        response, mask = await _render_mask(client)
        assert response["mask_of"]["mask_mode"] == "off"
        minimum, maximum = mask.getextrema()
        assert minimum > 250
        assert maximum <= 255


async def test_parametric_null_disables_channel(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        await _set_highlight_mask(client)
        before = await _module_params(client, "exposure")
        assert "Jz_in" in before["blend"]["parametric"]
        await client.call(
            "set_module_params",
            {
                "module": "exposure",
                "instance": 0,
                "values": {},
                "blend": {"parametric": {"Jz_in": None}},
            },
        )
        after = await _module_params(client, "exposure")
        assert "Jz_in" not in after["blend"]["parametric"]
        _, mask = await _render_mask(client)
        assert mask.getextrema()[0] > 250


async def test_show_mask_rejects_non_blending_module(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        with pytest.raises(ProtocolError) as excinfo:
            await client.call(
                "render_preview",
                {
                    "max_px": MASK_MAX_PX,
                    "quality": MASK_QUALITY,
                    "show_mask": {"op": "rawprepare", "instance": 0},
                },
            )
        assert excinfo.value.code == "unsupported_field"
        assert excinfo.value.details == {"parameter": "show_mask.op"}


async def test_parametric_write_is_one_history_item_and_undoable(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        await client.call(
            "set_module_params",
            {
                "module": "exposure",
                "instance": 0,
                "values": {},
                "blend": {"mask_mode": "uniform"},
            },
        )
        await _separator_edit(client)
        before = await client.call("get_history", {"limit": 100})
        result = await _set_highlight_mask(client)
        await harness.wait_for_stable_revision(client)
        after = await client.call("get_history", {"limit": 100})
        assert len(after["items"]) == len(before["items"]) + 1

        await client.call("undo", {"expected_revision": result["revision"]})
        restored = await _module_params(client, "exposure")
        assert restored["blend"]["mask_mode"] == "uniform"
```

- [ ] **Step 2: Run to verify failure before adding the dependency**

Run:
`DARKTABLE_BIN=$PWD/build/bin/darktable tools/mcp/.venv/bin/pytest -q -m integration tools/mcp/tests/integration/test_parametric_masks_tier2.py`

Expected: collection fails with `ModuleNotFoundError: No module named 'PIL'`
in the completed Tier-1 environment.

- [ ] **Step 3: Declare and install the test dependency**

Add to `[project.optional-dependencies].dev` in `tools/mcp/pyproject.toml`:

```toml
  "Pillow>=10.0,<13",
```

Run: `cd tools/mcp && .venv/bin/pip install -e '.[dev]'`
Expected: installation succeeds and `.venv/bin/python -c 'from PIL import Image'`
exits zero.

- [ ] **Step 4: Run the new gate and the complete integration suite**

Run:

```bash
DARKTABLE_BIN=$PWD/build/bin/darktable tools/mcp/.venv/bin/pytest -q -m integration tools/mcp/tests/integration/test_parametric_masks_tier2.py
DARKTABLE_BIN=$PWD/build/bin/darktable tools/mcp/.venv/bin/pytest -q -m integration tools/mcp
```

Expected: the new file's eight tests and all prior integration tests pass;
environmental skips remain skips rather than being assigned a fixed count.

- [ ] **Step 5: Commit**

```bash
git add tools/mcp/pyproject.toml \
        tools/mcp/tests/integration/test_blend_tier1.py \
        tools/mcp/tests/integration/test_parametric_masks_tier2.py
git commit -m "test: add deterministic Tier-2 mask-render integration gates"
```
(plus trailers)

---

### Task 9: Documentation + divergence-manifest rows + design amendments

**Files:**
- Modify: `docs/superpowers/specs/2026-07-05-darktable-mcp-protocol-reference.md`
- Modify: `docs/superpowers/specs/2026-07-19-darktable-mcp-parametric-masks-design.md`
- Modify: `docs/superpowers/specs/2026-07-19-darktable-upstream-divergence-manifest.md`
- Modify: `docs/superpowers/specs/2026-07-16-darktable-mcp-supported-operations.md`
- Modify: `docs/superpowers/plans/2026-07-19-darktable-mcp-drawn-masks-tier3.md` (ownership wording only)
- Modify: `tools/mcp/README.md`
- Modify: `.superpowers/sdd/progress.md` (ledger)

- [ ] **Step 1: Protocol reference** — add a "Parametric masks (`parametric_mask_params`)" subsection under the Tier-1 blend section: the state-aware `mask_mode` vocabulary and full transition matrix; the projected-family/legacy-conditional removal rule; RAW/NONE's lack of parametric channel semantics (without claiming the RAW mask kernel is absent); derived-enable rule; polarity XOR rule with the `blendop.cl:204` citation and `effective_inverted = inverted XOR inclusive`; open ends; boost/marker independence; foreign-slot policy; defensive `null` serialization for non-finite legacy marker/boost storage; `combine` mapping; strict nested-member validation; the conflict override; and the Task-4 error table. Add "Mask render (`mask_render`)": strict `show_mask {op, instance}`, disabled-target inspection in a throwaway pipe, white=full effect, `mask_of`, off/uniform → white, and the non-blending error. Document that the MCP tool returns mask metadata as a text block immediately before the image block.

- [ ] **Step 2: Design-doc amendments** — set status to "implemented (see plan 2026-07-19-darktable-mcp-parametric-masks-tier2.md)" and reconcile the body, not only an appendix: replace the stale "compound read-only" language with legal `drawn ↔ drawn+parametric`; correct "no RAW blendif kernel" to the actual source contract (RAW CPU/OpenCL mask paths exist but ignore parametric channels, and the GUI marks RAW blendif unsupported); replace live-flag save/restore with the throwaway export-pipe design; state that disabled targets are enabled only in that pipe; correct the sky example to omit/false `inverted` with `combine:"exclusive"`; replace indirect preview-delta testing with direct Pillow mask assertions; and record all seven Design amendments from this plan.

- [ ] **Step 3: Divergence manifest** — update the two existing M-A rows
for `blend.h`/`blend.c` and `remote_blend.c` to the final contents below,
then append the two genuinely new rows. Preserve the preamble and every
other current row; do not create duplicate rows for the same upstream file.

| upstream file | divergence | guard |
|---|---|---|
| `src/develop/blend.h` / `blend.c` | existing shared blend-mode section table; plus shared mask-display predicate used by both CPU and OpenCL gates, where export-pipe opt-in joins the unchanged focus/full-pipe condition | existing frozen mode-list parity + `test_mask_display_request_gate_truth_table` + direct mask integration gate |
| `src/develop/pixelpipe_hb.h` / `pixelpipe_hb.c` | new `dt_dev_pixelpipe_t.mask_display_request`, initialized FALSE once and intentionally preserved across process/OpenCL restart | `test_mask_display_request_gate_truth_table` + direct mask integration gate |
| `src/imageio/imageio_common.h` / `imageio.c` | additive `dt_imageio_export_with_flags_and_mask`; existing export signature/callers unchanged; target piece enabled only in throwaway export state | disabled-target direct mask integration gate |
| `src/control/remote_blend.c` | existing hand-written table over `dt_develop_blend_params_t`; plus engine blendif channel tables mirroring GUI `Lab_channels[]`/`rgb_channels[]`/`rgbj_channels[]` | existing `DEVELOP_BLEND_VERSION`/`offsetof` guards + GUI-parity test `test_channels_parity_*` |

Add a "rehearsed procedure: upstream changes a blendif channel/boost offset" paragraph: update the engine channel table row, rerun `test_channels_parity_*`; the parity test turns silent drift into a build/test failure.

- [ ] **Step 4: Supported operations and downstream ownership** — extend the M-A blend sentence: "Parametric (blendif) masks and read-only mask rendering are supported via `parametric_mask_params` and `mask_render`; creating/attaching drawn geometry remains out of scope (M-C)." In the Tier-3 plan's Global Constraints, change "`drawn ↔ drawn+parametric` transitions are handled by M-A" to "handled by M-B"; change no other Tier-3 task.

- [ ] **Step 5: README** — add a "Parametric masks" subsection with the corrected deterministic highlight/sky example (explicit `DEVELOP_BLEND_CS_RGB_SCENE`, `combine:"exclusive"`, no inversion), the boost and combine warnings, `drawn ↔ drawn+parametric` ownership, and a "Seeing the mask" example explaining the metadata text block followed by the JPEG block.

- [ ] **Step 6: Full verification stack**

Run all four:
```bash
cmake --build build -j$(nproc)
ctest --test-dir build
cd tools/mcp && .venv/bin/pytest -q && cd ../..
DARKTABLE_BIN=$PWD/build/bin/darktable tools/mcp/.venv/bin/pytest -q -m integration tools/mcp
```
Expected: each command exits zero with no failures. Integration tests may
retain an explicitly reported skip only for an unrelated gate already
encoded by the harness (for example, the shutdown-only private
D-Bus/`gdbus` gate); do not encode a fixed suite-wide skip count. The eight
tests in `test_parametric_masks_tier2.py` must have actually run and passed
with zero skips before recording Tier 2 complete. A missing GUI build or
display that skips that file is a completion blocker, not verification.

- [ ] **Step 7: Ledger + commit**

Append this exact line to `.superpowers/sdd/progress.md`:

```text
Mask Tier 2 / M-B: complete (Tasks 1-9; full C, Python unit, and integration stacks verified)
```

Then:
```bash
git add docs/ tools/mcp/README.md .superpowers/sdd/progress.md
git commit -m "docs: parametric masks + mask render (Tier 2 / M-B) -- reference, manifest, amendments"
```
(plus trailers)

---

## Self-review notes (performed while writing)

- **Spec coverage:** Tasks 1–4 cover every supported-family slot, derived enable, stored polarity, boost defaults/range edges, strict marker validation, null reset, foreign preservation, defensive non-finite reads, all authoritative `mask_mode` cells, and the combine conflict override. Tasks 5–8 cover persistent restart-safe mask request state, shared CPU/OpenCL gating, disabled targets, strict `show_mask`, raw-wire and MCP-visible `mask_of`, off/uniform white, direct pixel ratios, history, and undo. Task 9 covers reference/design/downstream-plan synchronization and the divergence manifest.
- **Mask-render lifecycle:** `mask_display_request` is initialized once, deliberately survives the process restart label, and is consumed by both CPU and OpenCL through one tested predicate. The additive export function leaves all existing export callers untouched and refuses a missing target.
- **Transition ownership:** the pure matrix test covers all 25 non-raster cells plus raster and unknown-target failures. Schema vocabulary is drawn-state-aware and stays writable for `CONDITIONAL` toggles while `MASK` is present in supported families, or when removing a legacy unsupported `CONDITIONAL` state.
- **Non-contradiction clarified (amendment 2):** the design's two "offsets" (Lab a/b marker `0.5` vs boost storage `−6.64385619`) are distinct; both are in the GUI tables verbatim (`boost_factor_offset` and the a/b branch of `_blendop_blendif_boost_factor_callback`).
- **Current-tree binding:** the plan uses `pixelpipe_hb.h`, preserves the public `dt_imageio_export_with_flags` signature, uses the existing fake server's `hello_override`, expects FastMCP `ToolError`, and declares Pillow explicitly. No task depends on an API absent from the completed Tier-1 tree.
- **RAW source audit:** RAW has real CPU/OpenCL mask paths, but both omit parametric-channel evaluation and the GUI explicitly rejects RAW blendif. The plan now states that precise reason instead of the false shorthand "no RAW kernel."
- **Intermediate buildability:** Task 1 keeps the public M-A parser projected to `off`/`uniform` until Task 4 changes parser and patch semantics together. Task 5 keeps the old preview-prepare signature until Task 6 updates `remote_edit`, the protocol calls table, handler, and stubs together.
- **Cumulative-suite hygiene:** superseded Tier-1 parser/schema/transition assertions, the obsolete `mask_configuration_present` fixture, and the Tier-1 integration schema expectation are explicitly replaced rather than left as downstream surprises.
- **Type consistency:** `dt_remote_blend_mask_mode_transition`, channel helper names, preview mask fields, the expanded prepare signature, the mask-aware export signature, and `mask_of` names match across every producer and consumer.
