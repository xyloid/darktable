# Darktable Silhouette and Amber Sky Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the tree and foreground ridge read as very dark silhouettes while enriching the existing warm-amber sky and moon halo without losing the stars or clipping the moon.

**Architecture:** Apply two independent, revision-guarded darktable history changes. The existing tone equalizer will shape only the darkest zones; the default color balance RGB instance will add hue-preserving saturation through a scene-referred luminance mask that excludes the silhouette. Verify every mutation by live read-back, mask render, final preview, and histogram scopes.

**Tech Stack:** darktable remote-control MCP, tone equalizer, color balance RGB, scene-referred parametric masks, JPEG preview rendering, histogram scopes

## Global Constraints

- Operate only on image id `4725`, filename `DSC05895.ARW`, while it is open in the darkroom.
- Preserve the warm amber hue direction; apply no hue rotation.
- Do not change crop, orientation, denoising, lens correction, or composition.
- Keep the stars visible and the moon's bright core controlled.
- Use the live revision as `expected_revision` for every mutation; stop on any image change or revision conflict.
- Final black and white clipping fractions must each remain below `0.005`.
- Allow at most one conservative refinement pass, limited to the specified tone, saturation, and mask-threshold values.

## Runtime State

No application source files are modified. The deliverable consists of reversible history entries in the running darktable darkroom session. This plan file is the only repository artifact.

---

### Task 1: Establish a Safe Live Baseline

**Files:**

- Reference: `docs/superpowers/specs/2026-07-20-darktable-silhouette-amber-sky-design.md`
- Modify: none; read live darktable state only

**Interfaces:**

- Consumes: the running darktable session
- Produces: baseline revision `R0`, current tone-equalizer values, a baseline preview, and baseline histogram summary

- [ ] **Step 1: Confirm the intended image and capture revision `R0`**

Call `get_current_image({})`.

Expected: `view` is `darkroom`, `image.id` is `4725`, `image.filename` is `DSC05895.ARW`, and `revision` is an integer. Stop without mutation if any identity check fails.

- [ ] **Step 2: Read the existing silhouette-shaping module**

Call `get_module_params({"module":"toneequal","instance":0})`.

Expected: the module is enabled and its returned revision equals `R0`. Record all returned values so unlisted parameters remain untouched.

- [ ] **Step 3: Capture baseline visual and numeric evidence**

Call `render_preview({"max_px":1600,"quality":90})`, then call:

```json
{
  "scopes": ["histogram"],
  "include_summary": true,
  "include_bins": false,
  "include_images": false
}
```

Expected: both calls succeed and the scope revision equals `R0`.

---

### Task 2: Shape the Tree and Ridge into a Dark Silhouette

**Files:**

- Modify: none; create one darktable history transition

**Interfaces:**

- Consumes: baseline revision `R0`
- Produces: post-tone revision `R1` and verified tone-equalizer values

- [ ] **Step 1: Apply the approved low-zone curve**

Call `set_module_params` with:

```json
{
  "module": "toneequal",
  "instance": 0,
  "expected_revision": "R0",
  "values": {
    "noise": -0.8,
    "ultra_deep_blacks": -1.2,
    "deep_blacks": -1.0,
    "blacks": -0.5,
    "shadows": 0.2
  }
}
```

Substitute the integer value of `R0` for the symbolic string. Record the returned revision as `R1`.

Expected: the module remains enabled, the five returned values match within normal float narrowing, and `R1` is newer than `R0`.

- [ ] **Step 2: Read back and visually test the silhouette change**

Call `get_module_params({"module":"toneequal","instance":0})` and `render_preview({"max_px":1600,"quality":90})`.

Expected: both responses use `R1`; the tree and ridge are darker than the baseline, while the sky and moon remain readable.

---

### Task 3: Enrich the Amber Sky and Moon Halo

**Files:**

- Modify: none; create one darktable history transition

**Interfaces:**

- Consumes: post-tone revision `R1`
- Produces: post-color revision `R2`, enabled color balance RGB settings, and a verified luminance mask

- [ ] **Step 1: Apply masked, hue-preserving color enhancement**

Call `set_module_params` with:

```json
{
  "module": "colorbalancergb",
  "instance": 0,
  "expected_revision": "R1",
  "enable": true,
  "values": {
    "chroma_global": 0.18,
    "saturation_global": 0.10,
    "vibrance": 0.12,
    "hue_angle": 0.0
  },
  "blend": {
    "colorspace": "DEVELOP_BLEND_CS_RGB_SCENE",
    "mode": "DEVELOP_BLEND_NORMAL2",
    "reverse": false,
    "fulcrum": 0.0,
    "opacity": 100.0,
    "combine": "exclusive",
    "mask_mode": "parametric",
    "parametric": {
      "Jz_in": {
        "markers": [0.02, 0.06, 1.0, 1.0],
        "inverted": false
      }
    }
  }
}
```

Substitute the integer value of `R1` for the symbolic string. Record the returned revision as `R2`.

Expected: the module is enabled, all four values and the complete blend state read back as requested, and `R2` is newer than `R1`.

- [ ] **Step 2: Test the luminance mask**

Call:

```json
{
  "max_px": 1600,
  "quality": 90,
  "show_mask": {
    "op": "colorbalancergb",
    "instance": 0
  }
}
```

Expected: the tree and ridge are nearly black in the mask, the sky and moon halo are light enough to receive the effect, and no bright outline appears around the branches.

- [ ] **Step 3: Correct the mask once only if the mask test fails**

If the silhouette is visibly affected, update only `Jz_in.markers` to `[0.04, 0.08, 1.0, 1.0]` using `expected_revision: R2`. If most of the sky is excluded instead, update only the markers to `[0.01, 0.04, 1.0, 1.0]`. Do not change the mask if Step 2 passes. When changed, replace `R2` with the returned new revision for all later steps and render the mask again.

Expected: the second mask render meets the Step 2 criteria.

---

### Task 4: Verify and, If Necessary, Refine the Finished Edit

**Files:**

- Modify: none; read live state and optionally create one bounded refinement transition per affected module

**Interfaces:**

- Consumes: the latest post-color revision `R2`
- Produces: final revision `RF`, final preview, scope summary, and module read-backs

- [ ] **Step 1: Render and inspect the finished image**

Call `render_preview({"max_px":1600,"quality":90})` and inspect it against the baseline.

Expected: the tree and ridge form cohesive near-black silhouettes; the sky is visibly richer warm amber; the crescent, halo, and stars remain legible; and there is no branch-edge halo.

- [ ] **Step 2: Run objective clipping checks**

Call:

```json
{
  "scopes": ["histogram"],
  "include_summary": true,
  "include_bins": false,
  "include_images": false
}
```

Expected: the histogram revision equals the latest live revision, `black_clip_fraction < 0.005`, and `white_clip_fraction < 0.005`.

- [ ] **Step 3: Make at most one conservative visual refinement if required**

Use the latest observed revision for every selected patch. Apply only the matching preset below:

- Silhouette still shows distracting internal texture: set tone equalizer `noise=-1.0`, `ultra_deep_blacks=-1.5`, `deep_blacks=-1.2`, `blacks=-0.7`, and `shadows=0.0`.
- Amber enhancement is too weak: set color balance RGB `chroma_global=0.23`, `saturation_global=0.14`, and `vibrance=0.16`.
- Amber enhancement looks artificial or obscures star color: set color balance RGB `chroma_global=0.12`, `saturation_global=0.06`, and `vibrance=0.08`.

Skip this step if the initial result meets the visual criteria. Do not perform another refinement cycle.

- [ ] **Step 4: Repeat final read-back and verification after any refinement**

Call `get_current_image({})`, read both edited modules with `get_module_params`, render the final preview at `1600` pixels and quality `90`, and repeat the histogram call from Step 2.

Expected: the image identity is unchanged, every response reports one coherent final revision `RF`, both edited modules match the chosen settings, all visual criteria pass, and both clipping fractions remain below `0.005`.
