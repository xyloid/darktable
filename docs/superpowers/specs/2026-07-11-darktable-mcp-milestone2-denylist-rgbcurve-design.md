# darktable MCP — Milestone 2: Field Denylist + rgbcurve Semantic Curves

Date: 2026-07-11
Status: approved design
Depends on: `2026-07-05-darktable-mcp-design.md`,
`2026-07-05-darktable-mcp-protocol-reference.md`,
`2026-07-05-darktable-mcp-supported-operations.md`,
`2026-07-05-darktable-mcp-curve-classes-low-level-design.md`

## Objective

Two deliverables, in order:

1. **Part A — safety debt.** Wire the per-module `writable: false` field
   denylist that the supported-operations reference mandates. The mechanism
   exists in `src/control/remote_edit.c` and is unit-tested, but both
   production call sites pass `NULL`, so bookkeeping fields such as
   `filmicrgb.version` are currently advertised writable and accepted in
   patches.
2. **Part B — editing power.** Implement the curve-classes low-level design,
   steps 1–7: semantic curve discovery, reading, validation, and atomic
   mutation for `rgbcurve`, exposed through the existing MCP tools behind a
   `curve_params` protocol capability, meeting that design's acceptance gate.

The curve-classes low-level design remains normative for all Part B
internals (types, registry, validator, transaction, wire shapes). This spec
pins milestone scope and resolves the decisions that document left open.

## Non-goals

- `tonecurve`, `colorzones`, and `basecurve` adapters (next milestone;
  design step 8).
- Sampled-response/banded parameter classes, e.g. `lowlight` (design step 9).
- Point-level curve CRUD (whole-value replacement only, per design
  decision 4).
- New MCP tools; the tool count stays at twelve.
- Curve presentation metadata and the Bauhaus units/increments open question
  from the main design.
- Library, export, masks, blending.

## Part A — per-module field denylist

### Design

- One static per-op table in `src/control/remote_edit.c`, next to the
  existing `dt_remote_denylisted()` helper. Central and hand-written, the
  same choice made for curve registry descriptors (no generation step).
- Contents come from the supported-operations appendix (~20 ops, ~40
  fields). The appendix is normative but dated 2026-07-05; during
  implementation every entry is re-verified against current `src/iop/*.c`
  source per the appendix's maintenance note, and corrections flow back
  into the appendix in the same commit.
- New internal lookup:

  ```c
  const dt_remote_denylist_t *dt_remote_denylist_for_op(const char *op);
  ```

  Returns NULL (deny nothing) for ops without an entry.
- Wire it into both production call sites:
  - schema generation in `dt_remote_get_module_schema` (currently passes
    NULL at `remote_edit.c:757`) — denylisted fields stay **in** the schema
    with `writable: false`, never omitted, consistent with the resolved
    unsupported-schema-fields decision;
  - patch application in `dt_remote_set_module_params` (currently passes
    NULL at `remote_edit.c:894`) — a patch naming a denylisted field fails
    whole with `DT_REMOTE_ERR_UNSUPPORTED_FIELD`, changing nothing.

### Error surface

No new error codes. Denylisted writes surface as the existing
`unsupported_field` protocol error; the sidecar already maps it to an
actionable hint.

### Tests

- C unit: for a representative sample (at minimum `filmicrgb.version`,
  `channelmixerrgb.x`, one `ashift` line-fit field), the generated schema
  reports `writable: false` and `dt_remote_set_module_params` rejects the
  field atomically.
- Shared fixture: one new error-response fixture for a denylisted-field
  patch, consumed by both the C dispatcher tests and the sidecar unit tests.
- Live integration: one assertion that `filmicrgb`'s schema marks `version`
  non-writable.

## Part B — rgbcurve semantic-curve slice

Scope is steps 1–7 of the curve-classes design's implementation order:

1. neutral semantic patch/value ownership and destroy functions;
2. safe introspection cursor and fixture tests;
3. curve validator and serializer fixtures;
4. registry validation and read-only `rgbcurve` descriptor;
5. schema/value responses gated behind the `curve_params` capability;
6. semantic patches integrated into the existing temporary-block
   transaction;
7. `rgbcurve` writes enabled, with integration tests.

Exit criterion is that design's acceptance gate verbatim: schema/value
round-trip without native-layout leakage; invalid points cannot mutate live
state; linked/manual conditions enforced against projected params; scalar +
curve + enable + revision + history + GUI + pixelpipe as one atomic
mutation; one undo restores the exact prior curve and mode; registry drift
fails closed; the sidecar exposes the capability without breaking
primitive-only clients or servers.

### Resolved decisions

These resolve the "Unresolved implementation decisions" list in the
curve-classes design for this milestone:

1. **Middle-grey transition without a work profile** — fail closed:
   `invalid_state`, mutating nothing. No silent fallback profile.
2. **`rgbcurve` mode transitions without GUI callbacks** — the adapter
   callback reproduces the two documented storage transitions (manual-RGB
   entry copies channel 0 into untouched identity G/B; middle-grey change
   transforms stored points through the current work profile) on the
   temporary params block only. Integration tests cover both transitions,
   with and without an available work profile.
3. **Protocol versioning** — no bump. `protocol_version` stays 1. The
   server adds `"curve_params"` to the hello `capabilities` array. The
   sidecar sends the `curves` request member only when the capability is
   advertised, so an older server can never silently drop the curve half of
   a patch: the client never sends it. This follows the protocol
   reference's additive-change rule.
4. **Inactive semantic values** — always returned, marked `"active": false`
   (e.g. `curve.red` while autoscale is not manual RGB). No read option;
   mirrors the never-omit / `writable: false` philosophy.
5. **Descriptor storage** — central static registry in new
   `src/control/remote_curves.[ch]` (the design's proposed files), not
   generated from a declarative source.
6. **Per-IOP transport-neutral callbacks and post-commit hooks** — not
   introduced. The registry callback mechanism suffices for `rgbcurve`;
   revisit only if adapter tests prove GUI synchronization insufficient.
7. **MCP exposure** — extend `set_module_params`; no dedicated curve tool
   (user decision, 2026-07-11).

### MCP tool surface

No new tools. Changes to existing tools, all additive:

- `set_module_params` gains an optional `curves` argument:

  ```json
  {
    "module": "rgbcurve",
    "instance": 0,
    "curves": {
      "curve.master": {
        "points": [[0.0, 0.0], [0.4, 0.5], [1.0, 1.0]],
        "interpolation": "cubic_spline"
      }
    }
  }
  ```

  `values` and `curves` may appear together; the pair is one atomic
  transaction and one history item. The sidecar raises a client-side error
  if `curves` is used against a server that does not advertise
  `curve_params`.
- `get_module_schema` and `get_module_params` include `curves` sections
  when the capability is advertised (shapes per the curve-classes design's
  protocol extension section).
- Tool docstrings teach the model the invariants it cannot infer: 2–20
  points, strict x ordering, minimum x spacing 0.0025, coordinates are
  stored values (no GUI log/semilog/zoom transforms), and whole-curve
  replacement semantics.

### Threading and ownership

Unchanged from the curve-classes design: all mutation work happens in the
existing main-thread transaction; neutral types have paired destroy
functions; no introspection pointers cross the protocol boundary.

## Testing strategy

Same layering as v1:

- **C unit + shared fixtures** — introspection cursor, validator (ordering,
  spacing, domain, point count, non-finite rejection), registry validation
  and drift fail-closed, transaction atomicity across scalar+curve patches,
  denylist behavior. Fixtures produced by the C dispatcher are consumed by
  sidecar unit tests, keeping both sides byte-compatible.
- **Sidecar unit** — `curves` threading and omission, capability gating
  (client-side refusal when unadvertised), schema/value passthrough,
  updated exact-tool-list/docstring assertions.
- **Live integration** — the acceptance-gate items: round-trip, atomic
  combined edit, one-undo restoration of curve and mode, both middle-grey
  paths, denylisted-field rejection.

## Delivery order

Part A lands first as an independent commit sequence (table + wiring +
tests + appendix corrections), then Part B follows the curve design's
implementation order. Each step keeps the full unit suite green; live
integration gates land with steps 7 (curve writes) and the Part A wiring.

## Risks

- **Appendix drift** — the denylist table may name fields that were renamed
  or removed since 2026-07-05; mitigated by per-entry source verification.
- **rgbcurve transition semantics** — reproducing `gui_changed()` behavior
  without GUI handlers is the highest-uncertainty item; mitigated by the
  two dedicated integration tests and the fail-closed no-profile policy.
- **Fixture sprawl** — curve schemas/values add many fixtures; mitigated by
  reusing the existing fixture-sharing convention rather than inventing a
  new harness.
