# dt_masks_form_remove legacy caller audit

Date: 2026-07-24
Marker: `MASKS_REMOVE_CALLER_AUDIT`
Replacement for remote full shape deletion:
`dt_masks_form_remove_shape_full`.

The legacy API remains behaviorally unchanged. “Review later” means the
caller was deliberately not migrated by the Task 5 remediation.

| caller | arguments | intent | nested exposure | history / ownership concern | recommendation | status |
|---|---|---|---|---|---|---|
| `src/libs/masks.c:759` mask-manager removal | `module, parent group, form` | detach one visible tree member | yes | legacy empty-group recursion and forced enable require GUI regression coverage | evaluate against the new graph API without changing tree-selection behavior | Review later |
| `src/control/remote_edit.c:1814` module reset | `module, NULL, base group` | remove a module group before defaults reload | group may contain nested forms | unlinked group ownership and enabled-state history need reset tests | migrate only with remote-reset undo coverage | Review later |
| `src/develop/masks/circle.c:430` right-click removal | `module, parent group, form` | detach circle | yes | GUI gesture and history coalescing are intentional behavior | retain until GUI interaction tests exist | Review later |
| `src/develop/masks/ellipse.c:798` right-click removal | `module, parent group, form` | detach ellipse | yes | GUI gesture and history coalescing are intentional behavior | retain until GUI interaction tests exist | Review later |
| `src/develop/masks/gradient.c:371` right-click removal | `module, parent group, form` | detach gradient | yes | GUI gesture and history coalescing are intentional behavior | retain until GUI interaction tests exist | Review later |
| `src/develop/masks/brush.c:1726` permanent removal | `module, NULL, form` | permanently delete brush | yes | shallow incoming-reference cleanup and unowned unlink need review | candidate for `dt_masks_form_remove_shape_full` after clone/brush semantics are pinned | Review later |
| `src/develop/masks/brush.c:1789` parent removal | `module, parent group, form` | detach brush | yes | empty-group recursion and GUI history must remain stable | retain pending GUI tests | Review later |
| `src/develop/masks/masks.c:1583` module mask reset | `module, NULL, base group` | drop all forms from one module | yes | clone groups cascade into hidden children | keep legacy until group-deletion semantics receive a separate design | Review later |
| `src/develop/masks/masks.c:2016` empty-parent recursion | `module, NULL, group` | retire an emptied parent | yes | ownership leak is known; changing it alters every detach path | migrate as part of a dedicated legacy-removal project | Review later |
| `src/develop/masks/masks.c:2030` clone-child recursion | `module, clone group, child` | remove inaccessible clone children | yes | clone ownership differs from ordinary drawn forms | retain legacy clone semantics | Review later |
| `src/develop/masks/masks.c:2075` empty module-group recursion | `module, NULL, base group` | retire an empty module group | yes | history enable and ownership need broad regression coverage | migrate with the parent-recursion caller | Review later |
| `src/develop/imageop.c:2326` module reset | `module, NULL, base group` | remove masks during parameter reset | yes | reset ordering and history differ from remote shape deletion | migrate only with imageop reset tests | Review later |
| `src/develop/masks/path.c:2289` permanent removal | `module, NULL, form` | permanently delete path | yes | shallow incoming-reference cleanup and unowned unlink need review | candidate for the new full-delete API after path/clone flags are covered | Review later |
| `src/develop/masks/path.c:2361` parent removal | `module, parent group, form` | detach path | yes | GUI list removal occurs immediately before the core call | retain until list/selection ordering is tested | Review later |

## Re-audit command

Run:

`rg -n 'dt_masks_form_remove\(' src --glob '!src/tests/**'`

Every production call must appear in the table before a rebase or
migration is approved.
