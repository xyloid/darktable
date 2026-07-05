---
type: source
tags: [dev-doc, gui, widgets, shortcuts]
created: 2026-07-04
updated: 2026-07-04
sources: [raw/dev-doc/GUI.md, raw/dev-doc/imageop_gui.md, raw/dev-doc/sliders.md, raw/dev-doc/Notebook_UI.md, raw/dev-doc/GUI_Recipes.md, raw/dev-doc/Shortcuts.md, raw/dev-doc/Module_Groups.md, raw/dev-doc/Quick_Access_Panel.md]
---

# dev-doc: GUI Development

The GUI cluster of the dev-doc set: GUI architecture (events, threads,
reparenting), widget helper reference, the bauhaus slider deep dive, tabbed
UIs, copy-paste recipes, the shortcut/action system, module groups, and the
Quick Access Panel.

## Main claims

- `self->widget` doubles as the *current packing target* during `gui_init()`
  and the module's top-level UI afterwards; tabbed and collapsible layouts
  are built by temporarily redirecting it. Distilled into [[iop-gui]] and
  [[widget-packing-and-reparenting]].
- Three event paths (framework widget / custom widget / external change)
  converge on `gui_changed()` as the single home for conditional UI state;
  `DT_GUARD_GUI_UPDATE` is a counter guarding all of it. Distilled into
  [[gui-event-flow]].
- Bauhaus sliders separate internal from displayed value
  (`displayed = internal × factor + offset`), with hard/soft limits,
  gradient stops, curves, and pickers. Distilled into [[bauhaus-widgets]].
- GTK is off-limits from `process()`; two sanctioned patterns
  (mutex + g_idle_add, message passing) plus mandatory guards. Distilled
  into [[gui-thread-safety]].
- The action system underlies all shortcuts; bauhaus `_from_params` widgets
  register automatically. Module groups via `default_group()`; the QAP
  steals/restores widgets by reparenting. Distilled into [[iop-gui]] and
  [[bauhaus-widgets]].

## Surprises / gotchas surfaced

- the `set_format("%")` heuristic can drive `digits` negative → slider snaps
  to endpoints → [[slider-config-order]] (order: factor → format → digits)
- non-static `dt_action_def_t` = dangling pointer in the action system →
  [[widget-packing-and-reparenting]]
- always `dt_ui_label_new()` (ellipsization), never raw `gtk_label_new()`
- toggles need manual sync in `gui_update()`; sliders/comboboxes don't

## Feeds into

[[iop-gui]], [[bauhaus-widgets]], [[gui-event-flow]], [[gui-thread-safety]],
[[slider-config-order]], [[widget-packing-and-reparenting]]
