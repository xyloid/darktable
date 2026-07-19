# darktable MCP — mask support: hard-challenge analysis

Date: 2026-07-19
Status: risk analysis of the three tier designs (adversarial pass; written
after, and against, the low-level designs)
Companions: `2026-07-19-darktable-mcp-blend-settings-design.md`,
`2026-07-19-darktable-mcp-parametric-masks-design.md`,
`2026-07-19-darktable-mcp-drawn-masks-design.md`,
`2026-07-19-darktable-mcp-mask-support-design-candidates.md`.

## Purpose and method

The tier designs describe how to build mask support. This document
stress-tests them: where the effort can actually fail, stall, or ship
something that does not work in practice. Each challenge is rated
**severity** (impact if unaddressed) and **confidence** (how sure the
analysis is that the problem is real, given today's verification depth).
It closes with the deflated risks — things that looked hard earlier and
demonstrably are not — and three scope recommendations.

## The ranked hard challenges

### H1. The closed-loop feedback gap: an agent cannot see a mask
**Severity: high (product). Confidence: high. Affects: Tiers 2 and 3.**

Masks are visual artifacts, but the only feedback surface is
`render_preview` — the *blended result*. A GUI user placing a parametric
threshold watches the yellow mask overlay
(`module->request_mask_display`, `DT_DEV_PIXELPIPE_DISPLAY_MASK` —
machinery verified present in `imageop.h:196` / `pixelpipe.h:68`); an
agent gets nothing. Consequences:

- Tier 2 threshold-picking is blind guesswork: choosing `Jz_in` markers
  requires knowing the channel's value distribution in the region of
  interest, which no existing tool exposes (`get_scopes` is
  display-referred, end-of-pipe, wrong space). The agent's only recourse
  is preview-diff inference — slow, indirect, and ambiguous ("did the sky
  change because the mask caught it, or because the ramp leaks
  everywhere?").
- Tier 3 placement verification via preview-diffing is similarly weak
  (a low-opacity effect inside a mask produces a near-zero diff).

Engineering-wise the tiers are buildable without this; *effectively*,
Tier 2 in particular risks shipping as a capability agents cannot use
well. **Recommendation (scope change): add a mask-render primitive to
the effort** — e.g. `render_preview` gains `show_mask: {op, instance}`
riding on the existing display-mask pipeline — ideally landing with
Tier 2, not after it. This is the cheapest high-leverage addition
available: the pipeline support already exists for the GUI.

### H2. Concurrent GUI mask editing vs remote form mutation
**Severity: high (crash/corruption). Confidence: medium-high. Affects: Tier 3.**

`dev->form_visible` and `dev->form_gui` hold live editing state —
including `form_gui->points`, cached derived geometry of the form being
edited (`masks.c:1320`) — while the user drags a shape. A remote
`update_mask_shape`/`delete_mask_shape` on *that* form mutates or frees
structures the GUI interaction code is concurrently reading on the same
GTK thread across events: no data race in the memory sense (same
thread), but a use-after-free or stale-cache desync across event
iterations is plausible (delete frees the form; the next motion event
dereferences `form_visible`). The GUI never has this problem because all
its edits flow through the interaction state machine.

The Tier 3 design does not address this. **Recommendation: the engine
must check `dev->form_visible`/`form_gui` before mutating a form and
either (a) cancel the GUI edit session first
(`dt_masks_change_form_gui(NULL)` — the GUI's own escape path), or (b)
refuse with a retryable error naming the conflict.** (a) matches the
"remote drives the GUI" posture; either way it needs a dedicated
integration test (start GUI edit via synthetic events is hard — at
minimum a unit test that the guard path exists and a documented manual
test). This is exactly the class of bug that survives until a human
user meets the agent mid-edit.

### H3. Pipe-state dependence of the coordinate transforms
**Severity: high (correctness/latency). Confidence: medium. Affects: Tier 3 (and sample_region).**

`dt_dev_distort_backtransform` walks the pixelpipe's distortion modules.
Between a mutation and pipe reprocess completion, or on a
freshly-loaded image, the pipe may be unprocessed, mid-invalidation, or
reflect *pre-mutation* geometry. The GUI tolerates this because
transforms re-run every redraw — a stale frame self-corrects. A wire
call answers once: a `create_mask_shape` issued immediately after
enabling a rotation could back-transform through the *old* geometry and
place the shape wrong, deterministically, with no self-correction.

The Tier 3 design says "the calls follow the same locking the GUI
relies on" — true but insufficient; locking prevents races, not
staleness. **Needs design-level resolution during planning:** define the
freshness contract (block until the preview pipe is clean for the
current history top? return `retry_later` when dirty? both, with a
timeout?), and test it explicitly (mutate distortion + create shape in
the same client breath). This is also the sharpest open question
inherited by `sample_region`, so solving it once here pays twice.

### H4. Parametric-mask ergonomics for a model client
**Severity: medium-high (product). Confidence: high. Affects: Tier 2.**

Three compounding cognitive hazards, all faithful to storage and all
documented, but documentation is not a mitigation for a model working
at inference time:

- effective polarity = `inverted XOR combine-inclusive` (kernel-verified)
  — sign errors will be common and *look* like mysterious mask
  inversion;
- stored 0–1 values vs display units vs `2^boost` scaling — a model
  reasoning "Lab L above 80" must compose two conversions correctly;
- the derived-enable rule makes "full-span markers" silently mean
  "channel off".

Mitigations worth building rather than documenting: sidecar-level
warnings (e.g. reject `inverted` changes in the same call as a `combine`
change unless explicitly overridden — the highest-confusion combo), a
worked-examples section in the tool description (models follow examples
far better than rules), and H1's mask render for empirical verification.
Without at least the last, expect Tier 2 to *demo* well and *work*
poorly.

### H5. Fork divergence and upstream drift
**Severity: medium (sustained cost). Confidence: high. Affects: all tiers, growing.**

The remote-edit branch is an unmerged fork. The mask effort deepens the
entanglement in the two most actively-maintained areas of darktable
(blend, masks): Tier 1 refactors `blend_gui.c`'s mode population, Tier 3
exports `_group_create`, Tier 2's parity tests bind to GUI table layouts,
and the hand-written blend schema tracks a struct upstream changes
without ceremony (DEVELOP_BLEND_VERSION guards layout, **not**
semantics — a new blend mode or changed slider range bumps nothing).
Each future rebase pays this tax; parity tests turn silent drift into
loud drift but still require a human to resolve it. **Recommendation:**
keep a single `docs/` manifest of every deliberate divergence from
upstream files (the two refactors, the parity bindings) so rebases have
a checklist; and treat "upstream added a blend mode" as a rehearsed,
tested procedure (one table row + one test row), not an incident.

### H6. Verification cost of spatial/visual behavior
**Severity: medium (schedule/flakiness). Confidence: high. Affects: Tiers 2–3.**

The integration gates that matter — "the effect is confined to the
circle", "the mask lands where preview coordinates said", "combine flip
changes the rendering" — are pixel-statistical assertions under Xvfb,
sensitive to tolerances, image choice, OpenCL availability, and preview
resolution. The existing suite has one benign skip; this effort
multiplies the flake surface. Budget explicitly for: fixture images
chosen for high-contrast regions, generous-but-meaningful tolerances
(assert *ratios* between inside/outside deltas, not absolutes), and a
policy that spatial gates run on the preview pipe at a pinned size.
Under-budgeting this is how the milestone slips a week at the end.

### H7. Tool-surface growth vs agent context budget
**Severity: medium (product). Confidence: medium. Affects: Tier 3 mainly.**

Tier 3 adds six MCP tools (12 → 18) plus the blend vocabulary inside
`set_module_params`. Every tool's schema and docstring lands in the
agent's context on every session; the mask tools carry the largest
docstrings yet written for this project (coordinate contract, state
vocabulary, warnings). Mitigation options to weigh at planning:
merge attach/detach into one `set_mask_attachment` tool, fold
update/delete into create-style dispatch, or accept the count but write
docstrings with a hard token budget. Do not discover this after the
integration suite passes.

### H8. Blend/mask state-machine interaction matrix
**Severity: medium (correctness). Confidence: medium-high. Affects: Tier 1→3 seams.**

`mask_mode` transitions interact with stored state across tiers:
Tier 1 refuses when drawn bits exist; Tier 3 ORs bits in and clears
them when groups empty; Tier 2 requires `CONDITIONAL` projected; the
GUI meanwhile can produce any combination (including raster). The
individual rules are each simple; the *matrix* — which transitions are
legal from which stored states via which tier's surface, and what the
compound read-only strings report — is where contradictory behavior
will hide. **Recommendation:** before Tier 3 planning, write the full
transition table (stored mode × requested operation → result/error) as
a spec appendix and generate unit tests from it mechanically. The
designs currently define rows of this table in three different
documents; nobody has checked they compose.

## Deflated risks (previously assumed hard, now known not to be)

- **Headless creation sequence** (the candidates doc's headline Tier 3
  cost): `dt_masks_gui_form_save_creation` is NULL-gui-safe; the engine
  reuses it. One static-function export remains.
- **History/undo/revision plumbing**: mask items flow through the
  existing signal and undo scopes; verified, no new design.
- **Tier 2 engineering**: pure functions over the Tier 1 copy; zero new
  commit surface. Its risk is H4 (usability), not construction.
- **Struct layout churn**: the version guard + offsetof asserts reduce
  it to a build failure. (Semantic churn remains — see H5.)

## Scope recommendations (actionable)

1. **Pull a mask-render primitive into the effort** (with Tier 2):
   `render_preview` + `show_mask` on the existing display-mask pipeline.
   Addresses H1, halves the practical weight of H4, and gives H6's
   integration gates a direct observable (assert on the mask image
   itself instead of inferring through blended previews).
2. **Resolve H3 (pipe freshness) as a design amendment before any
   Tier 3 planning** — it is a contract question, not an implementation
   detail, and it is shared with `sample_region`.
3. **Write the H8 transition table into the candidates doc as the
   cross-tier appendix** when the first tier is planned, and keep it the
   single source the per-tier plans cite.

## Bottom line

Nothing here says "don't build it". It says: the construction risks the
designs already handle are not where this effort will hurt. It will hurt
in (a) agents unable to *verify* what they masked — solve with the mask
render, (b) two undesigned interaction seams — GUI concurrency and pipe
freshness — that need contracts, not code, and (c) a long-tail
maintenance tax that is the price of forking darktable's two busiest
subsystems.
