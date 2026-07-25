# darktable MCP — editing skill design

Date: 2026-07-25
Status: design (approved)
Branch: `mcp-skills` (cut from `mask-support`)
Source grounding: `tools/mcp/src/darktable_mcp/server.py`,
`src/control/remote_masks.c`, and
`docs/superpowers/specs/2026-07-05-darktable-mcp-protocol-reference.md`
at commit `7537128c55`

## Purpose

A Claude Code skill that lets an agent edit a photograph in a running
darktable darkroom through the MCP sidecar — competently, on the first
try, from a natural request like "make the sky moodier".

The audience is the *user* of the MCP, not its maintainer. Nothing here
documents how to extend the remote-edit engines; that knowledge already
lives in the milestone design specs.

## The problem the skill solves

The sidecar exposes 17 tools across four semantic parameter classes, a
blend/mask surface with a state machine, and 71 editable modules across
three support tiers. An agent handed this surface cold makes four
predictable mistakes:

1. **Guessing field names and ranges** instead of reading
   `get_module_schema`, then burning turns on `unknown_field` /
   `invalid_value` rejections.
2. **Editing without verifying.** Parameter values are not the result.
   Without `render_preview` / `get_scopes` the agent cannot tell a good
   edit from a ruined one, and confidently reports success.
3. **Ignoring revisions.** A co-located human is editing the same image.
   Mutations without `expected_revision` silently clobber their work.
4. **Reaching for the wrong module.** darktable has several ways to do
   most things, many of them deprecated or superseded. "Lift the
   shadows" has a right answer (`toneequal`) and several wrong ones.

The skill is structured around preventing exactly these four.

## Structure

One skill, progressive disclosure:

```text
tools/mcp/skills/darktable-editing/
  SKILL.md                     the loop, the rules, the quick intent table
  references/
    intent-map.md              natural language -> module + parameter
    semantic-params.md         curves / vectors / bands / quantities
    masks.md                   blend, parametric, drawn shapes, attachment
    protocol-discipline.md     revisions, atomicity, history, freshness
```

Rejected alternatives:

- **Three sibling skills** (editing / masking / verification). Masking
  never triggers standalone — it is always reached mid-edit — so the
  router would fail to load it at exactly the moment it is needed.
- **One flat SKILL.md.** Masks plus four semantic classes cannot fit
  without burying the workflow, which is the part that matters most.

### Why no `tool-surface.md`

The original design included a reference file enumerating the 17 tools
and their arguments. Grounding against `server.py` killed it: the tool
docstrings are already exhaustive on argument shapes, invariants, and
capability gating, and the MCP client sees them at call time. Restating
them in a reference file would add a second source of truth that drifts.

`protocol-discipline.md` takes its place, carrying only what the
docstrings *do not* say — the cross-call discipline that no single tool
description can express.

## SKILL.md contents

**Preflight.** Confirm darktable is reachable and an image is open
(`get_current_image`). Everything else fails with `not_in_darkroom` or
`no_image_open` otherwise.

**The loop.** Orient → inspect → change → verify → repeat.

- *Orient*: `get_current_image` for the image and the starting revision;
  `list_modules` for what is live, in pixelpipe order.
- *Inspect*: `get_module_schema` before every first write to a module.
  Non-negotiable — it is the only authoritative source of field names,
  ranges, enum members, writability, and `semantic_fields`.
- *Change*: `set_module_params`, threading `expected_revision`.
- *Verify*: `render_preview` for the result; `get_scopes` for clipping
  and tonal distribution; `render_preview` with `show_mask` for mask
  shape.

**Five rules that prevent damage:**

1. Read the schema before the first write to any module.
2. Thread `expected_revision` through every mutation; on
   `revision_conflict`, re-read and re-plan rather than retrying blind —
   the human changed something.
3. `set_module_params` never enables a disabled module. Pass
   `enable: true` in the same call.
4. Verify with pixels, not parameters. Never report an edit as done on
   the strength of a successful write.
5. Prefer the modern module. Never add a deprecated module to a fresh
   edit; `colorbalancergb` over `colorbalance`, `filmicrgb`/`sigmoid`
   over curve hacks.

**Quick intent table.** The ~20 highest-value mappings inline
(exposure, white balance, contrast, shadows/highlights, saturation, hue,
sharpening, denoise, crop, vignette, graduated sky). The long tail lives
in `references/intent-map.md`.

**Error recovery table.** `not_in_darkroom`, `no_image_open`,
`revision_conflict`, `unknown_field`, `unsupported_field`,
`invalid_value`, `instance_not_supported`, `request_too_large`,
`retry_later`, `scope_failed`, and the five capability refusals — each
with the corrective action, not just the meaning.

## Reference file contents

**`intent-map.md`** — every Tier 1 and Tier 2 module keyed by what a
person actually says, carrying the module's usage rating and the
specific fields or semantic IDs to move. Organized by editing intent
(tone, color, detail, geometry, effects), not alphabetically. Flags
Tier 2 caveats and the sensor-level danger modules (`rawprepare`,
`colorin`/`colorout`).

**`semantic-params.md`** — the four classes, each with payload shape,
which modules expose which IDs, the capability gate, and the traps:
curve x-spacing and stored-space coordinates; vector identity values
(colorbalance stores identity as 1.0, not 0.0); band count/x-policy and
twin sharing; quantity lossy read-back and the coefficient conflict.

**`masks.md`** — the three surfaces in escalating order: blend settings
(opacity/mode/colorspace, and the warning that writing `colorspace`
resets `mode`/`reverse`/`fulcrum`/thresholds), parametric masks (slot
markers, combine polarity, the `Jz_in` sky and shadow recipes),
and drawn shapes (circle/ellipse/gradient geometry members,
preview-normalized coordinates, attachment states, shared-shape
`affects_instances`, drawn-mask-mode-via-attach-only). Includes the
undo-granularity wart: one `undo` reverts one history item, and a
create-with-attach records two.

**`protocol-discipline.md`** — the revision model and CAS, patch
atomicity, history semantics, undo granularity, preview-pipe freshness
(`retry_later` after distortion-changing edits), instance/
`multi_priority` addressing, and the 16 MiB frame limit.

## Grounding and drift

Each reference file carries a source line naming the files and commit it
was derived from. The intent map's usage ratings are inherited from
`2026-07-16-darktable-mcp-supported-operations.md` and are explicitly
editorial community judgment, not telemetry.

## Out of scope

- **Skill evals.** Triggering-accuracy and behavioral evals need a
  running darktable with an image open; the MCP's own integration tests
  already cover the wire surface. Can be added later as its own scope.
- **A contributor-facing skill** for extending the remote-edit engines.
- **Export, library/lighttable operations, and raster masks** — none are
  in the MCP surface.
