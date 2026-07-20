# darktable MCP — blend settings (mask Tier 1) low-level design

Date: 2026-07-19
Status: implemented (see plan 2026-07-19-darktable-mcp-blend-settings-tier1.md)
Companions: `2026-07-19-darktable-mcp-mask-support-design-candidates.md`
(the tier split this elaborates), `2026-07-05-darktable-mcp-protocol-reference.md`
(wire contract being extended),
`2026-07-17-darktable-mcp-milestone6-quantity-class-design.md` (the commit
and validation idioms reused here).

## Goal

Read and write the per-instance blend controls that need no mask — opacity,
blend mode, blend colorspace, fulcrum, feathering, blur, contrast,
brightness, details threshold, and the off/uniform mask-mode switch — as an
atomic extension of the existing `set_module_params` transaction, behind a
new `blend_params` hello capability.

## Non-goals

Parametric masks (`blendif*`, boost factors, `mask_combine`), drawn masks
(`mask_id`, mask-mode bits beyond off/uniform), raster masks
(`raster_mask_*`), and any new wire method. Tier 2/3 own those.

## Verified source facts this design rests on

- Blend state: `dt_develop_blend_params_t` (`src/develop/blend.h:180`),
  version `DEVELOP_BLEND_VERSION` = 14, not introspected.
- Canonical programmatic write: `dt_iop_commit_blend_params(module, bp)` —
  the preset-apply path (`src/gui/presets.c:1091`) uses exactly
  commit_blend_params → `dt_iop_gui_update` → `dt_dev_add_history_item`,
  i.e. the idiom `dt_remote_set_module_params` already follows for scalars.
- History items snapshot `blend_params` alongside `params`, and blend
  edits raise `DEVELOP_HISTORY_CHANGE` — revision tracking and `undo`
  (`DT_UNDO_DEVELOP`) work unchanged.
- GUI ranges (`src/develop/blend_gui.c:3545-3634`): opacity 0–100
  (default 100), fulcrum hard −18..18 / soft −3..3 EV (default 0),
  feathering_radius 0–250 px, blur_radius 0–100 px, contrast −1..1,
  brightness −1..1, details −1..1 (all default 0).
- Mode set per blending colorspace: encoded only in the GUI combobox
  population (`blend_gui.c:3126-3193`), grouped in sections; deprecated
  modes are only shown when already stored.
- Colorspace choice set (`blend_gui.c:2031-2090`): RAW-default modules get
  no choice; Lab-default modules may pick Lab / RGB display / RGB scene;
  RGB-default modules may pick RGB display / RGB scene (never Lab). The
  module default comes from
  `dt_develop_blend_default_module_blend_colorspace()`.
- GUI colorspace change (`_blendif_change_blend_colorspace`,
  `blend_gui.c:1912`): validates the target, calls
  `dt_develop_blend_init_blendif_parameters(bp, cst)` (resets mode,
  fulcrum, and the whole blendif block to defaults for the new space),
  then opportunistically restores those fields from the most recent
  history item with the same colorspace, then commits.
- `details` is only meaningful for images where
  `dt_image_is_rawprepare_supported()` holds; the GUI hides the slider
  otherwise.
- Fulcrum (`blend_parameter`) is consumed only by certain RGB-scene
  arithmetic/channel modes (`_blendif_blend_parameter_enabled`,
  `blend_gui.c:349`); for every other mode the value is stored but unused.

## Wire contract

### Capability

`hello` advertises `blend_params`. The sidecar refuses `blend` arguments
client-side with an upgrade message when the capability is absent, exactly
like `curves`/`vectors`/`bands`/`quantities`.

### Schema (`get_module_schema`)

Modules whose `flags()` include `IOP_FLAGS_SUPPORTS_BLENDING` gain a
top-level `"blend"` object; other modules have no `"blend"` key. Shape
(normative example, an RGB-default module on a raw image):

```json
"blend": {
  "mask_mode": { "type": "enum", "values": ["off", "uniform"],
                 "writable": true,
                 "current_extra_bits": false },
  "colorspace": { "type": "enum",
                  "values": ["DEVELOP_BLEND_CS_RGB_DISPLAY",
                             "DEVELOP_BLEND_CS_RGB_SCENE"],
                  "default": "DEVELOP_BLEND_CS_RGB_SCENE",
                  "writable": true },
  "mode": { "type": "enum",
            "values": ["DEVELOP_BLEND_NORMAL2", "DEVELOP_BLEND_MULTIPLY", "..."],
            "writable": true },
  "reverse": { "type": "bool", "writable": true },
  "fulcrum": { "type": "float", "range": [-18.0, 18.0],
               "soft_range": [-3.0, 3.0], "unit": "EV", "writable": true },
  "opacity": { "type": "float", "range": [0.0, 100.0], "unit": "%",
               "writable": true },
  "feathering_radius": { "type": "float", "range": [0.0, 250.0],
                         "unit": "px", "writable": true },
  "feathering_guide": { "type": "enum",
                        "values": ["DEVELOP_MASK_GUIDE_IN_BEFORE_BLUR", "..."],
                        "writable": true },
  "blur_radius": { "type": "float", "range": [0.0, 100.0], "unit": "px",
                   "writable": true },
  "contrast": { "type": "float", "range": [-1.0, 1.0], "writable": true },
  "brightness": { "type": "float", "range": [-1.0, 1.0], "writable": true },
  "details": { "type": "float", "range": [-1.0, 1.0], "writable": true }
}
```

Rules:

- Enum wire values are the C enumerator names, matching the scalar-enum
  convention. `mask_mode` is the one exception: its v1 value space is the
  two strings `"off"`/`"uniform"` (the raw bitfield is engine-internal,
  like Tier 2's `blendif` packing will be).
- `mode.values` lists exactly the modes the GUI offers for the module's
  **effective** colorspace (see below) — obsolete/deprecated enumerators
  are never listed and never writable, but a stored deprecated mode is
  still *reported* by name on read.
- `colorspace.values` is the module's GUI choice set (RAW-default →
  `writable: false`, values list just the default). `colorspace.default`
  is the resolved `dt_develop_blend_default_module_blend_colorspace()`.
- `details.writable` is `false` when the current image is not
  rawprepare-supported (the schema is already computed against the live
  darkroom session, so image-dependent gating is consistent).
- When the stored `mask_mode` contains drawn/parametric/raster bits
  (edits made in the GUI), `mask_mode.writable` is `false` and
  `current_extra_bits` is `true` — Tier 1 refuses to touch a mask
  configuration it cannot represent.

### Read (`get_module_params`)

Response gains, for blending modules:

```json
"blend": {
  "mask_mode": "uniform",
  "colorspace": "DEVELOP_BLEND_CS_NONE",
  "effective_colorspace": "DEVELOP_BLEND_CS_RGB_SCENE",
  "mode": "DEVELOP_BLEND_NORMAL2",
  "reverse": false,
  "fulcrum": 0.0,
  "opacity": 100.0,
  "feathering_radius": 0.0,
  "feathering_guide": "DEVELOP_MASK_GUIDE_OUT_BEFORE_BLUR",
  "blur_radius": 0.0,
  "contrast": 0.0,
  "brightness": 0.0,
  "details": 0.0
}
```

- `colorspace` is the stored value verbatim — `DEVELOP_BLEND_CS_NONE`
  means "module default", and `effective_colorspace` reports the resolved
  space so the client knows which mode set applies without re-deriving.
- When `mask_mode` has bits beyond off/uniform, it is reported as a
  compound string (`"drawn"`, `"parametric"`, `"drawn+parametric"`,
  `"raster"`, with `"uniform+"` prefix when `DEVELOP_MASK_ENABLED` is
  set) — readable truth, not writable vocabulary.
- Non-finite stored floats serialize as JSON `null` (the m6 rule).

### Patch (`set_module_params`)

New optional top-level `"blend"` object next to `values`/`curves`/…:

```json
{ "module": "exposure",
  "values": { "exposure": -1.0 },
  "blend": { "mask_mode": "uniform", "opacity": 50.0 } }
```

- **Atomicity**: the blend patch validates and commits in the same
  transaction as everything else — any invalid member fails the whole
  call, and the whole call is still exactly one history item (the history
  item snapshots params + blend params together, so this is free).
- **Enable rule unchanged**: a blend patch never implicitly enables a
  disabled module; `enable: true` in the same call works as today.
- **Colorspace-change semantics** (deliberate, documented divergence from
  the GUI): writing `colorspace` resets `mode`, `reverse`, `fulcrum`, and
  the (Tier-2) blendif block to the defaults for the new space via
  `dt_develop_blend_init_blendif_parameters()`, then applies the rest of
  the same patch on top. The GUI's extra behavior — scavenging the last
  same-colorspace history item to restore old settings — is **not**
  reproduced: it makes the result depend on invisible history state,
  which is wrong for a wire API. A client that wants old values back
  reads them first and sends them in the same patch (`colorspace` +
  `mode` + `fulcrum` together is fully deterministic).
- **Validation order** inside the blend object: `mask_mode` →
  `colorspace` → `mode`/`reverse` (validated against the *projected*
  effective colorspace, mirroring the `writable_when` projected-params
  precedent) → numeric fields. Blend errors rank after quantity errors in
  the established class precedence (scalar → curve → vector → bands →
  quantity → blend).
- **Readback**: when the patch contained `blend`, the mutation result
  carries the complete post-commit blend object (not just touched fields —
  a colorspace write changes fields the client did not send).

### Errors

Existing vocabulary only:

| condition | error |
|---|---|
| `blend` sent to a module without `IOP_FLAGS_SUPPORTS_BLENDING` | `unsupported_field`, `parameter: "blend"` |
| unknown member inside `blend` | `unsupported_field`, `parameter: "blend.<name>"` |
| out-of-range float / non-finite / unknown enum name | `invalid_value` with `{parameter, constraint}` |
| `mode` not in the effective colorspace's set (incl. deprecated names) | `invalid_value`, `constraint: "mode_not_available_in_colorspace"` |
| `colorspace` write on a RAW-fixed module, or value not in the choice set | `invalid_value` |
| `mask_mode` write while stored mode has drawn/parametric/raster bits | `invalid_value`, `constraint: "mask_configuration_present"` |
| `details` write on a non-raw image | `invalid_value`, `constraint: "requires_raw_image"` |

## Engine design

### New file: `src/control/remote_blend.c` (+ `.h`)

A hand-written field table — the mini-introspection the candidates doc
promised — plus pure pack/validate/serialize helpers:

```c
typedef enum dt_remote_blend_field_kind_t
{
  DT_REMOTE_BLEND_FLOAT,      // offset into dt_develop_blend_params_t
  DT_REMOTE_BLEND_ENUM,       // + dt_introspection_type_enum_tuple_t table
  DT_REMOTE_BLEND_BOOL_FLAG,  // bit inside a uint32 member (reverse)
} dt_remote_blend_field_kind_t;

typedef struct dt_remote_blend_field_t
{
  const char *name;           // wire name
  dt_remote_blend_field_kind_t kind;
  size_t offset;              // offsetof(dt_develop_blend_params_t, ...)
  float min, max;             // FLOAT kind
  float soft_min, soft_max;   // reported when != min/max
  const char *unit;           // NULL when none
  uint32_t flag_mask;         // BOOL_FLAG kind (DEVELOP_BLEND_REVERSE)
  const dt_introspection_type_enum_tuple_t *enum_table;  // ENUM kind
} dt_remote_blend_field_t;
```

Struct-churn tripwire, top of the file:

```c
#if DEVELOP_BLEND_VERSION != 14
#error "dt_develop_blend_params_t changed: re-audit the remote blend field \
table and the wire contract before bumping this guard"
#endif
```

plus `static_assert(offsetof(...))` checks for every table entry (cheap,
and turns silent struct reordering into a build failure).

Public surface (mirrors the semantic-class helpers in shape):

```c
// schema: NULL if module has no blending
JsonNode *dt_remote_blend_schema(dt_iop_module_t *module);
// read: serialize module->blend_params (never NULL for blending modules)
JsonNode *dt_remote_blend_read(dt_iop_module_t *module);
// validate patch INTO a caller-owned copy; fills error on failure
gboolean dt_remote_blend_patch_apply(dt_iop_module_t *module,
                                     JsonObject *patch,
                                     dt_develop_blend_params_t *dst,
                                     dt_remote_error_t **error);
```

`dt_remote_blend_patch_apply` starts from a copy of the live
`blend_params`, applies colorspace reset + members in the validation
order above, and never touches the module. Everything is unit-testable
headless.

### The mode × colorspace table: shared with the GUI

The mode sets currently live only in `blend_gui.c`'s combobox population.
Duplicating them in the engine would rot. This design extracts them into
`blend.c` as data the GUI and the engine both consume:

```c
typedef struct dt_develop_blend_mode_section_t
{
  const char *section;                   // N_("lighten") — GUI section label
  dt_develop_blend_mode_t from, to;      // tuple-order range, as _add_blendmode_combo takes today
} dt_develop_blend_mode_section_t;

// terminated by {NULL, 0, 0}; one table per colorspace family
const dt_develop_blend_mode_section_t *
dt_develop_blend_mode_sections(dt_develop_blend_colorspace_t csp);
```

`blend_gui.c`'s population switch becomes a loop over this table (a
mechanical refactor with identical visible behavior — the per-space
`if`/`else` bodies literally become table rows), and the engine
enumerates the same table for `mode.values` and write validation. This
is the single behavior-adjacent refactor in the milestone and gets its
own task and its own GUI-parity test.

### Mutation path (`remote_edit.c`)

`dt_remote_set_module_params` grows a step between semantic apply and
commit; `dt_remote_patch_t` gains `JsonObject *blend` (NULL when absent):

1. If `patch->blend` and module lacks blending → `unsupported_field`.
2. `dt_develop_blend_params_t temp_blend = *module->blend_params;`
   `dt_remote_blend_patch_apply(module, patch->blend, &temp_blend, ...)`
   — failure aborts the whole call before anything is committed (scalar
   temp_params are also still uncommitted at this point; ordering of the
   existing steps is unchanged).
3. Commit (existing step 8/9, extended): after the params memcpy, run
   `dt_iop_commit_blend_params(module, &temp_blend)` — the preset-apply
   idiom — then the unchanged `dt_iop_gui_update` →
   `dt_dev_add_history_item(dev, module, FALSE)` → queue_draw →
   `_remote_reveal_module`. Still exactly one history item.
4. Readback: `result` gains `JsonNode *blend_readback` =
   `dt_remote_blend_read(module)` post-commit, serialized by the protocol
   layer into the mutation result.

`get_module_params` and `get_module_schema` call `dt_remote_blend_read` /
`dt_remote_blend_schema` and attach the node when non-NULL.

### mask_mode handling

- Read: map the bitfield to the compound string (engine helper, pure).
- Write: accepted only when the stored mode is `DISABLED` or `ENABLED`
  (no drawn/parametric/raster bits) and the target is `"off"`
  (`DEVELOP_MASK_DISABLED`) or `"uniform"` (`DEVELOP_MASK_ENABLED`);
  anything else → `invalid_value` as tabled above. Tier 3 owns
  transitions involving mask bits.

## Sidecar changes (`tools/mcp`)

- `set_module_params` gains an optional `blend: dict` argument; refused
  client-side without the `blend_params` capability (same pattern and
  message shape as `quantities`).
- No collision check against semantic-ID namespaces is needed (`blend` is
  a sibling of `values`, not an ID namespace), but the tool docstring
  documents the colorspace-reset semantics prominently — it is the one
  surprising behavior.
- `get_module_schema`/`get_module_params` pass the `blend` section
  through untouched.
- README: new "blend" section after quantities with the opacity example
  above and the colorspace-reset warning.

## Protocol reference impact

New "Blend settings (`blend_params` capability)" section: schema shape,
read shape, patch semantics (atomicity, colorspace reset, mask_mode
gating, validation order), error table, and the class-precedence
extension. The "blending out of scope" sentence in the supported-operations
doc changes to "blend settings supported (Tier 1); parametric and drawn
masks out of scope", with no per-module tier changes (the surface is
orthogonal, gated by `IOP_FLAGS_SUPPORTS_BLENDING`).

## Testing

Unit (`test_remote_blend.c`, headless, real modules via `dt_init` like
`test_remote_quantity`):
- schema presence/absence by module flags; colorspace choice sets for a
  Lab module, an RGB module, a RAW-fixed module.
- mode listing matches `dt_develop_blend_mode_sections` for each space;
  deprecated mode readable-not-writable.
- patch_apply: each numeric range edge; enum by name; reverse flag
  round-trip; colorspace write resets mode/fulcrum then applies same-patch
  overrides; every error row in the table above.
- mask_mode string mapping both directions, including compound read-only
  strings.
- GUI-parity test for the table refactor: for each colorspace, the
  flattened section table equals the pre-refactor hardcoded expectation
  (frozen into the test as data).

Protocol (`test_remote_protocol.c` + shared fixtures): `blend` in schema /
read / patch / readback / error fixtures, consumed by both the C side and
`tools/mcp/tests/test_protocol.py`.

Integration (pytest `-m integration`): set opacity 50 on an enabled
exposure instance → preview changes, history +1, readback matches;
colorspace switch on colorbalancergb resets mode; blend patch on a
disabled module does not enable it; capability advertised in hello.

## Decisions recorded (and what review should challenge)

1. Extend existing methods, no new wire method — preserves one-call
   atomicity with scalar/semantic patches.
2. C enumerator names on the wire (scalar-enum consistency); `mask_mode`
   as `"off"`/`"uniform"` strings is the exception because the bitfield
   is engine-internal.
3. Colorspace write resets dependent fields deterministically; the GUI's
   history-scavenging restore is deliberately not reproduced.
4. mask_mode with drawn/parametric/raster bits: read-only compound
   string; Tier 1 never modifies a mask configuration it cannot express.
5. Mode×colorspace sets become a shared table in `blend.c` consumed by
   GUI and engine — the one refactor of GUI code in this milestone.
6. `fulcrum` always writable even for modes that ignore it (matches
   storage semantics; the GUI also preserves the value while hiding the
   slider).
7. Whole-blend-object readback after any blend patch.

## Amendments (implementation)

Two deviations from the text above, adopted during planning (see the
Tier-1 plan's "Design amendments" header) and binding on the
implementation:

1. **Blend schema is live-session data.** This design assumed
   `get_module_schema` was computed against the live darkroom; it is
   actually per-op/static ("from the loaded module .so alone",
   `remote_edit.h`). Resolution: `get_module_schema` gains an optional
   `instance` argument (default 0), and the `blend` member appears
   **only** when darktable is in darkroom with an image and that
   instance exists — otherwise the member is omitted (no error).
   `mode.values`, `current_extra_bits`, `details.writable`, and the
   colorspace choice set are computed against that live instance.
2. **mask_mode read strings follow the transition appendix**, not this
   design's "`uniform+` prefix" sentence: `off`, `uniform`,
   `parametric`, `drawn`, `drawn+parametric`, `raster` (`ENABLED`
   accompanying mask bits is implied and never spelled). The appendix in
   `2026-07-19-darktable-mcp-mask-support-design-candidates.md` is
   authoritative for mask_mode vocabulary and transitions.

One implementation finding beyond the design (live-verified by the
Tier-1 integration gates): unlike scalar params, the blend block also
flows **history → module** during every pixelpipe synch
(`dt_iop_commit_params` → `dt_iop_commit_blend_params`), so the
mutation transaction repairs a possible mid-transaction clobber under
`dev->history_mutex` after its history item lands — see the "Blend race
repair" comment in `dt_remote_set_module_params`.
