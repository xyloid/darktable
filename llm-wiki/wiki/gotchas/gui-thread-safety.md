---
type: gotcha
tags: [gui, threads, process, bug-source]
created: 2026-07-04
updated: 2026-07-04
sources: [raw/dev-doc/GUI.md, raw/dev-doc/IOP_Module_API.md]
---

# GUI Thread Safety — no GTK in process()

`process()` runs on worker threads; GTK is not thread-safe. Calling GTK from
`process()` crashes or corrupts silently.

## Guards before scheduling any GUI update from process()

```c
if(g != NULL                        // GUI exists (not export)
   && self->dev->gui_attached       // darkroom active
   && dt_pipe_is_full(piece->pipe)) // not preview/thumbnail
```

Skipping the pipe-type check floods the GUI with updates from every pipe
([[pixelpipe]] runs several concurrently).

## Pattern A — critical section + g_idle_add

Write computed values into `gui_data` under
`dt_iop_gui_enter/leave_critical_section(self)`, then `g_idle_add()` a
callback that re-takes the lock, reads, and touches GTK on the main thread.
Return `G_SOURCE_REMOVE` from the callback.

## Pattern B — message passing (preferred)

Allocate a message struct, fill it, `g_idle_add(_update_gui, msg)`. The
callback owns and `g_free()`s the message; no locks. The callback must
re-check `g != NULL` — the GUI may have been destroyed in the meantime.

## Thread-safe helpers

`dt_control_queue_redraw_widget(w)` and `dt_control_queue_redraw_center()`
use `g_idle_add` internally — callable from any thread.

## The four classic mistakes

1. direct GTK call in `process()`
2. writing `gui_data` without the mutex (race)
3. forgetting `g_free(msg)` in the callback (leak)
4. missing pipe-type guard (update flood from preview/thumbnail pipes)

Also remember: `process()` reads `piece->data`, never `self->params` — that
is the other half of the threading contract ([[params-vs-data]]).
