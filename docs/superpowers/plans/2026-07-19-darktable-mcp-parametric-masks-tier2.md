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
  - RAW blending (`DEVELOP_BLEND_CS_RAW`) and `DEVELOP_BLEND_CS_NONE`: **no** parametric support (no RAW blendif kernel) — schema omits `parametric`/`combine`; a `parametric` patch errors `unsupported_field`.
- **Marker 4-tuple ordering (verbatim):** each slot stores `[m0, m1, m2, m3]` in a 0–1 stored domain, ascending `m0 ≤ m1 ≤ m2 ≤ m3`. The mask factor ramps 0→1 over `m0..m1`, is 1 over `m1..m2`, falls 1→0 over `m2..m3` (`dt_develop_blendif_process_parameters`, `blend.c`). Open ends: `m0,m1 ≤ 0` unbounded low; `m2,m3 ≥ 1` unbounded high. Degenerate equal markers are legal (hard edge). Domain is `[0.0, 1.0]` inclusive.
- **Derived-enable rule (verbatim, single source of truth):** a slot is **disabled** iff its markers are the full-span identity `m1 == 0.0 && m2 == 1.0` (i.e. identity markers `[0,0,1,1]`); enabled otherwise. Exactly the GUI rule at `blend_gui.c:_blendop_blendif_sliders_callback` (`if(parameters[1]==0.0f && parameters[2]==1.0f) bp->blendif &= ~(1<<ch); else bp->blendif |= (1<<ch);`). Enable is never sent on the wire; it is derived on every patch and only enabled slots appear in reads.
- **Polarity / `inverted` (verbatim rule, never resolved server-side):** `inverted` is the stored polarity bit (`blendif` bits 16–31). The pixel kernels compute effective inversion as `invert_mask = (blendif >> 16) XOR (mask_combine & DEVELOP_COMBINE_INCL ? family_mask : 0)` and use `1 − factor` for inverted channels (`data/kernels/blendop.cl:204`; C twins in `src/develop/blends/`). Documented rule for clients: `effective_inverted = inverted XOR (combine is inclusive)`. The wire always carries the stored bit.
- **Bit 31 (`DEVELOP_BLENDIF_active`) is a legacy flag** — never set by current code; every pack helper masks it off.
- **Boost factors (verbatim):** per **slot** (`blendif_boost_factors[16]`), a log2 exponent applied as `(stored − offset_ab) × 2^boost` where `offset_ab = 0.5` for Lab `a`/`b` slots and `0` otherwise. The **boost value** itself has a per-channel storage offset (the GUI's "zero"): `−6.64385619` for `Jz`/`Cz` (`_blend_init_blendif_boost_parameters`, `blend.c`), `0` for all other channels. Stored boost range = displayed `0..18` EV shifted by the channel offset: `[0.0, 18.0]` for offset-0 channels, `[-6.64385619, 11.35614381]` for `Jz`/`Cz`. **Boost is exp2 exponents and never rescales markers on the wire** — the GUI's threshold-preserving marker rescale (`_blendop_blendif_boost_factor_callback`) is deliberately NOT reproduced; each member is written verbatim. `offset_ab = 0.5` and the two boost offsets are distinct concepts (see Design amendment 2).
- **`mask_combine` mapping (verbatim):** wire enum ↔ storage, low bits only (`DEVELOP_COMBINE_MASKS_POS` = `0x04` is a Tier-3 drawn bit, preserved untouched): `exclusive` = `DEVELOP_COMBINE_NORM_EXCL` (`0x00`), `inclusive` = `DEVELOP_COMBINE_NORM_INCL` (`0x02`), `exclusive_inverted` = `DEVELOP_COMBINE_INV_EXCL` (`0x01`), `inclusive_inverted` = `DEVELOP_COMBINE_INV_INCL` (`0x03`).
- **`mask_mode` writable vocabulary gains `"parametric"`** ↔ `DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL`, per the transition appendix. The read strings (`"parametric"`, `"drawn"`, `"drawn+parametric"`, `"raster"`) are M-A's `dt_remote_blend_mask_mode_string`, unchanged. Transitions follow the appendix table (authoritative), NOT the design's stale error row — `parametric` is reachable from `off`/`uniform`/`parametric` and refused (`drawn_via_attach_only`) from drawn/raster rows.
- **Validation order inside `blend`:** `mask_mode` → `colorspace` → `combine` → `parametric` (per channel: name → markers → inverted → boost) → Tier-1 numerics. Blend keeps its Tier-1 position at the end of the class error precedence.
- **Foreign slots** (enabled in storage but outside the effective family) are invisible in reads (surfaced only as `"foreign_channels": true`) and never touched by patches.
- **Struct tripwire:** M-A's `#if DEVELOP_BLEND_VERSION != 14 #error` and `offsetof` asserts stay; Tier 2 adds relative `offsetof`/size asserts for `blendif_parameters` and `blendif_boost_factors`.
- **Error vocabulary:** only existing codes (`unsupported_field` = `DT_REMOTE_ERR_UNSUPPORTED_FIELD`, `invalid_value` = `DT_REMOTE_ERR_INVALID_VALUE`), `details_json` = `{"parameter": "blend.<path>", "constraint": "<slug>"}`.
- **Verification stack:** `cmake --build build -j$(nproc)` → `ctest --test-dir build` → `cd tools/mcp && .venv/bin/pytest -q` → (final task only) `DARKTABLE_BIN=$PWD/build/bin/darktable tools/mcp/.venv/bin/pytest -q -m integration tools/mcp`.
- Every commit message ends with BOTH trailers:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_017UPe8bYYZLQ8aasRge4kBP`

## Design amendments (binding; applied to the design doc in Task 9)

1. **The mask render extends the export render path with a guarded per-pipe display-mask opt-in — it cannot ride the live flag.** The design left open "whether the export-path pipe honors `request_mask_display`". **Verified answer: it does not.** `dt_develop_blend_process` (`src/develop/blend.c:555`) gates the flag on `valid_request = dt_iop_has_focus(self) && (piece->pipe == self->dev->full.pipe)`. The export pipe used by `dt_remote_render_preview_execute` is `DT_DEV_PIXELPIPE_EXPORT`, never `full.pipe`, and export modules never hold GUI focus, so `request_mask_display` is unconditionally ignored on that path. **Resolution (chosen for identical framing / 1:1 coordinates, background threading, and zero live-GUI disturbance — the design's stated intent):** add a `gboolean mask_display_request` field to `dt_dev_pixelpipe_t`; relax the `blend.c:555` gate to also honor `self->request_mask_display` when `piece->pipe->mask_display_request` is set; and have the mask render set the target module's `request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_MASK` plus the pipe opt-in inside the export. Because the export builds its own throwaway `dt_develop_t`, there is no live flag to save/restore (cleaner than the design's premise). This is the single upstream divergence in a hot path, guarded by a unit test and a divergence-manifest row; its end-to-end correctness is verified by the integration mask-image gate (H6).
2. **`boost.offset` in the schema is the boost-value storage offset, distinct from the Lab a/b marker offset `offset_ab = 0.5`.** The engine channel table's `boost_offset` field mirrors the GUI table's `boost_factor_offset` (`−6.64385619` for `Jz`/`Cz`, `0` elsewhere — verified in `rgbj_channels[]` and `_blend_init_blendif_boost_parameters`). The `0.5` for Lab `a`/`b` is the separate `offset_ab` used only in the non-normative `display_hint` formula and in the GUI's boost-rescale (which the wire deliberately does not reproduce). No source contradiction; recorded so an implementer never conflates the two.
3. **The H4 `inverted`+`combine` conflict guard is server-side with an explicit override member.** A patch that both changes `combine` (to a value differing from stored) and sets any explicit `inverted: <bool>` inside `parametric` is refused `invalid_value`, `constraint: "inverted_and_combine_conflict"`, unless the `blend` patch also carries `"allow_inverted_combine": true`. Sidecar passes the member through; the engine is authoritative.
4. **`off`/`uniform` (no spatial-mask bit) mask render returns solid white at normal framing.** For a target whose stored `mask_mode` lacks both `DEVELOP_MASK_CONDITIONAL` and `DEVELOP_MASK_MASK`, `blend.c:558` emits no mask, so the export would return the normal image. The engine instead renders normally (for correct framing) then fills the buffer white before JPEG-encoding. `mask_of.mask_mode` reports the true stored mode string.
5. **HSL "value" channel wire name is `l` (lowercase).** `rgb_channels[]` labels the HSL value channel `N_("L")` but its slot is `DEVELOP_BLENDIF_l_in`/`_out`; the wire name is `l` to disambiguate from Lab `L`, matching the candidates sketch's `g R G B H S l`.

## File structure

| File | Responsibility |
|---|---|
| `src/control/remote_blend.h` / `.c` | blendif channel table + pure helpers (Task 1); `combine`+`parametric` in schema (Task 2), read (Task 3), patch-apply (Task 4); `mask_mode` `"parametric"` in `_from_string` (Task 1) |
| `src/tests/unittests/control/test_remote_blend.c` | parity + pack/unpack + validation + schema/read/patch tests (Tasks 1–4) |
| `src/develop/pixelpipe.h` (`dt_dev_pixelpipe_t`) / `pixelpipe_hb.c` | `mask_display_request` field + init/reset (Task 5) |
| `src/develop/blend.c` | relaxed `valid_request` gate (Task 5) |
| `src/imageio/imageio_common.h` / `imageio.c`, `src/common/mipmap_cache.c` | optional mask-display target on `dt_imageio_export_with_flags` (Task 5) |
| `src/control/remote_edit.h` / `.c` | preview request mask fields; prepare/execute mask path; white synth; `mask_of` (Task 5) |
| `src/control/remote_protocol.h` / `.c` | `parametric_mask_params`+`mask_render` capabilities; `show_mask` parse/thread; `mask_of` serialize (Task 6) |
| `src/tests/unittests/control/test_remote_protocol.c` + `fixtures/*.json` | protocol tests + shared fixtures (Task 6) |
| `tools/mcp/src/darktable_mcp/server.py` + `tools/mcp/tests/test_tools.py` | capability gates + `show_mask` passthrough + docstrings/worked examples (Task 7) |
| `tools/mcp/tests/integration/test_parametric_masks_tier2.py` (NEW) | live gates with `show_mask` image assertions (Task 8) |
| docs (protocol reference, design status, divergence manifest, README) | Task 9 |

---

### Task 1: Engine blendif channel table + pure bitfield/marker helpers + `mask_mode` parametric

**Files:**
- Modify: `src/control/remote_blend.h` (append declarations)
- Modify: `src/control/remote_blend.c` (append table, helpers; extend `dt_remote_blend_mask_mode_from_string`)
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
  - `dt_remote_blend_mask_mode_from_string` now also accepts `"parametric"` → `DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL`.

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
}

static void test_slot_enabled_inverted_pack(void **state)
{
  (void)state;
  // enable bits 0..15, polarity bits 16..31
  uint32_t bf = 0;
  assert_false(dt_remote_blendif_slot_enabled(bf, 3));
  assert_false(dt_remote_blendif_slot_inverted(bf, 3));

  bf = dt_remote_blendif_slot_pack(bf, 3, TRUE, TRUE);
  assert_true(dt_remote_blendif_slot_enabled(bf, 3));
  assert_true(dt_remote_blendif_slot_inverted(bf, 3));
  assert_int_equal(bf, (1u << 3) | (1u << (16 + 3)));

  bf = dt_remote_blendif_slot_pack(bf, 3, FALSE, FALSE);
  assert_false(dt_remote_blendif_slot_enabled(bf, 3));
  assert_false(dt_remote_blendif_slot_inverted(bf, 3));
  assert_int_equal(bf, 0u);

  // the legacy DEVELOP_BLENDIF_active bit (31) is always stripped by pack
  bf = (1u << 31);
  bf = dt_remote_blendif_slot_pack(bf, 0, TRUE, FALSE);
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

static void test_mask_mode_from_string_parametric(void **state)
{
  (void)state;
  uint32_t v = 0;
  assert_true(dt_remote_blend_mask_mode_from_string("parametric", &v));
  assert_int_equal(v, DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL);
  // drawn/raster still refused
  assert_false(dt_remote_blend_mask_mode_from_string("drawn", &v));
  assert_false(dt_remote_blend_mask_mode_from_string("raster", &v));
}
```

Register all eight tests in `main()`'s array.

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

/** bitfield <-> per-slot views. `slot` is 0..15. `slot_pack` sets/clears
 * the enable bit (slot) and polarity bit (16+slot) and always strips the
 * legacy DEVELOP_BLENDIF_active bit (31). */
gboolean dt_remote_blendif_slot_enabled(uint32_t blendif, int slot);
gboolean dt_remote_blendif_slot_inverted(uint32_t blendif, int slot);
uint32_t dt_remote_blendif_slot_pack(uint32_t blendif, int slot,
                                     gboolean enabled, gboolean inverted);

/** the derived-enable rule (single source of truth): a slot is DISABLED
 * iff its markers are the full-span identity (m[1]==0 && m[2]==1). */
gboolean dt_remote_blendif_markers_enable(const float m[4]);
```

- [ ] **Step 4: Implement the table + helpers in `remote_blend.c`**

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
  { "L", DEVELOP_BLENDIF_L_in, DEVELOP_BLENDIF_L_out, TRUE,  0.0f, 0.0f, 100.0f, "" },
  { "a", DEVELOP_BLENDIF_A_in, DEVELOP_BLENDIF_A_out, TRUE,  0.0f, 0.5f, 256.0f, "" },
  { "b", DEVELOP_BLENDIF_B_in, DEVELOP_BLENDIF_B_out, TRUE,  0.0f, 0.5f, 256.0f, "" },
  { "C", DEVELOP_BLENDIF_C_in, DEVELOP_BLENDIF_C_out, TRUE,  0.0f, 0.0f, 100.0f, "" },
  { "h", DEVELOP_BLENDIF_h_in, DEVELOP_BLENDIF_h_out, FALSE, 0.0f, 0.0f, 360.0f, "\xc2\xb0" },
  { NULL, 0, 0, FALSE, 0.0f, 0.0f, 0.0f, NULL } };

static const dt_remote_blendif_channel_t _channels_rgb_display[] = {
  { "g", DEVELOP_BLENDIF_GRAY_in,  DEVELOP_BLENDIF_GRAY_out,  TRUE,  0.0f, 0.0f, 255.0f, "" },
  { "R", DEVELOP_BLENDIF_RED_in,   DEVELOP_BLENDIF_RED_out,   TRUE,  0.0f, 0.0f, 255.0f, "" },
  { "G", DEVELOP_BLENDIF_GREEN_in, DEVELOP_BLENDIF_GREEN_out, TRUE,  0.0f, 0.0f, 255.0f, "" },
  { "B", DEVELOP_BLENDIF_BLUE_in,  DEVELOP_BLENDIF_BLUE_out,  TRUE,  0.0f, 0.0f, 255.0f, "" },
  { "H", DEVELOP_BLENDIF_H_in,     DEVELOP_BLENDIF_H_out,     FALSE, 0.0f, 0.0f, 360.0f, "\xc2\xb0" },
  { "S", DEVELOP_BLENDIF_S_in,     DEVELOP_BLENDIF_S_out,     FALSE, 0.0f, 0.0f, 100.0f, "" },
  { "l", DEVELOP_BLENDIF_l_in,     DEVELOP_BLENDIF_l_out,     FALSE, 0.0f, 0.0f, 100.0f, "" },
  { NULL, 0, 0, FALSE, 0.0f, 0.0f, 0.0f, NULL } };

static const dt_remote_blendif_channel_t _channels_rgb_scene[] = {
  { "g",  DEVELOP_BLENDIF_GRAY_in,  DEVELOP_BLENDIF_GRAY_out,  TRUE,  0.0f,         0.0f, 100.0f, "" },
  { "R",  DEVELOP_BLENDIF_RED_in,   DEVELOP_BLENDIF_RED_out,   TRUE,  0.0f,         0.0f, 100.0f, "" },
  { "G",  DEVELOP_BLENDIF_GREEN_in, DEVELOP_BLENDIF_GREEN_out, TRUE,  0.0f,         0.0f, 100.0f, "" },
  { "B",  DEVELOP_BLENDIF_BLUE_in,  DEVELOP_BLENDIF_BLUE_out,  TRUE,  0.0f,         0.0f, 100.0f, "" },
  { "Jz", DEVELOP_BLENDIF_Jz_in,    DEVELOP_BLENDIF_Jz_out,    TRUE,  -6.64385619f, 0.0f, 100.0f, "" },
  { "Cz", DEVELOP_BLENDIF_Cz_in,    DEVELOP_BLENDIF_Cz_out,    TRUE,  -6.64385619f, 0.0f, 100.0f, "" },
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
    default:                           return NULL;  // RAW / NONE: no blendif
  }
}

gboolean dt_remote_blendif_slot_enabled(uint32_t blendif, int slot)
{
  return (blendif & (1u << slot)) != 0;
}

gboolean dt_remote_blendif_slot_inverted(uint32_t blendif, int slot)
{
  return (blendif & (1u << (16 + slot))) != 0;
}

uint32_t dt_remote_blendif_slot_pack(uint32_t blendif, int slot,
                                     gboolean enabled, gboolean inverted)
{
  blendif &= ~(1u << 31);              // strip legacy DEVELOP_BLENDIF_active
  if(enabled)  blendif |=  (1u << slot);        else blendif &= ~(1u << slot);
  if(inverted) blendif |=  (1u << (16 + slot)); else blendif &= ~(1u << (16 + slot));
  return blendif;
}

gboolean dt_remote_blendif_markers_enable(const float m[4])
{
  return !(m[1] == 0.0f && m[2] == 1.0f);
}
```

Extend `dt_remote_blend_mask_mode_from_string` (M-A) to accept `"parametric"`:

```c
  if(!strcmp(s, "parametric")) { *out = DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL; return TRUE; }
```
(insert before the final `return FALSE;`).

- [ ] **Step 5: Build and run**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R test_remote_blend --output-on-failure`
Expected: PASS (M-A tests + 8 new).

- [ ] **Step 6: Commit**

```bash
git add src/control/remote_blend.h src/control/remote_blend.c \
        src/tests/unittests/control/test_remote_blend.c
git commit -m "feat: blendif channel table + pure slot/marker helpers (Tier 2 / M-B)

Family-scoped wire channel tables (Lab/RGB-display/RGB-scene) bound to the
GUI tables by a parity test; pure enable/polarity pack/unpack and the
derived-enable rule; mask_mode gains the 'parametric' write value."
```
(with both Global Constraints trailers)

---

### Task 2: `combine` + `parametric` schema

**Files:**
- Modify: `src/control/remote_blend.c` (`dt_remote_blend_schema`)
- Test: `src/tests/unittests/control/test_remote_blend.c` (append)

**Interfaces:**
- Consumes: Task 1's channel table; M-A's `blend_fixture_t` harness and `_node_object` helper.
- Produces: `dt_remote_blend_schema()` now emits `combine`, `parametric.channels`, and `"parametric"` inside `mask_mode.values`, for Lab/RGB families only.

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

  // mask_mode.values now includes "parametric"
  JsonArray *mm = json_object_get_array_member(
    json_object_get_object_member(schema, "mask_mode"), "values");
  gboolean found_param = FALSE;
  for(guint i = 0; i < json_array_get_length(mm); i++)
    if(!strcmp(json_array_get_string_element(mm, i), "parametric")) found_param = TRUE;
  assert_true(found_param);

  // combine enum, four values, writable
  JsonObject *combine = json_object_get_object_member(schema, "combine");
  assert_true(json_object_get_boolean_member(combine, "writable"));
  JsonArray *cv = json_object_get_array_member(combine, "values");
  assert_int_equal(json_array_get_length(cv), 4);
  assert_string_equal(json_array_get_string_element(cv, 0), "exclusive");
  assert_string_equal(json_array_get_string_element(cv, 1), "inclusive");
  assert_string_equal(json_array_get_string_element(cv, 2), "exclusive_inverted");
  assert_string_equal(json_array_get_string_element(cv, 3), "inclusive_inverted");

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
    assert_true(json_object_get_null_member(channels, "hz_in") == FALSE); // present
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

static void test_schema_no_parametric_for_raw_module(void **state)
{
  (void)state;
  // "rawprepare" is a RAW-blending or non-blending module: no parametric.
  // Use a module whose effective colorspace is RAW/NONE. "invert" or a
  // raw-domain module; fall back to a plain assertion via effective cs.
  blend_fixture_t *fx = blend_fixture_new("exposure");
  fx->module->blend_params->blend_cst = DEVELOP_BLEND_CS_RAW;
  JsonNode *node = dt_remote_blend_schema(fx->module);
  JsonObject *schema = _node_object(node);
  assert_false(json_object_has_member(schema, "parametric"));
  assert_false(json_object_has_member(schema, "combine"));
  json_node_unref(node);
  blend_fixture_free(fx);
}
```

Register both in `main()`.

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R test_remote_blend --output-on-failure 2>&1 | tail -20`
Expected: FAIL — schema has no `combine`/`parametric` members / `mask_mode.values` lacks `"parametric"`.

- [ ] **Step 3: Implement the schema additions**

In `dt_remote_blend_schema`, when building `mask_mode.values`, append `"parametric"` only when the effective colorspace has a channel table:

```c
  const dt_develop_blend_colorspace_t eff_mm =
    dt_remote_blend_effective_colorspace(module, module->blend_params->blend_cst);
  const gboolean parametric_supported = dt_remote_blendif_channels(eff_mm) != NULL;
  // ... inside the mask_mode "values" array, after "uniform":
  if(parametric_supported) json_builder_add_string_value(b, "parametric");
```

After the M-A float-field loop, before `json_builder_end_object(b)`, add:

```c
  // Tier 2: combine + parametric (Lab / RGB families only; absent for RAW)
  if(parametric_supported)
  {
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
or null, and a non-normative display_hint); mask_mode.values gains parametric.
Absent for RAW/NONE families."
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
  blend_fixture_free(fx);
}
```

Register both.

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R test_remote_blend --output-on-failure 2>&1 | tail -20`
Expected: FAIL — read lacks `combine`/`parametric`/`foreign_channels`.

- [ ] **Step 3: Implement the read additions**

Add a `combine` mapping helper near the top of `remote_blend.c` (both directions; Task 4 reuses the reverse):

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

static gboolean _combine_from_string(const char *s, uint32_t *bits /* INV|INCL only */)
{
  if(!s) return FALSE;
  if(!strcmp(s, "exclusive"))          { *bits = DEVELOP_COMBINE_NORM_EXCL; return TRUE; }
  if(!strcmp(s, "inclusive"))          { *bits = DEVELOP_COMBINE_NORM_INCL; return TRUE; }
  if(!strcmp(s, "exclusive_inverted")) { *bits = DEVELOP_COMBINE_INV_EXCL; return TRUE; }
  if(!strcmp(s, "inclusive_inverted")) { *bits = DEVELOP_COMBINE_INV_INCL; return TRUE; }
  return FALSE;
}

// family slot-mask for foreign detection
static uint32_t _family_slot_mask(dt_develop_blend_colorspace_t csp)
{
  switch(csp)
  {
    case DEVELOP_BLEND_CS_LAB:         return DEVELOP_BLENDIF_Lab_MASK;
    case DEVELOP_BLEND_CS_RGB_DISPLAY:
    case DEVELOP_BLEND_CS_RGB_SCENE:   return DEVELOP_BLENDIF_RGB_MASK;
    default:                           return 0u;
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
        for(int k = 0; k < 4; k++) json_builder_add_double_value(b, (double)p[k]);
        json_builder_end_array(b);
        json_builder_set_member_name(b, "inverted");
        json_builder_add_boolean_value(b, dt_remote_blendif_slot_inverted(bp->blendif, slot));
        json_builder_set_member_name(b, "boost");
        json_builder_add_double_value(b, (double)bp->blendif_boost_factors[slot]);
        json_builder_end_object(b);
      }
    }
    json_builder_end_object(b);

    // foreign: any enabled slot outside the effective family (legacy edits)
    const uint32_t enabled_slots = bp->blendif & 0xFFFFu & ~(1u << 31);
    json_builder_set_member_name(b, "foreign_channels");
    json_builder_add_boolean_value(b, (enabled_slots & ~in_family & _family_slot_mask(eff_r) ? TRUE
                                       : (enabled_slots & ~in_family) ? TRUE : FALSE));
  }
```

(The foreign test reduces to "any enabled slot not in `in_family`"; the redundant `_family_slot_mask` term above is defensive — simplify to `(enabled_slots & ~in_family) != 0` if preferred. Keep one expression; the test asserts a raw out-of-family bit triggers it.)

Simplify the `foreign_channels` line to exactly:
```c
    json_builder_add_boolean_value(b, (enabled_slots & ~in_family) != 0);
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
foreign_channels boolean when out-of-family slots are enabled."
```
(plus trailers)

---

### Task 4: `combine` + `parametric` patch apply

**Files:**
- Modify: `src/control/remote_blend.c` (`dt_remote_blend_patch_apply`)
- Test: `src/tests/unittests/control/test_remote_blend.c` (append)

**Interfaces:**
- Consumes: Tasks 1–3 helpers; M-A `dt_remote_blend_patch_apply` (extended in place); `dt_develop_blend_init_blendif_parameters()`.
- Produces: `dt_remote_blend_patch_apply` handles `combine` + `parametric` per the validation order, with the error table below.

**Error table this task implements (constraint slugs normative):**

| condition | code | constraint |
|---|---|---|
| `parametric`/`combine` on a RAW/NONE family (no blendif) | UNSUPPORTED_FIELD | (`parameter: blend.parametric`/`blend.combine`) |
| `combine` value unknown | INVALID_VALUE | `unknown_value` |
| `parametric` not an object, or a channel entry neither object nor null | INVALID_VALUE | `wrong_type` |
| channel name not in the effective family | UNSUPPORTED_FIELD | (`parameter: blend.parametric.<name>`) |
| `markers` missing / not an array of exactly 4 numbers | INVALID_VALUE | `markers` |
| markers non-finite, outside [0,1], or not ascending | INVALID_VALUE | `markers` |
| `inverted` present but not a boolean | INVALID_VALUE | `wrong_type` |
| `boost` on a `boost:null` (hue) channel | INVALID_VALUE | `boost` |
| `boost` non-number, non-finite, or out of `[offset, offset+18]` | INVALID_VALUE | `boost` |
| `parametric` while projected `mask_mode` lacks `CONDITIONAL` | INVALID_VALUE | `requires_parametric_mask_mode` |
| `inverted` change together with a `combine` change, no override | INVALID_VALUE | `inverted_and_combine_conflict` |
| `mask_mode: "parametric"` from a drawn/raster stored row | INVALID_VALUE | per transition appendix (`drawn_via_attach_only` / `raster_unsupported`) |

- [ ] **Step 1: Write the failing tests**

Append (reuse M-A's `_patch_from_string`, `_assert_patch_fails`, `blend_fixture_t`):

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
  // with the override flag it succeeds
  JsonObject *patch = _patch_from_string(
    "{\"allow_inverted_combine\":true,\"combine\":\"inclusive\","
    "\"parametric\":{\"Jz_in\":{\"markers\":[0.1,0.2,0.6,0.7],\"inverted\":true}}}");
  dt_develop_blend_params_t dst = *bp;
  dt_remote_error_t *error = NULL;
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
  // drawn stored -> parametric target: refused (drawn_via_attach_only)
  fx->module->blend_params->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK;
  _assert_patch_fails(fx, "{\"mask_mode\":\"parametric\"}",
                      DT_REMOTE_ERR_INVALID_VALUE, "drawn_via_attach_only");
  blend_fixture_free(fx);
}
```

Register all in `main()`.

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R test_remote_blend --output-on-failure 2>&1 | tail -20`
Expected: FAIL (combine/parametric stages not implemented; several asserts fail).

- [ ] **Step 3: Extend `dt_remote_blend_patch_apply`**

First, extend M-A's `allowed[]` member list with the Tier-2 members:
```c
    "combine", "parametric", "allow_inverted_combine",
```

Then, **replace M-A's `mask_mode` stage** so `"parametric"` is accepted and the transition appendix is honored (M-A only allowed `off`/`uniform`; the drawn/raster refusal now distinguishes constraint slugs):

```c
  // 1. mask_mode -- transition appendix (candidates doc, authoritative).
  if(json_object_has_member(patch, "mask_mode"))
  {
    const char *target = _json_member_string(patch, "mask_mode");
    if(dst->mask_mode & DEVELOP_MASK_RASTER)
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "mask_mode",
                                      "raster_unsupported", _("raster masks are not writable"));
      return FALSE;
    }
    if(dst->mask_mode & DEVELOP_MASK_MASK)
    {
      // drawn stored: mask_mode is entered/left via attach/detach only
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "mask_mode",
                                      "drawn_via_attach_only",
                                      _("drawn masks are managed via attach/detach"));
      return FALSE;
    }
    uint32_t v = 0;
    if(!dt_remote_blend_mask_mode_from_string(target, &v))
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "mask_mode", "unknown_value",
                                      _("mask_mode accepts \"off\", \"uniform\" or \"parametric\""));
      return FALSE;
    }
    dst->mask_mode = v;
  }
```

After the M-A `colorspace` stage and before/around the numeric stage, insert the `combine` + `parametric` stages (validation order: after colorspace, before Tier-1 numerics). Add near the top of the function a note of whether combine changed:

```c
  // Detect (for the H4 conflict guard) whether combine changes value.
  gboolean combine_changed = FALSE;
  const gboolean allow_inv_combine =
    json_object_has_member(patch, "allow_inverted_combine")
    && json_node_get_value_type(json_object_get_member(patch, "allow_inverted_combine")) == G_TYPE_BOOLEAN
    && json_node_get_boolean(json_object_get_member(patch, "allow_inverted_combine"));

  const dt_develop_blend_colorspace_t eff_cs =
    dt_remote_blend_effective_colorspace(module, dst->blend_cst);
  const dt_remote_blendif_channel_t *table = dt_remote_blendif_channels(eff_cs);

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
      float prev = -G_MAXFLOAT;
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
        m[k] = (float)json_node_get_double(mk);
        if(!isfinite(m[k]) || m[k] < 0.0f || m[k] > 1.0f || m[k] < prev)
        {
          if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "parametric", "markers",
                                          _("markers must be finite, ascending, within [0,1]"));
          g_list_free(names);
          return FALSE;
        }
        prev = m[k];
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

Place this block after the M-A `colorspace` stage and before the M-A numeric-field loop, so the validation order is `mask_mode → colorspace → combine → parametric → numerics`.

- [ ] **Step 4: Build and run**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R test_remote_blend --output-on-failure`
Expected: PASS. Then full `ctest --test-dir build`.

- [ ] **Step 5: Commit**

```bash
git add src/control/remote_blend.c src/tests/unittests/control/test_remote_blend.c
git commit -m "feat: parametric + combine blend patch apply (Tier 2 / M-B)

Validation order mask_mode -> colorspace -> combine -> parametric ->
numerics; derived enable, verbatim markers (no boost rescale), null reset,
foreign-slot preservation, family gating, the transition-appendix mask_mode
rows, and the H4 inverted+combine conflict guard with an explicit override."
```
(plus trailers)

---

### Task 5: Mask render engine — pipe opt-in, gate relax, export mask target, prepare/execute, white synth

**Files:**
- Modify: `src/develop/pixelpipe.h` (the `dt_dev_pixelpipe_t` struct — add `mask_display_request`)
- Modify: `src/develop/pixelpipe_hb.c` (init it FALSE in `dt_dev_pixelpipe_init_export` and reset in `dt_dev_pixelpipe_cleanup` / the two `mask_display = NONE` reset points)
- Modify: `src/develop/blend.c` (`valid_request` gate at ~line 555, and the raster twin at ~964 unchanged)
- Modify: `src/imageio/imageio_common.h`, `src/imageio/imageio.c`, `src/common/mipmap_cache.c` (optional mask-display target param)
- Modify: `src/control/remote_edit.h` (`dt_remote_preview_request_t` fields; `dt_remote_render_preview_prepare` signature)
- Modify: `src/control/remote_edit.c` (prepare validation + execute mask path + white synth + `mask_of`)
- Test: `src/tests/unittests/control/test_remote_blend.c` (pure gate-flag unit) — see note

**Interfaces:**
- Consumes: M-A/M-B `dt_remote_require_darkroom_image`, `dt_remote_find_module`, `dt_remote_blend_mask_mode_string`.
- Produces:
  - `dt_dev_pixelpipe_t.mask_display_request` (gboolean).
  - `dt_imageio_export_with_flags(..., const dt_imageio_mask_display_t *mask_target)` — new trailing param; `NULL` = normal export.
  - `dt_remote_preview_request_t` gains `dt_dev_operation_t mask_op; int mask_instance; gboolean want_mask; gboolean force_white; uint32_t mask_mode_stored;`.
  - `dt_remote_render_preview_prepare(out, show_mask_op /*nullable*/, show_mask_instance, error)`.
  - `dt_remote_preview_t` gains `gboolean is_mask; dt_dev_operation_t mask_op; int mask_instance; uint32_t mask_mode_stored;` (for the `mask_of` response metadata).

- [ ] **Step 1: Write the failing unit test (blend gate flag)**

The full mask render needs a live darkroom (integration, Task 8). The unit-testable slice is the blend gate: the flag must be honored when `pipe->mask_display_request` is set even without focus/full-pipe. Append to `test_remote_blend.c` a pure check that the new pipe field exists and the helper predicate reads it (a compile-level guard; the behavioral proof is Task 8):

```c
/* ------------------------------------------------------------------ */
/* Task 5: mask render plumbing (compile guard; behavior in Task 8)    */
/* ------------------------------------------------------------------ */
#include "develop/pixelpipe.h"

static void test_pipe_has_mask_display_request_field(void **state)
{
  (void)state;
  dt_dev_pixelpipe_t pipe;
  memset(&pipe, 0, sizeof(pipe));
  pipe.mask_display_request = TRUE;    // fails to compile if the field is missing
  assert_true(pipe.mask_display_request);
}
```

Register it in `main()`.

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build -j$(nproc) 2>&1 | tail -5`
Expected: compile FAILURE — `dt_dev_pixelpipe_t` has no member `mask_display_request`.

- [ ] **Step 3: Add the pipe field + gate relax**

In `src/develop/pixelpipe.h`, inside `struct dt_dev_pixelpipe_t`, next to `mask_display`:

```c
  /** remote mask render (M-B): when TRUE, dt_develop_blend_process honors
   * a module's request_mask_display on THIS pipe even though it is not the
   * darkroom full pipe and the module holds no GUI focus. Set only by the
   * remote-edit mask render on a throwaway export pipe; default FALSE. */
  gboolean mask_display_request;
```

In `src/develop/pixelpipe_hb.c`, set it FALSE wherever `pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;` is initialized (the two lines ~289 and ~3106) — add `pipe->mask_display_request = FALSE;` beside each.

In `src/develop/blend.c`, relax the `valid_request` at ~line 555 from:
```c
  const gboolean valid_request = dt_iop_has_focus(self) && (piece->pipe == self->dev->full.pipe);
```
to:
```c
  // M-B remote mask render: a throwaway export pipe may opt in to honor
  // request_mask_display without focus/full-pipe (parametric-masks design,
  // amendment 1). The live darkroom path (focus + full.pipe) is unchanged.
  const gboolean valid_request =
      (dt_iop_has_focus(self) && (piece->pipe == self->dev->full.pipe))
      || piece->pipe->mask_display_request;
```

- [ ] **Step 4: Add the export mask-display target parameter**

In `src/imageio/imageio_common.h`, before the `dt_imageio_export_with_flags` declaration:

```c
// Optional target for the remote mask render (M-B): render the display
// mask of this op/instance instead of the image. NULL means a normal export.
typedef struct dt_imageio_mask_display_t
{
  const char *op;    // dt_dev_operation string
  int instance;      // multi_priority discriminator
} dt_imageio_mask_display_t;
```

Add a trailing parameter `const dt_imageio_mask_display_t *mask_target` to the `dt_imageio_export_with_flags` declaration (after `history_end`).

In `src/imageio/imageio.c`, update the definition signature identically. After `dt_dev_pixelpipe_create_nodes(&pipe, &dev);` / `dt_dev_pixelpipe_synch_all(&pipe, &dev);` and before the process call, insert:

```c
  if(mask_target)
  {
    for(GList *m = dev.iop; m; m = g_list_next(m))
    {
      dt_iop_module_t *mod = m->data;
      if(!g_strcmp0(mod->op, mask_target->op) && mod->multi_priority == mask_target->instance)
      {
        mod->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_MASK;
        pipe.mask_display_request = TRUE;   // opt this export pipe in (M-B)
        break;
      }
    }
  }
```

Update the two other call sites to pass `NULL` for the new argument: `src/common/mipmap_cache.c` (the `dt_imageio_export_with_flags(...)` call) and any internal self-call in `imageio.c` (the thumbnail path at ~line 1790).

- [ ] **Step 5: Thread the mask target through remote_edit prepare/execute**

In `src/control/remote_edit.h`, extend the request struct:

```c
typedef struct dt_remote_preview_request_t
{
  int32_t imgid;
  uint64_t revision;
  // M-B mask render (parametric-masks design SS Mask rendering companion):
  dt_dev_operation_t mask_op;   // "" when not a mask render
  int mask_instance;
  gboolean want_mask;           // render the display mask, not the image
  gboolean force_white;         // target has no spatial mask -> solid white
  uint32_t mask_mode_stored;    // for the mask_of response metadata
} dt_remote_preview_request_t;
```

Extend `dt_remote_preview_t` with mask metadata:
```c
  gboolean is_mask;
  dt_dev_operation_t mask_op;
  int mask_instance;
  uint32_t mask_mode_stored;
```

Change the prepare signature to accept the requested mask target:
```c
/** ... (M-A doc). If `show_mask_op` is non-NULL, this is a mask render:
 * resolve the target module, require IOP_FLAGS_SUPPORTS_BLENDING
 * (DT_REMOTE_ERR_UNSUPPORTED_FIELD naming `show_mask.op` otherwise),
 * capture its op/instance and stored mask_mode into `*out`, and set
 * force_white when the stored mode has neither CONDITIONAL nor MASK bits
 * (off/uniform render as solid white -- design SS Mask render). */
gboolean dt_remote_render_preview_prepare(dt_remote_preview_request_t *out,
                                          const char *show_mask_op, int show_mask_instance,
                                          dt_remote_error_t **error);
```

In `src/control/remote_edit.c`, in `dt_remote_render_preview_prepare`, after resolving `dev`, add (before `dt_dev_write_history(dev)` so the module lookup is on the live tree):

```c
  memset(out->mask_op, 0, sizeof(out->mask_op));
  out->want_mask = FALSE;
  out->force_white = FALSE;
  out->mask_mode_stored = 0;
  if(show_mask_op)
  {
    const dt_remote_module_ref_t ref = { .op = show_mask_op, .instance = show_mask_instance };
    dt_iop_module_t *module = dt_remote_find_module(dev, &ref, error);
    if(!module) return FALSE;
    if(!(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING))
    {
      if(error)
        *error = dt_remote_error_new(DT_REMOTE_ERR_UNSUPPORTED_FIELD,
                                     _("module '%s' does not support blending"), show_mask_op);
      return FALSE;
    }
    g_strlcpy(out->mask_op, module->op, sizeof(out->mask_op));
    out->mask_instance = module->multi_priority;
    out->want_mask = TRUE;
    out->mask_mode_stored = module->blend_params->mask_mode;
    out->force_white = !(module->blend_params->mask_mode
                         & (DEVELOP_MASK_CONDITIONAL | DEVELOP_MASK_MASK));
  }
```

In `dt_remote_render_preview_execute`, build the mask target and pass it, then post-process for white and metadata:

```c
  dt_imageio_mask_display_t mtarget;
  const dt_imageio_mask_display_t *mtarget_ptr = NULL;
  if(req->want_mask)
  {
    mtarget.op = req->mask_op;
    mtarget.instance = req->mask_instance;
    mtarget_ptr = &mtarget;
  }
```
Pass `mtarget_ptr` as the new trailing `dt_imageio_export_with_flags(...)` argument. After the export returns and before JPEG-encoding, if `req->want_mask && req->force_white`, fill the RGBA buffer white:
```c
  if(req->want_mask && req->force_white)
    memset(sink.buf, 0xFF, sizeof(uint32_t) * (size_t)sink.width * sink.height);
```
When populating `preview`, carry the mask metadata:
```c
  preview->is_mask = req->want_mask;
  if(req->want_mask)
  {
    g_strlcpy(preview->mask_op, req->mask_op, sizeof(preview->mask_op));
    preview->mask_instance = req->mask_instance;
    preview->mask_mode_stored = req->mask_mode_stored;
  }
```

Update the M-A/M-B caller `_handler_render_preview` (Task 6) and the M-A protocol test stub to pass the new prepare arguments (Task 6 handles this).

- [ ] **Step 6: Build and run the unit + full C suite**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure`
Expected: all C suites PASS (behavioral mask proof is Task 8's integration gate).

- [ ] **Step 7: Commit**

```bash
git add src/develop/pixelpipe.h src/develop/pixelpipe_hb.c src/develop/blend.c \
        src/imageio/imageio_common.h src/imageio/imageio.c src/common/mipmap_cache.c \
        src/control/remote_edit.h src/control/remote_edit.c \
        src/tests/unittests/control/test_remote_blend.c
git commit -m "feat: mask render engine -- export display-mask opt-in (Tier 2 / M-B)

The export pipe cannot honor request_mask_display (blend.c gate requires
the darkroom full pipe + focus), so add a guarded per-pipe opt-in
(dt_dev_pixelpipe_t.mask_display_request), relax the blend gate for it,
and thread an optional mask-display target through
dt_imageio_export_with_flags. render_preview's prepare/execute resolve the
target, render its display mask at normal framing, synthesize solid white
for non-spatial modes, and carry mask_of metadata."
```
(plus trailers)

---

### Task 6: Protocol layer — capabilities, `show_mask` parse/thread, `mask_of` serialize, fixtures

**Files:**
- Modify: `src/control/remote_protocol.c` (hello; `RENDER_PREVIEW_KEYS`; `_handler_render_preview`; preview request struct plumbing; `_preview_job_params_t`; `_queue_preview_job`; `dt_remote_protocol_build_preview_response`)
- Modify: `src/control/remote_protocol.h` (`dt_remote_protocol_async_t.queue_preview` signature already takes `req`; no change unless the calls-table needs it)
- Create fixtures: `set_module_params_parametric_request.json`, `set_module_params_parametric_response.json`, `render_preview_show_mask_request.json`, `set_module_params_error_parametric_mask_mode_request.json`, `set_module_params_error_parametric_mask_mode_response.json`
- Test: `src/tests/unittests/control/test_remote_protocol.c` (append), `tools/mcp/tests/test_protocol.py` (extend)

**Interfaces:**
- Consumes: Task 5's `dt_remote_render_preview_prepare(out, op, instance, error)` and `dt_remote_preview_t` mask fields; M-A's blend parse/serialize (parametric/combine ride the existing `blend` object — no new parse code, they pass through `patch->blend`).
- Produces: `hello` advertises `parametric_mask_params` + `mask_render`; `render_preview` accepts `show_mask {op, instance}`; the preview response gains a `mask_of` member.

- [ ] **Step 1: Write the failing C protocol tests**

Append to `test_remote_protocol.c` (follow the neighboring stub-calls + `_assert_dispatch_matches` idiom):

1. `test_hello_advertises_parametric_and_mask_render` — extend the hello expected-capabilities list with `"parametric_mask_params"` and `"mask_render"`; update `fixtures/hello_response.json` accordingly.
2. `test_set_module_params_parametric_round_trip` — stub `set_module_params` asserts `patch->blend` contains a `"parametric"` member with a `"Jz_in"` child; returns a canned result whose `blend_readback` is parsed from the response fixture; `_assert_dispatch_matches("set_module_params_parametric_request.json", "set_module_params_parametric_response.json")`. (No new C parse code — the `blend` object already carries `parametric`/`combine` via M-A's borrowed `JsonObject`.)
3. `test_render_preview_show_mask_parsed` — install a stub `render_preview_prepare` (calls-table member from M-A) that records the `show_mask_op`/`instance` arguments; dispatch `render_preview_show_mask_request.json`; assert the stub saw `op == "exposure"`, `instance == 1`. Also dispatch a request with `show_mask` present but `op` missing → `invalid_value` naming `show_mask.op`; and a request with `show_mask` not an object → `invalid_value`.
4. `test_render_preview_show_mask_requires_capability_is_client_side` — (documentation-only C assertion) the C layer does NOT gate on `mask_render` (the sidecar does); assert a `show_mask` request is accepted at the protocol layer regardless.

Note: the protocol test harness stubs the async render, so these tests exercise parse/threading, not the render itself.

- [ ] **Step 2: Author fixtures**

`set_module_params_parametric_request.json` — params:
```json
{ "module": "exposure", "values": {},
  "blend": { "mask_mode": "parametric", "combine": "exclusive",
             "parametric": { "Jz_in": { "markers": [0.55, 0.65, 1.0, 1.0], "inverted": true } } } }
```
`set_module_params_parametric_response.json` — result mirrors M-A's blend readback shape plus:
```json
"blend": { "mask_mode": "parametric", "combine": "exclusive",
           "parametric": { "Jz_in": { "markers": [0.55, 0.65, 1.0, 1.0],
                                       "inverted": true, "boost": -6.64385619 } },
           "foreign_channels": false, "...": "M-A scalar members elided in this doc but present in the fixture" }
```
(Author the full blend object mirroring M-A's `set_module_params_blend_response.json`, with the Tier-2 members added.)

`render_preview_show_mask_request.json` — params:
```json
{ "max_px": 1024, "quality": 90, "show_mask": { "op": "exposure", "instance": 1 } }
```

Error pair — request sends `blend.parametric` while `mask_mode` is `uniform`; response is the standard error envelope with `code` = invalid_value, message `"set mask_mode to \"parametric\" to write parametric channels"`, details `{ "parameter": "blend.parametric", "constraint": "requires_parametric_mask_mode" }`.

- [ ] **Step 3: Run to verify failure**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R test_remote_protocol --output-on-failure 2>&1 | tail -20`
Expected: FAIL — hello lacks the two capabilities; `show_mask` not parsed; prepare signature mismatch.

- [ ] **Step 4: Implement the protocol changes**

1. Hello: after `"blend_params"` add:
```c
  json_builder_add_string_value(b, "parametric_mask_params");
  json_builder_add_string_value(b, "mask_render");
```
with a comment: `// parametric_mask_params gates blend.parametric/combine; mask_render gates render_preview.show_mask (both mask Tier 2 / M-B).`
2. `RENDER_PREVIEW_KEYS` becomes `{ "max_px", "quality", "show_mask", NULL }`.
3. In `_handler_render_preview`, after the `quality` clamp and before `render_preview_prepare`, parse `show_mask`:
```c
  const char *show_mask_op = NULL;
  gint64 show_mask_instance = 0;
  if(json_object_has_member(params, "show_mask"))
  {
    JsonNode *sm = json_object_get_member(params, "show_mask");
    if(!sm || !JSON_NODE_HOLDS_OBJECT(sm))
      return _handler_fail(_error_new(DT_REMOTE_ERR_INVALID_VALUE,
                                      _("parameter 'show_mask' must be an object")));
    JsonObject *smo = json_node_get_object(sm);
    JsonNode *opn = json_object_get_member(smo, "op");
    if(!opn || !JSON_NODE_HOLDS_VALUE(opn) || json_node_get_value_type(opn) != G_TYPE_STRING)
      return _handler_fail(_error_new(DT_REMOTE_ERR_INVALID_VALUE,
                                      _("'show_mask.op' is required and must be a string")));
    show_mask_op = json_node_get_string(opn);
    if(json_object_has_member(smo, "instance"))
    {
      JsonNode *inst = json_object_get_member(smo, "instance");
      if(!inst || !JSON_NODE_HOLDS_VALUE(inst) || json_node_get_value_type(inst) != G_TYPE_INT64)
        return _handler_fail(_error_new(DT_REMOTE_ERR_INVALID_VALUE,
                                        _("'show_mask.instance' must be an integer")));
      show_mask_instance = json_node_get_int(inst);
    }
  }
```
Then pass the target to prepare:
```c
  if(!s_calls.render_preview_prepare(&req, show_mask_op, (int)show_mask_instance, &err))
    return _handler_fail(err);
```
Update the calls-table member type in `remote_protocol.h` to `gboolean (*render_preview_prepare)(dt_remote_preview_request_t *, const char *, int, dt_remote_error_t **);` and both initializers (`DEFAULT_CALLS`/`s_calls`) already point at `dt_remote_render_preview_prepare` (Task 5's new signature) — no value change, only the typedef.
4. `dt_remote_protocol_build_preview_response`: when `preview->is_mask`, add a `mask_of` object before `data`:
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
      json_builder_add_string_value(b, dt_remote_blend_mask_mode_string(preview->mask_mode_stored));
      json_builder_end_object(b);
    }
```
(`dt_remote_blend_mask_mode_string` is declared in `remote_blend.h`; include it in `remote_protocol.c` if not already via `remote_edit.h`.)

- [ ] **Step 5: Build and run C + Python protocol suites**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure`
Expected: all PASS.

Extend `tools/mcp/tests/test_protocol.py`'s shared-fixture parametrization with the parametric success pair and the hello-capability assertion (`assert "parametric_mask_params" in client.capabilities` and `assert "mask_render" in client.capabilities`).

Run: `cd tools/mcp && .venv/bin/pytest -q`
Expected: all PASS.

- [ ] **Step 6: Commit**

```bash
git add src/control/remote_protocol.h src/control/remote_protocol.c \
        src/tests/unittests/control/test_remote_protocol.c \
        src/tests/unittests/control/fixtures/ tools/mcp/tests/test_protocol.py
git commit -m "feat: parametric + mask_render wire surface (Tier 2 / M-B)

hello advertises parametric_mask_params and mask_render; render_preview
parses show_mask {op, instance} and threads it to prepare; the preview
response carries mask_of {op, instance, mask_mode}. parametric/combine ride
the existing blend object (no new parse)."
```
(plus trailers)

---

### Task 7: Sidecar — capability gates + `show_mask` passthrough + worked-example docstrings (H4)

**Files:**
- Modify: `tools/mcp/src/darktable_mcp/server.py`
- Test: `tools/mcp/tests/test_tools.py` (append)

- [ ] **Step 1: Write the failing tests**

Append to `test_tools.py`, mirroring M-A's blend-gate tests:

```python
async def test_set_module_params_parametric_gated_on_capability(tmp_path, fake_server_factory):
    """A `parametric`/`combine` member needs parametric_mask_params."""
    server = await fake_server_factory(capabilities=["params", "blend_params"])
    calls = []
    server.handle("set_module_params", lambda params: calls.append(params) or {})
    app = await _built_server(tmp_path, server)
    with pytest.raises(TransportError) as excinfo:
        await app.call_tool(
            "set_module_params",
            {"module": "exposure", "values": {},
             "blend": {"mask_mode": "parametric",
                       "parametric": {"Jz_in": {"markers": [0.55, 0.65, 1.0, 1.0]}}}},
        )
    assert "parametric_mask_params" in str(excinfo.value)
    assert calls == []


async def test_set_module_params_parametric_passes_when_advertised(tmp_path, fake_server_factory):
    server = await fake_server_factory(capabilities=["params", "blend_params", "parametric_mask_params"])
    seen = []
    server.handle("set_module_params",
                  lambda params: seen.append(params)
                  or {"module": "exposure", "instance": 0, "enabled": True, "values": {}, "revision": 5})
    app = await _built_server(tmp_path, server)
    await app.call_tool(
        "set_module_params",
        {"module": "exposure", "values": {},
         "blend": {"mask_mode": "parametric", "combine": "exclusive",
                   "parametric": {"Jz_in": {"markers": [0.55, 0.65, 1.0, 1.0], "inverted": True}}}},
    )
    assert seen[0]["blend"]["parametric"]["Jz_in"]["inverted"] is True


async def test_render_preview_show_mask_gated_on_mask_render(tmp_path, fake_server_factory):
    server = await fake_server_factory(capabilities=["params", "preview"])
    app = await _built_server(tmp_path, server)
    with pytest.raises(TransportError) as excinfo:
        await app.call_tool("render_preview", {"show_mask": {"op": "exposure"}})
    assert "mask_render" in str(excinfo.value)


async def test_render_preview_show_mask_passes_when_advertised(tmp_path, fake_server_factory):
    server = await fake_server_factory(capabilities=["params", "preview", "mask_render"])
    seen = []
    server.handle("render_preview",
                  lambda params: seen.append(params)
                  or {"mime_type": "image/jpeg", "width": 8, "height": 8, "revision": 1, "data": ""})
    app = await _built_server(tmp_path, server)
    await app.call_tool("render_preview", {"show_mask": {"op": "exposure", "instance": 1}})
    assert seen[0]["show_mask"] == {"op": "exposure", "instance": 1}
```

(Match the file's exact capability-advertising idiom and `TransportError`/`ToolError` type used by M-A's blend gate.)

- [ ] **Step 2: Run to verify failure**

Run: `cd tools/mcp && .venv/bin/pytest -q tests/test_tools.py -k "parametric or show_mask"`
Expected: FAIL — `parametric`/`combine` not gated; `render_preview` has no `show_mask` argument.

- [ ] **Step 3: Implement**

In `server.py`'s `set_module_params`, after the M-A `blend` capability gate, add a nested gate:
```python
        if blend is not None:
            await client.ensure_connected()
            if "blend_params" not in client.capabilities:
                raise TransportError(
                    "this darktable does not advertise blend_params; "
                    "upgrade darktable to edit blend settings"
                )
            if ("parametric" in blend or "combine" in blend) \
                    and "parametric_mask_params" not in client.capabilities:
                raise TransportError(
                    "this darktable does not advertise parametric_mask_params; "
                    "upgrade darktable to edit parametric masks"
                )
            params["blend"] = blend
```

Append to the `set_module_params` docstring a parametric-masks paragraph with two worked examples and the two sharp-edge warnings (H4):
```
        `blend.parametric` writes per-channel blendif ramps (needs
        `parametric_mask_params`). Each channel is `<name>_in`/`<name>_out`
        for the effective colorspace (see `get_module_schema`'s
        `blend.parametric.channels`). A channel entry is
        `{markers:[m0,m1,m2,m3], inverted?, boost?}` with ascending
        markers in [0,1]; the mask ramps 0->1 over m0..m1, holds 1 over
        m1..m2, falls 1->0 over m2..m3. Full-span [0,0,1,1] means "no
        condition" (the slot is disabled). `null` resets a channel.
        WORKED EXAMPLE -- select the sky by luminance on an exposure
        instance: {"blend": {"mask_mode": "parametric", "parametric":
        {"Jz_in": {"markers": [0.55, 0.65, 1.0, 1.0], "inverted": true}}}}.
        WORKED EXAMPLE -- restrict to shadows: {"blend": {"mask_mode":
        "parametric", "parametric": {"Jz_in": {"markers": [0.0, 0.0, 0.2,
        0.35]}}}}. WARNING: boost is an exp2 exponent and does NOT rescale
        your markers -- change markers yourself if you change boost.
        WARNING: `combine`'s inclusive setting XORs every channel's
        effective polarity (effective_inverted = inverted XOR inclusive);
        changing `inverted` and `combine` together is refused unless you
        pass `blend.allow_inverted_combine: true`. Verify every threshold
        with render_preview's show_mask.
```

Add a `show_mask` argument to `render_preview`:
1. Signature: `show_mask: dict[str, Any] | None = None,`.
2. Docstring: append:
```
        `show_mask` ({"op": <module>, "instance": <n>}) renders that
        module's blend mask as a grayscale JPEG (white = full effect)
        instead of the blended image, framed identically so coordinates
        line up. Needs the `mask_render` capability. Use it to verify every
        parametric threshold you set. `off`/`uniform` masks render solid
        white; the response's `mask_of` echoes {op, instance, mask_mode}.
```
3. Before the wire call:
```python
        if show_mask is not None:
            await client.ensure_connected()
            if "mask_render" not in client.capabilities:
                raise TransportError(
                    "this darktable does not advertise mask_render; "
                    "upgrade darktable to render masks"
                )
            params["show_mask"] = show_mask
```

- [ ] **Step 4: Run tests**

Run: `cd tools/mcp && .venv/bin/pytest -q`
Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add tools/mcp/src/darktable_mcp/server.py tools/mcp/tests/test_tools.py
git commit -m "feat: sidecar parametric + show_mask gates and worked-example docstrings"
```
(plus trailers)

---

### Task 8: Integration gates (H6 — direct mask-image assertions)

**Files:**
- Create: `tools/mcp/tests/integration/test_parametric_masks_tier2.py`

**Prereq:** a running build. Run with:
`DARKTABLE_BIN=$PWD/build/bin/darktable tools/mcp/.venv/bin/pytest -q -m integration tools/mcp/tests/integration/test_parametric_masks_tier2.py`

- [ ] **Step 1: Write the gates**

Model on `test_blend_tier1.py`: `from . import harness`, `pytestmark = pytest.mark.integration`, raw wire calls via `client.call(...)`, the shared `_render()` helper, history bookkeeping. Add a `_render_mask(op, instance)` helper that calls `render_preview` with `show_mask` and decodes the JPEG to a grayscale numpy array (reuse harness JPEG-decode if present; otherwise `PIL.Image.open(io.BytesIO(base64.b64decode(...))).convert("L")`). Gates (each its own test):

1. `test_hello_advertises_parametric_and_mask_render` — `client.capabilities` (or raw `hello`) contains both `"parametric_mask_params"` and `"mask_render"`.
2. `test_schema_carries_parametric_section` — `get_module_schema {"module":"exposure","instance":0}` `blend.parametric.channels` contains `"Jz_in"` (RGB-scene) or the effective family's slots; `mask_mode.values` includes `"parametric"`; `combine.values` has the four strings.
3. `test_parametric_luminance_mask_render_is_dark_low_bright_high` — enable exposure; set `{"blend": {"mask_mode": "parametric", "parametric": {"Jz_in": {"markers": [0.5, 0.6, 1.0, 1.0]}}}}`; render the mask via `show_mask`. On a fixture image with a clear bright region and a clear dark region (pick the harness's high-contrast fixture), assert the mask's mean brightness over the bright region is **substantially greater** than over the dark region — assert the RATIO `mean_bright / (mean_dark + eps) > 3.0` (H6: ratios, pinned preview size, not absolutes). This is the direct-image gate replacing preview-diff inference.
4. `test_combine_flip_changes_mask` — with the same parametric mask, render the mask; then set `{"blend": {"combine": "inclusive"}}`; render again; assert the two mask arrays differ (mean absolute difference above a small threshold). (Inclusive XORs effective polarity.)
5. `test_mask_mode_off_renders_full_white` — set `{"blend": {"mask_mode": "off"}}`; `show_mask` render; assert the array is uniformly near-255 (min > 250) and the response `mask_of.mask_mode == "off"`.
6. `test_parametric_null_disables_channel` — set a Jz_in condition, confirm the slot appears in `get_module_params` `blend.parametric`; patch `{"blend": {"parametric": {"Jz_in": null}}}`; confirm it disappears and the mask render returns to uniform white (min > 250).
7. `test_show_mask_on_non_blending_module_errors` — `render_preview` with `show_mask` for a module without blending (e.g. `rawprepare`/`demosaic`) → `ProtocolError` with `unsupported_field`.
8. `test_parametric_write_is_one_history_item_and_undoable` — capture revision + history length; parametric write; assert history grew by 1; `undo` with the new revision; assert `get_module_params` `blend.mask_mode` returned to the pre-write value.

Use the neighboring files' tolerance/retry discipline (`harness.wait_for_stable_revision`, two-attempt `_render`), and a pinned `max_px` (e.g. 512) for all mask renders so ratios are stable.

- [ ] **Step 2: Run the new gates**

Run: `DARKTABLE_BIN=$PWD/build/bin/darktable tools/mcp/.venv/bin/pytest -q -m integration tools/mcp/tests/integration/test_parametric_masks_tier2.py`
Expected: all PASS.

- [ ] **Step 3: Run the full integration suite**

Run: `DARKTABLE_BIN=$PWD/build/bin/darktable tools/mcp/.venv/bin/pytest -q -m integration tools/mcp`
Expected: previous gates + new gates pass, 1 known OpenCL skip.

- [ ] **Step 4: Commit**

```bash
git add tools/mcp/tests/integration/test_parametric_masks_tier2.py
git commit -m "test: Tier-2 parametric + mask-render integration gates (H6 ratios)"
```
(plus trailers)

---

### Task 9: Documentation + divergence-manifest rows + design amendments

**Files:**
- Modify: `docs/superpowers/specs/2026-07-05-darktable-mcp-protocol-reference.md`
- Modify: `docs/superpowers/specs/2026-07-19-darktable-mcp-parametric-masks-design.md`
- Modify or Create: `docs/superpowers/specs/2026-07-19-darktable-upstream-divergence-manifest.md` (created by M-A Task 9; if absent at read time, create it with the M-A preamble + M-A rows first, then append the Tier-2 rows)
- Modify: `docs/superpowers/specs/2026-07-16-darktable-mcp-supported-operations.md`
- Modify: `tools/mcp/README.md`
- Modify: `.superpowers/sdd/progress.md` (ledger)

- [ ] **Step 1: Protocol reference** — add a "Parametric masks (`parametric_mask_params`)" subsection under the Tier-1 blend section: the derived-enable rule; the polarity XOR rule with the `blendop.cl:204` citation and the `effective_inverted = inverted XOR inclusive` formula; open-end marker semantics; the boost/marker independence note with the GUI-rescale formula for clients; foreign-slot policy; the `combine` mapping; and the Task-4 error table verbatim. Add a "Mask render (`mask_render`)" subsection: `render_preview`'s `show_mask {op, instance}`, grayscale white=full-effect output, the `mask_of` metadata, off/uniform → white, and the `unsupported_field` on non-blending modules.

- [ ] **Step 2: Design-doc amendments** — in the parametric-masks design: (a) status → "implemented (see plan 2026-07-19-darktable-mcp-parametric-masks-tier2.md)"; (b) an "Amendments (implementation)" subsection recording this plan's five Design amendments, especially amendment 1 (export pipe cannot honor `request_mask_display`; resolved via the guarded per-pipe opt-in) with the `blend.c:555` citation.

- [ ] **Step 3: Divergence manifest** — append these rows (create the file with the M-A preamble + M-A rows first if it does not yet exist):

| upstream file | divergence | guard |
|---|---|---|
| `src/develop/blend.c` | `valid_request` gate relaxed to honor `pipe->mask_display_request` on any pipe (remote mask render) | unit `test_pipe_has_mask_display_request_field` + integration mask-image gate |
| `src/develop/pixelpipe.h` / `pixelpipe_hb.c` | new `dt_dev_pixelpipe_t.mask_display_request` field (init/reset FALSE) | same |
| `src/imageio/imageio_common.h` / `imageio.c` | optional `dt_imageio_mask_display_t *` trailing arg on `dt_imageio_export_with_flags` (sets the target module's `request_mask_display` + pipe opt-in) | mask-render integration gates |
| `src/control/remote_blend.c` | engine blendif channel tables mirror the GUI `Lab_channels[]`/`rgb_channels[]`/`rgbj_channels[]` | GUI-parity test `test_channels_parity_*` |

Add a "rehearsed procedure: upstream changes a blendif channel/boost offset" paragraph: update the engine channel table row, rerun `test_channels_parity_*`; the parity test turns silent drift into a build/test failure.

- [ ] **Step 4: Supported operations** — extend the M-A blend sentence: "Parametric (blendif) masks and read-only mask rendering are supported via `parametric_mask_params` and `mask_render`; drawn masks remain out of scope (M-C)."

- [ ] **Step 5: README** — add a "Parametric masks" subsection with the sky-selection worked example and the two sharp-edge warnings, and a "Seeing the mask" note pointing at `render_preview(show_mask=...)`.

- [ ] **Step 6: Full verification stack**

Run all four:
```bash
cmake --build build -j$(nproc)
ctest --test-dir build
cd tools/mcp && .venv/bin/pytest -q && cd ../..
DARKTABLE_BIN=$PWD/build/bin/darktable tools/mcp/.venv/bin/pytest -q -m integration tools/mcp
```
Expected: all green.

- [ ] **Step 7: Ledger + commit**

Append one line to `.superpowers/sdd/progress.md` (task list, final commit, "full stack verified"). Then:
```bash
git add docs/ tools/mcp/README.md .superpowers/sdd/progress.md
git commit -m "docs: parametric masks + mask render (Tier 2 / M-B) -- reference, manifest, amendments"
```
(plus trailers)

---

## Self-review notes (performed while writing)

- **Spec coverage:** `parametric`+`combine` wire members → Tasks 2–4/6; capability `parametric_mask_params` → Tasks 6–7; channel vocabulary per family + derived enable + inverted stored bit + boost exp2 + null reset + foreign preservation → Tasks 1/3/4; `mask_mode` `parametric` transitions → Tasks 1/4; `mask_render` capability + `render_preview.show_mask` + off→white + grayscale + `mask_of` → Tasks 5–8; GUI-parity binding → Task 1; H4 inverted+combine guard + worked examples → Tasks 4/7; H6 ratio gates on the mask image → Task 8; docs/manifest/amendments → Task 9. Design decisions 1–7 land (1 derived-enable Task 1/3/4; 2 stored polarity Task 3; 3 no marker rescale Task 4; 4 whole-slot replace + null Task 4; 5 enabled-only + foreign boolean Task 3; 6 parity-by-test Task 1; 7 zero commit surface — Tasks 1–4 write only the scratch copy).
- **Contradiction found and resolved (flagged in Design amendment 1):** the export path structurally cannot honor `request_mask_display` (`blend.c:555` gate on `full.pipe` + focus). Resolved with a guarded per-pipe opt-in rather than silently assuming the design's export-honors-flag premise.
- **Non-contradiction clarified (amendment 2):** the design's two "offsets" (Lab a/b marker `0.5` vs boost storage `−6.64385619`) are distinct; both are in the GUI tables verbatim (`boost_factor_offset` and the a/b branch of `_blendop_blendif_boost_factor_callback`).
- **Verified TRUE:** the GUI channel tables `Lab_channels[]`/`rgb_channels[]`/`rgbj_channels[]` are non-`static` `const` and directly linkable (blend_gui.c:2340+); the parity test externs them.
- **Type consistency:** `dt_remote_blendif_channel_t` fields, `dt_remote_blendif_slot_pack/enabled/inverted`, `dt_remote_blendif_markers_enable`, `_combine_from_string/_to_string` names are identical across Tasks 1–4; `dt_remote_preview_request_t`/`dt_remote_preview_t` mask fields and the `dt_remote_render_preview_prepare(out, op, instance, error)` signature match across Tasks 5–6.
- Implementers adapting stub-table/test-helper names in Tasks 6–7 must match the existing local idioms in those files (`_assert_dispatch_matches`, `fake_server_factory`, the capability-advertising mechanism) — the plan names them but the neighboring tests are the authority.
