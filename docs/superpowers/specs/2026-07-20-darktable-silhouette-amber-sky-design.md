# Darktable Silhouette and Amber Sky Edit Design

## Context

The current darkroom image is `DSC05895.ARW` at history revision 145. It is a vertical night photograph of a Joshua tree and low ridge silhouetted against a moonlit, starry sky. The existing edit is low-key and warm, but the active tone equalizer lifts the darkest tonal zones, while the requested result is a much darker silhouette and a richer warm-amber sky and moon halo.

## Goal

Create a dramatic but controlled night edit in which:

- the tree and foreground ridge read as near-black silhouettes with little or no distracting internal detail;
- the sky retains visible tonal variation and stars;
- the existing warm amber color becomes noticeably richer;
- the moon's bright core remains controlled while its surrounding halo gains color presence; and
- the edit remains reversible through normal darktable history.

## Non-goals

- Do not change the crop, orientation, denoising, lens corrections, or composition.
- Do not introduce a cool-blue night treatment or a new hue direction.
- Do not recover detail inside the tree or land.
- Do not deliberately clip the moon or large areas of the sky.

## Selected Approach

Use two tonal selections rather than a global contrast-and-saturation change.

### Silhouette shaping

Adjust only the low zones of the existing `toneequal` instance. Replace its current shadow lifts with this descending darkening curve:

- `noise`: -0.8 EV
- `ultra_deep_blacks`: -1.2 EV
- `deep_blacks`: -1.0 EV
- `blacks`: -0.5 EV
- `shadows`: +0.2 EV

Leave its existing midtone, highlight, white, and specular values unchanged. This should close down residual tree and ridge detail while avoiding a global exposure reduction.

### Amber saturation

Enable the default `colorbalancergb` instance with this moderate, hue-preserving color enhancement:

- `chroma_global`: +0.18
- `saturation_global`: +0.10
- `vibrance`: +0.12
- no hue rotation

Limit the module with a feathered scene-referred luminance mask that fades in over the darkest tones and is fully active through the sky and moon halo. Set the starting `Jz_in` markers to `[0.02, 0.06, 1.0, 1.0]`. This excludes the near-black silhouette from the color boost while retaining smooth boundaries around branches.

## Safety and Data Flow

1. Re-read the live history revision immediately before editing.
2. Apply each mutation with compare-and-swap using the observed revision.
3. Read back the written values after each step.
4. Render the color mask before judging the final image. The tree and ridge should appear nearly black in the mask, with no visible halo around branch edges; adjust only the threshold if that criterion is not met.
5. Render a fresh preview and scopes from the resulting revision.

If the live image or revision changes unexpectedly, stop rather than applying the recipe to stale state.

## Verification

The edit is successful when a fresh preview shows all of the following:

- the tree and ridge are visibly darker than the baseline and read as cohesive silhouettes;
- the sky is clearly more saturated while retaining the same warm amber family;
- the bright crescent and halo remain visually controlled;
- stars remain visible and are not overwhelmed by the saturation increase;
- histogram analysis reports black and white clipping fractions below 0.5% each; and
- the live module read-back matches the intended settings.

One conservative refinement pass is allowed if the first render misses these visual criteria. Any refinement must use the new live revision and remain limited to the parameters named above or the luminance-mask threshold.
