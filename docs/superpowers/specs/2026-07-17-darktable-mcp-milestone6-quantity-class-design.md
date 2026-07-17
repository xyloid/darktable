# darktable MCP milestone 6 — quantity class (white balance) and vector mop-up

Date: 2026-07-17
Status: approved design (not yet implemented)
Companions: `2026-07-05-darktable-mcp-design.md` (base protocol),
`2026-07-05-darktable-mcp-parameter-class-investigation.md` (class taxonomy —
the "quantity" candidate family),
`2026-07-16-darktable-mcp-milestone5-bands-class-design.md` (dispatch-table
architecture this milestone extends),
`2026-07-12-darktable-mcp-milestone4-vector-class-design.md` (the vector
engine the ride-along adapters reuse),
`2026-07-16-darktable-mcp-supported-operations.md` (tier impact).

## Goal

Two deliverables, mirroring milestone 5's "one new class + cheap adapter
wins" shape:

1. **A quantity semantic class** whose first (and in this milestone, only)
   adapter gives `temperature` real Kelvin/tint white-balance editing —
   the model can finally act on "make it warmer" / "set 5500K" instead of
   being told the stored values are multipliers, not Kelvin. `temperature`
   is rated core* and is the highest-value Tier 2 holdout.
2. **Two vector ride-along adapters** using the existing milestone-4
   engine unchanged: `negadoctor` and `colorharmonizer`, both promoted to
   Tier 1.

Explicitly out of scope: `monochrome`/`colorcorrection` coordinate
semantics, `rawprepare` black levels, resource references, preset/style
application, any second quantity adapter, and hue periodicity modeling.
One module proves the class; the rest wait for demand.

## Why a new class

The stored `temperature` params are four RGB channel multipliers
(`red`/`green`/`blue`/`various`). Every human conversation about white
balance happens in Kelvin and tint — a *presentation domain* connected to
the stored domain by real color science (multipliers → XYZ → correlated
color temperature), not by a units factor. The class investigation calls
this family "quantity": scalars whose stored units differ from their
presentation units, with a conversion authority question. No existing
class fits: curves/bands/vectors all read and write the stored domain
directly through introspection offsets. The quantity class's defining
feature is a **module-owned conversion** between domains.

## Class model

- **Semantic field**: `wb.temperature` on op `temperature`, class
  `"quantity"`, one field carrying **two named components written
  atomically**: `temperature` (unit `kelvin`, domain [1901, 25000] —
  `DT_IOP_LOWEST/HIGHEST_TEMPERATURE`) and `tint` (unitless, domain
  [0.135, 2.326] — `DT_IOP_LOWEST/HIGHEST_TINT`).
- **Paired atomically because the conversion is joint**: coeffs → XYZ →
  (Kelvin, tint) together; `temperature.c` itself warns "temperature and
  tint cannot be disjoined". A caller changing only Kelvin reads the
  current pair first and sends the current tint back. There is no
  compose-with-stored machinery in the engine.
- **Derived and lossy**: the field is marked `derived: true` in the
  schema. Readback runs the reverse conversion (`_XYZ_to_temperature` is
  a binary search; tint is a Y-ratio hack), so a written 5500 K reads
  back as approximately 5500, not exactly. The mutation echo is the
  authoritative stored state; tests compare with tolerance.
- **Reject-don't-clamp, whole-pair replacement, doubles on the wire** —
  all invariants inherited from the established classes. Note the C
  conversion helpers clamp internally; the engine validates the wire
  values against the domains *before* invoking the conversion so the
  clamping paths are never reached by remote writes.
- **Params version**: `temperature` introspection version 4; the adapter
  pins minimum == maximum == 4.

## Conversion authority — a new optional iop API

This is the milestone's one genuinely new mechanism: the first time a
remote engine calls module code instead of reading blobs via
introspection offsets.

iop modules are GModule plugins; their optional API functions are
declared in `src/iop/iop_api.h` and resolved by symbol name into
`dt_iop_module_so_t` function pointers at load time
(`dt_iop_load_module_so`, `src/develop/imageop.c:305`). The quantity
class adds an optional function pair in that same pattern (exact names
and signatures fixed at planning; roughly):

```c
gboolean remote_quantity_read(struct dt_iop_module_t *self,
                              const void *params,
                              double *out_values /* component order */);
gboolean remote_quantity_write(struct dt_iop_module_t *self,
                               const double *values,
                               void *params);
```

`temperature.c` implements them as thin wrappers over its existing
static `_mul2temp(self, p, &TempK, &tint)` and
`_temp2mul(self, TempK, tint, coeffs)` (`temperature.c:453,496`); every
other module leaves them NULL. The conversion needs the **live module
instance** — it depends on camera coefficients held in module data, not
in the params blob — which is why this cannot be a control-side pure
function, and why the hook signature takes `self`.

The engine **fails closed**: `registry_validate` rejects an adapter
whose op does not export both hooks, exactly as the band registry
rejects a native path that does not resolve.

Writes run on the GUI/main context inside the existing mutation
transaction, against the temp params blob; the hook mutates only the
four coeff fields (registry validation asserts every other byte of the
blob is unchanged by a hook round-trip, mirroring the band engine's
byte-surgery guarantee).

## Coefficient coexistence — deliberate departure from precedent

Every previous adapter demoted its native storage to `writable: false` —
but those natives were never writable to begin with. `temperature`'s
`red`/`green`/`blue`/`various` have been writable scalars since
milestone 1, and multipliers are a legitimate expert surface (copying WB
between images, scripted pipelines). Removing them would be a capability
regression.

Decision: the coeffs **stay writable** and additionally carry
`represented_by: ["wb.temperature"]`. One request that writes any coeff
scalar *and* carries a `wb.temperature` entry is rejected with
`invalid_value` and a conflict detail — the twin-x conflict rule
generalized to scalar-vs-semantic overlap. The engine detects this in
`apply_entries` by checking the patch's scalar field names against the
adapter's declared native fields. The supported-operations reference and
protocol reference both document this as a deliberate exception to the
"native storage goes read-only" precedent.

## Wire and MCP contract

- New hello capability `"quantity_params"`; never advertised without
  accepting quantity entries in `semantic_values`; clients must not send
  quantity entries without it. No protocol version bump.
- Schema (normative shape):

```json
{
  "name": "wb.temperature", "class": "quantity",
  "display_name": "white balance",
  "readable": true, "writable": true, "derived": true,
  "components": [
    { "name": "temperature", "unit": "kelvin",
      "minimum": 1901.0, "maximum": 25000.0 },
    { "name": "tint", "minimum": 0.135, "maximum": 2.326 }
  ]
}
```

- Value (in `get_module_params` and mutation echoes):

```json
{
  "class": "quantity", "active": true, "effective": true,
  "writable_now": true,
  "values": { "temperature": 5502.7, "tint": 1.003 }
}
```

- Patch entry (in `set_module_params`'s `semantic_values`, `class`
  required as in every class):

```json
{
  "wb.temperature": {
    "class": "quantity",
    "values": { "temperature": 5500.0, "tint": 1.0 }
  }
}
```

  `values` must contain **exactly** the schema's component names, each a
  finite number within its component domain; missing, extra, or unknown
  members are `invalid_value`. Validation-error details follow the
  vector/band convention: `{"parameter", "component", "constraint"}`
  (`component` naming the offending component — the m5 lesson: document
  the shipped detail vocabulary in the protocol reference from day one).
- Error precedence extends the dispatch order: parse errors first, then
  curve → vector → bands → quantity.
- Sidecar: a `white_balance`-style convenience argument is *not* added;
  the generic `semantic_values` path plus tool-description guidance
  covers it. (Rationale: `curves`/`vectors`/`bands` sugar exists because
  those shapes needed client-side translation; a quantity entry is
  already the natural JSON a model would write. If usage shows friction,
  sugar is a one-task follow-up.) The sidecar gains only the
  `quantity_params` capability gate wording in `set_module_params`'s
  docstring and README examples.

## Ride-along adapters (milestone-4 vector engine, no new machinery)

| op | semantic name | native | components | range | notes |
|---|---|---|---|---|---|
| `negadoctor` v2 | `dmin` | `Dmin[4]` | 3 (R, G, B) | [0.00001, 1.5] | subtype `color`, `display_rgb` (film-substrate color; GUI shows a color patch + RGB sliders) |
| | `wb_high` | `wb_high[4]` | 3 | [0.25, 2] | plain `vector` (illuminant coefficients, not a display color) |
| | `wb_low` | `wb_low[4]` | 3 | [0.25, 2] | plain `vector` (base-light offsets) |
| `colorharmonizer` v1 | `custom_hue` | `custom_hue[4]` | 4 | [0.0, 1.0] | plain `vector`; `writable_when: rule == DT_COLORHARMONIZER_CUSTOM`; hues validate as plain ranged values (no periodic wrap modeling in v1) |
| | `node_saturation` | `node_saturation[4]` | 4 | [0.0, 2.0] | plain `vector`, always writable |

- Both ops' remaining fields are already-writable scalars/enums;
  `num_custom_nodes` (int, [2, 4]) stays an ordinary scalar governing how
  many of the 4 custom nodes are live — the vector is always written
  whole (4 components), per the whole-set-replacement rule.
- The three negadoctor natives are `float[4]` with 3 live components
  (4th is padding); the adapter maps 3 components onto the array's first
  three elements. If the m4 engine's registry validation requires
  resolved-length == component-count exactly, planning extends it with
  an explicit padded-length allowance rather than silently loosening the
  check.
- Semantic names above are working names; planning confirms them against
  the m4 naming convention (bare names like `lift`, or dotted groups
  like `levels.red`) before freezing.
- Both modules → Tier 1. `negadoctor` usage rare, `colorharmonizer`
  occasional; ratings unchanged.

## Validation and safety

- Quantity: component-domain validation in double space happens
  engine-side before the module hook runs; the hook's internal clamps
  are unreachable defense in depth. The write hook's output blob is
  verified to differ from the input only in the adapter's declared
  native fields.
- Coeff-vs-quantity conflict: rejected atomically, blob untouched.
- Ride-alongs: the m4 engine's existing per-component range validation,
  writability predicates, and byte-atomicity apply unchanged.
- All mutations remain single history items with the established
  undo/revision behavior.

## Testing

Unit (`test_remote_quantity.c`, mirroring the band suite's structure):

- registry fail-closed: missing read hook, missing write hook, wrong
  params version, component-domain inversion, duplicate/cross-class ID;
- pure validation: exact component-name set, finiteness, domain
  boundaries (reject just-outside, accept exact bounds), unknown/extra
  members;
- hook round-trip against the real loaded `temperature` .so: write
  5500/1.0, read back within tolerance, only coeff bytes changed;
- conflict: coeff scalar + quantity entry in one patch rejected, blob
  identical;
- ride-alongs in `test_remote_vector.c`'s per-adapter style: lookup,
  registry_validate against the real .so, schema shapes, round-trips,
  `custom_hue` gating under non-CUSTOM rule → `unsupported_field`,
  padded-length handling for negadoctor.

Integration (one new gate file, m5 file's structure):

- hello advertises `quantity_params`;
- temperature schema shape (components, units, `derived`, coeffs still
  writable with `represented_by`);
- Kelvin/tint write round-trips within tolerance with a visible
  render-diff; a pure-multiplier write still works (no regression);
- conflict request rejected, state unchanged;
- undo and stale-revision gates for a quantity write;
- negadoctor `dmin` write round-trips with render-diff; colorharmonizer
  `custom_hue` gated until `rule` switches to CUSTOM in the same request
  (projected-params precedent), then round-trips.

Tolerance for Kelvin readback is fixed at planning by measuring the real
binary-search quantization (expected well under 1 K; assert e.g. ±5 K to
be robust).

## Documentation (same milestone, not after)

- Protocol reference: `quantity_params` section beside `band_params` —
  schema/value/patch shapes including `class: "quantity"` in the patch
  example, the `{parameter, component, constraint}` error details, the
  lossy-readback note, the coeff-coexistence exception, and the extended
  error-precedence order.
- Supported operations: m6 milestone row; `temperature`, `negadoctor`,
  `colorharmonizer` → Tier 1 (counts 54/11/6 → 57/8/6); the temperature
  Tier 2 caveat ("stored values are multipliers, not Kelvin") replaced
  by the milestone-6 description; appendix unchanged.
- README + remote-control.md: white-balance example
  (`semantic_values` with `wb.temperature`), the coeff-conflict note,
  and the two ride-along mentions in the vectors list.
- This design doc's status flipped to implemented at completion, with
  any in-flight amendments recorded in the status line (the m5
  discipline).

## Open items deferred to planning

- Exact iop API function names/signatures and their `iop_api.h`
  declaration mechanics (follow an existing optional function's
  boilerplate end to end).
- Whether the m4 vector registry needs the padded-length allowance or
  already accepts it (read the validation code before writing tests).
- Final semantic names for the five ride-along fields.
- Measured Kelvin readback tolerance.
- Whether `wb.temperature`'s `display_name` should localize (follow
  whatever the existing engines do for display_name sourcing).
