# Darktable Drawn-Mask Task 5 Remediation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Task 5 mask creation, update, deletion, reporting, history, ownership, and GUI-session cancellation correct for nested mask groups without changing legacy removal callers.

**Architecture:** Add cycle-safe graph and ownership-aware full-delete primitives to the masks core while retaining `dt_masks_form_remove` unchanged. Add a forced-new masks-history primitive backed by the undo core's hard isolated scope, plus an extended creation helper, so remote mutations have coherent, distinct undo snapshots without enabling disabled modules; then migrate `remote_masks.c` and document every legacy removal caller for later review.

**Tech Stack:** darktable C, GLib collections, JSON-GLib, CMocka, CMake, CTest, Git.

## Global Constraints

- Execute in the existing isolated worktree `<REPO>/.claude/worktrees/mask-support`, starting from design commit `2b8a915a0d`.
- Treat `docs/superpowers/specs/2026-07-24-darktable-drawn-mask-task5-remediation-design.md` as authoritative.
- Do not change the wire protocol, error slugs, supported shape types, or group-ID rejection behavior.
- Keep `dt_masks_form_remove(dt_iop_module_t *, dt_masks_form_t *, dt_masks_form_t *)` behavior unchanged.
- All new graph traversal must terminate for missing children, duplicate paths, and malformed cycles.
- A retired form leaves `dev->forms` and enters `dev->allforms` exactly once; never free it immediately.
- Remote create, update, and delete must preserve `dt_iop_module_t.enabled`.
- Create-with-attachment must produce two distinct masks-history and
  production undo entries: unattached form first, coherent attachment
  second. Each forced-new entry bypasses target suppression and has an
  explicit no-coalesce undo boundary.
- Public undo/redo seals the current physical segment of a long-lived
  ordinary group without ending its logical depth; later accepted records
  resume lazily. One-shot record suppression is consumed only by the thread
  that armed it and is cancelled by clear.
- Full delete must produce one global masks-history entry plus one entry
  per unique module whose base group is retired by its cascade.
- The only bit cleared when a module loses its final drawn-mask group is `DEVELOP_MASK_MASK`.
- Test each behavior through a failing test before changing production code.
- End every implementation task with the focused `test_remote_masks` target green and a separate commit.

## Binding Pre-flight Corrections

The subagent-driven pre-flight review found three conflicts in the
literal snippets below. These corrections are part of the approved plan
and govern the affected tasks:

- Task 1 must retain the existing `module == NULL` mask-manager
  resolution before calling `_dev_add_history_item_ext`. The new
  forced-snapshot path uses that same resolution; it must never pass a
  null module to `_dev_add_history_item_ext`.
- Task 4 migrates the remote creation call to the extended helper in the
  same commit that restores compatibility-wrapper naming. This keeps the
  focused suite green at the Task 4 boundary. Task 5 verifies and retains
  that migration while adding its graph and H2 changes.
- Task 4 clamps requested-name suffix truncation to
  `strlen(requested_name)` before inspecting a UTF-8 boundary. It must
  not index a short caller-owned string at the destination-buffer limit.

## File Map

- `src/develop/develop.h`: declare a forced-new masks-history entry point.
- `src/develop/develop.c`: share the masks-history wrapper and pass `new_item = TRUE` when requested.
- `src/develop/masks.h`: declare graph queries, full shape deletion, creation options, and the extended creation helper; add the caller-audit marker.
- `src/develop/masks/masks.c`: implement cycle-safe traversal, owner discovery, ownership-aware deletion, explicit naming, and coherent creation history.
- `src/control/remote_masks.h`: document transitive GUI-target semantics.
- `src/control/remote_masks.c`: use core traversal/deletion/creation APIs for list, create, update, delete, and H2.
- `src/tests/unittests/control/test_remote_masks.c`: add graph, lifecycle, history, undo, naming, reporting, and GUI-session regressions.
- `docs/superpowers/specs/2026-07-24-darktable-mask-removal-caller-audit.md`: record all legacy removal callers and migration recommendations.
- `docs/superpowers/specs/2026-07-19-darktable-mcp-drawn-masks-design.md`: correct shallow-removal and history assertions.
- `docs/superpowers/plans/2026-07-19-darktable-mcp-drawn-masks-tier3.md`: supersede the incorrect Task 5 snippets.
- `docs/superpowers/specs/2026-07-19-darktable-upstream-divergence-manifest.md`: record new core APIs and their rebase guards.

---

### Task 1: Add Forced-New Masks-History Snapshots

**Files:**

- Modify: `src/develop/develop.h:407-416`
- Modify: `src/develop/develop.c:1510-1550`
- Test: `src/tests/unittests/control/test_remote_masks.c:194-280`
- Test: `src/tests/unittests/control/test_remote_masks.c:1538-1625`

**Interfaces:**

- Consumes:
  `_dev_add_history_item_ext(dt_develop_t *, dt_iop_module_t *, gboolean enable, gboolean new_item, gboolean no_image, gboolean include_masks, gboolean auto_name_module)` with `include_masks = TRUE`.
- Produces:

```c
void dt_dev_add_new_masks_history_item(dt_develop_t *dev,
                                       dt_iop_module_t *module,
                                       gboolean enable);
```

- [ ] **Step 1: Add a history-capable fixture helper**

Refactor module construction and add these helpers after
`blend_fixture_t`:

```c
static dt_iop_module_t *_blend_fixture_add_module(blend_fixture_t *fixture,
                                                  const char *op,
                                                  const int priority)
{
  dt_iop_module_t *module = g_malloc0(sizeof(dt_iop_module_t));
  dt_iop_module_so_t *so = dt_iop_get_module_so(op);
  assert_non_null(so);
  assert_false(dt_iop_load_module(module, so, &fixture->dev));
  memcpy(module->params, module->default_params, module->params_size);
  module->multi_priority = priority;
  fixture->dev.iop = g_list_append(fixture->dev.iop, module);
  return module;
}

static void _blend_fixture_enable_history(blend_fixture_t *fixture)
{
  gboolean has_mask_manager = FALSE;
  for(GList *modules = fixture->dev.iop;
      modules;
      modules = g_list_next(modules))
  {
    const dt_iop_module_t *module = modules->data;
    if(dt_iop_module_is(module, "mask_manager"))
    {
      has_mask_manager = TRUE;
      break;
    }
  }
  if(!has_mask_manager)
    _blend_fixture_add_module(fixture, "mask_manager", 0);
  fixture->dev.gui_attached = TRUE;
}
```

Replace the manual exposure allocation in `blend_fixture_new()` with:

```c
fixture->module = _blend_fixture_add_module(fixture, op, 0);
```

- [ ] **Step 2: Write the failing distinct-snapshot test**

Add these tests and register them with `blend_test_setup` /
`blend_test_teardown`:

```c
static void test_new_mask_history_forces_distinct_snapshots(void **state)
{
  blend_fixture_t *fixture = *state;
  _blend_fixture_enable_history(fixture);
  const int before = fixture->dev.history_end;

  dt_masks_form_t *first = dt_masks_create(DT_MASKS_CIRCLE);
  assert_non_null(first);
  first->formid = 7101;
  fixture->dev.forms = g_list_append(fixture->dev.forms, first);
  dt_dev_add_new_masks_history_item(&fixture->dev, fixture->module,
                                    fixture->module->enabled);

  dt_masks_form_t *second = dt_masks_create(DT_MASKS_ELLIPSE);
  assert_non_null(second);
  second->formid = 7102;
  fixture->dev.forms = g_list_append(fixture->dev.forms, second);
  dt_dev_add_new_masks_history_item(&fixture->dev, fixture->module,
                                    fixture->module->enabled);

  assert_int_equal(fixture->dev.history_end, before + 2);
  const dt_dev_history_item_t *first_hist =
    g_list_nth_data(fixture->dev.history, before);
  const dt_dev_history_item_t *second_hist =
    g_list_nth_data(fixture->dev.history, before + 1);
  assert_non_null(first_hist);
  assert_non_null(second_hist);
  assert_int_equal(g_list_length(first_hist->forms), 1);
  assert_int_equal(g_list_length(second_hist->forms), 2);
}

static void test_new_mask_history_resolves_global_mask_manager(void **state)
{
  blend_fixture_t *fixture = *state;
  _blend_fixture_enable_history(fixture);
  const int before = fixture->dev.history_end;

  dt_masks_form_t *form = dt_masks_create(DT_MASKS_CIRCLE);
  assert_non_null(form);
  form->formid = 7103;
  fixture->dev.forms = g_list_append(fixture->dev.forms, form);
  dt_dev_add_new_masks_history_item(&fixture->dev, NULL, FALSE);

  assert_int_equal(fixture->dev.history_end, before + 1);
  const dt_dev_history_item_t *hist =
    g_list_nth_data(fixture->dev.history, before);
  assert_non_null(hist);
  assert_true(dt_iop_module_is(hist->module, "mask_manager"));
  assert_false(hist->enabled);
  assert_int_equal(g_list_length(hist->forms), 1);
}
```

- [ ] **Step 3: Run the focused test and confirm the API is missing**

Run:

```bash
cmake --build build --target test_remote_masks -j2
```

Expected: compilation or linking fails because
`dt_dev_add_new_masks_history_item` is not declared or defined.

- [ ] **Step 4: Declare and implement the forced-new helper**

Add the declaration beside `dt_dev_add_masks_history_item` in
`develop.h`. First replace the body of
`dt_dev_add_masks_history_item_ext` with a shared private engine that
retains the existing null-module resolution:

```c
static void _dev_add_masks_history_item_ext(dt_develop_t *dev,
                                            dt_iop_module_t *module,
                                            gboolean enable,
                                            const gboolean new_item,
                                            const gboolean no_image)
{
  if(module == NULL)
  {
    for(GList *modules = dev->iop;
        modules;
        modules = g_list_next(modules))
    {
      dt_iop_module_t *candidate = modules->data;
      if(dt_iop_module_is(candidate, "mask_manager"))
      {
        module = candidate;
        break;
      }
    }
    enable = FALSE;
  }

  if(module)
    _dev_add_history_item_ext(dev, module, enable, new_item, no_image,
                              TRUE, TRUE);
  else
    dt_print(DT_DEBUG_ALWAYS,
             "[dt_dev_add_masks_history_item_ext] can't find mask manager module");
}

void dt_dev_add_masks_history_item_ext(dt_develop_t *dev,
                                       dt_iop_module_t *module,
                                       const gboolean enable,
                                       const gboolean no_image)
{
  _dev_add_masks_history_item_ext(dev, module, enable, FALSE, no_image);
}
```

Then replace the current masks-history wrapper with:

```c
static void _dev_add_masks_history_item(dt_develop_t *dev,
                                        dt_iop_module_t *module,
                                        const gboolean enable,
                                        const gboolean new_item)
{
  gpointer target = NULL;

  const dt_masks_form_t *form = dev->form_visible;
  const dt_masks_form_gui_t *gui = dev->form_gui;
  if(form && gui)
  {
    dt_masks_point_group_t *fpt =
      g_list_nth_data(form->points, gui->group_edited);
    if(fpt) target = GINT_TO_POINTER(fpt->formid);
  }

  dt_undo_t *undo = darktable.undo;
  const gboolean record_undo =
    undo && dev->gui_attached
    && dt_view_get_current() == DT_VIEW_DARKROOM;
  const gboolean isolate_undo_record =
    new_item && record_undo && dt_control_running();
  if(isolate_undo_record)
    dt_undo_start_isolated_group(undo, DT_UNDO_HISTORY);
  else if(record_undo)
    dt_undo_start_recording(undo);

  dt_pthread_mutex_lock(&dev->history_mutex);

  gboolean need_end_record = TRUE;
  if(new_item)
    dt_dev_undo_start_record(dev);
  else
    need_end_record = _dev_undo_start_record_target(dev, target);

  if(dev->gui_attached)
    _dev_add_masks_history_item_ext(dev, module, enable, new_item, FALSE);

  dt_dev_pipe_synch_all(dev);
  dt_dev_invalidate_all(dev);

  dt_pthread_mutex_unlock(&dev->history_mutex);

  if(need_end_record)
    dt_dev_undo_end_record(dev);
  if(isolate_undo_record)
    dt_undo_end_isolated_group(undo);
  else if(record_undo)
    dt_undo_end_recording(undo);

  if(dev->gui_attached)
  {
    dt_dev_masks_list_change(dev);
    dt_control_queue_redraw_center();
  }
}

void dt_dev_add_masks_history_item(dt_develop_t *dev,
                                   dt_iop_module_t *module,
                                   const gboolean enable)
{
  _dev_add_masks_history_item(dev, module, enable, FALSE);
}

void dt_dev_add_new_masks_history_item(dt_develop_t *dev,
                                       dt_iop_module_t *module,
                                       const gboolean enable)
{
  _dev_add_masks_history_item(dev, module, enable, TRUE);
}
```

- [ ] **Step 5: Build and run the focused suite**

Run:

```bash
cmake --build build --target test_remote_masks -j2
ctest --test-dir build -R '^test_remote_masks$' --output-on-failure
```

Expected: build succeeds and `test_remote_masks` passes.

- [ ] **Step 6: Commit**

```bash
git add src/develop/develop.h src/develop/develop.c \
  src/tests/unittests/control/test_remote_masks.c
git commit -m "develop: add distinct masks history snapshots"
```

---

### Task 2: Add Cycle-Safe Core Membership and Owner Queries

**Files:**

- Modify: `src/develop/masks.h:650-665`
- Modify: `src/develop/masks/masks.c:1997-2034`
- Test: `src/tests/unittests/control/test_remote_masks.c`

**Interfaces:**

- Consumes: `dt_masks_get_from_id(const dt_develop_t *, dt_mask_id_t)`.
- Produces:

```c
gboolean dt_masks_group_contains_form(const dt_develop_t *dev,
                                      const dt_masks_form_t *group,
                                      dt_mask_id_t formid);
GPtrArray *dt_masks_form_get_referencing_modules(const dt_develop_t *dev,
                                                 dt_mask_id_t formid);
```

- [ ] **Step 1: Add deterministic graph-fixture helpers**

Add these helpers after `_find_shape`:

```c
static dt_masks_form_t *_fixture_add_form(blend_fixture_t *fixture,
                                          const dt_masks_type_t type,
                                          const dt_mask_id_t id)
{
  dt_masks_form_t *form = dt_masks_create(type);
  assert_non_null(form);
  form->formid = id;
  fixture->dev.forms = g_list_append(fixture->dev.forms, form);
  return form;
}

static dt_masks_point_group_t *_fixture_add_member(
  dt_masks_form_t *group,
  const dt_mask_id_t child_id,
  const int state,
  const float opacity)
{
  dt_masks_point_group_t *member =
    g_malloc0(sizeof(dt_masks_point_group_t));
  member->formid = child_id;
  member->parentid = group->formid;
  member->state = state;
  member->opacity = opacity;
  group->points = g_list_append(group->points, member);
  return member;
}

static guint _list_pointer_count(const GList *list, const gpointer value)
{
  guint count = 0;
  for(const GList *item = list; item; item = g_list_next(item))
    if(item->data == value) count++;
  return count;
}
```

- [ ] **Step 2: Write failing traversal and owner tests**

Add and register both tests:

```c
static void test_core_group_contains_form_is_transitive_and_cycle_safe(
  void **state)
{
  blend_fixture_t *fixture = *state;
  dt_masks_form_t *leaf =
    _fixture_add_form(fixture, DT_MASKS_CIRCLE, 7201);
  dt_masks_form_t *inner =
    _fixture_add_form(fixture, DT_MASKS_GROUP, 7202);
  dt_masks_form_t *outer =
    _fixture_add_form(fixture, DT_MASKS_GROUP, 7203);
  _fixture_add_member(inner, leaf->formid,
                      DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW, 0.4f);
  _fixture_add_member(outer, inner->formid,
                      DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW, 0.8f);

  assert_true(dt_masks_group_contains_form(&fixture->dev, outer,
                                           leaf->formid));
  assert_true(dt_masks_group_contains_form(&fixture->dev, outer,
                                           inner->formid));
  assert_false(dt_masks_group_contains_form(&fixture->dev, outer, 7999));
  assert_false(dt_masks_group_contains_form(NULL, outer, leaf->formid));
  assert_false(dt_masks_group_contains_form(&fixture->dev, NULL,
                                            leaf->formid));
  assert_false(dt_masks_group_contains_form(&fixture->dev, leaf,
                                            leaf->formid));

  _fixture_add_member(inner, outer->formid,
                      DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW, 1.0f);
  assert_false(dt_masks_group_contains_form(&fixture->dev, outer, 7998));

  _fixture_add_member(outer, 7997,
                      DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW, 1.0f);
  assert_true(dt_masks_group_contains_form(&fixture->dev, outer, 7997));
}

static void test_core_referencing_modules_flattens_nested_and_shared_paths(
  void **state)
{
  blend_fixture_t *fixture = *state;
  dt_iop_module_t *second =
    _blend_fixture_add_module(fixture, "exposure", 2);
  dt_masks_form_t *leaf =
    _fixture_add_form(fixture, DT_MASKS_CIRCLE, 7211);
  dt_masks_form_t *inner =
    _fixture_add_form(fixture, DT_MASKS_GROUP, 7212);
  _fixture_add_member(inner, leaf->formid,
                      DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW, 0.5f);

  dt_masks_form_t *first_root = dt_masks_group_create_for_module(
    &fixture->dev, fixture->module, DT_MASKS_GROUP);
  dt_masks_form_t *second_root = dt_masks_group_create_for_module(
    &fixture->dev, second, DT_MASKS_GROUP);
  _fixture_add_member(first_root, inner->formid,
                      DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW, 1.0f);
  _fixture_add_member(second_root, inner->formid,
                      DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW, 1.0f);
  _fixture_add_member(first_root, inner->formid,
                      DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW, 1.0f);

  GPtrArray *owners =
    dt_masks_form_get_referencing_modules(&fixture->dev, leaf->formid);
  assert_non_null(owners);
  assert_int_equal(owners->len, 2);
  assert_ptr_equal(g_ptr_array_index(owners, 0), fixture->module);
  assert_ptr_equal(g_ptr_array_index(owners, 1), second);
  g_ptr_array_unref(owners);
}
```

- [ ] **Step 3: Run the focused build and confirm both APIs are missing**

Run:

```bash
cmake --build build --target test_remote_masks -j2
```

Expected: compilation or linking fails on the two new core symbols.

- [ ] **Step 4: Implement cycle-safe traversal**

Add the declarations to `masks.h`. Add this implementation near
`dt_masks_group_add_form`:

```c
static gboolean _group_contains_form(const dt_develop_t *dev,
                                     const dt_masks_form_t *group,
                                     const dt_mask_id_t formid,
                                     GHashTable *visited)
{
  if(!dev || !group || !(group->type & DT_MASKS_GROUP)) return FALSE;
  if(group->formid == formid) return TRUE;

  const gpointer key = GINT_TO_POINTER((gint)group->formid);
  if(g_hash_table_contains(visited, key)) return FALSE;
  g_hash_table_add(visited, key);

  for(GList *points = group->points;
      points;
      points = g_list_next(points))
  {
    const dt_masks_point_group_t *member = points->data;
    if(!member) continue;
    if(member->formid == formid) return TRUE;

    const dt_masks_form_t *child =
      dt_masks_get_from_id(dev, member->formid);
    if(child && (child->type & DT_MASKS_GROUP)
       && _group_contains_form(dev, child, formid, visited))
      return TRUE;
  }
  return FALSE;
}

gboolean dt_masks_group_contains_form(const dt_develop_t *dev,
                                      const dt_masks_form_t *group,
                                      const dt_mask_id_t formid)
{
  if(!dev || !group || !(group->type & DT_MASKS_GROUP)) return FALSE;
  GHashTable *visited =
    g_hash_table_new(g_direct_hash, g_direct_equal);
  const gboolean found =
    _group_contains_form(dev, group, formid, visited);
  g_hash_table_unref(visited);
  return found;
}

GPtrArray *dt_masks_form_get_referencing_modules(
  const dt_develop_t *dev,
  const dt_mask_id_t formid)
{
  GPtrArray *owners = g_ptr_array_new();
  if(!dev) return owners;

  for(GList *modules = dev->iop;
      modules;
      modules = g_list_next(modules))
  {
    dt_iop_module_t *module = modules->data;
    if(!module || !module->flags || !module->blend_params
       || !(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING))
      continue;

    const dt_masks_form_t *group =
      dt_masks_get_from_id(dev, module->blend_params->mask_id);
    if(group && dt_masks_group_contains_form(dev, group, formid))
      g_ptr_array_add(owners, module);
  }
  return owners;
}
```

Do not replace legacy `_find_in_group`; changing its behavior belongs to
the later caller audit.

- [ ] **Step 5: Run the focused suite**

```bash
cmake --build build --target test_remote_masks -j2
ctest --test-dir build -R '^test_remote_masks$' --output-on-failure
```

Expected: `test_remote_masks` passes, including missing-child and cycle
cases.

- [ ] **Step 6: Commit**

```bash
git add src/develop/masks.h src/develop/masks/masks.c \
  src/tests/unittests/control/test_remote_masks.c
git commit -m "masks: add transitive form ownership queries"
```

---

### Task 3: Add Ownership-Aware Full Shape Deletion

**Files:**

- Modify: `src/develop/masks.h:650-670`
- Modify: `src/develop/masks/masks.c:1823-1930`
- Test: `src/tests/unittests/control/test_remote_masks.c`

**Interfaces:**

- Consumes:
  - `dt_masks_form_get_referencing_modules(const dt_develop_t *, dt_mask_id_t)`.
  - `dt_dev_add_new_masks_history_item(dt_develop_t *, dt_iop_module_t *, gboolean)`.
- Produces:

```c
gboolean dt_masks_form_remove_shape_full(dt_develop_t *dev,
                                         dt_masks_form_t *form,
                                         GPtrArray **affected_modules);
```

- [ ] **Step 1: Write the failing nested-sibling ownership test**

Add and register:

```c
static void test_core_full_delete_keeps_nested_sibling_and_retires_leaf(
  void **state)
{
  blend_fixture_t *fixture = *state;
  dt_masks_form_t *leaf =
    _fixture_add_form(fixture, DT_MASKS_CIRCLE, 7301);
  dt_masks_form_t *sibling =
    _fixture_add_form(fixture, DT_MASKS_ELLIPSE, 7302);
  dt_masks_form_t *inner =
    _fixture_add_form(fixture, DT_MASKS_GROUP, 7303);
  _fixture_add_member(inner, leaf->formid,
                      DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW, 0.5f);
  _fixture_add_member(inner, sibling->formid,
                      DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW
                        | DT_MASKS_STATE_UNION, 0.7f);
  dt_masks_form_t *root = dt_masks_group_create_for_module(
    &fixture->dev, fixture->module, DT_MASKS_GROUP);
  _fixture_add_member(root, inner->formid,
                      DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW, 1.0f);

  GPtrArray *affected = NULL;
  assert_true(dt_masks_form_remove_shape_full(&fixture->dev, leaf,
                                              &affected));
  assert_non_null(affected);
  assert_int_equal(affected->len, 1);
  assert_ptr_equal(g_ptr_array_index(affected, 0), fixture->module);
  assert_null(dt_masks_get_from_id(&fixture->dev, leaf->formid));
  assert_non_null(dt_masks_get_from_id(&fixture->dev, sibling->formid));
  assert_non_null(dt_masks_get_from_id(&fixture->dev, inner->formid));
  assert_non_null(dt_masks_get_from_id(&fixture->dev, root->formid));
  assert_false(dt_masks_group_contains_form(&fixture->dev, root,
                                            leaf->formid));
  assert_true(dt_masks_group_contains_form(&fixture->dev, root,
                                           sibling->formid));
  assert_int_equal(_list_pointer_count(fixture->dev.allforms, leaf), 1);
  g_ptr_array_unref(affected);
}
```

- [ ] **Step 2: Write the failing cascade, module-state, and history test**

Add and register:

```c
static void test_core_full_delete_prunes_shared_empty_ancestors(void **state)
{
  blend_fixture_t *fixture = *state;
  dt_iop_module_t *second =
    _blend_fixture_add_module(fixture, "exposure", 3);
  fixture->module->enabled = TRUE;
  second->enabled = FALSE;
  fixture->module->blend_params->mask_mode =
    DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK | DEVELOP_MASK_CONDITIONAL;
  second->blend_params->mask_mode =
    DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK | DEVELOP_MASK_CONDITIONAL;

  dt_masks_form_t *leaf =
    _fixture_add_form(fixture, DT_MASKS_CIRCLE, 7311);
  dt_masks_form_t *inner =
    _fixture_add_form(fixture, DT_MASKS_GROUP, 7312);
  _fixture_add_member(inner, leaf->formid,
                      DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW, 1.0f);
  dt_masks_form_t *first_root = dt_masks_group_create_for_module(
    &fixture->dev, fixture->module, DT_MASKS_GROUP);
  dt_masks_form_t *second_root = dt_masks_group_create_for_module(
    &fixture->dev, second, DT_MASKS_GROUP);
  _fixture_add_member(first_root, inner->formid,
                      DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW, 1.0f);
  _fixture_add_member(second_root, inner->formid,
                      DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW, 1.0f);

  _blend_fixture_enable_history(fixture);
  const int history_before = fixture->dev.history_end;
  GPtrArray *affected = NULL;
  assert_true(dt_masks_form_remove_shape_full(&fixture->dev, leaf,
                                              &affected));

  assert_int_equal(affected->len, 2);
  assert_false(dt_is_valid_maskid(
    fixture->module->blend_params->mask_id));
  assert_false(dt_is_valid_maskid(second->blend_params->mask_id));
  assert_false(fixture->module->blend_params->mask_mode
               & DEVELOP_MASK_MASK);
  assert_false(second->blend_params->mask_mode & DEVELOP_MASK_MASK);
  assert_true(fixture->module->blend_params->mask_mode
              & DEVELOP_MASK_CONDITIONAL);
  assert_true(second->blend_params->mask_mode
              & DEVELOP_MASK_CONDITIONAL);
  assert_true(fixture->module->enabled);
  assert_false(second->enabled);
  assert_int_equal(fixture->dev.history_end, history_before + 3);

  assert_int_equal(_list_pointer_count(fixture->dev.allforms, leaf), 1);
  assert_int_equal(_list_pointer_count(fixture->dev.allforms, inner), 1);
  assert_int_equal(
    _list_pointer_count(fixture->dev.allforms, first_root), 1);
  assert_int_equal(
    _list_pointer_count(fixture->dev.allforms, second_root), 1);
  g_ptr_array_unref(affected);
}
```

Also add an invalid-input test asserting group input returns false and
leaves `forms`, `allforms`, `mask_id`, and history unchanged:

```c
static void test_core_full_delete_rejects_group_without_mutation(void **state)
{
  blend_fixture_t *fixture = *state;
  dt_masks_form_t *group = dt_masks_group_create_for_module(
    &fixture->dev, fixture->module, DT_MASKS_GROUP);
  const guint forms_before = g_list_length(fixture->dev.forms);
  const guint allforms_before = g_list_length(fixture->dev.allforms);
  const dt_mask_id_t mask_id_before =
    fixture->module->blend_params->mask_id;
  const int history_before = fixture->dev.history_end;
  GPtrArray *sentinel = g_ptr_array_new();
  GPtrArray *affected = sentinel;

  assert_false(dt_masks_form_remove_shape_full(&fixture->dev, group,
                                               &affected));
  assert_ptr_equal(affected, sentinel);
  assert_int_equal(g_list_length(fixture->dev.forms), forms_before);
  assert_int_equal(g_list_length(fixture->dev.allforms), allforms_before);
  assert_int_equal(fixture->module->blend_params->mask_id,
                   mask_id_before);
  assert_int_equal(fixture->dev.history_end, history_before);
  g_ptr_array_unref(sentinel);
}
```

- [ ] **Step 3: Run the build and confirm the full-delete symbol is missing**

```bash
cmake --build build --target test_remote_masks -j2
```

Expected: compilation or linking fails for
`dt_masks_form_remove_shape_full`.

- [ ] **Step 4: Implement removal, retirement, and cascade helpers**

Add these private helpers before the new public function:

```c
static gboolean _ptr_array_contains(const GPtrArray *array,
                                    const gpointer value)
{
  for(guint i = 0; array && i < array->len; i++)
    if(g_ptr_array_index(array, i) == value) return TRUE;
  return FALSE;
}

static gboolean _remove_id_from_group(dt_masks_form_t *group,
                                      const dt_mask_id_t id)
{
  if(!group || !(group->type & DT_MASKS_GROUP)) return FALSE;
  gboolean removed = FALSE;
  for(GList *link = group->points; link;)
  {
    GList *next = g_list_next(link);
    dt_masks_point_group_t *member = link->data;
    if(member && member->formid == id)
    {
      group->points = g_list_delete_link(group->points, link);
      free(member);
      removed = TRUE;
    }
    link = next;
  }
  return removed;
}

static void _queue_empty_group(GQueue *queue,
                               GHashTable *queued,
                               dt_masks_form_t *group)
{
  if(!group || !(group->type & DT_MASKS_GROUP) || group->points) return;
  if(g_hash_table_contains(queued, group)) return;
  g_hash_table_add(queued, group);
  g_queue_push_tail(queue, group);
}

static void _retire_form(dt_develop_t *dev, dt_masks_form_t *form)
{
  GList *link = g_list_find(dev->forms, form);
  if(link) dev->forms = g_list_delete_link(dev->forms, link);
  if(!g_list_find(dev->allforms, form))
    dev->allforms = g_list_append(dev->allforms, form);
}
```

- [ ] **Step 5: Implement full deletion**

Declare the function in `masks.h` and add:

```c
gboolean dt_masks_form_remove_shape_full(dt_develop_t *dev,
                                         dt_masks_form_t *form,
                                         GPtrArray **affected_modules)
{
  if(!dev || !form || (form->type & DT_MASKS_GROUP)
     || !g_list_find(dev->forms, form))
    return FALSE;

  GPtrArray *owners =
    dt_masks_form_get_referencing_modules(dev, form->formid);
  GPtrArray *emptied_modules = g_ptr_array_new();
  GPtrArray *forms_to_retire = g_ptr_array_new();
  GQueue empty_groups = G_QUEUE_INIT;
  GHashTable *queued =
    g_hash_table_new(g_direct_hash, g_direct_equal);

  for(GList *forms = dev->forms;
      forms;
      forms = g_list_next(forms))
  {
    dt_masks_form_t *group = forms->data;
    if(_remove_id_from_group(group, form->formid))
      _queue_empty_group(&empty_groups, queued, group);
  }
  g_ptr_array_add(forms_to_retire, form);

  while(!g_queue_is_empty(&empty_groups))
  {
    dt_masks_form_t *empty = g_queue_pop_head(&empty_groups);
    if(!g_list_find(dev->forms, empty)) continue;

    for(GList *modules = dev->iop;
        modules;
        modules = g_list_next(modules))
    {
      dt_iop_module_t *module = modules->data;
      if(!module || !module->blend_params
         || module->blend_params->mask_id != empty->formid)
        continue;
      module->blend_params->mask_id = NO_MASKID;
      module->blend_params->mask_mode &= ~DEVELOP_MASK_MASK;
      if(!_ptr_array_contains(emptied_modules, module))
      {
        g_ptr_array_add(emptied_modules, module);
        dt_dev_add_new_masks_history_item(dev, module, module->enabled);
      }
    }

    for(GList *forms = dev->forms;
        forms;
        forms = g_list_next(forms))
    {
      dt_masks_form_t *parent = forms->data;
      if(parent != empty
         && _remove_id_from_group(parent, empty->formid))
        _queue_empty_group(&empty_groups, queued, parent);
    }
    g_ptr_array_add(forms_to_retire, empty);
  }

  for(guint i = 0; i < forms_to_retire->len; i++)
    _retire_form(dev, g_ptr_array_index(forms_to_retire, i));
  dt_dev_add_new_masks_history_item(dev, NULL, FALSE);
  for(guint i = 0; i < owners->len; i++)
    dt_masks_iop_update(g_ptr_array_index(owners, i));

  g_ptr_array_unref(forms_to_retire);
  g_ptr_array_unref(emptied_modules);
  g_hash_table_unref(queued);
  if(affected_modules)
    *affected_modules = owners;
  else
    g_ptr_array_unref(owners);
  return TRUE;
}
```

The module history call remains inside the queue loop, before any target
or cascaded group is retired. Retirement is deferred until the queue
drains, then the global snapshot records the pruned graph. This preserves
the exact `1 + unique cleared modules` count while ensuring every valid
module `mask_id` resolves in every undo/redo prefix.

Keep `dt_masks_form_remove` byte-for-byte behaviorally unchanged.

- [ ] **Step 6: Run the focused suite**

```bash
cmake --build build --target test_remote_masks -j2
ctest --test-dir build -R '^test_remote_masks$' --output-on-failure
```

Expected: nested deletion, shared cascade, module-state preservation,
history count, invalid input, and existing tests all pass. Teardown must
complete without manual frees for retired forms.

- [ ] **Step 7: Commit**

```bash
git add src/develop/masks.h src/develop/masks/masks.c \
  src/tests/unittests/control/test_remote_masks.c
git commit -m "masks: add ownership-safe full shape deletion"
```

---

### Task 4: Add Explicit Creation Options and Coherent Undo States

**Files:**

- Modify: `src/develop/masks.h:620-635`
- Modify: `src/develop/masks/masks.c:336-450`
- Modify: `src/control/remote_masks.c:1020-1080`
- Test: `src/tests/unittests/control/test_remote_masks.c`

**Interfaces:**

- Consumes: `dt_dev_add_new_masks_history_item`.
- Produces:

```c
typedef struct dt_masks_form_creation_options_t
{
  const char *requested_name;
  gboolean preserve_module_enabled;
  uint32_t mask_mode_to_add;
} dt_masks_form_creation_options_t;

void dt_masks_gui_form_save_creation_ext(
  dt_develop_t *dev,
  dt_iop_module_t *module,
  dt_masks_form_t *form,
  dt_masks_form_gui_t *gui,
  const dt_masks_form_creation_options_t *options);
```

- [ ] **Step 1: Write the failing creation-history test**

Add and register with the normal blend fixture:

```c
static void test_creation_ext_preserves_disabled_and_orders_snapshots(
  void **state)
{
  blend_fixture_t *fixture = *state;
  _blend_fixture_enable_history(fixture);
  fixture->module->enabled = FALSE;
  fixture->module->blend_params->mask_mode =
    DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL;
  const uint32_t mode_before =
    fixture->module->blend_params->mask_mode;
  const int history_before = fixture->dev.history_end;

  dt_masks_form_t *form = dt_masks_create(DT_MASKS_CIRCLE);
  assert_non_null(form);
  const dt_masks_form_creation_options_t options = {
    .requested_name = "remote subject",
    .preserve_module_enabled = TRUE,
    .mask_mode_to_add = DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK,
  };
  dt_masks_gui_form_save_creation_ext(&fixture->dev, fixture->module,
                                      form, NULL, &options);
  const dt_mask_id_t form_id = form->formid;

  assert_false(fixture->module->enabled);
  assert_int_equal(fixture->dev.history_end, history_before + 2);
  const dt_dev_history_item_t *unattached =
    g_list_nth_data(fixture->dev.history, history_before);
  const dt_dev_history_item_t *attached =
    g_list_nth_data(fixture->dev.history, history_before + 1);
  assert_non_null(unattached);
  assert_non_null(attached);
  assert_false(unattached->enabled);
  assert_false(attached->enabled);
  assert_int_equal(unattached->blend_params->mask_mode, mode_before);
  assert_int_equal(unattached->blend_params->mask_id, NO_MASKID);
  assert_non_null(
    dt_masks_get_from_id_ext(unattached->forms, form_id));
  assert_true(attached->blend_params->mask_mode & DEVELOP_MASK_MASK);
  assert_true(dt_is_valid_maskid(attached->blend_params->mask_id));

  dt_dev_pop_history_items(&fixture->dev, history_before + 1);
  assert_non_null(dt_masks_get_from_id(&fixture->dev, form_id));
  assert_int_equal(fixture->module->blend_params->mask_mode, mode_before);
  assert_int_equal(fixture->module->blend_params->mask_id, NO_MASKID);
  assert_false(fixture->module->enabled);

  dt_dev_pop_history_items(&fixture->dev, history_before);
  assert_null(dt_masks_get_from_id(&fixture->dev, form_id));
  assert_false(fixture->module->enabled);
}
```

- [ ] **Step 2: Write the failing compatibility-naming test**

```c
static void test_creation_wrapper_keeps_gui_default_naming(void **state)
{
  blend_fixture_t *fixture = *state;
  dt_masks_form_t *form = dt_masks_create(DT_MASKS_CIRCLE);
  assert_non_null(form);
  g_strlcpy(form->name, "implicit remote name", sizeof(form->name));

  dt_masks_gui_form_save_creation(&fixture->dev, NULL, form, NULL);

  assert_string_not_equal(form->name, "implicit remote name");
  assert_true(form->name[0] != '\0');
}
```

Expected current failure: the Task 5 implementation treats the
pre-populated field as an implicit requested name.

- [ ] **Step 3: Run the build and confirm the extended API is missing**

```bash
cmake --build build --target test_remote_masks -j2
```

Expected: compilation or linking fails for the options type and extended
helper.

- [ ] **Step 4: Extract explicit unique naming**

Add this helper above the creation functions:

```c
static void _set_unique_form_name(dt_develop_t *dev,
                                  dt_masks_form_t *form,
                                  const char *requested_name)
{
  guint same_type_count = 0;
  for(GList *forms = dev->forms;
      forms;
      forms = g_list_next(forms))
  {
    const dt_masks_form_t *existing = forms->data;
    if(existing->type == form->type) same_type_count++;
  }

  guint suffix_number = 1;
  gboolean exists = FALSE;
  do
  {
    exists = FALSE;
    if(requested_name && *requested_name)
    {
      if(suffix_number == 1)
      {
        g_strlcpy(form->name, requested_name, sizeof(form->name));
      }
      else
      {
        char suffix[32];
        g_snprintf(suffix, sizeof(suffix), " #%u", suffix_number);
        int base_length =
          MIN((int)strlen(requested_name),
              MAX(0, (int)sizeof(form->name) - 1
                     - (int)strlen(suffix)));
        while(base_length > 0
              && (((unsigned char)requested_name[base_length] & 0xc0)
                  == 0x80))
          base_length--;
        g_snprintf(form->name, sizeof(form->name), "%.*s%s",
                   base_length, requested_name, suffix);
      }
      suffix_number++;
    }
    else
    {
      same_type_count++;
      if(form->functions && form->functions->set_form_name)
        form->functions->set_form_name(form, same_type_count);
    }

    for(GList *forms = dev->forms;
        forms;
        forms = g_list_next(forms))
    {
      const dt_masks_form_t *existing = forms->data;
      if(!strcmp(existing->name, form->name))
      {
        exists = TRUE;
        break;
      }
    }
  } while(exists);
}
```

- [ ] **Step 5: Implement the shared creation engine and both wrappers**

Declare the options and extended helper in `masks.h`. Replace the current
creation body with a private engine plus two wrappers:

```c
static void _save_form_creation(
  dt_develop_t *dev,
  dt_iop_module_t *module,
  dt_masks_form_t *form,
  dt_masks_form_gui_t *gui,
  const dt_masks_form_creation_options_t *options,
  const gboolean force_new_history)
{
  _check_id(form);
  if(gui) gui->creation = FALSE;

  _set_unique_form_name(dev, form,
                        options ? options->requested_name : NULL);
  dev->forms = g_list_append(dev->forms, form);

  const gboolean history_enable =
    module && options && options->preserve_module_enabled
      ? module->enabled
      : TRUE;
  if(force_new_history)
    dt_dev_add_new_masks_history_item(dev, module, history_enable);
  else
    dt_dev_add_masks_history_item(dev, module, history_enable);

  if(module)
  {
    dt_masks_form_t *group = _group_from_module(dev, module);
    if(!group)
    {
      const dt_masks_type_t group_type =
        form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE)
          ? DT_MASKS_GROUP | DT_MASKS_CLONE
          : DT_MASKS_GROUP;
      group =
        dt_masks_group_create_for_module(dev, module, group_type);
    }

    dt_masks_point_group_t *member =
      malloc(sizeof(dt_masks_point_group_t));
    member->formid = form->formid;
    member->parentid = group->formid;
    member->state = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE;
    if(group->points)
      member->state |= form->type == DT_MASKS_BRUSH
        ? DT_MASKS_STATE_SUM
        : DT_MASKS_STATE_UNION;
    member->opacity =
      dt_conf_get_float("plugins/darkroom/masks/opacity");
    group->points = g_list_append(group->points, member);

    if(options)
      module->blend_params->mask_mode |= options->mask_mode_to_add;
    if(force_new_history)
      dt_dev_add_new_masks_history_item(dev, module, history_enable);
    else
      dt_dev_add_masks_history_item(dev, module, history_enable);
    if(gui) dt_masks_iop_update(module);
  }

  if(gui) dev->form_gui->formid = form->formid;
}

void dt_masks_gui_form_save_creation(dt_develop_t *dev,
                                     dt_iop_module_t *module,
                                     dt_masks_form_t *form,
                                     dt_masks_form_gui_t *gui)
{
  _save_form_creation(dev, module, form, gui, NULL, FALSE);
}

void dt_masks_gui_form_save_creation_ext(
  dt_develop_t *dev,
  dt_iop_module_t *module,
  dt_masks_form_t *form,
  dt_masks_form_gui_t *gui,
  const dt_masks_form_creation_options_t *options)
{
  _save_form_creation(dev, module, form, gui, options, TRUE);
}
```

- [ ] **Step 6: Migrate remote creation atomically with the wrapper change**

In `dt_remote_masks_create`, delete the pre-populated-name and
pre-set-mask-mode blocks. Replace the old save call with:

```c
const dt_masks_form_creation_options_t options = {
  .requested_name = name_or_null,
  .preserve_module_enabled = TRUE,
  .mask_mode_to_add =
    attach_module ? DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK : 0,
};
dt_masks_gui_form_save_creation_ext(dev, attach_module, form, NULL,
                                    &options);
```

This adoption belongs in Task 4 because the restored compatibility
wrapper intentionally ignores a pre-populated `form->name`; leaving the
remote caller on that wrapper until Task 5 would break requested naming
at the Task 4 checkpoint.

- [ ] **Step 7: Run focused tests**

```bash
cmake --build build --target test_remote_masks -j2
ctest --test-dir build -R '^test_remote_masks$' --output-on-failure
```

Expected: distinct snapshots, both undo states, disabled-module
preservation, default naming, and existing GUI creation tests pass.

- [ ] **Step 8: Commit**

```bash
git add src/develop/masks.h src/develop/masks/masks.c \
  src/control/remote_masks.c \
  src/tests/unittests/control/test_remote_masks.c
git commit -m "masks: add coherent extended form creation"
```

---

### Task 5: Migrate Remote CRUD, Reporting, and H2 to Core Graph APIs

**Files:**

- Modify: `src/control/remote_masks.h:95-105`
- Modify: `src/control/remote_masks.c:853-933`
- Modify: `src/control/remote_masks.c:985-1235`
- Test: `src/tests/unittests/control/test_remote_masks.c:930-1625`

**Interfaces:**

- Consumes all interfaces produced by Tasks 1-4.
- Produces:
  - Transitive `used_by`, `affects_instances`, and `removed_from`.
  - Transitive H2 cancellation.
  - Guarded create into an existing group.
  - Ownership-safe remote full deletion.

- [ ] **Step 1: Write the failing nested reporting and update test**

Add and register this complete test:

```c
static void test_nested_membership_reports_and_updates_owner(void **state)
{
  blend_fixture_t *fixture = *state;
  fixture->module->multi_priority = 6;
  dt_masks_form_t *circle =
    _fixture_add_form(fixture, DT_MASKS_CIRCLE, 7401);
  dt_masks_point_circle_t *point =
    g_malloc0(sizeof(dt_masks_point_circle_t));
  *point = (dt_masks_point_circle_t){
    .center = { 0.5f, 0.5f },
    .radius = 0.1f,
    .border = 0.02f,
  };
  circle->points = g_list_append(circle->points, point);
  dt_masks_form_t *inner =
    _fixture_add_form(fixture, DT_MASKS_GROUP, 7402);
  _fixture_add_member(inner, circle->formid,
                      DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW
                        | DT_MASKS_STATE_DIFFERENCE
                        | DT_MASKS_STATE_INVERSE,
                      0.42f);
  dt_masks_form_t *root = dt_masks_group_create_for_module(
    &fixture->dev, fixture->module, DT_MASKS_GROUP);
  _fixture_add_member(root, inner->formid,
                      DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW, 1.0f);

  JsonNode *node = dt_remote_masks_list(&fixture->dev);
  JsonObject *root_object = _node_object(node);
  JsonArray *shapes =
    json_object_get_array_member(root_object, "shapes");
  JsonObject *listed = _find_shape(shapes, circle->formid);
  assert_non_null(listed);
  JsonArray *used_by = json_object_get_array_member(listed, "used_by");
  assert_int_equal(json_array_get_length(used_by), 1);
  JsonObject *usage = json_array_get_object_element(used_by, 0);
  assert_string_equal(json_object_get_string_member(usage, "op"),
                      "exposure");
  assert_int_equal(json_object_get_int_member(usage, "instance"), 6);
  JsonArray *state_array =
    json_object_get_array_member(usage, "state");
  assert_string_equal(json_array_get_string_element(state_array, 0),
                      "difference");
  assert_true(json_object_get_boolean_member(usage, "inverted"));
  assert_float_equal(json_object_get_double_member(usage, "opacity"),
                     0.42, 1e-6);

  JsonObject *replacement =
    _geom("{\"center\":[0.2,0.3],\"radius\":0.15,\"border\":0.02}");
  int affects = -1;
  dt_remote_error_t *error = NULL;
  fixture->dev.form_visible = root;
  fixture->dev.form_gui->formid = root->formid;
  assert_true(dt_remote_masks_update(&fixture->dev, circle->formid,
                                     replacement, NULL, &affects,
                                     &error));
  assert_null(error);
  assert_int_equal(affects, 1);
  assert_null(fixture->dev.form_visible);
  assert_int_equal(fixture->dev.form_gui->formid, 0);
  json_object_unref(replacement);
  json_node_unref(node);
}
```

Expected current failure: nested `used_by` is empty and `affects` is zero.

- [ ] **Step 2: Write failing transitive and create-group H2 tests**

Add and register:

```c
static void test_guard_cancels_transitive_group_target(void **state)
{
  blend_fixture_t *fixture = *state;
  dt_masks_form_t *leaf =
    _fixture_add_form(fixture, DT_MASKS_CIRCLE, 7501);
  dt_masks_form_t *inner =
    _fixture_add_form(fixture, DT_MASKS_GROUP, 7502);
  dt_masks_form_t *outer =
    _fixture_add_form(fixture, DT_MASKS_GROUP, 7503);
  _fixture_add_member(inner, leaf->formid,
                      DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW, 1.0f);
  _fixture_add_member(outer, inner->formid,
                      DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW, 1.0f);
  fixture->dev.form_visible = outer;
  fixture->dev.form_gui->formid = outer->formid;

  dt_remote_masks_cancel_gui_edit_if_targeting(&fixture->dev, leaf);

  assert_null(fixture->dev.form_visible);
  assert_int_equal(fixture->dev.form_gui->formid, 0);
}

static void test_create_attached_cancels_visible_existing_group(void **state)
{
  blend_fixture_t *fixture = *state;
  dt_masks_form_t *group = dt_masks_group_create_for_module(
    &fixture->dev, fixture->module, DT_MASKS_GROUP);
  fixture->dev.form_visible = group;
  fixture->dev.form_gui->formid = group->formid;
  JsonObject *geometry =
    _geom("{\"center\":[0.5,0.5],\"radius\":0.1,\"border\":0.02}");
  dt_remote_error_t *error = NULL;
  dt_mask_id_t id = INVALID_MASKID;

  assert_true(dt_remote_masks_create(&fixture->dev, DT_MASKS_CIRCLE,
                                     geometry, "guarded",
                                     fixture->module, &id, &error));
  assert_null(error);
  assert_null(fixture->dev.form_visible);
  assert_int_equal(fixture->dev.form_gui->formid, 0);
  assert_true(dt_masks_group_contains_form(&fixture->dev, group, id));
  json_object_unref(geometry);
}
```

- [ ] **Step 3: Write the failing remote nested-delete test**

Add and register:

```c
static void test_remote_delete_nested_leaf_keeps_sibling(void **state)
{
  blend_fixture_t *fixture = *state;
  fixture->module->multi_priority = 7;
  dt_masks_form_t *leaf =
    _fixture_add_form(fixture, DT_MASKS_CIRCLE, 7511);
  dt_masks_form_t *sibling =
    _fixture_add_form(fixture, DT_MASKS_ELLIPSE, 7512);
  dt_masks_form_t *inner =
    _fixture_add_form(fixture, DT_MASKS_GROUP, 7513);
  _fixture_add_member(inner, leaf->formid,
                      DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW, 0.6f);
  _fixture_add_member(inner, sibling->formid,
                      DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW
                        | DT_MASKS_STATE_UNION,
                      0.8f);
  dt_masks_form_t *root = dt_masks_group_create_for_module(
    &fixture->dev, fixture->module, DT_MASKS_GROUP);
  _fixture_add_member(root, inner->formid,
                      DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW, 1.0f);
  fixture->dev.form_visible = root;
  fixture->dev.form_gui->formid = root->formid;
  JsonArray *removed_from = json_array_new();
  dt_remote_error_t *error = NULL;

  assert_true(dt_remote_masks_delete(&fixture->dev, leaf->formid,
                                     removed_from, &error));
  assert_null(error);
  assert_int_equal(json_array_get_length(removed_from), 1);
  JsonObject *removed =
    json_array_get_object_element(removed_from, 0);
  assert_string_equal(json_object_get_string_member(removed, "op"),
                      "exposure");
  assert_int_equal(json_object_get_int_member(removed, "instance"), 7);
  assert_null(fixture->dev.form_visible);
  assert_int_equal(fixture->dev.form_gui->formid, 0);
  assert_null(dt_masks_get_from_id(&fixture->dev, leaf->formid));
  assert_true(dt_masks_group_contains_form(&fixture->dev, root,
                                           sibling->formid));
  assert_false(dt_masks_group_contains_form(&fixture->dev, root,
                                            leaf->formid));
  assert_int_equal(_list_pointer_count(fixture->dev.allforms, leaf), 1);
  json_array_unref(removed_from);
}
```

Expected current failure: `removed_from` is empty and the inner group
retains the deleted ID.

- [ ] **Step 4: Run the tests and observe the nested failures**

```bash
cmake --build build --target test_remote_masks -j2
ctest --test-dir build -R '^test_remote_masks$' --output-on-failure
```

Expected: the newly added nested reporting, H2, and deletion assertions
fail.

- [ ] **Step 5: Make H2 transitive and guard existing attachment groups**

Replace the immediate membership loop in
`dt_remote_masks_cancel_gui_edit_if_targeting` with:

```c
const gboolean hit =
  visible->formid == form->formid
  || (dev->form_gui && dev->form_gui->formid == form->formid)
  || dt_masks_group_contains_form(dev, visible, form->formid);
if(hit) dt_masks_change_form_gui(NULL);
```

Before the extended creation call, guard an existing module group:

```c
if(attach_module)
{
  dt_masks_form_t *group =
    dt_masks_get_from_id(dev, attach_module->blend_params->mask_id);
  if(group)
    dt_remote_masks_cancel_gui_edit_if_targeting(dev, group);
}
```

Update the declaration comment in `remote_masks.h` to say that visible
groups target members transitively.

- [ ] **Step 6: Verify the explicit creation options migration**

Task 4 already migrated `dt_remote_masks_create` atomically with the
compatibility-wrapper naming change. Verify that the caller still uses
`dt_masks_gui_form_save_creation_ext`, passes `name_or_null`, preserves
module enabled state, and adds `DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK`
only for attached creation. Do not add another creation path.

- [ ] **Step 7: Add deterministic nested membership metadata lookup**

Add this private helper before `_append_used_by`:

```c
static const dt_masks_point_group_t *_find_membership(
  const dt_develop_t *dev,
  const dt_masks_form_t *group,
  const dt_mask_id_t id,
  GHashTable *visited)
{
  if(!dev || !group || !(group->type & DT_MASKS_GROUP)) return NULL;
  if(g_hash_table_contains(visited, group)) return NULL;
  g_hash_table_add(visited, (gpointer)group);

  for(GList *points = group->points;
      points;
      points = g_list_next(points))
  {
    const dt_masks_point_group_t *member = points->data;
    if(member && member->formid == id) return member;
    const dt_masks_form_t *child =
      member ? dt_masks_get_from_id(dev, member->formid) : NULL;
    const dt_masks_point_group_t *found =
      child ? _find_membership(dev, child, id, visited) : NULL;
    if(found) return found;
  }
  return NULL;
}
```

Replace `_append_used_by` with:

```c
static void _append_used_by(dt_develop_t *dev,
                            const dt_mask_id_t id,
                            JsonArray *out)
{
  for(GList *iops = dev->iop; iops; iops = g_list_next(iops))
  {
    dt_iop_module_t *module = iops->data;
    if(!module || !module->flags || !module->blend_params
       || !(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING))
      continue;

    const dt_masks_form_t *group =
      dt_masks_get_from_id(dev, module->blend_params->mask_id);
    if(!group || !dt_masks_group_contains_form(dev, group, id))
      continue;

    GHashTable *visited =
      g_hash_table_new(g_direct_hash, g_direct_equal);
    const dt_masks_point_group_t *member =
      _find_membership(dev, group, id, visited);
    g_hash_table_unref(visited);
    if(!member) continue;

    JsonObject *usage = json_object_new();
    json_object_set_string_member(usage, "op", module->op);
    json_object_set_int_member(usage, "instance",
                               module->multi_priority);
    JsonArray *state = json_array_new();
    json_array_add_string_element(
      state, dt_remote_masks_state_op_string(member->state));
    json_object_set_array_member(usage, "state", state);
    json_object_set_boolean_member(
      usage, "inverted",
      (member->state & DT_MASKS_STATE_INVERSE) != 0);
    json_object_set_double_member(usage, "opacity", member->opacity);
    json_array_add_object_element(out, usage);
  }
}
```

This emits at most one entry per module, using the target edge reached
first by an ordered depth-first traversal. For each membership in list
order, the walk tests that edge and then recurses immediately before
advancing to its next sibling. The selected target edge supplies state,
inversion, and opacity; outer-group composition remains engine-internal.

- [ ] **Step 8: Replace shallow update and delete ownership**

Replace update's module loop with:

```c
GPtrArray *owners =
  dt_masks_form_get_referencing_modules(dev, id);
if(affects_out) *affects_out = owners->len;
g_ptr_array_unref(owners);
dt_dev_add_new_masks_history_item(dev, NULL, FALSE);
```

Delete `_group_has_other_members` and the entire direct-reference loop in
`dt_remote_masks_delete`. Replace the core mutation and response setup
with:

```c
GPtrArray *references = NULL;
if(!dt_masks_form_remove_shape_full(dev, form, &references))
  return _operation_error(error, DT_REMOTE_ERR_INTERNAL,
                          "failed to delete mask shape", NULL, NULL);

for(guint i = 0; i < references->len; i++)
{
  dt_iop_module_t *module = g_ptr_array_index(references, i);
  if(!removed_from_out) continue;
  JsonObject *removed = json_object_new();
  json_object_set_string_member(removed, "op", module->op);
  json_object_set_int_member(removed, "instance",
                             module->multi_priority);
  json_array_add_object_element(removed_from_out, removed);
}
g_ptr_array_unref(references);
```

- [ ] **Step 9: Run focused tests**

```bash
cmake --build build --target test_remote_masks -j2
ctest --test-dir build -R '^test_remote_masks$' --output-on-failure
```

Expected: direct and nested reporting, update counts, GUI cancellation,
create guarding, deletion, naming, and existing validation tests pass.

- [ ] **Step 10: Commit**

```bash
git add src/control/remote_masks.h src/control/remote_masks.c \
  src/tests/unittests/control/test_remote_masks.c
git commit -m "remote_masks: make Task 5 graph and history safe"
```

---

### Task 6: Add End-to-End Remote History and Ownership Regressions

**Files:**

- Modify: `src/tests/unittests/control/test_remote_masks.c`

**Interfaces:**

- Consumes the complete Task 5 remote CRUD surface.
- Produces a production-history regression gate for module enablement,
  two-step undo, deletion counts, and develop-lifetime ownership.

- [ ] **Step 1: Add the disabled-module create-and-undo regression**

Register this with the history-capable fixture setup:

```c
static int blend_history_test_setup(void **state)
{
  const int result = blend_test_setup(state);
  if(result != 0) return result;
  _blend_fixture_enable_history(*state);
  return 0;
}

static void test_remote_create_attached_preserves_disabled_and_undo(
  void **state)
{
  blend_fixture_t *fixture = *state;
  fixture->module->enabled = FALSE;
  fixture->module->blend_params->mask_mode =
    DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL;
  const uint32_t mode_before =
    fixture->module->blend_params->mask_mode;
  const int history_before = fixture->dev.history_end;
  JsonObject *geometry =
    _geom("{\"center\":[0.4,0.6],\"radius\":0.12,\"border\":0.02}");
  dt_remote_error_t *error = NULL;
  dt_mask_id_t id = INVALID_MASKID;

  assert_true(dt_remote_masks_create(&fixture->dev, DT_MASKS_CIRCLE,
                                     geometry, "undo subject",
                                     fixture->module, &id, &error));
  assert_null(error);
  assert_false(fixture->module->enabled);
  assert_int_equal(fixture->dev.history_end, history_before + 2);

  dt_dev_pop_history_items(&fixture->dev, history_before + 1);
  assert_non_null(dt_masks_get_from_id(&fixture->dev, id));
  assert_int_equal(fixture->module->blend_params->mask_mode, mode_before);
  assert_int_equal(fixture->module->blend_params->mask_id, NO_MASKID);
  assert_false(fixture->module->enabled);

  dt_dev_pop_history_items(&fixture->dev, history_before);
  assert_null(dt_masks_get_from_id(&fixture->dev, id));
  assert_false(fixture->module->enabled);
  json_object_unref(geometry);
}
```

- [ ] **Step 2: Convert the existing delete test to ownership assertions**

Remove:

```c
dt_masks_free_form(group);
dt_masks_free_form(form);
```

Replace it with:

```c
assert_int_equal(_list_pointer_count(fixture->dev.allforms, group), 1);
assert_int_equal(_list_pointer_count(fixture->dev.allforms, form), 1);
```

Add a history-enabled variant that records `history_before`, performs
remote deletion of the sole group member:

```c
static void test_remote_delete_history_count_and_ownership(void **state)
{
  blend_fixture_t *fixture = *state;
  fixture->module->blend_params->mask_mode =
    DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK | DEVELOP_MASK_CONDITIONAL;
  JsonObject *geometry =
    _geom("{\"center\":[0.5,0.5],\"radius\":0.1,\"border\":0.02}");
  dt_remote_error_t *error = NULL;
  dt_mask_id_t id = INVALID_MASKID;
  assert_true(dt_remote_masks_create(&fixture->dev, DT_MASKS_CIRCLE,
                                     geometry, "delete history",
                                     fixture->module, &id, &error));
  assert_null(error);
  dt_masks_form_t *form =
    dt_masks_get_from_id(&fixture->dev, id);
  dt_masks_form_t *group = dt_masks_get_from_id(
    &fixture->dev, fixture->module->blend_params->mask_id);
  assert_non_null(form);
  assert_non_null(group);
  const int history_before = fixture->dev.history_end;
  JsonArray *removed_from = json_array_new();

  assert_true(dt_remote_masks_delete(&fixture->dev, id,
                                     removed_from, &error));
  assert_null(error);
  assert_int_equal(fixture->dev.history_end, history_before + 2);
  assert_int_equal(_list_pointer_count(fixture->dev.allforms, form), 1);
  assert_int_equal(_list_pointer_count(fixture->dev.allforms, group), 1);
  assert_false(fixture->module->blend_params->mask_mode
               & DEVELOP_MASK_MASK);
  assert_true(fixture->module->blend_params->mask_mode
              & DEVELOP_MASK_CONDITIONAL);
  json_array_unref(removed_from);
  json_object_unref(geometry);
}
```

Register it with `blend_history_test_setup` and
`blend_test_teardown`.

- [ ] **Step 3: Add enabled-module and existing-group creation cases**

Add and register this test with `blend_history_test_setup` and
`blend_test_teardown`:

```c
static void test_remote_create_existing_group_snapshots_are_coherent(
  void **state)
{
  blend_fixture_t *fixture = *state;
  fixture->module->enabled = TRUE;
  fixture->module->blend_params->mask_mode =
    DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK | DEVELOP_MASK_CONDITIONAL;
  const uint32_t mode_before =
    fixture->module->blend_params->mask_mode;

  dt_masks_form_t *existing =
    _fixture_add_form(fixture, DT_MASKS_ELLIPSE, 7601);
  dt_masks_form_t *group = dt_masks_group_create_for_module(
    &fixture->dev, fixture->module, DT_MASKS_GROUP);
  _fixture_add_member(group, existing->formid,
                      DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW, 0.8f);
  const dt_mask_id_t group_id = group->formid;
  dt_dev_add_new_masks_history_item(&fixture->dev, fixture->module,
                                    fixture->module->enabled);
  const int history_before = fixture->dev.history_end;
  JsonObject *geometry =
    _geom("{\"center\":[0.3,0.4],\"radius\":0.11,\"border\":0.02}");
  dt_remote_error_t *error = NULL;
  dt_mask_id_t new_id = INVALID_MASKID;

  assert_true(dt_remote_masks_create(&fixture->dev, DT_MASKS_CIRCLE,
                                     geometry, "second member",
                                     fixture->module, &new_id, &error));
  assert_null(error);
  assert_true(fixture->module->enabled);
  assert_int_equal(fixture->dev.history_end, history_before + 2);

  const dt_dev_history_item_t *unattached =
    g_list_nth_data(fixture->dev.history, history_before);
  const dt_dev_history_item_t *attached =
    g_list_nth_data(fixture->dev.history, history_before + 1);
  assert_non_null(unattached);
  assert_non_null(attached);
  assert_true(unattached->enabled);
  assert_true(attached->enabled);
  assert_int_equal(unattached->blend_params->mask_mode, mode_before);
  assert_int_equal(unattached->blend_params->mask_id, group_id);
  const dt_masks_form_t *unattached_group =
    dt_masks_get_from_id_ext(unattached->forms, group_id);
  const dt_masks_form_t *attached_group =
    dt_masks_get_from_id_ext(attached->forms, group_id);
  assert_non_null(unattached_group);
  assert_non_null(attached_group);
  assert_int_equal(g_list_length(unattached_group->points), 1);
  assert_int_equal(g_list_length(attached_group->points), 2);
  assert_non_null(dt_masks_get_from_id_ext(unattached->forms, new_id));
  assert_non_null(dt_masks_get_from_id_ext(attached->forms, new_id));

  dt_dev_pop_history_items(&fixture->dev, history_before + 1);
  assert_true(fixture->module->enabled);
  assert_int_equal(fixture->module->blend_params->mask_id, group_id);
  assert_int_equal(fixture->module->blend_params->mask_mode, mode_before);
  group = dt_masks_get_from_id(&fixture->dev, group_id);
  assert_non_null(group);
  assert_int_equal(g_list_length(group->points), 1);

  dt_dev_pop_history_items(&fixture->dev, history_before);
  assert_true(fixture->module->enabled);
  assert_null(dt_masks_get_from_id(&fixture->dev, new_id));
  assert_non_null(dt_masks_get_from_id(&fixture->dev, existing->formid));
  group = dt_masks_get_from_id(&fixture->dev, group_id);
  assert_non_null(group);
  assert_int_equal(g_list_length(group->points), 1);
  json_object_unref(geometry);
}
```

- [ ] **Step 4: Add single-item create, update, and delete history tests**

Add and register both tests with `blend_history_test_setup` and
`blend_test_teardown`:

```c
static void test_remote_create_unattached_has_one_history_item(void **state)
{
  blend_fixture_t *fixture = *state;
  const int history_before = fixture->dev.history_end;
  JsonObject *geometry =
    _geom("{\"center\":[0.2,0.2],\"radius\":0.1,\"border\":0.02}");
  dt_remote_error_t *error = NULL;
  dt_mask_id_t id = INVALID_MASKID;

  assert_true(dt_remote_masks_create(&fixture->dev, DT_MASKS_CIRCLE,
                                     geometry, "unattached history",
                                     NULL, &id, &error));
  assert_null(error);
  assert_true(dt_is_valid_maskid(id));
  assert_int_equal(fixture->dev.history_end, history_before + 1);
  json_object_unref(geometry);
}

static void test_remote_update_has_one_history_item(void **state)
{
  blend_fixture_t *fixture = *state;
  JsonObject *initial =
    _geom("{\"center\":[0.4,0.4],\"radius\":0.1,\"border\":0.02}");
  JsonObject *replacement =
    _geom("{\"center\":[0.6,0.6],\"radius\":0.2,\"border\":0.03}");
  dt_remote_error_t *error = NULL;
  dt_mask_id_t id = INVALID_MASKID;
  assert_true(dt_remote_masks_create(&fixture->dev, DT_MASKS_CIRCLE,
                                     initial, "update history",
                                     fixture->module, &id, &error));
  assert_null(error);
  const int history_before = fixture->dev.history_end;
  int affects = -1;

  assert_true(dt_remote_masks_update(&fixture->dev, id, replacement,
                                     NULL, &affects, &error));
  assert_null(error);
  assert_int_equal(affects, 1);
  assert_int_equal(fixture->dev.history_end, history_before + 1);
  json_object_unref(replacement);
  json_object_unref(initial);
}

static void test_remote_delete_unattached_has_one_history_item(void **state)
{
  blend_fixture_t *fixture = *state;
  JsonObject *geometry =
    _geom("{\"center\":[0.7,0.7],\"radius\":0.1,\"border\":0.02}");
  dt_remote_error_t *error = NULL;
  dt_mask_id_t id = INVALID_MASKID;
  assert_true(dt_remote_masks_create(&fixture->dev, DT_MASKS_CIRCLE,
                                     geometry, "delete unattached",
                                     NULL, &id, &error));
  assert_null(error);
  dt_masks_form_t *form =
    dt_masks_get_from_id(&fixture->dev, id);
  assert_non_null(form);
  const int history_before = fixture->dev.history_end;
  JsonArray *removed_from = json_array_new();

  assert_true(dt_remote_masks_delete(&fixture->dev, id,
                                     removed_from, &error));
  assert_null(error);
  assert_int_equal(json_array_get_length(removed_from), 0);
  assert_int_equal(fixture->dev.history_end, history_before + 1);
  assert_int_equal(_list_pointer_count(fixture->dev.allforms, form), 1);
  json_array_unref(removed_from);
  json_object_unref(geometry);
}
```

- [ ] **Step 5: Run the focused suite twice**

```bash
cmake --build build --target test_remote_masks -j2
ctest --test-dir build -R '^test_remote_masks$' --output-on-failure
ctest --test-dir build -R '^test_remote_masks$' --output-on-failure
```

Expected: both runs pass. The second run guards against fixture cleanup
that accidentally relies on process-lifetime leakage.

- [ ] **Step 6: Commit**

```bash
git add src/tests/unittests/control/test_remote_masks.c
git commit -m "tests: cover remote mask history and ownership"
```

---

### Task 7: Record the Legacy Caller Audit and Correct Normative Docs

**Files:**

- Create: `docs/superpowers/specs/2026-07-24-darktable-mask-removal-caller-audit.md`
- Modify: `src/develop/masks.h:660-665`
- Modify: `src/develop/masks/masks.c:1823-1828`
- Modify: `docs/superpowers/specs/2026-07-19-darktable-mcp-drawn-masks-design.md`
- Modify: `docs/superpowers/plans/2026-07-19-darktable-mcp-drawn-masks-tier3.md`
- Modify: `docs/superpowers/specs/2026-07-19-darktable-upstream-divergence-manifest.md`

**Interfaces:**

- Consumes: final core and remote behavior from Tasks 1-6.
- Produces: a stable `MASKS_REMOVE_CALLER_AUDIT` source marker and a
  complete caller inventory.

- [ ] **Step 1: Create the caller-audit document**

Use this table as the complete initial inventory:

```markdown
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
```

Update line anchors to the post-implementation line numbers while keeping
the caller names and conclusions unchanged.

- [ ] **Step 2: Add the source-level audit marker**

Add this comment above both the declaration and definition of the legacy
API:

```c
// MASKS_REMOVE_CALLER_AUDIT: legacy semantics are intentional; review
// docs/superpowers/specs/2026-07-24-darktable-mask-removal-caller-audit.md
// before changing this API or migrating one of its callers.
```

- [ ] **Step 3: Correct the Tier-3 design**

Make these statements explicit:

```markdown
- Legacy `dt_masks_form_remove(NULL, NULL, form)` scans only immediate
  members of module base groups and does not retain unlinked objects.
  Remote full shape deletion therefore uses
  `dt_masks_form_remove_shape_full`, which removes incoming references at
  every nesting depth and retires objects into `dev->allforms`.
- Remote creation uses `dt_masks_gui_form_save_creation_ext`. The first
  forced-new masks snapshot contains the unattached form and unchanged
  module state; the second contains the membership and the
  `ENABLED|MASK` mode addition. Both preserve `module->enabled`.
- Nested `used_by` is flattened to one entry per referencing module.
  State, inversion, and opacity come from the target edge reached first by
  an ordered depth-first traversal from that module's base group: test
  each membership and recurse immediately before advancing.
```

Remove the statements that the legacy helper is already sufficient for
full deletion and that the unextended GUI helper supplies the remote
history contract.

- [ ] **Step 4: Correct the original Task 5 plan**

Add a normative remediation note at the start of Task 5:

```markdown
> **Task 5 remediation (2026-07-24):** The original snippets below assumed
> flat module groups and set `mask_mode` before both creation snapshots.
> Those details are superseded by
> `2026-07-24-darktable-drawn-mask-task5-remediation.md`: use the
> cycle-safe core ownership queries, ownership-aware full deletion,
> transitive H2 guard, and extended creation helper. Binding Amendment 2
> remains transitive.
```

Replace the original direct-loop helper snippets with the final function
names. Replace the “set mask mode before save creation” instruction with
the explicit options block from Task 5 Step 6. Replace the claim that
`dt_masks_form_remove(NULL, NULL, form)` removes nested references with
the new full-delete call.

- [ ] **Step 5: Extend the divergence manifest**

Append these rows:

```markdown
| `src/common/undo.h` / `undo.c` | paired recording guard plus lazy mutex-held isolated groups with two-sided coalescing epochs, public-traversal segment sealing, and thread-owned one-shot suppression | isolated/open-group/cross-thread + public create undo/redo tests |
| `src/develop/develop.h` / `develop.c` | additive `dt_dev_add_new_masks_history_item`, hard-isolated target-bypass records, and explicit in-memory empty-forms history sentinel; persisted empty replay is inferred only for `mask_manager`, while the pre-existing arbitrary non-manager limitation remains out of scope | forced-new + create two-step + empty-delete undo tests |
| `src/develop/masks.h` / `masks.c` | additive cycle-safe graph queries, staged ownership-aware full deletion, explicit creation options, and extended creation helper; legacy removal retained | nested/cycle/shared-owner/full-delete prefix tests + `MASKS_REMOVE_CALLER_AUDIT` |
```

- [ ] **Step 6: Verify documentation and caller coverage**

Run:

```bash
rg -n 'dt_masks_form_remove\(' src --glob '!src/tests/**'
rg -n 'MASKS_REMOVE_CALLER_AUDIT|dt_masks_form_remove_shape_full|dt_masks_gui_form_save_creation_ext' \
  src/develop docs/superpowers
git diff --check
```

Expected: every production legacy caller is represented in the audit,
both source markers resolve to the audit path, corrected docs name the
new APIs, and `git diff --check` is silent.

- [ ] **Step 7: Commit**

```bash
git add src/develop/masks.h src/develop/masks/masks.c \
  docs/superpowers/specs/2026-07-24-darktable-mask-removal-caller-audit.md \
  docs/superpowers/specs/2026-07-19-darktable-mcp-drawn-masks-design.md \
  docs/superpowers/plans/2026-07-19-darktable-mcp-drawn-masks-tier3.md \
  docs/superpowers/specs/2026-07-19-darktable-upstream-divergence-manifest.md
git commit -m "docs: record mask removal migration audit"
```

---

### Task 8: Final Verification and Review Gate

**Files:**

- Verify all files changed by Tasks 1-7.

**Interfaces:**

- Consumes the completed remediation.
- Produces evidence that the branch is ready for a new tech-lead review.

- [ ] **Step 1: Build the focused target from the final source state**

```bash
cmake --build build --target test_remote_masks -j2
```

Expected: target reaches 100% with no compiler or linker errors.

- [ ] **Step 2: Run the focused suite**

```bash
ctest --test-dir build -R '^test_remote_masks$' --output-on-failure
```

Expected: one CTest target passes with every CMocka case green.

- [ ] **Step 3: Run the complete configured CTest suite**

```bash
ctest --test-dir build --output-on-failure
```

Expected: all configured tests pass; the baseline at plan creation is
19/19.

- [ ] **Step 4: Check patch hygiene and worktree scope**

```bash
git diff --check 2b8a915a0d..HEAD
git status --short
git log --oneline 2b8a915a0d..HEAD
```

Expected: `git diff --check` and `git status --short` are silent. The log
contains one focused commit for each implementation task plus the docs
audit commit.

- [ ] **Step 5: Reconcile the caller audit one final time**

```bash
rg -n 'dt_masks_form_remove\(' src --glob '!src/tests/**'
```

Expected: the remote full-delete caller is gone, all remaining production
callers are legacy callers listed in the audit, and the declaration and
definition carry `MASKS_REMOVE_CALLER_AUDIT`.

- [ ] **Step 6: Request a fresh code review**

Review range:

```text
BASE=2b8a915a0d
HEAD=$(git rev-parse HEAD)
```

The reviewer must explicitly check:

- dangling IDs cannot survive successful remote deletion;
- no traversal can loop on a malformed cycle;
- retired forms are owned exactly once;
- disabled modules remain disabled;
- both create undo states are coherent;
- history counts match the Tier-3 contract;
- legacy removal behavior did not change;
- the audit includes every remaining legacy caller.
