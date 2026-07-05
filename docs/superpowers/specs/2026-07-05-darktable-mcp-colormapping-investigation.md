# darktable MCP — Color Mapping Investigation

Date: 2026-07-05
Status: investigation; input to acquired-parameter and workflow API design
Companions: `2026-07-05-darktable-mcp-design.md`,
`2026-07-05-darktable-mcp-supported-operations.md`,
`2026-07-05-darktable-mcp-parameter-class-investigation.md`
Source: `src/iop/colormapping.c` at commit `81129368e0` (this fork)

## Purpose

Investigate what would be required to support the `colormapping` IOP through
MCP without exposing its acquired histograms and cluster arrays as generic
writable parameters.

Color mapping is a representative Tier 3 workflow. Its effect is controlled
by a small scalar surface, but those scalars only become useful after
darktable analyzes a reference image and a subject image. The acquired state
is stored in the module params block, while its creation currently depends on
the preview pixelpipe, GUI callbacks, temporary GUI-owned buffers, and a
cross-image transfer mechanism.

This document records current behavior and design constraints. Proposed
operation names and payloads are illustrative, not protocol decisions.

## User-visible purpose

The module transfers both tonal distribution and color palette from one image
to another in Lab space:

- luminance is matched through cumulative histogram mapping;
- chroma is divided into clusters in the Lab `a`/`b` plane; and
- subject clusters are shifted and scaled toward matched reference clusters.

The native source/target terminology is easy to misread. In processing:

- `source_*` supplies the distribution and palette to reproduce; and
- `target_*` describes the image being transformed.

A future semantic API should consider `reference` and `subject` instead of
reusing the native field names.

## Native parameter inventory

`dt_iop_colormapping_params_t` contains scalar controls, persistent acquired
data, and state-machine flags in one params block.

### Direct controls

| field | native range/default | meaning | dependency |
|---|---|---|---|
| `n` | 1–5, default 3 | number of Lab color clusters | changing it invalidates both acquired datasets |
| `dominance` | 0–100, default 100 | cluster matching balance: color proximity versus cluster population | useful only with both datasets |
| `equalization` | 0–100, default 50 | strength of luminance-distribution transfer | useful only with both datasets |

These are ordinary scalar fields by storage type, but their useful
writability is conditional. In particular, changing `n` is not an isolated
numeric edit: `gui_changed()` clears every histogram, cluster statistic, and
availability flag.

### Acquired reference data

| field | shape | meaning |
|---|---|---|
| `source_ihist` | 2048 floats | inverse cumulative luminance histogram, in Lab L units |
| `source_mean` | up to 5 Lab `a`/`b` pairs | reference cluster centers |
| `source_var` | up to 5 `a`/`b` pairs | reference cluster standard deviations despite the historical `var` name |
| `source_weight` | up to 5 floats | reference cluster population weights |

### Acquired subject data

| field | shape | meaning |
|---|---|---|
| `target_hist` | 2048 integers | normalized cumulative luminance histogram |
| `target_mean` | up to 5 Lab `a`/`b` pairs | subject cluster centers |
| `target_var` | up to 5 `a`/`b` pairs | subject cluster standard deviations |
| `target_weight` | up to 5 floats | subject cluster population weights |

The complete acquired payload is only tens of KiB, so size alone is not the
reason to exclude it. The problem is semantic ownership: the arrays are
algorithm-produced state with cross-field invariants, not user-authored
vectors or tables.

### State flags

The params field `flag` combines persistent readiness with transient commands:

| flag | role |
|---|---|
| `HAS_SOURCE` | reference dataset is present |
| `HAS_TARGET` | subject dataset is present |
| `ACQUIRE` | preview input should be captured for analysis |
| `GET_SOURCE` | captured pixels should populate reference fields |
| `GET_TARGET` | captured pixels should populate subject fields |

`ACQUIRE`, `GET_SOURCE`, and `GET_TARGET` are command/state-machine bits, not
editable user parameters. A remote caller must never write them directly.

## Current acquisition lifecycle

The GUI workflow is distributed across several callbacks:

1. Pressing an acquisition button sets `ACQUIRE` and one `GET_*` bit, clears
   the corresponding `HAS_*` bit, focuses the module, and adds history.
2. On the next preview-pipe process, the module copies its input pixels into a
   GUI-owned buffer. The capture happens before color mapping is applied.
3. `DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED` invokes `process_clusters()`.
4. The callback computes the histogram and k-means results into the params
   block, sets the corresponding `HAS_*` bit, and clears transient flags.
5. Redraw is requested after acquisition. History is added when the resulting
   state contains reference data, which is normally true in the complete
   reference-then-subject workflow.

This implementation has consequences for remote support:

- acquisition is asynchronous relative to the initiating call;
- it currently requires `gui_data` and a preview pipe;
- setting flags through `set_module_params` would not constitute a complete
  acquisition operation;
- transient request flags can enter history before results exist;
- the input is whatever preview revision reaches the module when capture
  happens, not necessarily the revision at button press; and
- CPU and OpenCL paths both copy preview input into host memory before the
  analysis callback runs.

The analysis and state transition should be extracted behind a
transport-neutral service before an MCP operation is added. The remote layer
should not synthesize GUI button presses.

## Cross-image reference lifecycle

Color mapping needs a reference derived from one image and a subject derived
from another. The module instance and its params normally belong to one image,
so reference transfer is a separate lifecycle problem.

The current GUI implementation keeps the newly acquired reference in a
`flowback` structure and writes it to `/tmp/dt_colormapping_loaded`. A later
GUI/module initialization reads that file and uses the data as defaults. The
source comment also describes using a preset to carry an acquired reference
between images.

Neither mechanism should define the MCP contract:

- `/tmp` is process-external, platform-specific transient state with unclear
  ownership and cleanup;
- hidden file transfer is not discoverable or revision-aware;
- raw acquired arrays are coupled to the module params version; and
- presets are useful durable resources, but applying one is not equivalent to
  a general in-session acquisition handle.

Candidate lifecycle models to investigate:

1. **Preset bridge.** The user or MCP applies an existing darktable preset
   containing reference data, then acquires the current subject. This is the
   smallest safe initial surface.
2. **Session-scoped opaque reference.** Acquisition returns an opaque handle
   owned by the running darktable process. The handle can be applied to a
   `colormapping` instance on another image in that session.
3. **Named darktable resource.** Acquired reference data is stored through a
   darktable-owned resource or preset API and selected by stable ID.
4. **Opaque import/export payload.** A versioned dataset could travel through
   the sidecar without exposing native arrays. Portability and params-version
   migration would have to be defined explicitly.

Session handles are operationally simple but not durable. Presets/resources
are durable but require naming, collision, discovery, and lifecycle rules.
Opaque import/export introduces compatibility and trust concerns. No choice is
made here.

## Analysis algorithm

### Luminance distribution

For each acquired image, the module builds a 2048-bin histogram over Lab L.
The subject stores a normalized cumulative histogram. The reference stores an
inverse cumulative histogram.

During processing, subject L selects a cumulative-histogram position, which
is then mapped through the reference inverse histogram. `equalization`, scaled
from 0–100 to 0–1, blends the mapped luminance effect. A bilateral smoothing
step is used for the luminance delta when equalization is active.

### Color clusters

The module runs k-means over Lab `a`/`b` values with at most five clusters.
For each cluster it stores center, per-axis standard deviation, and population
weight. Empty or zero-variance clusters are zeroed, and clusters are sorted by
ascending weight for more stable GUI presentation.

At processing time, each subject cluster is matched to a reference cluster.
The cost combines:

- squared distance between cluster centers in `a`/`b`; and
- squared difference in cluster population, with a large scale factor.

`dominance`, scaled from 0–100 to 0–1, interpolates between those costs. Each
pixel is assigned inverse-distance weights against subject clusters, then its
`a`/`b` values are shifted and scaled using the matched reference center and
the ratio of reference to subject standard deviation.

The k-means initialization uses darktable's random-point helper. Repeated
acquisition may therefore need determinism testing before the API promises
stable results for identical pixels and settings.

## Processing prerequisites and conditional controls

The effect runs only when both `HAS_SOURCE` and `HAS_TARGET` are set. With an
incomplete acquisition state, the module copies input to output unchanged.

This suggests capability and state metadata such as:

```json
{
  "reference": "ready",
  "subject": "missing",
  "effect_ready": false,
  "controls": {
    "dominance": { "writable": true, "effective": false },
    "equalization": { "writable": true, "effective": false },
    "n": { "writable": true, "invalidates": ["reference", "subject"] }
  }
}
```

The exact schema is undecided. The important distinction is between writable,
currently effective, and invalidating.

`dominance` and `equalization` can be supported by generic scalar mutation as
long as the model can discover that they have no visible effect until both
datasets exist. `n` should not be exposed as an ordinary isolated scalar
unless the remote service reproduces the module's invalidation behavior
atomically.

## Why this is architectural Tier 3

Under the proposed parameter-class breakpoint, `colormapping` remains Tier 3
because producing its essential state requires:

- pixel analysis rather than only value validation;
- an asynchronous preview-pipeline lifecycle;
- state acquired from two different image contexts;
- transfer or persistent identity for the reference dataset;
- invalidation rules coupling scalar and acquired fields; and
- provenance that should remain visible even though native arrays stay
  opaque.

It is not impossible to support, and it does not need arbitrary array writes.
It needs an acquired-dataset workflow API.

## Candidate semantic operations

The eventual surface may use module-specific operations or generalized
acquired-parameter capabilities. Illustrative operations are:

```text
get_colormapping_state
acquire_colormapping_reference
apply_colormapping_reference
acquire_colormapping_subject
clear_colormapping_reference
clear_colormapping_subject
```

Equivalent generalized names might be:

```text
get_acquired_parameter_state
acquire_parameter
apply_acquired_resource
clear_acquired_parameter
```

Generalization should wait until another acquired workflow is investigated.
Forcing `colormapping` into a generic API prematurely could encode the wrong
roles, lifecycle, or completion semantics.

## Asynchronous operation requirements

An acquisition request needs a defined lifecycle:

1. validate darkroom, image, module instance, role, and cluster count;
2. capture the starting darkroom revision and relevant module/input identity;
3. arrange analysis of the correct module input at a bounded preview size;
4. report pending/running/completed/failed/cancelled state;
5. reject or explicitly accept completion if the image or upstream pipeline
   changed while analysis ran;
6. commit only completed acquired data, without persisting transient command
   flags; and
7. create a clear, bounded history/undo result.

Open question: should acquisition create one history item when the result is
committed, or should applying a reference plus acquiring the subject be one
larger logical transaction? The current GUI can add history both when
requesting and after completing acquisition; the MCP API should not inherit
that behavior without review.

The existing protocol already anticipates asynchronous preview operations but
does not yet define a general job model for edits. Color mapping may justify a
small job/status capability, or the private connection may keep one request
open until analysis completes. Cancellation, timeout, disconnect, and stale
revision behavior must be specified either way.

## Dataset validation and summaries

Native arrays should be adapter-owned, but the adapter still needs validation
before commit:

- `n` remains within 1–5 and matches the active cluster prefix;
- histogram entries are finite/in range and monotonic where required;
- cluster means and standard deviations are finite;
- standard deviations are non-negative;
- weights are finite, non-negative, and have a sane total;
- zeroed/empty clusters are handled consistently;
- readiness flags match populated datasets; and
- transient acquisition flags are absent from committed external input.

The model may benefit from a compact read-only summary rather than raw arrays:

- readiness and provenance for each role;
- image ID/name at acquisition time, if still available;
- acquisition revision and timestamp;
- cluster count;
- cluster center colors, spread, and weight;
- luminance percentiles instead of 2048 histogram bins; and
- warnings about stale or incompatible data.

Cluster colors used for presentation require a declared conversion from Lab
to a display color space. The native GUI uses an sRGB transform for its
swatches; the API should distinguish numeric Lab summaries from optional
presentation colors.

## Preset-first support slice

A practical early implementation can avoid remote acquisition:

1. enumerate applicable `colormapping` presets;
2. apply a preset containing reference data;
3. report whether the reference dataset is ready;
4. leave subject acquisition in the GUI initially; and
5. expose `dominance` and `equalization` once both datasets are ready.

A later slice can add subject acquisition from the current image, followed by
session-scoped reference capture and transfer. This sequence validates state
reporting and conditional controls before introducing cross-image handles.

## Relationship to parameter classes

Color mapping likely needs more than one semantic class:

- primitive scalar for `dominance` and `equalization`;
- invalidating scalar/workflow setting for `n`;
- acquired dataset for reference and subject analysis;
- optional resource reference for presets or durable acquired references; and
- read-only cluster/histogram summary for model inspection.

This mixed composition reinforces why support and complexity should be
reported per semantic parameter/capability rather than assigning one fixed
behavior to the entire module.

## Open questions

- Should subject analysis happen automatically after a reference is applied,
  or require an explicit acquisition request?
- Should reference acquisition include `n`, making cluster count immutable for
  that dataset, or should changing `n` trigger reacquisition?
- Is a session-scoped reference handle sufficient for v1 support, with presets
  as the durable path?
- Can acquisition analyze a stable pixelpipe snapshot, or must it reject any
  intervening upstream edit?
- What preview resolution produces acceptably stable histograms and clusters?
- Must CPU and OpenCL acquisition yield equivalent summaries?
- Is deterministic k-means initialization required for repeatability and
  tests?
- Should applying a reference and acquiring a subject be separate undo items?
- How should a reference record image provenance without leaking filesystem
  paths or unstable database IDs?
- Can presets containing only reference data be identified reliably without
  loading malformed or incomplete acquired state?
- Should the semantic operation remain module-specific until another acquired
  module supplies evidence for a shared class?

## Acceptance criteria for a future prototype

- No remote operation writes histogram, cluster, or flag arrays directly.
- Acquisition analyzes the intended module input and reports clear completion
  state.
- Incomplete acquisition leaves processing as pass-through and is visible in
  state metadata.
- Changing cluster count invalidates both datasets atomically.
- Reference transfer does not depend on `/tmp` or hidden filesystem state.
- Stale revisions, image switches, cancellation, and disconnects cannot commit
  analysis to the wrong module instance.
- Completed acquisition produces a bounded, undoable history result.
- Scalar tuning visibly affects a fully acquired mapping.
- CPU/OpenCL paths and repeated runs have defined tolerance or determinism.
- Preset and session-reference paths reject incompatible params versions.

## Maintenance

Update this investigation if the native acquisition workflow, params layout,
histogram size, cluster algorithm, or cross-image transfer mechanism changes.
Any future shared acquired-parameter design should be checked against at least
one additional module before `colormapping` behavior is generalized.
