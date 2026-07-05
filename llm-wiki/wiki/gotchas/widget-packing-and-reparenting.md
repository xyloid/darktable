---
type: gotcha
tags: [gui, gtk, reparenting, dangling-pointer, bug-source]
created: 2026-07-04
updated: 2026-07-04
sources: [raw/dev-doc/GUI.md, raw/dev-doc/Notebook_UI.md, raw/dev-doc/GUI_Recipes.md]
---

# Widget Packing & Reparenting Traps

Two related traps in [[iop-gui]] construction.

## Trap 1: setting self->widget too early

`_from_params()` helpers pack into whatever `self->widget` currently points
at. If you set `self->widget = main_vbox` at the top of `gui_init()` and then
create notebook pages, **every widget silently lands in main_vbox instead of
the pages**. Correct pattern:

```c
GtkWidget *main_vbox = dt_gui_vbox();          // don't assign self->widget yet
g->notebook = dt_ui_notebook_new(&notebook_def);
dt_gui_box_add(main_vbox, GTK_WIDGET(g->notebook));

GtkWidget *page1 = dt_ui_notebook_page(g->notebook, N_("basic"), NULL);
self->widget = page1;                          // redirect
/* ..._from_params calls → page1 ... */

GtkWidget *page2 = dt_ui_notebook_page(g->notebook, N_("advanced"), NULL);
self->widget = page2;                          // redirect
/* ... */

self->widget = main_vbox;                      // restore LAST
```

Same redirect/restore dance for collapsible sections (`cs.container`).
`self->widget` must end up pointing at the container holding the module's
*entire* UI.

## Trap 2: non-static dt_action_def_t (dangling pointer)

`dt_ui_notebook_new(&notebook_def)` and `dt_action_define_iop(...)` keep the
**address** of the `dt_action_def_t`; `dt_ui_notebook_page()` populates it.
The action system dereferences that address long after `gui_init()` returns
(shortcut press, prefs dialog). If `notebook_def` is a plain local, its stack
frame is gone → dangling pointer → crash or silent corruption. It must be
`static`:

```c
static dt_action_def_t notebook_def = { };
```

## QAP reparenting is framework business

The Quick Access Panel steals and restores widgets itself (ref → remove →
placeholder → repack → visibility sync; see [[iop-gui]]). Your job is only to
make widgets reparent-safe: standard [[bauhaus-widgets]], GtkBox/GtkGrid
parents, meaningful in isolation. Complex custom widgets with parent
dependencies won't reparent cleanly.
