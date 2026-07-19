# darktable MCP — picker-related candidates for a future milestone

Date: 2026-07-18
Status: candidate notes (no milestone scheduled; postponed by decision after
milestone 6)
Companions: `2026-07-16-darktable-mcp-supported-operations.md` (tier state
these notes start from), `2026-07-17-darktable-mcp-milestone6-quantity-class-design.md`
(the quantity class and conversion-authority precedent referenced below),
`2026-07-05-darktable-mcp-parameter-class-investigation.md` (class taxonomy).

## Purpose

Post-milestone-6 discussion notes on the remaining "picker" territory:
which modules are held back by picker-set parameters, what a
photo-sampling primitive would look like, and the recommended candidates
for whenever this topic is picked up. Nothing here is designed or
committed — this document exists so the discussion does not have to be
re-derived.

## Two different kinds of "picker"

The GUI's pickers split into two categories with very different remote
stories:

1. **Picker as a convenience for setting an ordinary parameter.**
   Temperature's spot white balance, monochrome's filter picker, filmic's
   auto-tune, rgblevels' black/white point pickers. The parameter is just
   a value; the picker is one GUI way of deriving it. These modules can be
   made remotely editable *without* any picker — the model supplies the
   value directly. This is what milestone 6 did for `temperature`.
2. **Pickers whose job is to read the photo itself.** The parameter's
   *correct* value is defined by the image content: `negadoctor`'s `Dmin`
   is the color of the unexposed film base, sampled from the negative's
   border; spot white balance samples a patch that should be neutral;
   `channelmixerrgb`'s illuminant estimate and monochrome's
   pick-filter-from-photo are the same shape. Remote support here needs a
   way to *sample the image*, not just write values.

## The gap: the preview cannot substitute for a picker

The model can see the rendered preview JPEG and reason about *where* to
sample ("the film border is in the top-left corner") — but it cannot
recover the *numbers* from the preview. The preview is a display-referred
sRGB render of the **end** of the pixelpipe, while a picked `Dmin` needs
values in negadoctor's working space at negadoctor's **point** in the
pipe. No image analysis of the preview closes that gap.

## Proposed primitive: `sample_region`

A new read-only protocol method (a new *method*, not a new parameter
class — a different milestone shape than m2–m6): given normalized image
coordinates and a radius plus a target module, return mean/min/max pixel
values **as that module's input sees them**.

darktable already has the machinery internally: the GUI color picker runs
through the preview pixelpipe (`DT_REQUEST_COLORPICK` / `picked_color` in
the picker proxy), computing region statistics at a module's position in
its working space. A remote method would be a thin, bounded, read-only
exposure of existing infrastructure. It fits the security posture (no new
mutation surface, no file access) and would be capability-gated like
everything else.

This makes the model a *user* of pickers rather than locked out of them:
look at the preview → choose coordinates → `sample_region` → write the
value through the existing m4/m6 write paths.

### Critical-path risk: coordinate back-transform

The model picks coordinates off the finished preview, which is
post-crop/flip/perspective; the sample must be back-transformed through
the pipe to the target module's input position (darktable's
`distort_backtransform` machinery — the GUI picker already does this).
This is where the engineering time will go; the sampling statistics
themselves are nearly free.

### Open questions for the eventual design

- Coordinate space on the wire (preview-relative? full-image normalized?
  who performs the back-transform?). Note: the drawn-masks design
  (`2026-07-19-darktable-mcp-drawn-masks-design.md`) has since answered
  this for its own case (preview-normalized, engine back-transforms) and
  the two efforts share one transform helper — align with it.
- **Pipe-freshness contract** (added 2026-07-19, shared with the
  drawn-masks milestone): a transform/sample issued between a mutation
  and pipe reprocess must not silently use stale geometry — define
  block-until-clean vs `retry_later` semantics once; whichever effort is
  planned first writes the contract, the other inherits it.
- Which pipeline point and colorspace the response reports — per-module
  input space is what the write side needs, but it must be explicit wire
  vocabulary, not implicit.
- Region shape (box vs circle) and which statistics (mean/min/max) are
  returned.
- Capability name (e.g. `sample_params` / `color_sample`) and error
  vocabulary.

## Consumer ranking (first workflows to prove the primitive)

1. **`negadoctor` `Dmin` — best first consumer.** The GUI picker copies
   the sampled values into `Dmin` verbatim: no conversion math between
   sample and write, and the write path already exists (m6's `dmin`
   vector). The purest end-to-end proof: preview → coordinates →
   `sample_region` → `vectors: {"dmin": [...]}` → visible render change.
   Physically well-defined target (film-base color), so an integration
   gate can assert something meaningful (sample the border, write it,
   verify the border renders near-white). Weakness: usage rated rare.
2. **`temperature` spot white balance — high-value second.** Core-usage
   module, write path shipped in m6, and "make this patch neutral" is a
   common request. One conversion step: sample the patch in camera RGB at
   temperature's input; coefficients are the normalized inverse. Where
   that inversion lives (sidecar, engine, or a module hook like m6's) is
   the same conversion-authority question the quantity class already
   answered once. Natural milestone shape: negadoctor proves the
   mechanism, temperature is the high-value ride-along.
3. **`monochrome` / `channelmixerrgb` illuminant — later.** Monochrome's
   pick-from-photo needs the monochrome parameter surface to exist first
   (see below). The channelmixerrgb illuminant estimate is area-based
   statistics rather than a spot sample, and that module already has a
   workable conversational white-balance path — low marginal value.

## Related but separable: `monochrome` as a quantity adapter (no sampling needed)

If a milestone wants "one picker-related operation" *without* building
`sample_region`, `monochrome` is the cheapest promotion — its picker is
category 1 (convenience), so direct value writes suffice:

- Highest usage (occasional) among the picker holdouts; `colorcorrection`
  is rare, `channelmixerrgb`'s denylisted illuminant coords are marginal.
- Its Tier 2 row lists exactly one unsupported item: the `a`/`b` pair
  (unranged Lab filter coordinates). One adapter → Tier 1.
- Technically a second quantity-class adapter, but strictly easier than
  temperature: `(a, b)` ↔ (filter hue, strength/chroma) is closed-form
  `atan2`/`hypot` with **no module state** — pure, deterministic hooks,
  exactly invertible readback. It would prove the m6 class generalizes
  (the m6 design deferred "any second quantity adapter … wait for
  demand").
- Design questions to settle at brainstorming: hue periodicity (m6
  deferred modeling it; colorharmonizer's plain-ranged-hue precedent is
  probably still the right v1 answer); the origin singularity (`a = b =
  0`, the module default, has undefined hue — the read hook needs a
  convention, e.g. hue 0 with strength 0); domains (`a`/`b` are unranged
  in introspection, so component bounds come from the GUI panel's working
  range, pinned explicitly the way temperature pinned its Kelvin/tint
  constants).
- Natural ride-along: `colorcorrection` (same Lab-coordinate semantics,
  two point-pairs instead of one, reuses whatever conversion convention
  monochrome establishes; rare usage but nearly free, clears another
  Tier 2 row).

## Recommendation recorded

For a milestone about **operations that require picking from the photo**:
`sample_region` + the negadoctor film-base workflow as the proving
consumer, with temperature spot-WB as the ride-along if scope allows.
When picked up, take it through brainstorming to a design doc — the open
questions above are exactly spec material.
