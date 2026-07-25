# Intent → module

Every usefully editable module, keyed by what a person actually says.
Organized by editing intent, not alphabetically.

**Usage ratings** (`core` / `common` / `occasional` / `rare`) are
inherited from `2026-07-16-darktable-mcp-supported-operations.md` and
are editorial community judgment, not telemetry. A trailing `*` marks
modules darktable auto-applies, so their presence in the history is not
a user choice.

Everything here is Tier 1 (fully editable) unless marked **T2**
(partial) — see the bottom section for what T2 costs you.

Source: `docs/superpowers/specs/2026-07-16-darktable-mcp-supported-operations.md`
at commit `7537128c55`.

## Tone and exposure

| The user says | Module | Usage | What to move |
|---|---|---|---|
| too dark, too bright, underexposed | `exposure` | core* | `exposure` (EV), `black` |
| lift the shadows, open up the darks, tame the highlights | `toneequal` | core | the nine named EV-band scalars — the natural target for tonal-range language |
| more contrast, punchier, flat | `colorbalancergb` | core | `contrast`; global saturation/brilliance live here too |
| tone mapping, HDR-ish, filmic look | `filmicrgb` | core | full scene-referred curve as scalars and enums |
| simpler tone mapping | `sigmoid` | core* | `contrast`, `skew`, per-primary attenuation |
| modern alternative tone mapper | `agx` | common | look + curve + primaries, all scalars |
| local contrast, micro-contrast, clarity | `bilat` | common | mode enum + detail scalars |
| hazy, foggy, milky | `hazeremoval` | occasional | `strength`, `distance` |
| shadows/highlights recovery (legacy style) | `shadhi` | occasional | `shadows`, `highlights`, `radius` |
| blown highlights on a raw file | `highlights` | occasional* | method enum + scalars; raw only |
| precise tonal curve | `rgbcurve` / `tonecurve` | common | semantic `curves` — see `semantic-params.md` |
| multi-scale contrast, frequency-band contrast | `atrous` | common | semantic `bands` |
| camera-look starting curve | `basecurve` | occasional | semantic `curve.master` |
| levels, black/white point | `rgblevels` | occasional | semantic vectors `levels.*` |
| unbreak an odd input profile, log footage | `profile_gamma` | rare | log/gamma scalars + mode enum |

## White balance and color

| The user says | Module | Usage | What to move |
|---|---|---|---|
| too warm, too cool, wrong white balance, orange indoor light | `temperature` | core* | quantity `wb.temperature` = `{temperature, tint}` |
| color cast, chromatic adaptation, illuminant | `channelmixerrgb` | core* | illuminant/adaptation enums, `temperature`; vectors for mixing rows |
| black and white, monochrome conversion | `channelmixerrgb` | core* | vector `grey` |
| more vivid, more saturated, more pop | `colorbalancergb` | core | `saturation`, `brilliance` (4-way Y/C/H) |
| the greens look yellow, fix one specific hue, skin tones | `colorequal` | common | per-hue-band saturation / hue / brightness scalars |
| shift a range of colors by lightness or chroma | `colorzones` | common | semantic curves `curve.lightness` / `.chroma` / `.hue` |
| warm highlights and cool shadows, split tone | `splittoning` | occasional | shadow/highlight hue + saturation, `balance`, `compress` |
| slide film look, punchy color | `velvia` | occasional | `strength`, `bias` |
| color harmony, complementary grading | `colorharmonizer` | occasional | `rule` enum, anchor hue; vectors `custom_hue`, `node_saturation` |
| adjust the primaries themselves | `primaries` | occasional | per-primary hue/purity scalars |
| lift/gamma/gain grading (legacy) | `colorbalance` | occasional | superseded by `colorbalancergb` — prefer that |
| tint the whole image one color | `colorize` | rare | hue, saturation, lightness |
| scan a film negative | `negadoctor` | rare | film stock enum, `D_max`, vectors `dmin`/`wb_high`/`wb_low` |
| recover color in clipped areas | `colorreconstruct` | rare | threshold / spatial / range |

## Detail — sharpening and noise

| The user says | Module | Usage | What to move |
|---|---|---|---|
| sharpen, soft, not crisp | `sharpen` | common | `radius`, `amount`, `threshold` |
| better sharpening, deconvolution, texture | `diffuse` | common | fully parametric; pairs well with presets |
| noisy, grainy (unwanted), high ISO | `denoiseprofile` | common | `strength`, `radius`, mode enums; semantic `bands` for per-scale control |
| raw-level noise | `rawdenoise` | occasional | `threshold`; semantic `bands` |
| astro / extreme denoise | `nlmeans` | rare | patch size, strength, luma, chroma |
| purple fringing | `defringe` | rare | `radius`, `threshold`, mode enum |
| color fringing at edges | `cacorrectrgb` | occasional | guide channel enum, `radius`, `strength` |
| raw chromatic aberration | `cacorrect` | occasional | bool + iterations enum; raw only |
| hot pixels, stuck pixels | `hotpixels` | occasional | `strength`, `threshold`; raw only |
| smooth skin, soften | `soften` | rare | orton-effect scalars |
| blur a region | `blurs` | occasional | lens / motion / gaussian, fully parametric |
| heavy smoothing | `bilateral` | rare | radius + per-channel sigmas |
| demosaic quality | `demosaic` | occasional* | method enums + capture-sharpen scalars; raw only |

## Geometry and framing

| The user says | Module | Usage | What to move |
|---|---|---|---|
| crop, tighter, cut the bottom | `crop` | core | normalized `cx`/`cy`/`cw`/`ch`; the ratio ints are GUI aspect presets — treat with caution |
| straighten, level the horizon, converging verticals, keystone | `ashift` | common | `rotation`, lens-shift, shear scalars |
| rotate 90°, upside down | `flip` | rare* | single orientation enum |
| lens distortion, vignetting, lens CA | `lens` **T2** | common | `method`, `modify_flags`, `target_geom`, `scale` |
| more canvas, extend the frame | `enlargecanvas` | rare | per-side percentages + color enum |
| add a border or frame | `borders` | occasional | `aspect`, `size`, `pos_h`/`pos_v`; vectors `color`, `frame_color` |
| pixel aspect (anamorphic) | `scalepixels` | rare | single scalar |

## Effects

| The user says | Module | Usage | What to move |
|---|---|---|---|
| vignette, darken the corners | `vignette` | common | `scale`, `falloff`, `brightness`, `saturation`, `shape`, `center.x`/`center.y` |
| darken the sky, ND grad | `graduatednd` | occasional | `density`, `hardness`, `rotation`, hue/saturation — or `exposure` with a drawn gradient mask for more control |
| film grain | `grain` | occasional | channel enum, `coarseness`, `strength` |
| dreamy glow, bloom | `bloom` | rare | `size`, `threshold`, `strength` |
| high-pass sharpening look | `highpass` | rare | `sharpness`, `contrast` |
| soft dreamy lowpass | `lowpass` | rare | radius, contrast, brightness, saturation |
| watermark | `watermark` **T2** | occasional | `opacity`, `scale`, `rotate`, offsets; vector `color` |
| composite another image | `overlay` **T2** | rare | `opacity`, `scale`, `rotate`, offsets |
| blur out a face or plate | `censorize` | rare | blur / pixelate radii |
| posterize, dither | `dither` | rare | method enum, `random.damping` |
| boost color contrast in Lab | `colorcontrast` | rare | a/b steepness |
| night-vision look | `lowlight` | rare | `blueness`; semantic `bands.transition` |

## Tier 2 — partial support

The core controls work; some user-facing state is unreachable.

| Module | You can set | You cannot |
|---|---|---|
| `lens` | method / flags / geometry enums, `scale`, TCA overrides, fine-tune | `camera`/`lens` name strings, EXIF-derived crop/focal/aperture/distance |
| `watermark` | everything but text | `filename`, `text`, `font` strings |
| `overlay` | placement and opacity | the overlay image reference — the user must pick it in the GUI first |
| `monochrome` | `size`, `highlights` | `a`/`b` — unranged Lab coordinates set by GUI drag or picker |
| `colorcorrection` | `saturation` | `hia`/`hib`/`loa`/`lob` — unranged, stored semantics unclear |
| `colorin` / `colorout` | profile `type`/`intent` enums | ICC `filename` strings |
| `rawprepare` | crop scalars, `raw_white_point`, flat-field enum | `raw_black_level_separate[4]` |

## Do not edit these

**Tier 3 — parameter editing excluded.** `set_module_params` is useless
or dangerous because the module's essence is data the protocol cannot
carry. Enable/disable, reset, history, and undo still work.

`retouch` (common — drawn-form data), `liquify` (warp-node blob),
`lut3d` (LUT path + blob), `colorchecker` (Lab patch tables),
`colormapping` (acquired histograms), `rasterfile` (file paths).

If a user asks for one of these, say it needs the GUI.

**Pipeline-level danger.** `rawprepare`, `colorin`, `colorout` — wrong
values break the image rather than degrade it. Touch only on explicit
request.

**Deprecated — never add to a fresh edit.** They appear only in images
edited with old darktable versions. Use the replacement:

`basicadj`→exposure+filmic, `channelmixer`→`channelmixerrgb`,
`clahe`→`bilat`, `clipping`→`crop`+`ashift`, `colisa`→`colorbalancergb`,
`colortransfer`→`colormapping`, `equalizer`→`atrous`,
`filmic`→`filmicrgb`, `globaltonemap`→`filmicrgb`/`sigmoid`,
`invert`→`negadoctor`, `levels`→`rgblevels`, `relight`→`toneequal`,
`spots`→`retouch`, `tonemap`→`filmicrgb`/`sigmoid`,
`vibrance`→`colorbalancergb`, `zonesystem`→`toneequal`.
