# darktable MCP — Blend Settings (Tier 1 / M-A) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Read and write per-instance blend controls (opacity, blend mode, colorspace, fulcrum, feathering, blur, contrast, brightness, details, off/uniform mask mode) as an atomic extension of `set_module_params`, behind a new `blend_params` hello capability.

**Architecture:** A new pure engine file `src/control/remote_blend.c` holds a hand-written field table over `dt_develop_blend_params_t` (version-guarded against struct churn) plus JSON schema/read/patch-apply helpers. The mode×colorspace sets are extracted from `blend_gui.c`'s combobox population into a shared data table in `blend.c` consumed by both GUI and engine. `dt_remote_set_module_params` grows a blend step that validates into a scratch `dt_develop_blend_params_t` and commits via `dt_iop_commit_blend_params` (the preset-apply idiom) in the same single-history-item transaction.

**Tech Stack:** C (GLib, json-glib, cmocka), Python (FastMCP sidecar, pytest).

**Spec:** `docs/superpowers/specs/2026-07-19-darktable-mcp-blend-settings-design.md`. The mask_mode transition appendix in `docs/superpowers/specs/2026-07-19-darktable-mcp-mask-support-design-candidates.md` is authoritative for mask_mode vocabulary and transitions.

## Global Constraints

- `#if DEVELOP_BLEND_VERSION != 14 #error` guard at the top of `remote_blend.c`, plus `G_STATIC_ASSERT(offsetof(...))` for every field-table entry.
- Capability name: exactly `blend_params`, advertised in `hello`.
- Wire enum values are C enumerator names (`"DEVELOP_BLEND_NORMAL2"`, `"DEVELOP_BLEND_CS_RGB_SCENE"`, `"DEVELOP_MASK_GUIDE_OUT_BEFORE_BLUR"`). Exception: `mask_mode` uses the transition-appendix vocabulary — writable: `"off"`, `"uniform"`; read-only compounds: `"parametric"`, `"drawn"`, `"drawn+parametric"`, `"raster"`.
- Validation order inside the blend object: `mask_mode` → `colorspace` → `mode` → `reverse` → `feathering_guide` → numeric fields (table order). Blend errors rank after quantity errors (blend apply runs after the `s_class_ops[]` loop).
- Writing `colorspace` resets `mode`/`reverse`/`fulcrum`/blendif via `dt_develop_blend_init_blendif_parameters()`; the GUI's history-scavenging restore is deliberately NOT reproduced (design decision 3).
- A blend patch is atomic with the rest of the call and produces exactly ONE history item; it never implicitly enables a disabled module.
- Mutation readback carries the COMPLETE post-commit blend object, not just touched fields.
- Error vocabulary: only existing codes (`unsupported_field` = `DT_REMOTE_ERR_UNSUPPORTED_FIELD`, `invalid_value` = `DT_REMOTE_ERR_INVALID_VALUE`), with `details_json` = `{"parameter": "blend.<name>", "constraint": "<slug>"}`.
- Ranges (from `blend_gui.c:3555-3634`, verbatim): fulcrum hard −18..18 soft −3..3 EV; opacity 0–100 %; feathering_radius 0–250 px; blur_radius 0–100 px; contrast −1..1; brightness −1..1; details −1..1.
- Every commit message ends with BOTH trailers:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_017UPe8bYYZLQ8aasRge4kBP`
- Verification stack: `cmake --build build -j$(nproc)` → `ctest --test-dir build` → `cd tools/mcp && .venv/bin/pytest -q` → (final task only) `DARKTABLE_BIN=$PWD/build/bin/darktable tools/mcp/.venv/bin/pytest -q -m integration tools/mcp`.

## Design amendments (binding; applied to the design doc in Task 9)

1. **Blend schema is live-session data.** The design assumed `get_module_schema` was computed against the live darkroom; it is actually per-op/static (`remote_edit.h`: "from the loaded module .so alone"). Resolution: `get_module_schema` gains an optional `instance` argument (default 0), and the `blend` member appears **only** when darktable is in darkroom with an image and that instance exists; otherwise the member is omitted (no error). `mode.values`, `current_extra_bits`, `details.writable`, and the colorspace choice set are computed against that live instance.
2. **mask_mode read strings follow the transition appendix**, not the design's "`uniform+` prefix" sentence: `off`, `uniform`, `parametric`, `drawn`, `drawn+parametric`, `raster`. (`ENABLED` accompanying mask bits is implied and never spelled.)

## File structure

| File | Responsibility |
|---|---|
| `src/develop/blend.h` / `blend.c` | `dt_develop_blend_mode_section_t` + `dt_develop_blend_mode_sections()` shared table (Task 1) |
| `src/develop/blend_gui.c` | mode-combobox population becomes a loop over the shared table (Task 1) |
| `src/control/remote_blend.h` / `.c` (NEW) | field table, version tripwire, enum-name tables, mask_mode mapping, mode expansion, JSON schema/read/patch-apply (Tasks 2–4) |
| `src/control/remote_edit.h` / `.c` | patch/result struct extensions, mutation-path wiring, live `_for_ref` wrappers (Task 5) |
| `src/control/remote_protocol.h` / `.c` | hello capability, `blend` parse/serialize, schema/params attach, `instance` arg on get_module_schema (Task 6) |
| `src/CMakeLists.txt` | add `control/remote_blend.c` (Task 2) |
| `src/tests/unittests/control/test_remote_blend.c` (NEW) + `CMakeLists.txt` | parity + engine unit tests (Tasks 1–4) |
| `src/tests/unittests/control/test_remote_protocol.c` + `fixtures/*.json` | protocol tests + shared fixtures (Task 6) |
| `tools/mcp/src/darktable_mcp/server.py` + `tools/mcp/tests/test_tools.py` + `test_protocol.py` | `blend` tool argument, capability gate (Task 7) |
| `tools/mcp/tests/integration/test_blend_tier1.py` (NEW) | live gates (Task 8) |
| docs (protocol reference, supported-operations, README, design doc, NEW divergence manifest) | Task 9 |

---

### Task 1: Shared mode×colorspace table + GUI refactor + parity test

**Files:**
- Modify: `src/develop/blend.h` (after the `dt_develop_blendif_channels_t` enum, before `dt_develop_blend_params_t`)
- Modify: `src/develop/blend.c` (near `_default_blendop_params`)
- Modify: `src/develop/blend_gui.c:3126-3191` (the `bd->csp != bd->blend_modes_csp` population block)
- Create: `src/tests/unittests/control/test_remote_blend.c`
- Modify: `src/tests/unittests/control/CMakeLists.txt`

**Interfaces:**
- Produces: `dt_develop_blend_mode_section_t { const char *section; dt_develop_blend_mode_t from, to; }` and `const dt_develop_blend_mode_section_t *dt_develop_blend_mode_sections(dt_develop_blend_colorspace_t csp)` — Task 2's engine consumes these. `from`/`to` are an inclusive range **in `dt_develop_blend_mode_names[]` tuple order** (exactly what `_add_blendmode_combo`/`dt_bauhaus_combobox_add_introspection` takes: scan to the tuple whose value == `from`, add every tuple until value == `to`).

- [ ] **Step 1: Write the failing parity test**

Create `src/tests/unittests/control/test_remote_blend.c` (standard darktable GPL header, then):

```c
/*
 * cmocka unit tests for the Tier-1 blend surface:
 *  - Task 1: GUI-parity of dt_develop_blend_mode_sections() against the
 *    pre-refactor hardcoded combobox population (frozen here as data).
 *  - Tasks 2-4 append their sections to this file.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "develop/blend.h"

#include <glib.h>

// Flattens dt_develop_blend_mode_sections(csp) exactly the way
// dt_bauhaus_combobox_add_introspection() walks dt_develop_blend_mode_names:
// per section row, scan tuples to value==from, append until value==to.
static GArray *_flatten_sections(dt_develop_blend_colorspace_t csp)
{
  GArray *out = g_array_new(FALSE, FALSE, sizeof(int));
  for(const dt_develop_blend_mode_section_t *s = dt_develop_blend_mode_sections(csp);
      s && s->section; s++)
  {
    const dt_introspection_type_enum_tuple_t *item = dt_develop_blend_mode_names;
    while(item->name && item->value != (int)s->from) item++;
    for(; item->name; item++)
    {
      g_array_append_val(out, item->value);
      if(item->value == (int)s->to) break;
    }
  }
  return out;
}

static void _assert_flattened_equals(dt_develop_blend_colorspace_t csp,
                                     const int *expected, guint count)
{
  GArray *got = _flatten_sections(csp);
  assert_int_equal(got->len, count);
  for(guint i = 0; i < count; i++)
    assert_int_equal(g_array_index(got, int, i), expected[i]);
  g_array_unref(got);
}

// Frozen from blend_gui.c's pre-refactor population (this plan's Task 1),
// expanded through dt_develop_blend_mode_names[] tuple order.
static const int EXPECTED_RAW[] = {
  DEVELOP_BLEND_NORMAL2, DEVELOP_BLEND_AVERAGE, DEVELOP_BLEND_DIFFERENCE2,
  DEVELOP_BLEND_BOUNDED,
  DEVELOP_BLEND_LIGHTEN, DEVELOP_BLEND_ADD, DEVELOP_BLEND_SCREEN,
  DEVELOP_BLEND_DARKEN, DEVELOP_BLEND_SUBTRACT, DEVELOP_BLEND_MULTIPLY,
  DEVELOP_BLEND_OVERLAY, DEVELOP_BLEND_SOFTLIGHT, DEVELOP_BLEND_HARDLIGHT,
  DEVELOP_BLEND_VIVIDLIGHT, DEVELOP_BLEND_LINEARLIGHT, DEVELOP_BLEND_PINLIGHT,
};

static const int EXPECTED_LAB[] = {
  DEVELOP_BLEND_NORMAL2, DEVELOP_BLEND_AVERAGE, DEVELOP_BLEND_DIFFERENCE2,
  DEVELOP_BLEND_BOUNDED,
  DEVELOP_BLEND_LIGHTEN, DEVELOP_BLEND_ADD, DEVELOP_BLEND_SCREEN,
  DEVELOP_BLEND_DARKEN, DEVELOP_BLEND_SUBTRACT, DEVELOP_BLEND_MULTIPLY,
  DEVELOP_BLEND_OVERLAY, DEVELOP_BLEND_SOFTLIGHT, DEVELOP_BLEND_HARDLIGHT,
  DEVELOP_BLEND_VIVIDLIGHT, DEVELOP_BLEND_LINEARLIGHT, DEVELOP_BLEND_PINLIGHT,
  DEVELOP_BLEND_LAB_LIGHTNESS, DEVELOP_BLEND_LAB_A, DEVELOP_BLEND_LAB_B,
  DEVELOP_BLEND_LAB_COLOR,
  DEVELOP_BLEND_HUE, DEVELOP_BLEND_COLOR, DEVELOP_BLEND_COLORADJUST,
  DEVELOP_BLEND_LIGHTNESS, DEVELOP_BLEND_CHROMATICITY,
};

static const int EXPECTED_RGB_DISPLAY[] = {
  DEVELOP_BLEND_NORMAL2, DEVELOP_BLEND_AVERAGE, DEVELOP_BLEND_DIFFERENCE2,
  DEVELOP_BLEND_BOUNDED,
  DEVELOP_BLEND_LIGHTEN, DEVELOP_BLEND_ADD, DEVELOP_BLEND_SCREEN,
  DEVELOP_BLEND_DARKEN, DEVELOP_BLEND_SUBTRACT, DEVELOP_BLEND_MULTIPLY,
  DEVELOP_BLEND_OVERLAY, DEVELOP_BLEND_SOFTLIGHT, DEVELOP_BLEND_HARDLIGHT,
  DEVELOP_BLEND_VIVIDLIGHT, DEVELOP_BLEND_LINEARLIGHT, DEVELOP_BLEND_PINLIGHT,
  DEVELOP_BLEND_RGB_R, DEVELOP_BLEND_RGB_G, DEVELOP_BLEND_RGB_B,
  DEVELOP_BLEND_HSV_VALUE, DEVELOP_BLEND_HSV_COLOR,
  DEVELOP_BLEND_HUE, DEVELOP_BLEND_COLOR, DEVELOP_BLEND_COLORADJUST,
  DEVELOP_BLEND_LIGHTNESS, DEVELOP_BLEND_CHROMATICITY,
};

static const int EXPECTED_RGB_SCENE[] = {
  DEVELOP_BLEND_NORMAL2, DEVELOP_BLEND_AVERAGE, DEVELOP_BLEND_DIFFERENCE2,
  DEVELOP_BLEND_MULTIPLY, DEVELOP_BLEND_DIVIDE, DEVELOP_BLEND_ADD,
  DEVELOP_BLEND_SUBTRACT, DEVELOP_BLEND_GEOMETRIC_MEAN, DEVELOP_BLEND_HARMONIC_MEAN,
  DEVELOP_BLEND_RGB_R, DEVELOP_BLEND_RGB_G, DEVELOP_BLEND_RGB_B,
  DEVELOP_BLEND_LIGHTNESS, DEVELOP_BLEND_CHROMATICITY,
};

static void test_mode_sections_parity_raw(void **state)
{ (void)state; _assert_flattened_equals(DEVELOP_BLEND_CS_RAW, EXPECTED_RAW, G_N_ELEMENTS(EXPECTED_RAW)); }
static void test_mode_sections_parity_lab(void **state)
{ (void)state; _assert_flattened_equals(DEVELOP_BLEND_CS_LAB, EXPECTED_LAB, G_N_ELEMENTS(EXPECTED_LAB)); }
static void test_mode_sections_parity_rgb_display(void **state)
{ (void)state; _assert_flattened_equals(DEVELOP_BLEND_CS_RGB_DISPLAY, EXPECTED_RGB_DISPLAY, G_N_ELEMENTS(EXPECTED_RGB_DISPLAY)); }
static void test_mode_sections_parity_rgb_scene(void **state)
{ (void)state; _assert_flattened_equals(DEVELOP_BLEND_CS_RGB_SCENE, EXPECTED_RGB_SCENE, G_N_ELEMENTS(EXPECTED_RGB_SCENE)); }
static void test_mode_sections_none_is_empty(void **state)
{
  (void)state;
  const dt_develop_blend_mode_section_t *s = dt_develop_blend_mode_sections(DEVELOP_BLEND_CS_NONE);
  assert_non_null(s);
  assert_null(s->section);
}
// No mode listed for any colorspace is deprecated/obsolete: assert none of
// the flattened values appears past the "deprecated" marker in
// dt_develop_blend_mode_names (DIFFERENCE, SUBTRACT_INVERSE, DIVIDE_INVERSE,
// LAB_L) nor is an *_OBSOLETE enumerator.
static void test_mode_sections_never_list_deprecated(void **state)
{
  (void)state;
  const int deprecated[] = { DEVELOP_BLEND_DIFFERENCE, DEVELOP_BLEND_SUBTRACT_INVERSE,
                             DEVELOP_BLEND_DIVIDE_INVERSE, DEVELOP_BLEND_LAB_L };
  const dt_develop_blend_colorspace_t spaces[] = { DEVELOP_BLEND_CS_RAW, DEVELOP_BLEND_CS_LAB,
    DEVELOP_BLEND_CS_RGB_DISPLAY, DEVELOP_BLEND_CS_RGB_SCENE };
  for(guint c = 0; c < G_N_ELEMENTS(spaces); c++)
  {
    GArray *got = _flatten_sections(spaces[c]);
    for(guint i = 0; i < got->len; i++)
      for(guint d = 0; d < G_N_ELEMENTS(deprecated); d++)
        assert_int_not_equal(g_array_index(got, int, i), deprecated[d]);
    g_array_unref(got);
  }
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_mode_sections_parity_raw),
    cmocka_unit_test(test_mode_sections_parity_lab),
    cmocka_unit_test(test_mode_sections_parity_rgb_display),
    cmocka_unit_test(test_mode_sections_parity_rgb_scene),
    cmocka_unit_test(test_mode_sections_none_is_empty),
    cmocka_unit_test(test_mode_sections_never_list_deprecated),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
```

Append to `src/tests/unittests/control/CMakeLists.txt` (after the `test_remote_quantity` block, same shape):

```cmake
add_cmocka_test(test_remote_blend
                SOURCES test_remote_blend.c
                LINK_LIBRARIES lib_darktable cmocka)

target_compile_definitions(test_remote_blend PRIVATE
  DT_TEST_MODULEDIR="${CMAKE_BINARY_DIR}/lib/darktable")

if(WIN32)
    _copy_required_library(test_remote_blend lib_darktable)
endif(WIN32)
```

(`DT_TEST_MODULEDIR` is unused in Task 1 but Tasks 3–4 load real module .so's; adding it now avoids touching the CMake block twice.)

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build -j$(nproc) 2>&1 | tail -5`
Expected: compile FAILURE — `dt_develop_blend_mode_section_t` / `dt_develop_blend_mode_sections` undeclared.

- [ ] **Step 3: Declare the type + function in `blend.h`**

Insert after the `dt_develop_blendif_channels_t` enum (blend.h:176), before `dt_develop_blend_params_t`:

```c
/** one section of the blend-mode chooser: a GUI section label plus an
 * inclusive range in dt_develop_blend_mode_names[] TUPLE order (exactly
 * the (start, end) pair dt_bauhaus_combobox_add_introspection() takes).
 * Shared between blend_gui.c's combobox population and the remote-edit
 * engine so the mode x colorspace availability sets cannot drift. */
typedef struct dt_develop_blend_mode_section_t
{
  const char *section;                 // N_()-marked label; GUI shows _(section)
  dt_develop_blend_mode_t from, to;
} dt_develop_blend_mode_section_t;

/** the per-blend-colorspace section table, terminated by {NULL, 0, 0}.
 * DEVELOP_BLEND_CS_NONE (and any unknown value) returns an empty table. */
const dt_develop_blend_mode_section_t *
dt_develop_blend_mode_sections(dt_develop_blend_colorspace_t csp);
```

- [ ] **Step 4: Add the tables to `blend.c`**

Insert after `_default_blendop_params` (blend.c:63):

```c
// The mode x colorspace availability sets, one row per
// _add_blendmode_combo() call in blend_gui.c's pre-refactor population.
// Consecutive rows sharing a section label render under one section header.
#define BLEND_MODE_SECTIONS_COMMON \
  { N_("normal & difference"), DEVELOP_BLEND_NORMAL2, DEVELOP_BLEND_DIFFERENCE2 }, \
  { N_("normal & difference"), DEVELOP_BLEND_BOUNDED, DEVELOP_BLEND_BOUNDED },     \
  { N_("lighten"), DEVELOP_BLEND_LIGHTEN, DEVELOP_BLEND_LIGHTEN },                 \
  { N_("lighten"), DEVELOP_BLEND_ADD, DEVELOP_BLEND_ADD },                         \
  { N_("lighten"), DEVELOP_BLEND_SCREEN, DEVELOP_BLEND_SCREEN },                   \
  { N_("darken"), DEVELOP_BLEND_DARKEN, DEVELOP_BLEND_DARKEN },                    \
  { N_("darken"), DEVELOP_BLEND_SUBTRACT, DEVELOP_BLEND_SUBTRACT },                \
  { N_("darken"), DEVELOP_BLEND_MULTIPLY, DEVELOP_BLEND_MULTIPLY },                \
  { N_("contrast enhancing"), DEVELOP_BLEND_OVERLAY, DEVELOP_BLEND_PINLIGHT }

static const dt_develop_blend_mode_section_t _blend_mode_sections_raw[] = {
  BLEND_MODE_SECTIONS_COMMON,
  { NULL, 0, 0 } };

static const dt_develop_blend_mode_section_t _blend_mode_sections_lab[] = {
  BLEND_MODE_SECTIONS_COMMON,
  { N_("color channel"), DEVELOP_BLEND_LAB_LIGHTNESS, DEVELOP_BLEND_LAB_COLOR },
  { N_("color channel"), DEVELOP_BLEND_HUE, DEVELOP_BLEND_COLORADJUST },
  { N_("chromaticity & lightness"), DEVELOP_BLEND_LIGHTNESS, DEVELOP_BLEND_CHROMATICITY },
  { NULL, 0, 0 } };

static const dt_develop_blend_mode_section_t _blend_mode_sections_rgb_display[] = {
  BLEND_MODE_SECTIONS_COMMON,
  { N_("color channel"), DEVELOP_BLEND_RGB_R, DEVELOP_BLEND_HSV_COLOR },
  { N_("color channel"), DEVELOP_BLEND_HUE, DEVELOP_BLEND_COLORADJUST },
  { N_("chromaticity & lightness"), DEVELOP_BLEND_LIGHTNESS, DEVELOP_BLEND_CHROMATICITY },
  { NULL, 0, 0 } };

static const dt_develop_blend_mode_section_t _blend_mode_sections_rgb_scene[] = {
  { N_("normal & arithmetic"), DEVELOP_BLEND_NORMAL2, DEVELOP_BLEND_DIFFERENCE2 },
  { N_("normal & arithmetic"), DEVELOP_BLEND_MULTIPLY, DEVELOP_BLEND_HARMONIC_MEAN },
  { N_("color channel"), DEVELOP_BLEND_RGB_R, DEVELOP_BLEND_RGB_B },
  { N_("chromaticity & lightness"), DEVELOP_BLEND_LIGHTNESS, DEVELOP_BLEND_CHROMATICITY },
  { NULL, 0, 0 } };

static const dt_develop_blend_mode_section_t _blend_mode_sections_none[] = {
  { NULL, 0, 0 } };

const dt_develop_blend_mode_section_t *
dt_develop_blend_mode_sections(dt_develop_blend_colorspace_t csp)
{
  switch(csp)
  {
    case DEVELOP_BLEND_CS_RAW:         return _blend_mode_sections_raw;
    case DEVELOP_BLEND_CS_LAB:         return _blend_mode_sections_lab;
    case DEVELOP_BLEND_CS_RGB_DISPLAY: return _blend_mode_sections_rgb_display;
    case DEVELOP_BLEND_CS_RGB_SCENE:   return _blend_mode_sections_rgb_scene;
    default:                           return _blend_mode_sections_none;
  }
}
```

- [ ] **Step 5: Refactor `blend_gui.c`'s population**

Replace the entire `if(bd->csp == DEVELOP_BLEND_CS_LAB || ... ) { ... } else if(bd->csp == DEVELOP_BLEND_CS_RGB_SCENE) { ... }` body inside `if(bd->csp != bd->blend_modes_csp)` (blend_gui.c:3130-3189, between `dt_bauhaus_combobox_clear(...)` and `bd->blend_modes_csp = bd->csp;`) with:

```c
    const char *prev_section = NULL;
    for(const dt_develop_blend_mode_section_t *s = dt_develop_blend_mode_sections(bd->csp);
        s && s->section; s++)
    {
      if(g_strcmp0(prev_section, s->section))
        dt_bauhaus_combobox_add_section(bd->blend_modes_combo, _(s->section));
      prev_section = s->section;
      _add_blendmode_combo(bd->blend_modes_combo, s->from, s->to);
    }
```

- [ ] **Step 6: Build and run the test**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R test_remote_blend --output-on-failure`
Expected: PASS (6 tests). Then run the full suite: `ctest --test-dir build` — expected 17/17 (16 existing + new).

- [ ] **Step 7: Commit**

```bash
git add src/develop/blend.h src/develop/blend.c src/develop/blend_gui.c \
        src/tests/unittests/control/test_remote_blend.c src/tests/unittests/control/CMakeLists.txt
git commit -m "refactor: extract blend mode x colorspace sets into a shared table

blend_gui.c's combobox population becomes a loop over
dt_develop_blend_mode_sections(); a GUI-parity test freezes the
pre-refactor mode lists per colorspace. Groundwork for the remote-edit
blend surface (Tier 1 / M-A)."
```
(with the two Global Constraints trailers appended)

---

### Task 2: `remote_blend.c` core — field table, tripwire, name tables, mask_mode mapping, mode expansion

**Files:**
- Create: `src/control/remote_blend.h`, `src/control/remote_blend.c`
- Modify: `src/CMakeLists.txt` (add `"control/remote_blend.c"` to the source list next to `"control/remote_edit.c"`)
- Test: `src/tests/unittests/control/test_remote_blend.c` (append)

**Interfaces:**
- Consumes: `dt_develop_blend_mode_sections()` (Task 1), `dt_develop_blend_mode_names[]` (blend_gui.c, extern via blend.h), `dt_remote_error_t` (remote_edit.h).
- Produces (for Tasks 3–5):
  - `const char *dt_remote_blend_mask_mode_string(uint32_t mask_mode);`
  - `gboolean dt_remote_blend_mask_mode_from_string(const char *s, uint32_t *out);` (accepts only `"off"`/`"uniform"`)
  - `GPtrArray *dt_remote_blend_mode_names_for_colorspace(dt_develop_blend_colorspace_t csp);` (elements are static `const char *` C names; free with `g_ptr_array_unref` only)
  - `dt_develop_blend_colorspace_t dt_remote_blend_effective_colorspace(struct dt_iop_module_t *module, int32_t stored_cst);`
  - internal (file-scope, exercised through Tasks 3–4's public functions): enum-name tables `_colorspace_names`, `_feathering_guide_names`, `_mode_c_names`; float field table `_float_fields`.

- [ ] **Step 1: Write the failing tests**

Append to `test_remote_blend.c` (new section; add `#include "control/remote_blend.h"` at the top):

```c
/* ------------------------------------------------------------------ */
/* Task 2: mask_mode mapping + mode expansion                          */
/* ------------------------------------------------------------------ */

static void test_mask_mode_string_mapping(void **state)
{
  (void)state;
  assert_string_equal(dt_remote_blend_mask_mode_string(DEVELOP_MASK_DISABLED), "off");
  assert_string_equal(dt_remote_blend_mask_mode_string(DEVELOP_MASK_ENABLED), "uniform");
  assert_string_equal(dt_remote_blend_mask_mode_string(DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL),
                      "parametric");
  assert_string_equal(dt_remote_blend_mask_mode_string(DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK),
                      "drawn");
  assert_string_equal(dt_remote_blend_mask_mode_string(DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK_CONDITIONAL),
                      "drawn+parametric");
  assert_string_equal(dt_remote_blend_mask_mode_string(DEVELOP_MASK_ENABLED | DEVELOP_MASK_RASTER),
                      "raster");
  // raster wins over any other bit combination
  assert_string_equal(dt_remote_blend_mask_mode_string(
                        DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK | DEVELOP_MASK_RASTER),
                      "raster");
}

static void test_mask_mode_from_string(void **state)
{
  (void)state;
  uint32_t v = 999;
  assert_true(dt_remote_blend_mask_mode_from_string("off", &v));
  assert_int_equal(v, DEVELOP_MASK_DISABLED);
  assert_true(dt_remote_blend_mask_mode_from_string("uniform", &v));
  assert_int_equal(v, DEVELOP_MASK_ENABLED);
  // read-only compounds and junk are rejected
  assert_false(dt_remote_blend_mask_mode_from_string("drawn", &v));
  assert_false(dt_remote_blend_mask_mode_from_string("parametric", &v));
  assert_false(dt_remote_blend_mask_mode_from_string("raster", &v));
  assert_false(dt_remote_blend_mask_mode_from_string("", &v));
  assert_false(dt_remote_blend_mask_mode_from_string(NULL, &v));
}

static void test_mode_names_for_colorspace_matches_sections(void **state)
{
  (void)state;
  // spot-check RGB scene: first name, arithmetic run, count matches the
  // Task-1 frozen expectation (14 modes)
  GPtrArray *names = dt_remote_blend_mode_names_for_colorspace(DEVELOP_BLEND_CS_RGB_SCENE);
  assert_int_equal(names->len, 14);
  assert_string_equal(g_ptr_array_index(names, 0), "DEVELOP_BLEND_NORMAL2");
  assert_string_equal(g_ptr_array_index(names, 3), "DEVELOP_BLEND_MULTIPLY");
  assert_string_equal(g_ptr_array_index(names, 8), "DEVELOP_BLEND_HARMONIC_MEAN");
  assert_string_equal(g_ptr_array_index(names, 13), "DEVELOP_BLEND_CHROMATICITY");
  g_ptr_array_unref(names);

  GPtrArray *lab = dt_remote_blend_mode_names_for_colorspace(DEVELOP_BLEND_CS_LAB);
  assert_int_equal(lab->len, 25);
  assert_string_equal(g_ptr_array_index(lab, 16), "DEVELOP_BLEND_LAB_LIGHTNESS");
  g_ptr_array_unref(lab);

  GPtrArray *none = dt_remote_blend_mode_names_for_colorspace(DEVELOP_BLEND_CS_NONE);
  assert_int_equal(none->len, 0);
  g_ptr_array_unref(none);
}
```

Register the three tests in `main()`'s array.

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build -j$(nproc) 2>&1 | tail -5`
Expected: FAIL — `control/remote_blend.h` not found.

- [ ] **Step 3: Write `remote_blend.h`**

```c
/* (darktable GPL header, Copyright (C) 2026) */

// Tier-1 blend surface for the darktable MCP remote-edit engine: a
// hand-written mini-introspection over dt_develop_blend_params_t (the
// struct is NOT introspected) plus pure JSON schema/read/patch helpers.
// Unlike remote_edit.h's neutral-type boundary, this layer deliberately
// speaks json-glib: the blend object's wire shape is bespoke (see the
// blend-settings design doc SS Wire contract), and both producers and the
// single consumer (remote_protocol.c) already live in JSON space.
//
// Threading: every function taking a dt_iop_module_t must run on the GTK
// main thread (same rule as remote_edit.h). The pure string/table helpers
// have no such constraint.

#pragma once

#include "control/remote_edit.h"
#include "develop/blend.h"

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

struct dt_iop_module_t;

/** stored mask_mode bitfield -> transition-appendix vocabulary:
 * "off", "uniform", "parametric", "drawn", "drawn+parametric", "raster"
 * (raster wins whenever the RASTER bit is set). Returns a static string. */
const char *dt_remote_blend_mask_mode_string(uint32_t mask_mode);

/** Tier-1 writable vocabulary only: "off" -> DEVELOP_MASK_DISABLED,
 * "uniform" -> DEVELOP_MASK_ENABLED. Anything else (including the
 * read-only compound strings) returns FALSE with *out untouched. */
gboolean dt_remote_blend_mask_mode_from_string(const char *s, uint32_t *out);

/** resolves a stored blend_cst to the effective blending colorspace:
 * a concrete stored space is returned verbatim; DEVELOP_BLEND_CS_NONE
 * (or garbage) resolves through
 * dt_develop_blend_default_module_blend_colorspace(module). */
dt_develop_blend_colorspace_t
dt_remote_blend_effective_colorspace(struct dt_iop_module_t *module, int32_t stored_cst);

/** the writable mode set for `csp`, as C enumerator names, in GUI order
 * (dt_develop_blend_mode_sections() expanded through
 * dt_develop_blend_mode_names[] tuple order -- the exact combobox
 * contents). Elements are static strings; caller frees only the array. */
GPtrArray *dt_remote_blend_mode_names_for_colorspace(dt_develop_blend_colorspace_t csp);

/** the per-instance blend schema object (design SS Schema), or NULL when
 * `module` is NULL or lacks IOP_FLAGS_SUPPORTS_BLENDING. Caller owns the
 * returned node (json_node_unref). */
JsonNode *dt_remote_blend_schema(struct dt_iop_module_t *module);

/** the current blend values object (design SS Read), or NULL when
 * `module` is NULL or lacks blending. Caller owns the node. */
JsonNode *dt_remote_blend_read(struct dt_iop_module_t *module);

/** validates `patch` (the request's "blend" member) and applies it to
 * `dst`, a caller-owned scratch copy of the live blend params. Never
 * touches the module. On failure returns FALSE with *error allocated
 * (DT_REMOTE_ERR_UNSUPPORTED_FIELD for unknown members / non-blending is
 * checked by the caller; DT_REMOTE_ERR_INVALID_VALUE otherwise, with
 * details_json {"parameter": "blend.<name>", "constraint": ...}) and
 * `dst` possibly partially written -- callers must treat `dst` as
 * poisoned on failure (the mutation path discards its scratch copy). */
gboolean dt_remote_blend_patch_apply(struct dt_iop_module_t *module,
                                     JsonObject *patch,
                                     dt_develop_blend_params_t *dst,
                                     dt_remote_error_t **error);

G_END_DECLS
```

- [ ] **Step 4: Write `remote_blend.c` (core half)**

```c
/* (darktable GPL header) */

#include "control/remote_blend.h"

#include "common/darktable.h"
#include "common/image.h"
#include "develop/develop.h"
#include "develop/imageop.h"

#include <math.h>
#include <string.h>

// Struct-churn tripwire: a DEVELOP_BLEND_VERSION bump means
// dt_develop_blend_params_t changed shape or meaning. Re-audit the field
// table below AND the wire contract (protocol reference SS Blend settings)
// before raising this guard. NOTE the guard covers layout only -- a new
// blend mode or a changed slider range bumps nothing; those are caught by
// the GUI-parity test (mode sets) and this file's range table review.
#if DEVELOP_BLEND_VERSION != 14
#error "dt_develop_blend_params_t changed: re-audit remote_blend.c's field table and the wire contract"
#endif

G_STATIC_ASSERT(offsetof(dt_develop_blend_params_t, mask_mode) == 0);
G_STATIC_ASSERT(offsetof(dt_develop_blend_params_t, blend_cst) == 4);
G_STATIC_ASSERT(offsetof(dt_develop_blend_params_t, blend_mode) == 8);
G_STATIC_ASSERT(offsetof(dt_develop_blend_params_t, blend_parameter) == 12);
G_STATIC_ASSERT(offsetof(dt_develop_blend_params_t, opacity) == 16);
G_STATIC_ASSERT(offsetof(dt_develop_blend_params_t, mask_combine) == 20);
G_STATIC_ASSERT(offsetof(dt_develop_blend_params_t, blendif) == 28);
G_STATIC_ASSERT(offsetof(dt_develop_blend_params_t, feathering_radius) == 32);
G_STATIC_ASSERT(offsetof(dt_develop_blend_params_t, feathering_guide) == 36);
G_STATIC_ASSERT(offsetof(dt_develop_blend_params_t, blur_radius) == 40);
G_STATIC_ASSERT(offsetof(dt_develop_blend_params_t, contrast) == 44);
G_STATIC_ASSERT(offsetof(dt_develop_blend_params_t, brightness) == 48);
G_STATIC_ASSERT(offsetof(dt_develop_blend_params_t, details) == 52);

/* ------------------------------------------------------------------ */
/* enum name tables (wire = C enumerator names)                        */
/* ------------------------------------------------------------------ */

typedef struct _enum_name_t
{
  const char *name;
  uint32_t value;
} _enum_name_t;

static const _enum_name_t _colorspace_names[] = {
  { "DEVELOP_BLEND_CS_NONE", DEVELOP_BLEND_CS_NONE },
  { "DEVELOP_BLEND_CS_RAW", DEVELOP_BLEND_CS_RAW },
  { "DEVELOP_BLEND_CS_LAB", DEVELOP_BLEND_CS_LAB },
  { "DEVELOP_BLEND_CS_RGB_DISPLAY", DEVELOP_BLEND_CS_RGB_DISPLAY },
  { "DEVELOP_BLEND_CS_RGB_SCENE", DEVELOP_BLEND_CS_RGB_SCENE },
  { NULL, 0 } };

static const _enum_name_t _feathering_guide_names[] = {
  { "DEVELOP_MASK_GUIDE_OUT_BEFORE_BLUR", DEVELOP_MASK_GUIDE_OUT_BEFORE_BLUR },
  { "DEVELOP_MASK_GUIDE_IN_BEFORE_BLUR", DEVELOP_MASK_GUIDE_IN_BEFORE_BLUR },
  { "DEVELOP_MASK_GUIDE_OUT_AFTER_BLUR", DEVELOP_MASK_GUIDE_OUT_AFTER_BLUR },
  { "DEVELOP_MASK_GUIDE_IN_AFTER_BLUR", DEVELOP_MASK_GUIDE_IN_AFTER_BLUR },
  { NULL, 0 } };

// Every dt_develop_blend_mode_t enumerator (minus REVERSE/MODE_MASK):
// reads report a stored deprecated/obsolete mode by its real name; writes
// are restricted to the per-colorspace section expansion.
static const _enum_name_t _mode_c_names[] = {
  { "DEVELOP_BLEND_DISABLED_OBSOLETE", DEVELOP_BLEND_DISABLED_OBSOLETE },
  { "DEVELOP_BLEND_NORMAL_OBSOLETE", DEVELOP_BLEND_NORMAL_OBSOLETE },
  { "DEVELOP_BLEND_LIGHTEN", DEVELOP_BLEND_LIGHTEN },
  { "DEVELOP_BLEND_DARKEN", DEVELOP_BLEND_DARKEN },
  { "DEVELOP_BLEND_MULTIPLY", DEVELOP_BLEND_MULTIPLY },
  { "DEVELOP_BLEND_AVERAGE", DEVELOP_BLEND_AVERAGE },
  { "DEVELOP_BLEND_ADD", DEVELOP_BLEND_ADD },
  { "DEVELOP_BLEND_SUBTRACT", DEVELOP_BLEND_SUBTRACT },
  { "DEVELOP_BLEND_DIFFERENCE", DEVELOP_BLEND_DIFFERENCE },
  { "DEVELOP_BLEND_SCREEN", DEVELOP_BLEND_SCREEN },
  { "DEVELOP_BLEND_OVERLAY", DEVELOP_BLEND_OVERLAY },
  { "DEVELOP_BLEND_SOFTLIGHT", DEVELOP_BLEND_SOFTLIGHT },
  { "DEVELOP_BLEND_HARDLIGHT", DEVELOP_BLEND_HARDLIGHT },
  { "DEVELOP_BLEND_VIVIDLIGHT", DEVELOP_BLEND_VIVIDLIGHT },
  { "DEVELOP_BLEND_LINEARLIGHT", DEVELOP_BLEND_LINEARLIGHT },
  { "DEVELOP_BLEND_PINLIGHT", DEVELOP_BLEND_PINLIGHT },
  { "DEVELOP_BLEND_LIGHTNESS", DEVELOP_BLEND_LIGHTNESS },
  { "DEVELOP_BLEND_CHROMATICITY", DEVELOP_BLEND_CHROMATICITY },
  { "DEVELOP_BLEND_HUE", DEVELOP_BLEND_HUE },
  { "DEVELOP_BLEND_COLOR", DEVELOP_BLEND_COLOR },
  { "DEVELOP_BLEND_INVERSE_OBSOLETE", DEVELOP_BLEND_INVERSE_OBSOLETE },
  { "DEVELOP_BLEND_UNBOUNDED_OBSOLETE", DEVELOP_BLEND_UNBOUNDED_OBSOLETE },
  { "DEVELOP_BLEND_COLORADJUST", DEVELOP_BLEND_COLORADJUST },
  { "DEVELOP_BLEND_DIFFERENCE2", DEVELOP_BLEND_DIFFERENCE2 },
  { "DEVELOP_BLEND_NORMAL2", DEVELOP_BLEND_NORMAL2 },
  { "DEVELOP_BLEND_BOUNDED", DEVELOP_BLEND_BOUNDED },
  { "DEVELOP_BLEND_LAB_LIGHTNESS", DEVELOP_BLEND_LAB_LIGHTNESS },
  { "DEVELOP_BLEND_LAB_COLOR", DEVELOP_BLEND_LAB_COLOR },
  { "DEVELOP_BLEND_HSV_VALUE", DEVELOP_BLEND_HSV_VALUE },
  { "DEVELOP_BLEND_HSV_COLOR", DEVELOP_BLEND_HSV_COLOR },
  { "DEVELOP_BLEND_LAB_L", DEVELOP_BLEND_LAB_L },
  { "DEVELOP_BLEND_LAB_A", DEVELOP_BLEND_LAB_A },
  { "DEVELOP_BLEND_LAB_B", DEVELOP_BLEND_LAB_B },
  { "DEVELOP_BLEND_RGB_R", DEVELOP_BLEND_RGB_R },
  { "DEVELOP_BLEND_RGB_G", DEVELOP_BLEND_RGB_G },
  { "DEVELOP_BLEND_RGB_B", DEVELOP_BLEND_RGB_B },
  { "DEVELOP_BLEND_MULTIPLY_REVERSE_OBSOLETE", DEVELOP_BLEND_MULTIPLY_REVERSE_OBSOLETE },
  { "DEVELOP_BLEND_SUBTRACT_INVERSE", DEVELOP_BLEND_SUBTRACT_INVERSE },
  { "DEVELOP_BLEND_DIVIDE", DEVELOP_BLEND_DIVIDE },
  { "DEVELOP_BLEND_DIVIDE_INVERSE", DEVELOP_BLEND_DIVIDE_INVERSE },
  { "DEVELOP_BLEND_GEOMETRIC_MEAN", DEVELOP_BLEND_GEOMETRIC_MEAN },
  { "DEVELOP_BLEND_HARMONIC_MEAN", DEVELOP_BLEND_HARMONIC_MEAN },
  { NULL, 0 } };

static const char *_enum_name_for_value(const _enum_name_t *table, uint32_t value)
{
  for(const _enum_name_t *e = table; e->name; e++)
    if(e->value == value) return e->name;
  return NULL;
}

static gboolean _enum_value_for_name(const _enum_name_t *table, const char *name, uint32_t *out)
{
  if(!name) return FALSE;
  for(const _enum_name_t *e = table; e->name; e++)
    if(!strcmp(e->name, name)) { *out = e->value; return TRUE; }
  return FALSE;
}

/* ------------------------------------------------------------------ */
/* float field table                                                   */
/* ------------------------------------------------------------------ */

typedef struct _float_field_t
{
  const char *name;
  size_t offset;
  float min, max;
  float soft_min, soft_max;  // reported iff soft_min != soft_max
  const char *unit;          // NULL when none
} _float_field_t;

// Ranges frozen from blend_gui.c's slider construction (Tier-1 design
// SS Verified source facts); "details" carries the raw-image write gate.
static const _float_field_t _float_fields[] = {
  { "fulcrum", offsetof(dt_develop_blend_params_t, blend_parameter), -18.0f, 18.0f, -3.0f, 3.0f, "EV" },
  { "opacity", offsetof(dt_develop_blend_params_t, opacity), 0.0f, 100.0f, 0.0f, 0.0f, "%" },
  { "feathering_radius", offsetof(dt_develop_blend_params_t, feathering_radius), 0.0f, 250.0f, 0.0f, 0.0f, "px" },
  { "blur_radius", offsetof(dt_develop_blend_params_t, blur_radius), 0.0f, 100.0f, 0.0f, 0.0f, "px" },
  { "contrast", offsetof(dt_develop_blend_params_t, contrast), -1.0f, 1.0f, 0.0f, 0.0f, NULL },
  { "brightness", offsetof(dt_develop_blend_params_t, brightness), -1.0f, 1.0f, 0.0f, 0.0f, NULL },
  { "details", offsetof(dt_develop_blend_params_t, details), -1.0f, 1.0f, 0.0f, 0.0f, NULL },
  { NULL, 0, 0, 0, 0, 0, NULL } };

/* ------------------------------------------------------------------ */
/* pure helpers                                                        */
/* ------------------------------------------------------------------ */

const char *dt_remote_blend_mask_mode_string(uint32_t mask_mode)
{
  if(mask_mode & DEVELOP_MASK_RASTER) return "raster";
  const gboolean drawn = (mask_mode & DEVELOP_MASK_MASK) != 0;
  const gboolean parametric = (mask_mode & DEVELOP_MASK_CONDITIONAL) != 0;
  if(drawn && parametric) return "drawn+parametric";
  if(drawn) return "drawn";
  if(parametric) return "parametric";
  return (mask_mode & DEVELOP_MASK_ENABLED) ? "uniform" : "off";
}

gboolean dt_remote_blend_mask_mode_from_string(const char *s, uint32_t *out)
{
  if(!s) return FALSE;
  if(!strcmp(s, "off")) { *out = DEVELOP_MASK_DISABLED; return TRUE; }
  if(!strcmp(s, "uniform")) { *out = DEVELOP_MASK_ENABLED; return TRUE; }
  return FALSE;
}

dt_develop_blend_colorspace_t
dt_remote_blend_effective_colorspace(struct dt_iop_module_t *module, int32_t stored_cst)
{
  switch(stored_cst)
  {
    case DEVELOP_BLEND_CS_RAW:
    case DEVELOP_BLEND_CS_LAB:
    case DEVELOP_BLEND_CS_RGB_DISPLAY:
    case DEVELOP_BLEND_CS_RGB_SCENE:
      return stored_cst;
    default:
      return dt_develop_blend_default_module_blend_colorspace(module);
  }
}

GPtrArray *dt_remote_blend_mode_names_for_colorspace(dt_develop_blend_colorspace_t csp)
{
  GPtrArray *out = g_ptr_array_new();
  for(const dt_develop_blend_mode_section_t *s = dt_develop_blend_mode_sections(csp);
      s && s->section; s++)
  {
    const dt_introspection_type_enum_tuple_t *item = dt_develop_blend_mode_names;
    while(item->name && item->value != (int)s->from) item++;
    for(; item->name; item++)
    {
      const char *cname = _enum_name_for_value(_mode_c_names, (uint32_t)item->value);
      if(cname) g_ptr_array_add(out, (gpointer)cname);
      if(item->value == (int)s->to) break;
    }
  }
  return out;
}
```

- [ ] **Step 5: Register in `src/CMakeLists.txt`**

Add `"control/remote_blend.c"` to the source list, alphabetically between `"control/remote_band_registry.c"` and `"control/remote_curve.c"`.

- [ ] **Step 6: Build and run**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R test_remote_blend --output-on-failure`
Expected: PASS (9 tests).

- [ ] **Step 7: Commit**

```bash
git add src/control/remote_blend.h src/control/remote_blend.c src/CMakeLists.txt \
        src/tests/unittests/control/test_remote_blend.c
git commit -m "feat: remote blend core -- field table, version tripwire, name tables

remote_blend.c's hand-written mini-introspection over
dt_develop_blend_params_t: DEVELOP_BLEND_VERSION guard + offsetof
asserts, C-enumerator wire-name tables, transition-appendix mask_mode
string mapping, and the per-colorspace writable-mode expansion over the
shared section table."
```
(plus trailers)

---

### Task 3: `dt_remote_blend_schema` + `dt_remote_blend_read`

**Files:**
- Modify: `src/control/remote_blend.c` (append)
- Test: `src/tests/unittests/control/test_remote_blend.c` (append)

**Interfaces:**
- Consumes: Task 2's tables/helpers; the module-loading fixture idiom from `test_remote_quantity.c:212-325` (dt_init group setup + `dt_iop_load_module` on a `dt_dev_init`'d dev, `gui_attached = FALSE`).
- Produces: working `dt_remote_blend_schema()` / `dt_remote_blend_read()` for Tasks 5–6.

- [ ] **Step 1: Write the failing tests**

Append to `test_remote_blend.c`. First add the harness includes and fixture (copy the idiom from `test_remote_quantity.c` — group setup boots `dt_init` with a scratch confdir, `--library :memory:`, `--moduledir DT_TEST_MODULEDIR`; fixture loads a real module):

```c
#include "common/darktable.h"
#include "develop/develop.h"
#include "develop/imageop.h"

static char *s_blend_confdir = NULL;

static int harness_group_setup(void **state)
{
  (void)state;
  GError *gerror = NULL;
  s_blend_confdir = g_dir_make_tmp("test_remote_blend-XXXXXX", &gerror);
  if(!s_blend_confdir)
  {
    fprintf(stderr, "test_remote_blend: scratch confdir failed: %s\n", gerror->message);
    g_error_free(gerror);
    return -1;
  }
  char *argv_override[] = {
    "test_remote_blend",
    "--configdir", s_blend_confdir,
    "--library", ":memory:",
    "--moduledir", DT_TEST_MODULEDIR,
    "--conf", "write_sidecar_files=never",
    NULL
  };
  int argc_override = G_N_ELEMENTS(argv_override) - 1;
  return dt_init(argc_override, argv_override, FALSE, FALSE, NULL) ? -1 : 0;
}

static int harness_group_teardown(void **state)
{
  (void)state;
  dt_cleanup();
  if(s_blend_confdir)
  {
    gchar *cmd = g_strdup_printf("rm -rf '%s'", s_blend_confdir);
    if(system(cmd) != 0)
      fprintf(stderr, "test_remote_blend: could not remove %s\n", s_blend_confdir);
    g_free(cmd);
    g_clear_pointer(&s_blend_confdir, g_free);
  }
  return 0;
}

typedef struct blend_fixture_t
{
  dt_develop_t dev;
  dt_iop_module_t *module;
} blend_fixture_t;

static blend_fixture_t *blend_fixture_new(const char *op)
{
  blend_fixture_t *fixture = g_new0(blend_fixture_t, 1);
  dt_dev_init(&fixture->dev, TRUE);
  fixture->dev.gui_attached = FALSE;
  fixture->module = g_malloc0(sizeof(dt_iop_module_t));
  dt_iop_module_so_t *so = dt_iop_get_module_so(op);
  assert_non_null(so);
  assert_false(dt_iop_load_module(fixture->module, so, &fixture->dev));
  memcpy(fixture->module->params, fixture->module->default_params, fixture->module->params_size);
  fixture->dev.iop = g_list_append(fixture->dev.iop, fixture->module);
  return fixture;
}

static void blend_fixture_free(blend_fixture_t *fixture)
{
  if(!fixture) return;
  dt_dev_cleanup(&fixture->dev);
  g_free(fixture);
}

static JsonObject *_node_object(JsonNode *node)
{
  assert_non_null(node);
  assert_true(JSON_NODE_HOLDS_OBJECT(node));
  return json_node_get_object(node);
}
```

Then the tests:

```c
// "exposure" supports blending (RGB module); "rawprepare" does not.
static void test_schema_null_for_non_blending_module(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("rawprepare");
  assert_null(dt_remote_blend_schema(fx->module));
  assert_null(dt_remote_blend_read(fx->module));
  blend_fixture_free(fx);
}

static void test_schema_shape_for_rgb_module(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  JsonNode *node = dt_remote_blend_schema(fx->module);
  JsonObject *schema = _node_object(node);

  // mask_mode: writable off/uniform, no extra bits on defaults
  JsonObject *mm = json_object_get_object_member(schema, "mask_mode");
  assert_true(json_object_get_boolean_member(mm, "writable"));
  assert_false(json_object_get_boolean_member(mm, "current_extra_bits"));
  JsonArray *mm_values = json_object_get_array_member(mm, "values");
  assert_int_equal(json_array_get_length(mm_values), 2);
  assert_string_equal(json_array_get_string_element(mm_values, 0), "off");
  assert_string_equal(json_array_get_string_element(mm_values, 1), "uniform");

  // colorspace: exposure defaults to an RGB space -> choice set is the two
  // RGB spaces, Lab absent, writable
  JsonObject *cs = json_object_get_object_member(schema, "colorspace");
  assert_true(json_object_get_boolean_member(cs, "writable"));
  JsonArray *cs_values = json_object_get_array_member(cs, "values");
  assert_int_equal(json_array_get_length(cs_values), 2);
  assert_string_equal(json_array_get_string_element(cs_values, 0), "DEVELOP_BLEND_CS_RGB_DISPLAY");
  assert_string_equal(json_array_get_string_element(cs_values, 1), "DEVELOP_BLEND_CS_RGB_SCENE");
  const char *cs_default = json_object_get_string_member(cs, "default");
  assert_true(!strcmp(cs_default, "DEVELOP_BLEND_CS_RGB_DISPLAY")
              || !strcmp(cs_default, "DEVELOP_BLEND_CS_RGB_SCENE"));

  // mode values match the effective colorspace's expansion
  JsonObject *mode = json_object_get_object_member(schema, "mode");
  const dt_develop_blend_colorspace_t eff =
    dt_remote_blend_effective_colorspace(fx->module, fx->module->blend_params->blend_cst);
  GPtrArray *expected = dt_remote_blend_mode_names_for_colorspace(eff);
  JsonArray *mode_values = json_object_get_array_member(mode, "values");
  assert_int_equal(json_array_get_length(mode_values), expected->len);
  for(guint i = 0; i < expected->len; i++)
    assert_string_equal(json_array_get_string_element(mode_values, i),
                        g_ptr_array_index(expected, i));
  g_ptr_array_unref(expected);

  // fulcrum carries hard + soft range and EV unit
  JsonObject *fulcrum = json_object_get_object_member(schema, "fulcrum");
  JsonArray *range = json_object_get_array_member(fulcrum, "range");
  assert_int_equal((int)json_array_get_double_element(range, 0), -18);
  assert_int_equal((int)json_array_get_double_element(range, 1), 18);
  JsonArray *soft = json_object_get_array_member(fulcrum, "soft_range");
  assert_int_equal((int)json_array_get_double_element(soft, 0), -3);
  assert_string_equal(json_object_get_string_member(fulcrum, "unit"), "EV");

  // details: no image in this fixture -> not writable
  JsonObject *details = json_object_get_object_member(schema, "details");
  assert_false(json_object_get_boolean_member(details, "writable"));

  // feathering_guide: the four C names
  JsonObject *fg = json_object_get_object_member(schema, "feathering_guide");
  assert_int_equal(json_array_get_length(json_object_get_array_member(fg, "values")), 4);

  json_node_unref(node);
  blend_fixture_free(fx);
}

static void test_schema_mask_mode_not_writable_with_extra_bits(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  fx->module->blend_params->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL;
  JsonNode *node = dt_remote_blend_schema(fx->module);
  JsonObject *mm = json_object_get_object_member(_node_object(node), "mask_mode");
  assert_false(json_object_get_boolean_member(mm, "writable"));
  assert_true(json_object_get_boolean_member(mm, "current_extra_bits"));
  json_node_unref(node);
  blend_fixture_free(fx);
}

static void test_read_defaults(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  JsonNode *node = dt_remote_blend_read(fx->module);
  JsonObject *o = _node_object(node);
  assert_string_equal(json_object_get_string_member(o, "mask_mode"), "off");
  assert_string_equal(json_object_get_string_member(o, "colorspace"), "DEVELOP_BLEND_CS_NONE");
  const char *eff = json_object_get_string_member(o, "effective_colorspace");
  assert_true(!strcmp(eff, "DEVELOP_BLEND_CS_RGB_DISPLAY") || !strcmp(eff, "DEVELOP_BLEND_CS_RGB_SCENE"));
  assert_string_equal(json_object_get_string_member(o, "mode"), "DEVELOP_BLEND_NORMAL2");
  assert_false(json_object_get_boolean_member(o, "reverse"));
  assert_float_equal(json_object_get_double_member(o, "opacity"), 100.0, 1e-6);
  assert_float_equal(json_object_get_double_member(o, "fulcrum"), 0.0, 1e-6);
  assert_string_equal(json_object_get_string_member(o, "feathering_guide"),
                      "DEVELOP_MASK_GUIDE_IN_AFTER_BLUR");
  json_node_unref(node);
  blend_fixture_free(fx);
}

static void test_read_reverse_and_deprecated_mode(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  fx->module->blend_params->blend_mode = DEVELOP_BLEND_LAB_L | DEVELOP_BLEND_REVERSE;
  JsonNode *node = dt_remote_blend_read(fx->module);
  JsonObject *o = _node_object(node);
  assert_string_equal(json_object_get_string_member(o, "mode"), "DEVELOP_BLEND_LAB_L");
  assert_true(json_object_get_boolean_member(o, "reverse"));
  json_node_unref(node);
  blend_fixture_free(fx);
}

static void test_read_non_finite_serializes_null(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  fx->module->blend_params->opacity = NAN;
  JsonNode *node = dt_remote_blend_read(fx->module);
  JsonObject *o = _node_object(node);
  assert_true(json_object_get_null_member(o, "opacity"));
  json_node_unref(node);
  blend_fixture_free(fx);
}
```

Change `main()` to run the whole suite under the harness: `return cmocka_run_group_tests(tests, harness_group_setup, harness_group_teardown);` (the Task 1–2 pure tests run unchanged under the booted harness).

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build -j$(nproc) 2>&1 | tail -5`
Expected: link FAILURE — `dt_remote_blend_schema` / `dt_remote_blend_read` undefined.

- [ ] **Step 3: Implement schema + read in `remote_blend.c`**

Append:

```c
/* ------------------------------------------------------------------ */
/* schema + read                                                       */
/* ------------------------------------------------------------------ */

// choice set per the GUI colorspace menu (blend_gui.c:2029-2090): modules
// whose DEFAULT space is Lab / RGB-display / RGB-scene may choose; Lab is
// offered only to Lab-default modules; RAW-default (and CS_NONE-default)
// modules have no choice. Returns the number of choices written to
// `choices` (sized >= 3) and sets *writable.
static guint _colorspace_choices(dt_iop_module_t *module,
                                 dt_develop_blend_colorspace_t *choices,
                                 gboolean *writable)
{
  const dt_develop_blend_colorspace_t def =
    dt_develop_blend_default_module_blend_colorspace(module);
  guint n = 0;
  if(def == DEVELOP_BLEND_CS_LAB
     || def == DEVELOP_BLEND_CS_RGB_DISPLAY
     || def == DEVELOP_BLEND_CS_RGB_SCENE)
  {
    *writable = TRUE;
    if(def == DEVELOP_BLEND_CS_LAB) choices[n++] = DEVELOP_BLEND_CS_LAB;
    choices[n++] = DEVELOP_BLEND_CS_RGB_DISPLAY;
    choices[n++] = DEVELOP_BLEND_CS_RGB_SCENE;
    return n;
  }
  *writable = FALSE;
  choices[n++] = def;
  return n;
}

static gboolean _details_writable(dt_iop_module_t *module)
{
  return module->dev
         && dt_is_valid_imgid(module->dev->image_storage.id)
         && dt_image_is_rawprepare_supported(&module->dev->image_storage);
}

static void _add_float_member(JsonBuilder *b, const char *name, float value)
{
  json_builder_set_member_name(b, name);
  if(isfinite(value)) json_builder_add_double_value(b, (double)value);
  else json_builder_add_null_value(b);
}

JsonNode *dt_remote_blend_schema(dt_iop_module_t *module)
{
  if(!module || !(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)) return NULL;
  const dt_develop_blend_params_t *bp = module->blend_params;
  const gboolean extra_bits =
    (bp->mask_mode & (DEVELOP_MASK_MASK | DEVELOP_MASK_CONDITIONAL | DEVELOP_MASK_RASTER)) != 0;

  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);

  json_builder_set_member_name(b, "mask_mode");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "type");
  json_builder_add_string_value(b, "enum");
  json_builder_set_member_name(b, "values");
  json_builder_begin_array(b);
  json_builder_add_string_value(b, "off");
  json_builder_add_string_value(b, "uniform");
  json_builder_end_array(b);
  json_builder_set_member_name(b, "writable");
  json_builder_add_boolean_value(b, !extra_bits);
  json_builder_set_member_name(b, "current_extra_bits");
  json_builder_add_boolean_value(b, extra_bits);
  json_builder_end_object(b);

  dt_develop_blend_colorspace_t choices[3];
  gboolean cs_writable = FALSE;
  const guint n_choices = _colorspace_choices(module, choices, &cs_writable);
  json_builder_set_member_name(b, "colorspace");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "type");
  json_builder_add_string_value(b, "enum");
  json_builder_set_member_name(b, "values");
  json_builder_begin_array(b);
  for(guint i = 0; i < n_choices; i++)
    json_builder_add_string_value(b, _enum_name_for_value(_colorspace_names, choices[i]));
  json_builder_end_array(b);
  json_builder_set_member_name(b, "default");
  json_builder_add_string_value(b,
    _enum_name_for_value(_colorspace_names,
                         dt_develop_blend_default_module_blend_colorspace(module)));
  json_builder_set_member_name(b, "writable");
  json_builder_add_boolean_value(b, cs_writable);
  json_builder_end_object(b);

  const dt_develop_blend_colorspace_t eff =
    dt_remote_blend_effective_colorspace(module, bp->blend_cst);
  GPtrArray *mode_names = dt_remote_blend_mode_names_for_colorspace(eff);
  json_builder_set_member_name(b, "mode");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "type");
  json_builder_add_string_value(b, "enum");
  json_builder_set_member_name(b, "values");
  json_builder_begin_array(b);
  for(guint i = 0; i < mode_names->len; i++)
    json_builder_add_string_value(b, g_ptr_array_index(mode_names, i));
  json_builder_end_array(b);
  json_builder_set_member_name(b, "writable");
  json_builder_add_boolean_value(b, TRUE);
  json_builder_end_object(b);
  g_ptr_array_unref(mode_names);

  json_builder_set_member_name(b, "reverse");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "type");
  json_builder_add_string_value(b, "bool");
  json_builder_set_member_name(b, "writable");
  json_builder_add_boolean_value(b, TRUE);
  json_builder_end_object(b);

  json_builder_set_member_name(b, "feathering_guide");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "type");
  json_builder_add_string_value(b, "enum");
  json_builder_set_member_name(b, "values");
  json_builder_begin_array(b);
  for(const _enum_name_t *e = _feathering_guide_names; e->name; e++)
    json_builder_add_string_value(b, e->name);
  json_builder_end_array(b);
  json_builder_set_member_name(b, "writable");
  json_builder_add_boolean_value(b, TRUE);
  json_builder_end_object(b);

  for(const _float_field_t *f = _float_fields; f->name; f++)
  {
    json_builder_set_member_name(b, f->name);
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "float");
    json_builder_set_member_name(b, "range");
    json_builder_begin_array(b);
    json_builder_add_double_value(b, (double)f->min);
    json_builder_add_double_value(b, (double)f->max);
    json_builder_end_array(b);
    if(f->soft_min != f->soft_max)
    {
      json_builder_set_member_name(b, "soft_range");
      json_builder_begin_array(b);
      json_builder_add_double_value(b, (double)f->soft_min);
      json_builder_add_double_value(b, (double)f->soft_max);
      json_builder_end_array(b);
    }
    if(f->unit)
    {
      json_builder_set_member_name(b, "unit");
      json_builder_add_string_value(b, f->unit);
    }
    json_builder_set_member_name(b, "writable");
    json_builder_add_boolean_value(b,
      strcmp(f->name, "details") ? TRUE : _details_writable(module));
    json_builder_end_object(b);
  }

  json_builder_end_object(b);
  JsonNode *root = json_builder_get_root(b);
  g_object_unref(b);
  return root;
}

JsonNode *dt_remote_blend_read(dt_iop_module_t *module)
{
  if(!module || !(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)) return NULL;
  const dt_develop_blend_params_t *bp = module->blend_params;

  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);

  json_builder_set_member_name(b, "mask_mode");
  json_builder_add_string_value(b, dt_remote_blend_mask_mode_string(bp->mask_mode));

  const char *cs_name = _enum_name_for_value(_colorspace_names, (uint32_t)bp->blend_cst);
  json_builder_set_member_name(b, "colorspace");
  if(cs_name) json_builder_add_string_value(b, cs_name);
  else json_builder_add_int_value(b, bp->blend_cst);

  json_builder_set_member_name(b, "effective_colorspace");
  json_builder_add_string_value(b,
    _enum_name_for_value(_colorspace_names,
                         dt_remote_blend_effective_colorspace(module, bp->blend_cst)));

  const uint32_t mode_value = bp->blend_mode & DEVELOP_BLEND_MODE_MASK;
  const char *mode_name = _enum_name_for_value(_mode_c_names, mode_value);
  json_builder_set_member_name(b, "mode");
  if(mode_name) json_builder_add_string_value(b, mode_name);
  else json_builder_add_int_value(b, (gint64)mode_value);

  json_builder_set_member_name(b, "reverse");
  json_builder_add_boolean_value(b, (bp->blend_mode & DEVELOP_BLEND_REVERSE) != 0);

  const char *fg_name = _enum_name_for_value(_feathering_guide_names, bp->feathering_guide);
  json_builder_set_member_name(b, "feathering_guide");
  if(fg_name) json_builder_add_string_value(b, fg_name);
  else json_builder_add_int_value(b, (gint64)bp->feathering_guide);

  for(const _float_field_t *f = _float_fields; f->name; f++)
    _add_float_member(b, f->name, *(const float *)((const guint8 *)bp + f->offset));

  json_builder_end_object(b);
  JsonNode *root = json_builder_get_root(b);
  g_object_unref(b);
  return root;
}
```

- [ ] **Step 4: Build and run**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R test_remote_blend --output-on-failure`
Expected: PASS (15 tests). If `rawprepare` turns out to support blending in this build, substitute another non-blending op (check with `grep -l IOP_FLAGS_SUPPORTS_BLENDING src/iop/rawprepare.c` — absence of the flag in `flags()` is what the test needs; `gamma` and `demosaic` are other candidates).

- [ ] **Step 5: Commit**

```bash
git add src/control/remote_blend.c src/tests/unittests/control/test_remote_blend.c
git commit -m "feat: blend schema and read serializers for the remote surface"
```
(plus trailers)

---

### Task 4: `dt_remote_blend_patch_apply` — validation + colorspace reset + transition rules

**Files:**
- Modify: `src/control/remote_blend.c` (append)
- Test: `src/tests/unittests/control/test_remote_blend.c` (append)

**Interfaces:**
- Consumes: Task 2–3 helpers; `dt_develop_blend_init_blendif_parameters()` (blend.h).
- Produces: `dt_remote_blend_patch_apply()` exactly as declared in Task 2's header — Task 5 wires it into the mutation path.

**Error table this task implements (from the design, constraint slugs normative):**

| condition | code | constraint |
|---|---|---|
| unknown member `blend.<x>` | UNSUPPORTED_FIELD | — |
| empty blend object | INVALID_VALUE | `empty` |
| `mask_mode` write while stored has drawn/parametric/raster bits | INVALID_VALUE | `mask_configuration_present` |
| `mask_mode` value not `"off"`/`"uniform"` | INVALID_VALUE | `unknown_value` |
| `colorspace` write on a fixed (RAW/NONE-default) module | INVALID_VALUE | `colorspace_not_available` |
| `colorspace` value not in the choice set (incl. `CS_NONE`, `CS_RAW`, Lab on non-Lab) or unknown string | INVALID_VALUE | `colorspace_not_available` / `unknown_value` |
| `mode` unknown enumerator string | INVALID_VALUE | `unknown_value` |
| `mode` valid enumerator not in the projected colorspace's set (incl. deprecated) | INVALID_VALUE | `mode_not_available_in_colorspace` |
| `reverse` not a boolean | INVALID_VALUE | `wrong_type` |
| `feathering_guide` unknown string | INVALID_VALUE | `unknown_value` |
| float member not a JSON number | INVALID_VALUE | `wrong_type` |
| float non-finite | INVALID_VALUE | `non_finite` |
| float outside hard range | INVALID_VALUE | `range` |
| `details` write when image is not rawprepare-supported | INVALID_VALUE | `requires_raw_image` |

- [ ] **Step 1: Write the failing tests**

Append to `test_remote_blend.c`:

```c
/* ------------------------------------------------------------------ */
/* Task 4: patch apply                                                 */
/* ------------------------------------------------------------------ */

static JsonObject *_patch_from_string(const char *json)
{
  JsonParser *parser = json_parser_new();
  assert_true(json_parser_load_from_data(parser, json, -1, NULL));
  JsonObject *o = json_node_dup_object(json_parser_get_root(parser));
  g_object_unref(parser);
  return o;
}

static void _assert_patch_fails(blend_fixture_t *fx, const char *patch_json,
                                dt_remote_error_code_t code, const char *constraint)
{
  JsonObject *patch = _patch_from_string(patch_json);
  dt_develop_blend_params_t dst = *fx->module->blend_params;
  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  assert_non_null(error);
  assert_int_equal(error->code, code);
  if(constraint)
  {
    assert_non_null(error->details_json);
    assert_non_null(strstr(error->details_json, constraint));
  }
  dt_remote_error_free(error);
  json_object_unref(patch);
}

static void test_patch_opacity_and_mask_mode(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  JsonObject *patch = _patch_from_string("{\"mask_mode\":\"uniform\",\"opacity\":50.0}");
  dt_develop_blend_params_t dst = *fx->module->blend_params;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  assert_null(error);
  assert_int_equal(dst.mask_mode, DEVELOP_MASK_ENABLED);
  assert_float_equal(dst.opacity, 50.0f, 1e-6);
  // untouched members stay untouched
  assert_float_equal(dst.feathering_radius, fx->module->blend_params->feathering_radius, 1e-6);
  json_object_unref(patch);
  blend_fixture_free(fx);
}

static void test_patch_unknown_member_and_empty(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  _assert_patch_fails(fx, "{\"blendif\":3}", DT_REMOTE_ERR_UNSUPPORTED_FIELD, NULL);
  _assert_patch_fails(fx, "{}", DT_REMOTE_ERR_INVALID_VALUE, "empty");
  blend_fixture_free(fx);
}

// Mechanical M-A projection of the transition appendix: stored state x
// target in {"off","uniform"}. Rows off/uniform succeed (including the
// no-op diagonal); every masked row refuses with
// mask_configuration_present.
static void test_patch_mask_mode_transition_table(void **state)
{
  (void)state;
  const struct { uint32_t stored; gboolean writable; } rows[] = {
    { DEVELOP_MASK_DISABLED, TRUE },
    { DEVELOP_MASK_ENABLED, TRUE },
    { DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL, FALSE },
    { DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK, FALSE },
    { DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK_CONDITIONAL, FALSE },
    { DEVELOP_MASK_ENABLED | DEVELOP_MASK_RASTER, FALSE },
  };
  const struct { const char *target; uint32_t bits; } targets[] = {
    { "off", DEVELOP_MASK_DISABLED }, { "uniform", DEVELOP_MASK_ENABLED },
  };
  blend_fixture_t *fx = blend_fixture_new("exposure");
  for(guint r = 0; r < G_N_ELEMENTS(rows); r++)
    for(guint t = 0; t < G_N_ELEMENTS(targets); t++)
    {
      fx->module->blend_params->mask_mode = rows[r].stored;
      gchar *json = g_strdup_printf("{\"mask_mode\":\"%s\"}", targets[t].target);
      if(rows[r].writable)
      {
        JsonObject *patch = _patch_from_string(json);
        dt_develop_blend_params_t dst = *fx->module->blend_params;
        dt_remote_error_t *error = NULL;
        assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
        assert_int_equal(dst.mask_mode, targets[t].bits);
        json_object_unref(patch);
      }
      else
        _assert_patch_fails(fx, json, DT_REMOTE_ERR_INVALID_VALUE, "mask_configuration_present");
      g_free(json);
    }
  fx->module->blend_params->mask_mode = DEVELOP_MASK_DISABLED;
  _assert_patch_fails(fx, "{\"mask_mode\":\"drawn\"}", DT_REMOTE_ERR_INVALID_VALUE, "unknown_value");
  blend_fixture_free(fx);
}

static void test_patch_colorspace_reset_then_overrides(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  // start from a non-default state so the reset is observable
  fx->module->blend_params->blend_cst = DEVELOP_BLEND_CS_RGB_DISPLAY;
  fx->module->blend_params->blend_mode = DEVELOP_BLEND_HSV_VALUE | DEVELOP_BLEND_REVERSE;
  fx->module->blend_params->blend_parameter = 2.5f;

  // switch alone: mode/reverse/fulcrum reset to defaults for the new space
  JsonObject *patch = _patch_from_string("{\"colorspace\":\"DEVELOP_BLEND_CS_RGB_SCENE\"}");
  dt_develop_blend_params_t dst = *fx->module->blend_params;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  assert_int_equal(dst.blend_cst, DEVELOP_BLEND_CS_RGB_SCENE);
  assert_int_equal(dst.blend_mode, DEVELOP_BLEND_NORMAL2);
  assert_float_equal(dst.blend_parameter, 0.0f, 1e-6);
  json_object_unref(patch);

  // switch + same-patch overrides: deterministic composition
  patch = _patch_from_string(
    "{\"colorspace\":\"DEVELOP_BLEND_CS_RGB_SCENE\",\"mode\":\"DEVELOP_BLEND_MULTIPLY\",\"fulcrum\":1.5}");
  dst = *fx->module->blend_params;
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  assert_int_equal(dst.blend_mode, DEVELOP_BLEND_MULTIPLY);
  assert_float_equal(dst.blend_parameter, 1.5f, 1e-6);
  json_object_unref(patch);

  // writing the ALREADY-stored colorspace must NOT reset anything
  fx->module->blend_params->blend_cst = DEVELOP_BLEND_CS_RGB_SCENE;
  fx->module->blend_params->blend_mode = DEVELOP_BLEND_MULTIPLY;
  patch = _patch_from_string("{\"colorspace\":\"DEVELOP_BLEND_CS_RGB_SCENE\"}");
  dst = *fx->module->blend_params;
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  assert_int_equal(dst.blend_mode, DEVELOP_BLEND_MULTIPLY);
  json_object_unref(patch);
  blend_fixture_free(fx);
}

static void test_patch_colorspace_rejections(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  // exposure is RGB-default: Lab is not in its choice set
  _assert_patch_fails(fx, "{\"colorspace\":\"DEVELOP_BLEND_CS_LAB\"}",
                      DT_REMOTE_ERR_INVALID_VALUE, "colorspace_not_available");
  _assert_patch_fails(fx, "{\"colorspace\":\"DEVELOP_BLEND_CS_NONE\"}",
                      DT_REMOTE_ERR_INVALID_VALUE, "colorspace_not_available");
  _assert_patch_fails(fx, "{\"colorspace\":\"bogus\"}",
                      DT_REMOTE_ERR_INVALID_VALUE, "unknown_value");
  blend_fixture_free(fx);
}

static void test_patch_mode_validated_against_projected_colorspace(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  fx->module->blend_params->blend_cst = DEVELOP_BLEND_CS_RGB_SCENE;
  // HSV_VALUE exists only in RGB display -> rejected in scene space
  _assert_patch_fails(fx, "{\"mode\":\"DEVELOP_BLEND_HSV_VALUE\"}",
                      DT_REMOTE_ERR_INVALID_VALUE, "mode_not_available_in_colorspace");
  // deprecated modes are never writable
  _assert_patch_fails(fx, "{\"mode\":\"DEVELOP_BLEND_LAB_L\"}",
                      DT_REMOTE_ERR_INVALID_VALUE, "mode_not_available_in_colorspace");
  _assert_patch_fails(fx, "{\"mode\":\"nonsense\"}",
                      DT_REMOTE_ERR_INVALID_VALUE, "unknown_value");
  // projection: colorspace + mode in one patch validates mode against the NEW space
  JsonObject *patch = _patch_from_string(
    "{\"colorspace\":\"DEVELOP_BLEND_CS_RGB_DISPLAY\",\"mode\":\"DEVELOP_BLEND_HSV_VALUE\"}");
  dt_develop_blend_params_t dst = *fx->module->blend_params;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  assert_int_equal(dst.blend_mode & DEVELOP_BLEND_MODE_MASK, DEVELOP_BLEND_HSV_VALUE);
  json_object_unref(patch);
  blend_fixture_free(fx);
}

static void test_patch_reverse_flag(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  JsonObject *patch = _patch_from_string("{\"reverse\":true}");
  dt_develop_blend_params_t dst = *fx->module->blend_params;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  assert_true((dst.blend_mode & DEVELOP_BLEND_REVERSE) != 0);
  assert_int_equal(dst.blend_mode & DEVELOP_BLEND_MODE_MASK,
                   fx->module->blend_params->blend_mode & DEVELOP_BLEND_MODE_MASK);
  json_object_unref(patch);
  patch = _patch_from_string("{\"reverse\":false}");
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  assert_true((dst.blend_mode & DEVELOP_BLEND_REVERSE) == 0);
  json_object_unref(patch);
  _assert_patch_fails(fx, "{\"reverse\":1}", DT_REMOTE_ERR_INVALID_VALUE, "wrong_type");
  blend_fixture_free(fx);
}

static void test_patch_numeric_edges(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  // hard-range edges are inclusive
  JsonObject *patch = _patch_from_string(
    "{\"opacity\":0.0,\"fulcrum\":-18.0,\"feathering_radius\":250.0,\"blur_radius\":100.0,"
    "\"contrast\":1.0,\"brightness\":-1.0}");
  dt_develop_blend_params_t dst = *fx->module->blend_params;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  assert_float_equal(dst.opacity, 0.0f, 1e-6);
  assert_float_equal(dst.blend_parameter, -18.0f, 1e-6);
  assert_float_equal(dst.feathering_radius, 250.0f, 1e-6);
  json_object_unref(patch);
  // soft range is NOT a write limit: fulcrum 10 (beyond soft 3) is legal
  patch = _patch_from_string("{\"fulcrum\":10.0}");
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  json_object_unref(patch);
  _assert_patch_fails(fx, "{\"opacity\":100.001}", DT_REMOTE_ERR_INVALID_VALUE, "range");
  _assert_patch_fails(fx, "{\"opacity\":-0.001}", DT_REMOTE_ERR_INVALID_VALUE, "range");
  _assert_patch_fails(fx, "{\"opacity\":\"high\"}", DT_REMOTE_ERR_INVALID_VALUE, "wrong_type");
  blend_fixture_free(fx);
}

static void test_patch_feathering_guide(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  JsonObject *patch = _patch_from_string("{\"feathering_guide\":\"DEVELOP_MASK_GUIDE_OUT_AFTER_BLUR\"}");
  dt_develop_blend_params_t dst = *fx->module->blend_params;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  assert_int_equal(dst.feathering_guide, DEVELOP_MASK_GUIDE_OUT_AFTER_BLUR);
  json_object_unref(patch);
  _assert_patch_fails(fx, "{\"feathering_guide\":\"sideways\"}",
                      DT_REMOTE_ERR_INVALID_VALUE, "unknown_value");
  blend_fixture_free(fx);
}

static void test_patch_details_requires_raw_image(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  // fixture dev has no image -> details is never writable here
  _assert_patch_fails(fx, "{\"details\":0.5}", DT_REMOTE_ERR_INVALID_VALUE, "requires_raw_image");
  blend_fixture_free(fx);
}
```

Register all of them in `main()`.

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build -j$(nproc) 2>&1 | tail -5`
Expected: link FAILURE — `dt_remote_blend_patch_apply` undefined.

- [ ] **Step 3: Implement `dt_remote_blend_patch_apply`**

Append to `remote_blend.c`:

```c
/* ------------------------------------------------------------------ */
/* patch apply                                                         */
/* ------------------------------------------------------------------ */

static dt_remote_error_t *_blend_error(dt_remote_error_code_t code,
                                       const char *member, const char *constraint,
                                       const char *fmt, ...) G_GNUC_PRINTF(4, 5);
static dt_remote_error_t *_blend_error(dt_remote_error_code_t code,
                                       const char *member, const char *constraint,
                                       const char *fmt, ...)
{
  dt_remote_error_t *error = g_malloc0(sizeof(dt_remote_error_t));
  error->code = code;
  va_list args;
  va_start(args, fmt);
  error->message = g_strdup_vprintf(fmt, args);
  va_end(args);
  if(member && constraint)
    error->details_json =
      g_strdup_printf("{\"parameter\":\"blend.%s\",\"constraint\":\"%s\"}", member, constraint);
  else if(member)
    error->details_json = g_strdup_printf("{\"parameter\":\"blend.%s\"}", member);
  return error;
}

static gboolean _json_member_double(JsonObject *o, const char *name, double *out)
{
  JsonNode *node = json_object_get_member(o, name);
  if(!node || !JSON_NODE_HOLDS_VALUE(node)) return FALSE;
  const GType t = json_node_get_value_type(node);
  if(t == G_TYPE_DOUBLE) { *out = json_node_get_double(node); return TRUE; }
  if(t == G_TYPE_INT64) { *out = (double)json_node_get_int(node); return TRUE; }
  return FALSE;
}

static const char *_json_member_string(JsonObject *o, const char *name)
{
  JsonNode *node = json_object_get_member(o, name);
  if(!node || !JSON_NODE_HOLDS_VALUE(node)
     || json_node_get_value_type(node) != G_TYPE_STRING)
    return NULL;
  return json_node_get_string(node);
}

gboolean dt_remote_blend_patch_apply(dt_iop_module_t *module,
                                     JsonObject *patch,
                                     dt_develop_blend_params_t *dst,
                                     dt_remote_error_t **error)
{
  static const char *const allowed[] = {
    "mask_mode", "colorspace", "mode", "reverse", "fulcrum", "opacity",
    "feathering_radius", "feathering_guide", "blur_radius", "contrast",
    "brightness", "details", NULL };

  GList *members = json_object_get_members(patch);
  if(!members)
  {
    if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, NULL, "empty",
                                    _("'blend' must be non-empty"));
    return FALSE;
  }
  for(GList *m = members; m; m = m->next)
  {
    gboolean known = FALSE;
    for(const char *const *a = allowed; *a && !known; a++)
      known = !strcmp(*a, m->data);
    if(!known)
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_UNSUPPORTED_FIELD, (const char *)m->data, NULL,
                                      _("unknown blend member 'blend.%s'"), (const char *)m->data);
      g_list_free(members);
      return FALSE;
    }
  }
  g_list_free(members);

  // 1. mask_mode -- gated by the transition appendix's M-A projection:
  // writable only while the stored mode has no drawn/parametric/raster
  // bits, and only to "off"/"uniform".
  if(json_object_has_member(patch, "mask_mode"))
  {
    if(dst->mask_mode & (DEVELOP_MASK_MASK | DEVELOP_MASK_CONDITIONAL | DEVELOP_MASK_RASTER))
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "mask_mode",
                                      "mask_configuration_present",
                                      _("a drawn/parametric/raster mask configuration is present"));
      return FALSE;
    }
    uint32_t target = 0;
    if(!dt_remote_blend_mask_mode_from_string(_json_member_string(patch, "mask_mode"), &target))
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "mask_mode", "unknown_value",
                                      _("mask_mode accepts \"off\" or \"uniform\""));
      return FALSE;
    }
    dst->mask_mode = target;
  }

  // 2. colorspace -- deterministic reset semantics (design decision 3):
  // a change resets mode/reverse/fulcrum/blendif to the new space's
  // defaults; later members of the SAME patch then apply on top. The
  // GUI's history-scavenging restore is deliberately not reproduced.
  if(json_object_has_member(patch, "colorspace"))
  {
    dt_develop_blend_colorspace_t choices[3];
    gboolean cs_writable = FALSE;
    const guint n_choices = _colorspace_choices(module, choices, &cs_writable);
    const char *cs_string = _json_member_string(patch, "colorspace");
    uint32_t cs_value = 0;
    if(!cs_string || !_enum_value_for_name(_colorspace_names, cs_string, &cs_value))
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "colorspace", "unknown_value",
                                      _("unknown blend colorspace"));
      return FALSE;
    }
    gboolean in_choices = FALSE;
    for(guint i = 0; cs_writable && i < n_choices && !in_choices; i++)
      in_choices = (choices[i] == (dt_develop_blend_colorspace_t)cs_value);
    if(!in_choices)
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "colorspace",
                                      "colorspace_not_available",
                                      _("blend colorspace not available for this module"));
      return FALSE;
    }
    if((int32_t)cs_value != dst->blend_cst)
      dt_develop_blend_init_blendif_parameters(dst, (dt_develop_blend_colorspace_t)cs_value);
  }

  // 3. mode -- validated against the PROJECTED effective colorspace.
  const dt_develop_blend_colorspace_t eff =
    dt_remote_blend_effective_colorspace(module, dst->blend_cst);
  if(json_object_has_member(patch, "mode"))
  {
    const char *mode_string = _json_member_string(patch, "mode");
    uint32_t mode_value = 0;
    if(!mode_string || !_enum_value_for_name(_mode_c_names, mode_string, &mode_value))
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "mode", "unknown_value",
                                      _("unknown blend mode"));
      return FALSE;
    }
    GPtrArray *names = dt_remote_blend_mode_names_for_colorspace(eff);
    gboolean available = FALSE;
    for(guint i = 0; i < names->len && !available; i++)
      available = !strcmp(g_ptr_array_index(names, i), mode_string);
    g_ptr_array_unref(names);
    if(!available)
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "mode",
                                      "mode_not_available_in_colorspace",
                                      _("blend mode not available in the effective colorspace"));
      return FALSE;
    }
    dst->blend_mode = (dst->blend_mode & DEVELOP_BLEND_REVERSE) | mode_value;
  }

  // 4. reverse
  if(json_object_has_member(patch, "reverse"))
  {
    JsonNode *node = json_object_get_member(patch, "reverse");
    if(!node || !JSON_NODE_HOLDS_VALUE(node)
       || json_node_get_value_type(node) != G_TYPE_BOOLEAN)
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "reverse", "wrong_type",
                                      _("'reverse' must be a boolean"));
      return FALSE;
    }
    if(json_node_get_boolean(node)) dst->blend_mode |= DEVELOP_BLEND_REVERSE;
    else dst->blend_mode &= ~DEVELOP_BLEND_REVERSE;
  }

  // 5. feathering_guide
  if(json_object_has_member(patch, "feathering_guide"))
  {
    uint32_t fg_value = 0;
    const char *fg_string = _json_member_string(patch, "feathering_guide");
    if(!fg_string || !_enum_value_for_name(_feathering_guide_names, fg_string, &fg_value))
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "feathering_guide",
                                      "unknown_value", _("unknown feathering guide"));
      return FALSE;
    }
    dst->feathering_guide = fg_value;
  }

  // 6. numeric fields, table order
  for(const _float_field_t *f = _float_fields; f->name; f++)
  {
    if(!json_object_has_member(patch, f->name)) continue;
    if(!strcmp(f->name, "details") && !_details_writable(module))
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "details",
                                      "requires_raw_image",
                                      _("the details threshold needs a raw image"));
      return FALSE;
    }
    double value = 0.0;
    if(!_json_member_double(patch, f->name, &value))
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, f->name, "wrong_type",
                                      _("'%s' must be a number"), f->name);
      return FALSE;
    }
    if(!isfinite(value))
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, f->name, "non_finite",
                                      _("'%s' must be finite"), f->name);
      return FALSE;
    }
    if(value < (double)f->min || value > (double)f->max)
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, f->name, "range",
                                      _("'%s' must be within [%g, %g]"), f->name,
                                      (double)f->min, (double)f->max);
      return FALSE;
    }
    *(float *)((guint8 *)dst + f->offset) = (float)value;
  }

  return TRUE;
}
```

- [ ] **Step 4: Build and run**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R test_remote_blend --output-on-failure`
Expected: PASS (25 tests). Then full `ctest --test-dir build` — all suites green.

- [ ] **Step 5: Commit**

```bash
git add src/control/remote_blend.c src/tests/unittests/control/test_remote_blend.c
git commit -m "feat: blend patch validation and apply

Validation order mask_mode -> colorspace -> mode -> reverse ->
feathering_guide -> numerics; deterministic colorspace reset (no history
scavenging); mask_mode gated by the transition appendix's M-A projection,
with the stored x target table tested mechanically."
```
(plus trailers)

---

### Task 5: Engine wiring — patch/result structs, mutation path, live wrappers

**Files:**
- Modify: `src/control/remote_edit.h`
- Modify: `src/control/remote_edit.c`
- Test: covered by Task 4's pure tests plus Task 6's protocol tests and Task 8's integration gates (the live mutation path needs a darkroom; no new unit file here). `test_remote_edit.c`'s existing `dt_remote_patch_apply` emptiness tests are extended (below).

**Interfaces:**
- Consumes: Tasks 2–4's `dt_remote_blend_*`.
- Produces (for Task 6):
  - `dt_remote_patch_t` gains `JsonObject *blend;` (borrowed from the request tree; NULL when absent)
  - `dt_remote_mutation_result_t` gains `JsonNode *blend_readback;` (owned; NULL unless the patch carried blend)
  - `JsonNode *dt_remote_blend_schema_for_ref(const dt_remote_module_ref_t *ref);`
  - `JsonNode *dt_remote_blend_read_for_ref(const dt_remote_module_ref_t *ref);`
  (both return NULL — never an error — when not in darkroom, unknown module/instance, or no blending; must run on the GTK main thread)

- [ ] **Step 1: Extend the structs in `remote_edit.h`**

Add `#include <json-glib/json-glib.h>` to the header's include block with the comment:

```c
#include <json-glib/json-glib.h>  // JsonObject/JsonNode: the blend surface
                                  // (remote_blend.h) deliberately speaks
                                  // JSON -- the one exception to this
                                  // header's "no JSON" rule, see
                                  // remote_blend.h's own top comment.
```

In `dt_remote_patch_t`, after `gboolean enable;`:

```c
  JsonObject *blend;           // the request's "blend" member, borrowed
                               // from the caller's parsed request tree
                               // (never owned/freed here); NULL when the
                               // request carried no blend patch
```

In `dt_remote_mutation_result_t`, after `GHashTable *semantic_values;`:

```c
  JsonNode *blend_readback;     // complete post-commit blend object
                                // (dt_remote_blend_read()), owned; NULL
                                // unless the patch carried `blend`
```

After the mutation API section, declare the wrappers:

```c
/** live-lookup conveniences for the protocol layer: resolve `ref` against
 * the current darkroom and return dt_remote_blend_schema()/_read() for
 * that instance. Return NULL -- never an error -- when not in darkroom,
 * the instance does not exist, or the op has no blending. GTK main
 * thread only. */
JsonNode *dt_remote_blend_schema_for_ref(const dt_remote_module_ref_t *ref);
JsonNode *dt_remote_blend_read_for_ref(const dt_remote_module_ref_t *ref);
```

- [ ] **Step 2: Wire the mutation path in `remote_edit.c`**

Add `#include "control/remote_blend.h"` and `#include "develop/blend.h"` to the include block.

(a) In `dt_remote_patch_apply()` (line ~588), the emptiness guard currently rejects a patch whose `scalar_values`, `semantic_values`, and `has_enable` are all empty/unset. Extend the condition so a non-NULL `patch->blend` also counts as content (a blend-only patch is legal). Locate the condition testing those three and add `&& !patch->blend`.

(b) In `dt_remote_set_module_params()`, after the `s_class_ops[]` apply loop (after line ~1477) and before the semantic read-back block, insert:

```c
  // Blend step (Tier 1 / M-A): validate the blend patch into a scratch
  // copy of the live blend params -- same discipline as temp_params; any
  // rejection aborts with live state untouched. Blend errors rank after
  // every semantic class by construction (this runs after the class-ops
  // loop).
  dt_develop_blend_params_t temp_blend;
  const gboolean have_blend = patch->blend != NULL;
  if(have_blend)
  {
    if(!(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING))
    {
      g_free(temp_params);
      if(error)
        *error = dt_remote_error_new(DT_REMOTE_ERR_UNSUPPORTED_FIELD,
                                     _("module '%s' does not support blending"), module->op);
      return FALSE;
    }
    temp_blend = *module->blend_params;
    if(!dt_remote_blend_patch_apply(module, patch->blend, &temp_blend, error))
    {
      g_free(temp_params);
      return FALSE;
    }
  }
```

(The semantic read-back block between this and the commit only touches `temp_params`; no change there.)

(c) In the commit step, immediately after `memcpy(module->params, temp_params, module->params_size);` and `g_free(temp_params);` (and before `if(patch->has_enable) ...`), insert:

```c
  // the preset-apply idiom's blend half (src/gui/presets.c:1091): commit
  // the validated blend block before the shared gui_update/history step,
  // so the single dt_dev_add_history_item() below snapshots params AND
  // blend params together -- still exactly one history item.
  if(have_blend) dt_iop_commit_blend_params(module, &temp_blend);
```

(d) In the result-building block (after `result->semantic_values = semantic_readback;`), insert:

```c
  result->blend_readback = have_blend ? dt_remote_blend_read(module) : NULL;
```

(e) In `dt_remote_mutation_result_free()`, free the new member:

```c
  if(result->blend_readback) json_node_unref(result->blend_readback);
```

(f) Append the wrappers near `dt_remote_get_module_params`:

```c
static JsonNode *_blend_node_for_ref(const dt_remote_module_ref_t *ref,
                                     JsonNode *(*build)(dt_iop_module_t *))
{
  g_assert(!darktable.control || pthread_equal(darktable.control->gui_thread, pthread_self()));
  dt_develop_t *dev = NULL;
  dt_remote_error_t *error = NULL;
  if(!dt_remote_require_darkroom_image(&dev, &error))
  {
    dt_remote_error_free(error);
    return NULL;
  }
  dt_iop_module_t *module = dt_remote_find_module(dev, ref, &error);
  if(!module)
  {
    dt_remote_error_free(error);
    return NULL;
  }
  return build(module);
}

JsonNode *dt_remote_blend_schema_for_ref(const dt_remote_module_ref_t *ref)
{
  return _blend_node_for_ref(ref, dt_remote_blend_schema);
}

JsonNode *dt_remote_blend_read_for_ref(const dt_remote_module_ref_t *ref)
{
  return _blend_node_for_ref(ref, dt_remote_blend_read);
}
```

- [ ] **Step 3: Extend the emptiness unit test**

In `src/tests/unittests/control/test_remote_edit.c`, find the test covering "empty patch rejected" for `dt_remote_patch_apply` (search `must be non-empty`). Add a sibling assertion: a patch with NULL `scalar_values`/`semantic_values`, `has_enable = FALSE`, but a non-NULL `blend` object is NOT rejected by the emptiness rule (build a one-member `JsonObject` with `json_object_new()` + `json_object_set_string_member(o, "mask_mode", "off")`; expected: `dt_remote_patch_apply` returns TRUE without touching the params blob, since the blend member is applied later by the caller, not by this pure core). Add `#include <json-glib/json-glib.h>` if absent.

- [ ] **Step 4: Build and run all unit suites**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure`
Expected: all suites PASS.

- [ ] **Step 5: Commit**

```bash
git add src/control/remote_edit.h src/control/remote_edit.c src/tests/unittests/control/test_remote_edit.c
git commit -m "feat: wire blend patches through the mutation transaction

set_module_params validates blend into a scratch blend block alongside
temp_params and commits via dt_iop_commit_blend_params in the same
single-history-item transaction; mutation results carry a complete blend
readback; live _for_ref wrappers serve the read/schema attach points."
```
(plus trailers)

---

### Task 6: Protocol layer — capability, parse, serialize, fixtures

**Files:**
- Modify: `src/control/remote_protocol.h` (calls table), `src/control/remote_protocol.c`
- Create: `src/tests/unittests/control/fixtures/set_module_params_blend_request.json`, `set_module_params_blend_response.json`, `get_module_params_blend_request.json`, `get_module_params_blend_response.json`, `set_module_params_error_blend_mask_configuration_request.json`, `set_module_params_error_blend_mask_configuration_response.json`
- Test: `src/tests/unittests/control/test_remote_protocol.c` (append), `tools/mcp/tests/test_protocol.py` (extend fixture parametrization)

**Interfaces:**
- Consumes: Task 5's struct members and `dt_remote_blend_schema_for_ref`/`dt_remote_blend_read_for_ref`.
- Produces: wire members `blend` (schema/read/patch/readback), optional `instance` on `get_module_schema`, `blend_params` in hello.

- [ ] **Step 1: Write the failing C protocol tests**

Append to `test_remote_protocol.c`, following the file's established stub-calls + `_assert_dispatch_matches()` idiom (mirror the neighboring curve/quantity test sections for the exact stub-table plumbing — the calls-table type and installer are `dt_remote_protocol_calls_t` / `dt_remote_protocol_set_calls()`):

1. `test_hello_advertises_blend_params` — dispatch `hello_request.json`, assert the result's `capabilities` array contains `"blend_params"` (extend the existing hello test's expected-capabilities list rather than adding a new test if that is how vector/band/quantity did it; also update `fixtures/hello_response.json`'s capability array to include `"blend_params"`).
2. `test_set_module_params_blend_fixture_round_trip` — install a stub `set_module_params` that (i) asserts `patch->blend` is non-NULL and contains member `"opacity"` with value 50.0 and `"mask_mode"` == `"uniform"`, (ii) returns a canned `dt_remote_mutation_result_t` whose `blend_readback` is parsed (with `json_from_string()`) from the response fixture's `blend` member; then `_assert_dispatch_matches("set_module_params_blend_request.json", "set_module_params_blend_response.json")`.
3. `test_set_module_params_blend_must_be_object` — dispatch a built request with `"blend": 5`, assert `invalid_value` error mentioning `'blend' must be an object`.
4. `test_set_module_params_blend_only_patch_allowed` — request with empty `values: {}` plus a valid `blend` object; stub `set_module_params` returns success; assert no `'values' must be non-empty` rejection.
5. `test_set_module_params_blend_error_fixture` — stub returns the mask_configuration_present error; `_assert_dispatch_matches` on the error fixture pair.
6. `test_get_module_params_attaches_blend` — stub `blend_read` (new calls-table member) returning a small parsed object `{"mask_mode":"off","opacity":100.0}`; `_assert_dispatch_matches("get_module_params_blend_request.json", "get_module_params_blend_response.json")`. Also assert a NULL `blend_read` (or one returning NULL) emits no `blend` member (reuse the existing plain `get_module_params_request.json` dispatch and check the response object has no `blend` key).
7. `test_get_module_schema_accepts_instance_and_attaches_blend` — stub `blend_schema` returning `{"mask_mode":{"writable":true}}`; dispatch a built `get_module_schema` request with `"instance": 0`, assert the result has a `blend` member equal to the stub node and that a request WITHOUT `instance` still succeeds (default 0). With stub `blend_schema` returning NULL, assert no member.

- [ ] **Step 2: Author the fixtures**

Copy the JSON-RPC envelope framing (`jsonrpc`/`id`/`method`/`params` and response `result`) byte-for-byte in style from `set_module_params_curve_request.json` / `_response.json`. Contents:

`set_module_params_blend_request.json` — params:
```json
{ "module": "exposure", "values": {}, "blend": { "mask_mode": "uniform", "opacity": 50.0 } }
```

`set_module_params_blend_response.json` — result:
```json
{ "module": "exposure", "instance": 0, "enabled": true, "values": {},
  "blend": {
    "mask_mode": "uniform",
    "colorspace": "DEVELOP_BLEND_CS_NONE",
    "effective_colorspace": "DEVELOP_BLEND_CS_RGB_SCENE",
    "mode": "DEVELOP_BLEND_NORMAL2",
    "reverse": false,
    "fulcrum": 0.0,
    "opacity": 50.0,
    "feathering_radius": 0.0,
    "feathering_guide": "DEVELOP_MASK_GUIDE_IN_AFTER_BLUR",
    "blur_radius": 0.0,
    "contrast": 0.0,
    "brightness": 0.0,
    "details": 0.0
  },
  "revision": 12 }
```

`get_module_params_blend_request.json` — params: `{ "module": "exposure" }`; response mirrors the existing `get_module_params_response.json` for exposure with an added `"blend": { "mask_mode": "off", "opacity": 100.0 }` member (matching the stub in test 6; keep the stub and fixture identical).

Error pair — request: params `{ "module": "exposure", "values": {}, "blend": { "mask_mode": "uniform" } }`; response: the standard error envelope (copy shape from `set_module_params_error_invalid_value_response.json`) with `code` = invalid_value's wire code, message `"a drawn/parametric/raster mask configuration is present"`, and `details` `{ "parameter": "blend.mask_mode", "constraint": "mask_configuration_present" }`.

- [ ] **Step 3: Run to verify failure**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R test_remote_protocol --output-on-failure 2>&1 | tail -15`
Expected: compile FAILURE (calls table has no `blend_read`/`blend_schema`) or test failures.

- [ ] **Step 4: Implement the protocol changes**

In `remote_protocol.h`, append to `dt_remote_protocol_calls_t`:

```c
  // Tier-1 blend surface (blend_params capability): live-lookup JSON
  // builders; NULL-returning (never erroring) -- the handlers omit the
  // member. A NULL function pointer (older test call tables) also omits.
  JsonNode *(*blend_schema)(const dt_remote_module_ref_t *ref);
  JsonNode *(*blend_read)(const dt_remote_module_ref_t *ref);
```

In `remote_protocol.c`:

1. Both `DEFAULT_CALLS` and `s_calls` initializers gain `.blend_schema = dt_remote_blend_schema_for_ref, .blend_read = dt_remote_blend_read_for_ref,` (include `"control/remote_blend.h"` is not needed — the declarations live in remote_edit.h).
2. Hello: add `json_builder_add_string_value(b, "blend_params");` after `"quantity_params"`, extending the comment: `// "blend_params" (mask Tier 1 / M-A) gates the blend schema/read/patch/readback wire members.`
3. `GET_MODULE_SCHEMA_KEYS` becomes `{ "module", "instance", NULL }`; parse `instance` with the same `_optional_int_default(params, "instance", 0, ...)` used by `_handler_get_module_params`; after the schema object's existing members are built (immediately before the closing `json_builder_end_object(b)` of the schema result), attach:

```c
  if(s_calls.blend_schema)
  {
    const dt_remote_module_ref_t ref = { .op = module, .instance = (int)instance };
    JsonNode *blend_node = s_calls.blend_schema(&ref);
    if(blend_node)
    {
      json_builder_set_member_name(b, "blend");
      json_builder_add_value(b, blend_node);  // builder takes ownership
    }
  }
```

4. `_handler_get_module_params`: after the `semantic_values` block (before the final `json_builder_end_object(b)`), attach identically via `s_calls.blend_read(&ref)` (a `dt_remote_module_ref_t` from the handler's module/instance).
5. `SET_MODULE_PARAMS_KEYS` gains `"blend"`. After the `enable` parse block, add:

```c
  JsonObject *blend_obj = NULL;
  if(json_object_has_member(params, "blend"))
  {
    JsonNode *blend_node = json_object_get_member(params, "blend");
    if(!blend_node || !JSON_NODE_HOLDS_OBJECT(blend_node))
    {
      g_list_free(value_keys);
      if(semantic_patches) g_ptr_array_unref(semantic_patches);
      return _handler_fail(_error_new(DT_REMOTE_ERR_INVALID_VALUE,
                                      _("parameter 'blend' must be an object")));
    }
    blend_obj = json_node_get_object(blend_node);
  }
```

   Relax the emptiness check to `if(!value_keys && !semantic_patches && !blend_obj)`, and set `patch.blend = blend_obj;` alongside the other patch fields. (The object is borrowed from the request tree, which outlives the synchronous engine call — same lifetime argument as `ref.op`.)
6. Result serializer: after the `semantic_values` member block, add:

```c
  if(result->blend_readback)
  {
    json_builder_set_member_name(b, "blend");
    json_builder_add_value(b, result->blend_readback);  // ownership moves to the builder
    result->blend_readback = NULL;
  }
```

- [ ] **Step 5: Build and run C + Python protocol suites**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure`
Expected: all PASS.

Extend `tools/mcp/tests/test_protocol.py`'s shared-fixture parametrization with the two success pairs:

```python
        (
            "set_module_params",
            "set_module_params_blend_request.json",
            "set_module_params_blend_response.json",
        ),
        (
            "get_module_params",
            "get_module_params_blend_request.json",
            "get_module_params_blend_response.json",
        ),
```

(Check how `load_fixture` locates files — the same `src/tests/unittests/control/fixtures/` directory is shared; no copying.) Also extend the hello-capability assertion (`test_client_capabilities_populated_from_hello`) with `assert "blend_params" in client.capabilities` if the fixture-driven hello includes it (Step 2 updated `hello_response.json`).

Run: `cd tools/mcp && .venv/bin/pytest -q`
Expected: all PASS.

- [ ] **Step 6: Commit**

```bash
git add src/control/remote_protocol.h src/control/remote_protocol.c \
        src/tests/unittests/control/test_remote_protocol.c \
        src/tests/unittests/control/fixtures/ tools/mcp/tests/test_protocol.py
git commit -m "feat: blend wire surface -- blend_params capability, parse/serialize, fixtures

hello advertises blend_params; set_module_params accepts a sibling
'blend' object (blend-only patches legal) and returns the complete blend
readback; get_module_params and get_module_schema (now with optional
'instance', default 0) attach the live blend member when available."
```
(plus trailers)

---

### Task 7: Sidecar — `blend` tool argument + capability gate

**Files:**
- Modify: `tools/mcp/src/darktable_mcp/server.py`
- Test: `tools/mcp/tests/test_tools.py` (append)

- [ ] **Step 1: Write the failing tests**

Append to `test_tools.py`, mirroring `test_set_module_params_quantities_gated_on_quantity_params_capability` exactly:

```python
async def test_set_module_params_blend_gated_on_blend_params_capability(
    tmp_path, fake_server_factory
):
    """A darktable whose hello does not advertise `blend_params` must be
    refused client-side with an upgrade message, before any wire call."""
    server = await fake_server_factory()
    calls = []
    server.handle("set_module_params", lambda params: calls.append(params) or {})

    app = await _built_server(tmp_path, server)
    with pytest.raises(TransportError) as excinfo:
        await app.call_tool(
            "set_module_params",
            {
                "module": "exposure",
                "values": {},
                "blend": {"mask_mode": "uniform", "opacity": 50.0},
            },
        )

    message = str(excinfo.value)
    assert "blend_params" in message
    assert "upgrade darktable" in message
    assert calls == []


async def test_set_module_params_blend_passes_through_when_advertised(
    tmp_path, fake_server_factory
):
    """With the capability advertised, the `blend` dict is forwarded
    verbatim as the wire `blend` member."""
    server = await fake_server_factory(capabilities=["params", "blend_params"])
    seen = []
    server.handle(
        "set_module_params",
        lambda params: seen.append(params)
        or {"module": "exposure", "instance": 0, "enabled": True, "values": {}, "revision": 3},
    )

    app = await _built_server(tmp_path, server)
    await app.call_tool(
        "set_module_params",
        {"module": "exposure", "values": {}, "blend": {"opacity": 50.0}},
    )

    assert len(seen) == 1
    assert seen[0]["blend"] == {"opacity": 50.0}
```

If `fake_server_factory` has no `capabilities=` parameter, use whatever mechanism the neighboring capability tests use to advertise capabilities (e.g. a `hello_override` — copy the exact idiom from the file's existing advertised-capability test; `test_protocol.py`'s `hello_override` lambda shows the shape). If `TransportError` vs `ToolError` differs from the quantities test's actual raise, match the quantities test exactly.

- [ ] **Step 2: Run to verify failure**

Run: `cd tools/mcp && .venv/bin/pytest -q tests/test_tools.py -k blend`
Expected: FAIL — unexpected keyword argument `blend`.

- [ ] **Step 3: Implement**

In `server.py`'s `set_module_params`:
1. Signature: add `blend: dict[str, Any] | None = None,` after `quantities`.
2. Docstring: append this paragraph after the quantities paragraph:

```
        `blend` edits the module's blend settings (opacity, blend mode,
        blending colorspace, mask refinement) and needs a darktable that
        advertises the `blend_params` capability. Members: `mask_mode`
        ("off"/"uniform" -- drawn/parametric/raster configurations are
        read-only here), `colorspace` and `mode` (C enumerator names; see
        `get_module_schema`'s `blend` section for this instance's choice
        sets), `reverse` (bool), `fulcrum` (EV), `opacity` (0-100),
        `feathering_radius` (0-250 px), `feathering_guide`,
        `blur_radius` (0-100 px), `contrast`/`brightness`/`details`
        (-1..1; `details` needs a raw image). WARNING: writing
        `colorspace` deterministically resets `mode`, `reverse`,
        `fulcrum`, and any parametric-mask thresholds to the new space's
        defaults -- send replacement values in the same call if you want
        them. The response's `blend` member reads back the complete
        post-commit blend state.
```

3. Before `return await client.call(...)`, add:

```python
        if blend is not None:
            await client.ensure_connected()
            if "blend_params" not in client.capabilities:
                raise TransportError(
                    "this darktable does not advertise blend_params; "
                    "upgrade darktable to edit blend settings"
                )
            params["blend"] = blend
```

- [ ] **Step 4: Run tests**

Run: `cd tools/mcp && .venv/bin/pytest -q`
Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add tools/mcp/src/darktable_mcp/server.py tools/mcp/tests/test_tools.py
git commit -m "feat: sidecar blend argument on set_module_params, gated on blend_params"
```
(plus trailers)

---

### Task 8: Integration gates

**Files:**
- Create: `tools/mcp/tests/integration/test_blend_tier1.py`

**Prereq:** a running build (`build/bin/darktable`). Run with:
`DARKTABLE_BIN=$PWD/build/bin/darktable tools/mcp/.venv/bin/pytest -q -m integration tools/mcp/tests/integration/test_blend_tier1.py`

- [ ] **Step 1: Write the gates**

Model the file on `test_quantity_milestone6.py`: module docstring stating scope and ordering discipline, `from . import harness`, `pytestmark = pytest.mark.integration`, raw wire calls via `client.call(...)` (not the tool-layer sugar), the shared `_render()` helper copied from that file, and history-length bookkeeping via `get_history`. Gates (each its own test, definition order):

1. `test_hello_advertises_blend_params` — `client.capabilities` (or a raw `hello`) contains `"blend_params"`.
2. `test_schema_and_params_carry_blend_section` — `get_module_schema {"module": "exposure", "instance": 0}` has a `blend` member whose `mask_mode.values == ["off", "uniform"]`; `get_module_params {"module": "exposure"}` has a `blend` member with `mask_mode` in the appendix vocabulary and numeric `opacity`.
3. `test_opacity_write_changes_render_and_history` — ensure exposure is enabled with a visible exposure delta (reuse the harness's enable/edit idiom); render; `set_module_params {"module": "exposure", "values": {}, "blend": {"mask_mode": "uniform", "opacity": 50.0}}`; assert: response `blend.opacity == 50.0` and `blend.mask_mode == "uniform"`, history grew by exactly 1, a re-render differs from the pre-write render (bytes unequal), and a fresh `get_module_params` reads back `opacity == 50.0`.
4. `test_colorspace_switch_resets_mode` — on `colorbalancergb` (RGB-scene default): read `blend.effective_colorspace`; write `{"blend": {"colorspace": "DEVELOP_BLEND_CS_RGB_DISPLAY"}}` (or `_SCENE` if display is the default — pick the non-default of the two from the schema's `default`); assert readback `mode == "DEVELOP_BLEND_NORMAL2"` and `colorspace` equals what was written; then write `{"blend": {"colorspace": <other>, "mode": "DEVELOP_BLEND_MULTIPLY"}}` and assert readback mode is MULTIPLY (deterministic same-patch override).
5. `test_blend_patch_does_not_enable_disabled_module` — disable a module (e.g. `vibrance` via `set_module_enabled`), send a blend-only patch, assert response `enabled` is false.
6. `test_mask_mode_rejections_and_roundtrip` — write `{"blend": {"mask_mode": "drawn"}}` → expect `ProtocolError` with `invalid_value`; round-trip `uniform` → `off` → readback `"off"`.
7. `test_undo_covers_blend_edit` — capture revision, blend write, `undo` with the new revision, assert `get_module_params` blend state returned to the pre-write value.

Use the same tolerance/retry discipline as the neighboring files (`harness.wait_for_stable_revision`, the two-attempt `_render`); assert render *inequality* only (no pixel statistics — H6 discipline says spatial ratio assertions start in M-B with the mask render).

- [ ] **Step 2: Run the new gates**

Run: `DARKTABLE_BIN=$PWD/build/bin/darktable tools/mcp/.venv/bin/pytest -q -m integration tools/mcp/tests/integration/test_blend_tier1.py`
Expected: all PASS.

- [ ] **Step 3: Run the full integration suite**

Run: `DARKTABLE_BIN=$PWD/build/bin/darktable tools/mcp/.venv/bin/pytest -q -m integration tools/mcp`
Expected: previous 56 + new gates pass, 1 known OpenCL skip.

- [ ] **Step 4: Commit**

```bash
git add tools/mcp/tests/integration/test_blend_tier1.py
git commit -m "test: Tier-1 blend integration gates"
```
(plus trailers)

---

### Task 9: Documentation + divergence manifest + design amendments

**Files:**
- Modify: `docs/superpowers/specs/2026-07-05-darktable-mcp-protocol-reference.md`
- Modify: `docs/superpowers/specs/2026-07-16-darktable-mcp-supported-operations.md`
- Modify: `docs/superpowers/specs/2026-07-19-darktable-mcp-blend-settings-design.md`
- Create: `docs/superpowers/specs/2026-07-19-darktable-upstream-divergence-manifest.md`
- Modify: `tools/mcp/README.md`
- Modify: `.superpowers/sdd/progress.md` (ledger)

- [ ] **Step 1: Protocol reference** — add a "Blend settings (`blend_params` capability)" section after the quantity-class section covering: the schema member (live-instance, `instance` argument on `get_module_schema`, omitted outside darkroom), read shape, patch semantics (atomicity, validation order, colorspace reset, mask_mode gating with a pointer to the transition appendix, blend-only patches legal, whole-object readback), and the Task-4 error table verbatim.

- [ ] **Step 2: Supported operations** — replace the "blending out of scope" sentence with: "Blend settings (opacity, mode, colorspace, refinement controls, off/uniform mask mode) are supported for every module with `IOP_FLAGS_SUPPORTS_BLENDING` via the `blend_params` capability; parametric and drawn masks remain out of scope (M-B/M-C)." No per-module tier changes.

- [ ] **Step 3: Design-doc amendments** — in the blend-settings design: (a) status line → "implemented (see plan 2026-07-19-darktable-mcp-blend-settings-tier1.md)"; (b) an "Amendments (implementation)" subsection recording the two Design amendments from this plan's header (live schema + `instance` argument; appendix mask_mode vocabulary superseding the `uniform+` prefix sentence).

- [ ] **Step 4: Divergence manifest** (H5 recommendation) — create the new doc with a short preamble ("every deliberate divergence from upstream darktable files on this fork; check each entry on every rebase") and a table with these initial rows:

| upstream file | divergence | guard |
|---|---|---|
| `src/develop/blend.h` / `blend.c` | `dt_develop_blend_mode_section_t` + `dt_develop_blend_mode_sections()` extracted from blend_gui.c's combobox population | GUI-parity test `test_remote_blend` (frozen mode lists) |
| `src/develop/blend_gui.c` | mode-combobox population is a loop over the shared table | same parity test |
| `src/control/remote_blend.c` | hand-written field table over `dt_develop_blend_params_t` (not introspected upstream) | `DEVELOP_BLEND_VERSION` `#error` + offsetof asserts |

Plus a "rehearsed procedure: upstream adds a blend mode" paragraph: add the enumerator to `_mode_c_names`, extend the section table row (or add one), update the frozen parity arrays, rerun `test_remote_blend`.

- [ ] **Step 5: README** — in `tools/mcp/README.md`, add a "Blend settings" subsection after quantities with the opacity example (`blend={"mask_mode": "uniform", "opacity": 50}`) and the colorspace-reset warning sentence.

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

Append one line to `.superpowers/sdd/progress.md` recording the milestone (task list, final commit, "full stack verified"). Then:

```bash
git add docs/ tools/mcp/README.md .superpowers/sdd/progress.md
git commit -m "docs: blend settings (Tier 1 / M-A) -- protocol reference, manifest, amendments"
```
(plus trailers)

---

## Self-review notes (performed while writing)

- **Spec coverage:** wire contract → Tasks 6–7; engine field table/tripwire → Task 2; schema/read → Task 3; patch semantics + full error table → Task 4; mutation path/commit idiom/readback → Task 5; shared mode table + parity test → Task 1; sidecar → Task 7; testing section (unit/protocol/integration) → Tasks 1–6/8; doc impact → Task 9. Design decisions 1–7 all land (decision 6, fulcrum-always-writable, is implicit: the float table never gates fulcrum).
- **Known deviations from the design doc, recorded as amendments:** live schema + `instance` argument; appendix mask_mode vocabulary. Both flagged in the header and written back to the design doc in Task 9.
- **Type consistency:** `dt_remote_blend_patch_apply(module, JsonObject*, dt_develop_blend_params_t*, dt_remote_error_t**)` is identical in Tasks 2 (header), 4 (impl), 5 (caller). `blend_readback`/`patch->blend` names match across Tasks 5–6.
- Implementers adapting stub-table/test-helper names in Tasks 6–7 must match the *existing local idioms* in those files (`_assert_dispatch_matches`, `dt_remote_protocol_set_calls`, `fake_server_factory`) — the plan names them but the neighboring tests are the authority on exact plumbing.
