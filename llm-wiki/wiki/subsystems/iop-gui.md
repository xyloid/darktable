---
type: subsystem
tags: [gui, gtk, layout, notebooks, qap]
created: 2026-07-04
updated: 2026-07-04
sources: [raw/dev-doc/GUI.md, raw/dev-doc/Notebook_UI.md, raw/dev-doc/GUI_Recipes.md, raw/dev-doc/Module_Groups.md, raw/dev-doc/Quick_Access_Panel.md]
---

# IOP GUI Architecture

How module UIs are constructed and organized. Widgets themselves are covered
in [[bauhaus-widgets]]; the event/callback flow in [[gui-event-flow]];
threading rules in [[gui-thread-safety]].

## gui_init() and self->widget

`gui_init()` runs once per module instance entering the darkroom. It creates
and configures widgets — it must **not** set their values (that's
`gui_update()`'s job). `IOP_GUI_ALLOC(modulename)` callocs `gui_data_t`.

`self->widget` has a dual role:

1. during `gui_init()` it is the *current packing target* — `_from_params()`
   helpers implicitly pack into it;
2. after `gui_init()` it tells the framework which widget is the module's
   whole UI.

Building tabs or collapsibles means temporarily redirecting `self->widget`
to sub-containers and restoring it at the end. Getting this wrong silently
puts widgets in the wrong container — see [[widget-packing-and-reparenting]].

## Layout API (GTK4-compat wrappers — always use these)

| Function | Replaces |
|---|---|
| `dt_gui_vbox()` / `dt_gui_hbox()` | `gtk_box_new()` |
| `dt_gui_box_add(box, child)` | `gtk_box_pack_start()` |
| `dt_ui_label_new(text)` | `gtk_label_new()` (adds ellipsization — raw labels can stretch the panel) |
| `dt_ui_section_label_new(text)` | — visual section header |

Control layout via widget properties before adding: `gtk_widget_set_hexpand`,
`halign`/`valign` (`GTK_ALIGN_FILL` for inputs, `CENTER` for checkboxes).

## Notebooks (tabbed UIs)

`dt_ui_notebook_new(&notebook_def)` + `dt_ui_notebook_page(nb, N_("tab"),
tooltip)` which returns the page's content container. Pattern per page:
set `self->widget = page`, create widgets, next page; finally restore
`self->widget = main_vbox` and register the notebook action:
`dt_action_define_iop(self, NULL, N_("page"), GTK_WIDGET(nb), &notebook_def)`.

The `dt_action_def_t` **must be `static`** — the action system keeps its
address after `gui_init()` returns ([[widget-packing-and-reparenting]]).

Advanced: `colorequal.c` pairs a GtkNotebook with a `GtkStack` (switch-page
signal drives `gtk_stack_set_visible_child_name`) when similar slider banks
operate on different params per tab. Persist the active tab with
`dt_conf_set_int`/`dt_conf_get_int` if desired.

## Collapsible sections

`dt_gui_new_collapsible_section(&cs, "plugins/darkroom/mod/expand_x",
_("advanced"), GTK_BOX(parent), DT_ACTION(self))`, then redirect
`self->widget = GTK_WIDGET(cs.container)`, pack, restore. Expanded state
persists via the conf key.

## Cursor management

Three override levels: regular (`dt_control_change_cursor("crosshair")`,
CSS cursor names), temporary (`dt_control_set_temp_cursor()` /
`_clear_temp_cursor()`), and global (`dt_control_forbid_change_cursor()` /
`_allow_...`; `dt_gui_cursor_set_busy()` for modal busy state). Widgets don't
set GDK window cursors directly — they catch enter/leave events and call
these, so global overrides (busy, help) always win.

## Module groups

Tabs in the right darkroom panel (`src/libs/modulegroups.c`). A module
declares its default tab via `default_group()` returning
`IOP_GROUP_TECHNICAL` / `IOP_GROUP_GRADING` / `IOP_GROUP_EFFECTS` (OR-able;
`IOP_GROUP_NO_GROUP` hides). Users can override with module-group presets.

## Quick Access Panel (QAP)

A special "Basics" group (`DT_MODULEGROUP_BASICS` in `modulegroups.c`) that
aggregates widgets from many modules. Modules don't push widgets — the QAP
*pulls* them by reparenting: ref, remove from original parent, leave a
placeholder, pack into QAP, sync visibility signals; reversed on hide.
QAP-compatible widgets must be standard bauhaus widgets with introspection
names, live in a GtkBox/GtkGrid, and make sense in isolation (label +
tooltip). Multi-instance modules are limited/disabled in the QAP.

## Canvas interaction

Modules with center-view interaction (crop, masks) implement
`mouse_moved()`, `button_pressed()`, `button_released()`, `scrolled()`
(return 1 if handled) and draw overlays in `gui_post_expose()` with Cairo.
