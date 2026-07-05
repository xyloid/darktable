---
type: pipeline
tags: [gui, events, callbacks, history]
created: 2026-07-04
updated: 2026-07-04
sources: [raw/dev-doc/GUI.md, raw/dev-doc/README.md]
---

# GUI Event Flow

The three paths a change takes through an [[iop-modules|IOP module]]'s GUI,
and the rules for each. Widget creation itself is [[bauhaus-widgets]].

## Path A — `_from_params` widget changed by user

```
user drags slider
  → framework writes self->params
  → framework calls gui_changed(self, widget, previous)
  → framework calls dt_dev_add_history_item() internally
  → commit_params() → process()
```

No manual code needed — the framework does history, undo/redo, shortcuts.

## Path B — custom widget changed by user

```
callback fires
  → DT_GUARD_GUI_UPDATE()            (always, first line)
  → modify self->params directly
  → dt_dev_add_history_item(darktable.develop, self, TRUE)
  → commit_params() → process()
```

## Path C — external change (image switch, undo, preset, paste)

```
framework loads new params into self->params
  → DT_ENTER_GUI_UPDATE()            (suppresses widget callbacks)
  → your gui_update(): sync toggles/custom widgets
                       (sliders/comboboxes auto-sync),
                       end with gui_changed(self, NULL, NULL)
  → DT_LEAVE_GUI_UPDATE()
```

## The guard counter

`DT_GUARD_GUI_UPDATE` is a counter, not a boolean. Two patterns:

1. first line of every manual callback: `DT_GUARD_GUI_UPDATE()` — bail out if
   a programmatic update is in flight
2. wrap programmatic widget writes:
   `DT_ENTER_GUI_UPDATE() … dt_bauhaus_slider_set(...) … DT_LEAVE_GUI_UPDATE()`

## When to call dt_dev_add_history_item()

`dt_dev_add_history_item(dev, module, enable)` records `self->params` to the
history stack and triggers a reprocess. `enable=TRUE` also enables the module
(user action should switch it on); `FALSE` for continuous drags on an
already-enabled module (then `TRUE` on release).

| Situation | Call? |
|---|---|
| manual callback (custom widget) | yes |
| `color_picker_apply()` | yes |
| mouse drag on canvas/graph | yes (FALSE during, TRUE on release) |
| `gui_changed()` | no — framework handles `_from_params` |
| `gui_update()` | no — syncing, not changing |

## Color picker lifecycle

`dt_color_picker_new(self, DT_COLOR_PICKER_AREA, slider)` wraps a slider.
Click → `request_color_pick` set → pipeline processes → sample lands in
`self->picked_color[0..3]` (+ `_min`/`_max`) → framework calls
`color_picker_apply(self, picker, pipe)` → set params, add history item.

## gui_changed() contract

The single home for conditional UI state (visibility, sensitivity, dynamic
labels). Called by the framework after Path A (with the changed widget) and
by you at the end of `gui_update()` (with NULL) — so every path that changes
params converges on the same UI adjustment code.
