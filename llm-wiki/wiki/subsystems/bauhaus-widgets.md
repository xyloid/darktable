---
type: subsystem
tags: [gui, widgets, bauhaus, sliders]
created: 2026-07-04
updated: 2026-07-04
sources: [raw/dev-doc/imageop_gui.md, raw/dev-doc/sliders.md, raw/dev-doc/Shortcuts.md]
---

# Bauhaus Widgets

darktable's custom widget family (`src/bauhaus/bauhaus.c`) plus the IOP
helper layer (`src/develop/imageop_gui.c`). The `_from_params()` helpers use
[[introspection]] to auto-configure, auto-pack, auto-callback, and
auto-register widgets with the action/shortcut system.

## The `_from_params` family

- `dt_bauhaus_slider_from_params(self, "param")` — range/default/label from
  `$MIN/$MAX/$DEFAULT/$DESCRIPTION`; callback syncs slider → `self->params`;
  packs into `self->widget`; registers a shortcut action. Array fields use
  bracket notation: `"Dmin[0]"`.
- `dt_bauhaus_combobox_from_params(self, "param")` — populated from the enum's
  `$DESCRIPTION` members; stores the **enum value**, not the index.
- `dt_bauhaus_toggle_from_params(self, "param")` — checkbox for a gboolean.
  **Unlike sliders/comboboxes, toggles need manual sync in `gui_update()`**
  via `gtk_toggle_button_set_active()`.

Manual creation when no param maps:
`dt_bauhaus_slider_new_with_range(self, min, max, step, defval, digits)`.

## Slider value model

Internal value (what's stored in params; get/set and min/max operate on it)
vs displayed value: `displayed = internal * factor + offset`, then formatted
with `digits` and a literal `format` suffix (`" EV"`, `" %"`, `"°"`).

Two-tier limits: **hard** (absolute bounds) and **soft** (default visible
range; user can exceed up to hard limits with Ctrl+Shift drag).

⚠️ `set_format("%")` has a heuristic side effect that can corrupt `digits` —
always configure in the order factor → format → digits. Full failure analysis
in [[slider-config-order]].

## Slider extras

- gradient backgrounds: `dt_bauhaus_slider_set_stop(w, pos, r, g, b)` (up to
  20 stops); stops paint across the *hard* range, so rescale positions when
  soft ≠ hard. `dt_bauhaus_slider_set_feedback(w, 0)` hides the position bar
  (useful for hue rainbows).
- non-linear response: `dt_bauhaus_slider_set_log_curve(w)` or a custom
  `dt_bauhaus_slider_set_curve(w, fn)`.
- step size: `dt_bauhaus_slider_set_step(w, s)` (default soft-range/100).

## Buttons and pickers

- `dt_iop_togglebutton_new(...)` / `dt_iop_button_new(...)` — icon buttons
  (icons from `dtgtk/paint.h`) wired into the action system.
- `dt_color_picker_new(self, flags, slider)` — wraps a slider with a picker
  button; **store and pack the returned combined widget**, not the original
  slider. Results arrive in `color_picker_apply()` via
  `self->picked_color[]` — see [[gui-event-flow]].
- `DT_IOP_SECTION_FOR_PARAMS(self, N_("chroma"))` — hierarchical *shortcut*
  sections (`module/chroma/...`); visual headers are separate
  (`dt_ui_section_label_new`).

## Shortcut/action system

Everything shortcut-able is a `dt_action_def_t` in a hierarchical action tree
(e.g. `iop/exposure/exposure`). Bauhaus `_from_params` widgets register
automatically; custom widgets use `dt_action_register()` or
`dt_action_button_new()`. Users remap in Preferences → Shortcuts.
`dt_action_def_t` structs passed by address must be `static` — see
[[widget-packing-and-reparenting]] for the dangling-pointer trap.

## See also

- [[iop-gui]] — layout, containers, notebooks, reparenting
- [[gui-event-flow]] — how widget changes reach `process()`
