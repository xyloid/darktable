/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
/*
 * cmocka unit tests for the Tier-3 drawn-mask surface (M-C).
 *  - Task 1: linkage of the exported dt_masks_group_create_for_module.
 *  - Tasks 3-6 append geometry/validation/CRUD/attach/guard sections.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "common/darktable.h"
#include "control/remote_masks.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/pixelpipe_hb.h"
#include "gui/gtk.h"
#include "views/view.h"

#include <glib.h>
#include <json-glib/json-glib.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DT_TEST_MODULEDIR
#error "DT_TEST_MODULEDIR must be defined by the build (see CMakeLists.txt)"
#endif

static char *s_confdir = NULL;

typedef struct masks_fixture_t
{
  dt_dev_pixelpipe_t *preview_pipe;
  int processed_width;
  int processed_height;
  int iwidth;
  int iheight;
  float iscale;
} masks_fixture_t;

static int harness_group_setup(void **state)
{
  (void)state;
  GError *gerror = NULL;
  s_confdir = g_dir_make_tmp("test_remote_masks-XXXXXX", &gerror);
  if(!s_confdir)
  {
    fprintf(stderr, "test_remote_masks: failed to create scratch config dir: %s\n",
            gerror->message);
    g_error_free(gerror);
    return -1;
  }

  char *argv_override[] = {
    "test_remote_masks",
    "--configdir", s_confdir,
    "--library", ":memory:",
    "--moduledir", DT_TEST_MODULEDIR,
    "--conf", "write_sidecar_files=never",
    NULL
  };
  int argc_override = G_N_ELEMENTS(argv_override) - 1;
  return dt_init(argc_override, argv_override, FALSE, FALSE, NULL) ? -1 : 0;
}

static int harness_group_teardown(void **state)
{
  (void)state;
  dt_cleanup();
  if(s_confdir)
  {
    gchar *cmd = g_strdup_printf("rm -rf '%s'", s_confdir);
    if(system(cmd) != 0)
      fprintf(stderr, "test_remote_masks: failed to remove scratch config dir %s\n",
              s_confdir);
    g_free(cmd);
    g_clear_pointer(&s_confdir, g_free);
  }
  return 0;
}

static int masks_test_setup(void **state)
{
  assert_non_null(darktable.develop);
  assert_non_null(darktable.develop->preview_pipe);

  masks_fixture_t *fixture = g_new0(masks_fixture_t, 1);
  fixture->preview_pipe = darktable.develop->preview_pipe;
  fixture->processed_width = fixture->preview_pipe->processed_width;
  fixture->processed_height = fixture->preview_pipe->processed_height;
  fixture->iwidth = fixture->preview_pipe->iwidth;
  fixture->iheight = fixture->preview_pipe->iheight;
  fixture->iscale = fixture->preview_pipe->iscale;

  fixture->preview_pipe->processed_width = 1000;
  fixture->preview_pipe->processed_height = 1000;
  fixture->preview_pipe->iwidth = 1000;
  fixture->preview_pipe->iheight = 1000;
  fixture->preview_pipe->iscale = 1.0f;

  *state = fixture;
  return 0;
}

static int masks_test_teardown(void **state)
{
  masks_fixture_t *fixture = *state;
  if(!fixture) return 0;

  fixture->preview_pipe->processed_width = fixture->processed_width;
  fixture->preview_pipe->processed_height = fixture->processed_height;
  fixture->preview_pipe->iwidth = fixture->iwidth;
  fixture->preview_pipe->iheight = fixture->iheight;
  fixture->preview_pipe->iscale = fixture->iscale;
  g_free(fixture);
  *state = NULL;
  return 0;
}

static JsonObject *_geom(const char *json)
{
  JsonParser *parser = json_parser_new();
  GError *error = NULL;
  assert_true(json_parser_load_from_data(parser, json, -1, &error));
  assert_null(error);
  JsonObject *object =
    json_object_ref(json_node_get_object(json_parser_get_root(parser)));
  g_object_unref(parser);
  return object;
}

static void _assert_error(dt_remote_error_t **error,
                          dt_remote_error_code_t code,
                          const char *parameter,
                          const char *constraint)
{
  assert_non_null(*error);
  assert_int_equal((*error)->code, code);
  if(parameter || constraint)
  {
    assert_non_null((*error)->details_json);
    JsonParser *parser = json_parser_new();
    GError *parse_error = NULL;
    assert_true(json_parser_load_from_data(parser, (*error)->details_json, -1,
                                           &parse_error));
    assert_null(parse_error);
    JsonObject *details =
      json_node_get_object(json_parser_get_root(parser));
    if(parameter)
      assert_string_equal(json_object_get_string_member(details, "parameter"),
                          parameter);
    if(constraint)
      assert_string_equal(json_object_get_string_member(details, "constraint"),
                          constraint);
    g_object_unref(parser);
  }
  dt_remote_error_free(*error);
  *error = NULL;
}

static void _assert_no_space(JsonObject *object)
{
  assert_false(json_object_has_member(object, "space"));
}

static JsonObject *_node_object(JsonNode *node)
{
  assert_non_null(node);
  assert_int_equal(json_node_get_node_type(node), JSON_NODE_OBJECT);
  return json_node_get_object(node);
}

typedef struct blend_fixture_t
{
  dt_develop_t dev;
  dt_iop_module_t *module;
  dt_develop_t *saved_global_develop;
  dt_gui_gtk_t *saved_global_gui;
  dt_view_manager_t *saved_view_manager;
  dt_gui_gtk_t fake_gui;
  dt_view_manager_t fake_view_manager;
} blend_fixture_t;

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

static blend_fixture_t *blend_fixture_new(const char *op)
{
  blend_fixture_t *fixture = g_new0(blend_fixture_t, 1);
  dt_dev_init(&fixture->dev, TRUE);
  fixture->dev.gui_attached = FALSE;
  assert_non_null(fixture->dev.preview_pipe);

  // A square identity fixture makes preview and raw-normalized geometry
  // deterministic. dt_masks_get_image_size() consults darktable.develop
  // rather than the explicit dev passed to the remote serializer.
  fixture->dev.preview_pipe->processed_width = 1000;
  fixture->dev.preview_pipe->processed_height = 1000;
  fixture->dev.preview_pipe->iwidth = 1000;
  fixture->dev.preview_pipe->iheight = 1000;
  fixture->dev.preview_pipe->iscale = 1.0f;
  fixture->dev.preview_pipe->status = DT_DEV_PIXELPIPE_VALID;
  fixture->dev.image_storage.width = 1000;
  fixture->dev.image_storage.height = 1000;

  fixture->module = _blend_fixture_add_module(fixture, op, 0);
  return fixture;
}

static void blend_fixture_free(blend_fixture_t *fixture)
{
  if(!fixture) return;
  dt_dev_cleanup(&fixture->dev);
  g_free(fixture);
}

static int blend_test_setup(void **state)
{
  blend_fixture_t *fixture = blend_fixture_new("exposure");
  fixture->saved_global_develop = darktable.develop;
  fixture->saved_global_gui = darktable.gui;
  fixture->saved_view_manager = darktable.view_manager;
  darktable.develop = &fixture->dev;
  darktable.gui = &fixture->fake_gui;
  darktable.view_manager = &fixture->fake_view_manager;
  fixture->dev.form_gui = calloc(1, sizeof(dt_masks_form_gui_t));
  assert_non_null(fixture->dev.form_gui);
  dt_masks_init_form_gui(fixture->dev.form_gui);
  *state = fixture;
  return 0;
}

static int blend_test_teardown(void **state)
{
  blend_fixture_t *fixture = *state;
  if(!fixture) return 0;

  // A standalone dt_dev_init() has no darkroom-view GUI state. Tests that
  // exercise the real mask cancellation/removal paths install minimal,
  // zeroed GUI/view-manager shells and the same form_gui allocation that
  // views/darkroom.c owns in production.
  if(fixture->dev.form_gui)
  {
    dt_masks_clear_form_gui(&fixture->dev);
    free(fixture->dev.form_gui);
    fixture->dev.form_gui = NULL;
    fixture->dev.form_visible = NULL;
  }

  // Restore globals before cleaning up the standalone develop context,
  // including when cmocka reaches teardown through a failed assertion.
  darktable.develop = fixture->saved_global_develop;
  darktable.gui = fixture->saved_global_gui;
  darktable.view_manager = fixture->saved_view_manager;
  blend_fixture_free(fixture);
  *state = NULL;
  return 0;
}

static JsonObject *_find_shape(JsonArray *shapes, const dt_mask_id_t id)
{
  for(guint i = 0; i < json_array_get_length(shapes); i++)
  {
    JsonObject *shape = json_array_get_object_element(shapes, i);
    if(json_object_get_int_member(shape, "id") == id) return shape;
  }
  return NULL;
}

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
  assert_false(dt_masks_group_contains_form(&fixture->dev, outer, 7997));
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
  _fixture_add_member(first_root, 7997,
                      DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW, 1.0f);

  assert_int_equal(_list_pointer_count(fixture->dev.iop, fixture->module), 1);
  assert_int_equal(_list_pointer_count(fixture->dev.iop, second), 1);
  GPtrArray *dangling_owners =
    dt_masks_form_get_referencing_modules(&fixture->dev, 7997);
  assert_non_null(dangling_owners);
  assert_int_equal(dangling_owners->len, 0);
  g_ptr_array_unref(dangling_owners);
  GPtrArray *owners =
    dt_masks_form_get_referencing_modules(&fixture->dev, leaf->formid);
  assert_non_null(owners);
  assert_int_equal(owners->len, 2);
  assert_ptr_equal(g_ptr_array_index(owners, 0), fixture->module);
  assert_ptr_equal(g_ptr_array_index(owners, 1), second);
  g_ptr_array_unref(owners);
}

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

// Task 1: the symbol must be linkable (public, non-static). We only assert
// the declaration compiles and the pointer is non-NULL; behavioral tests
// that need a live dev/module arrive in Task 3 with the dt_init harness.
static void test_group_create_symbol_is_public(void **state)
{
  (void)state;
  void (*p)(void) = (void (*)(void))dt_masks_group_create_for_module;
  assert_non_null(p);
}

static void test_type_mapping_and_composite_precedence(void **state)
{
  (void)state;
  assert_int_equal(dt_remote_masks_kind_from_type(DT_MASKS_CIRCLE),
                   DT_REMOTE_SHAPE_CIRCLE);
  assert_int_equal(dt_remote_masks_kind_from_type(DT_MASKS_ELLIPSE),
                   DT_REMOTE_SHAPE_ELLIPSE);
  assert_int_equal(dt_remote_masks_kind_from_type(DT_MASKS_GRADIENT),
                   DT_REMOTE_SHAPE_GRADIENT);
  assert_int_equal(dt_remote_masks_kind_from_type(DT_MASKS_PATH),
                   DT_REMOTE_SHAPE_UNSUPPORTED);

  assert_string_equal(dt_remote_masks_type_string(DT_MASKS_CIRCLE), "circle");
  assert_string_equal(dt_remote_masks_type_string(DT_MASKS_ELLIPSE), "ellipse");
  assert_string_equal(dt_remote_masks_type_string(DT_MASKS_GRADIENT), "gradient");
  assert_string_equal(dt_remote_masks_type_string(DT_MASKS_PATH), "path");
  assert_string_equal(dt_remote_masks_type_string(DT_MASKS_BRUSH), "brush");
  assert_string_equal(dt_remote_masks_type_string(DT_MASKS_GROUP | DT_MASKS_CIRCLE),
                      "group");
  assert_int_equal(dt_remote_masks_kind_from_type(DT_MASKS_GROUP | DT_MASKS_CIRCLE),
                   DT_REMOTE_SHAPE_UNSUPPORTED);
  assert_string_equal(dt_remote_masks_type_string(DT_MASKS_CLONE | DT_MASKS_CIRCLE),
                      "clone");
  assert_int_equal(dt_remote_masks_kind_from_type(DT_MASKS_CLONE | DT_MASKS_CIRCLE),
                   DT_REMOTE_SHAPE_UNSUPPORTED);
  assert_string_equal(
    dt_remote_masks_type_string(DT_MASKS_NON_CLONE | DT_MASKS_ELLIPSE), "clone");
  assert_int_equal(
    dt_remote_masks_kind_from_type(DT_MASKS_NON_CLONE | DT_MASKS_ELLIPSE),
    DT_REMOTE_SHAPE_UNSUPPORTED);
  assert_string_equal(
    dt_remote_masks_type_string(DT_MASKS_GROUP | DT_MASKS_CLONE | DT_MASKS_CIRCLE),
    "group");
#ifdef HAVE_AI
  assert_string_equal(dt_remote_masks_type_string(DT_MASKS_OBJECT), "object");
  assert_int_equal(dt_remote_masks_kind_from_type(DT_MASKS_OBJECT | DT_MASKS_CIRCLE),
                   DT_REMOTE_SHAPE_UNSUPPORTED);
#endif
  assert_string_equal(dt_remote_masks_type_string(DT_MASKS_NONE), "unsupported");

  dt_masks_type_t type = DT_MASKS_NONE;
  assert_true(dt_remote_masks_type_from_string("circle", &type));
  assert_int_equal(type, DT_MASKS_CIRCLE);
  assert_true(dt_remote_masks_type_from_string("ellipse", &type));
  assert_int_equal(type, DT_MASKS_ELLIPSE);
  assert_true(dt_remote_masks_type_from_string("gradient", &type));
  assert_int_equal(type, DT_MASKS_GRADIENT);
  assert_false(dt_remote_masks_type_from_string("path", &type));
  assert_false(dt_remote_masks_type_from_string("nonsense", &type));
  assert_false(dt_remote_masks_type_from_string(NULL, &type));
  assert_false(dt_remote_masks_type_from_string("circle", NULL));
}

static void test_validate_circle_policy(void **state)
{
  (void)state;
  dt_remote_error_t *error = NULL;

  JsonObject *ok =
    _geom("{\"center\":[0.5,0.5],\"radius\":0.1,\"border\":0.03}");
  assert_true(dt_remote_masks_geometry_validate(DT_MASKS_CIRCLE, ok, &error));
  assert_null(error);
  json_object_unref(ok);

  JsonObject *integers =
    _geom("{\"center\":[0,1],\"radius\":1,\"border\":0}");
  assert_true(
    dt_remote_masks_geometry_validate(DT_MASKS_CIRCLE, integers, &error));
  assert_null(error);
  json_object_unref(integers);

  JsonObject *missing = _geom("{\"center\":[0.5,0.5],\"radius\":0.1}");
  assert_false(
    dt_remote_masks_geometry_validate(DT_MASKS_CIRCLE, missing, &error));
  _assert_error(&error, DT_REMOTE_ERR_INVALID_VALUE, "border", "missing_member");
  json_object_unref(missing);

  JsonObject *negative =
    _geom("{\"center\":[0.5,0.5],\"radius\":-0.1,\"border\":0}");
  assert_false(
    dt_remote_masks_geometry_validate(DT_MASKS_CIRCLE, negative, &error));
  _assert_error(&error, DT_REMOTE_ERR_INVALID_VALUE, "radius",
                "must_be_positive");
  json_object_unref(negative);

  JsonObject *off_canvas =
    _geom("{\"center\":[1.500001,0.5],\"radius\":0.1,\"border\":0}");
  assert_false(
    dt_remote_masks_geometry_validate(DT_MASKS_CIRCLE, off_canvas, &error));
  _assert_error(&error, DT_REMOTE_ERR_INVALID_VALUE, "center", "out_of_canvas");
  json_object_unref(off_canvas);

  JsonObject *wrong =
    _geom("{\"center\":[0.5,0.5],\"radius\":\"wide\",\"border\":0}");
  assert_false(dt_remote_masks_geometry_validate(DT_MASKS_CIRCLE, wrong,
                                                  &error));
  _assert_error(&error, DT_REMOTE_ERR_INVALID_VALUE, "radius", "not_a_number");
  json_object_unref(wrong);
}

static void test_validate_ellipse_policy(void **state)
{
  (void)state;
  dt_remote_error_t *error = NULL;

  JsonObject *ok = _geom(
    "{\"center\":[-0.5,1.5],\"radius\":[0.2,0.1],\"rotation\":-30,"
    "\"border\":0,\"border_mode\":\"equidistant\"}");
  assert_true(dt_remote_masks_geometry_validate(DT_MASKS_ELLIPSE, ok, &error));
  assert_null(error);
  json_object_unref(ok);

  JsonObject *bad_radius = _geom(
    "{\"center\":[0.5,0.5],\"radius\":[0.2,0],\"rotation\":0,"
    "\"border\":0.1,\"border_mode\":\"proportional\"}");
  assert_false(dt_remote_masks_geometry_validate(DT_MASKS_ELLIPSE, bad_radius,
                                                  &error));
  _assert_error(&error, DT_REMOTE_ERR_INVALID_VALUE, "radius",
                "must_be_positive");
  json_object_unref(bad_radius);

  JsonObject *bad_mode = _geom(
    "{\"center\":[0.5,0.5],\"radius\":[0.2,0.1],\"rotation\":0,"
    "\"border\":0.1,\"border_mode\":17}");
  assert_false(dt_remote_masks_geometry_validate(DT_MASKS_ELLIPSE, bad_mode,
                                                  &error));
  _assert_error(&error, DT_REMOTE_ERR_INVALID_VALUE, "border_mode",
                "not_a_string");
  json_object_unref(bad_mode);

  JsonObject *unknown_mode = _geom(
    "{\"center\":[0.5,0.5],\"radius\":[0.2,0.1],\"rotation\":0,"
    "\"border\":0.1,\"border_mode\":\"radial\"}");
  assert_false(dt_remote_masks_geometry_validate(DT_MASKS_ELLIPSE,
                                                  unknown_mode, &error));
  _assert_error(&error, DT_REMOTE_ERR_INVALID_VALUE, "border_mode",
                "must_be_equidistant_or_proportional");
  json_object_unref(unknown_mode);
}

static void test_validate_gradient_policy_and_integer_coercion(void **state)
{
  (void)state;
  dt_remote_error_t *error = NULL;

  JsonObject *integers =
    _geom("{\"anchor\":[0,1],\"rotation\":0,\"compression\":0.5,"
          "\"steepness\":0,\"curvature\":0}");
  assert_true(
    dt_remote_masks_geometry_validate(DT_MASKS_GRADIENT, integers, &error));
  assert_null(error);
  json_object_unref(integers);

  JsonObject *curvature =
    _geom("{\"anchor\":[0.5,0.5],\"rotation\":10,\"compression\":0.5,"
          "\"steepness\":0,\"curvature\":3}");
  assert_false(
    dt_remote_masks_geometry_validate(DT_MASKS_GRADIENT, curvature, &error));
  _assert_error(&error, DT_REMOTE_ERR_INVALID_VALUE, "curvature", "abs_gt_2");
  json_object_unref(curvature);

  JsonObject *compression =
    _geom("{\"anchor\":[0.5,0.5],\"rotation\":10,\"compression\":0,"
          "\"steepness\":0,\"curvature\":0}");
  assert_false(
    dt_remote_masks_geometry_validate(DT_MASKS_GRADIENT, compression, &error));
  _assert_error(&error, DT_REMOTE_ERR_INVALID_VALUE, "compression",
                "must_be_positive");
  json_object_unref(compression);
}

static void test_validate_rejects_nonfinite_overflow_unknown_and_unsupported(void **state)
{
  (void)state;
  dt_remote_error_t *error = NULL;

  JsonObject *nonfinite =
    _geom("{\"center\":[0.5,0.5],\"radius\":0.1,\"border\":0.1}");
  json_object_set_double_member(nonfinite, "radius", NAN);
  assert_false(dt_remote_masks_geometry_validate(DT_MASKS_CIRCLE, nonfinite,
                                                  &error));
  _assert_error(&error, DT_REMOTE_ERR_INVALID_VALUE, "radius", "not_finite");
  json_object_unref(nonfinite);

  JsonObject *overflow =
    _geom("{\"anchor\":[0.5,0.5],\"rotation\":0,\"compression\":0.5,"
          "\"steepness\":0,\"curvature\":0}");
  json_object_set_double_member(overflow, "steepness", DBL_MAX);
  assert_false(dt_remote_masks_geometry_validate(DT_MASKS_GRADIENT, overflow,
                                                  &error));
  _assert_error(&error, DT_REMOTE_ERR_INVALID_VALUE, "steepness",
                "not_float_representable");
  json_object_unref(overflow);

  const char *hostile_name = "bad\"member\nname";
  JsonObject *hostile =
    _geom("{\"center\":[0.5,0.5],\"radius\":0.1,\"border\":0.1}");
  json_object_set_int_member(hostile, hostile_name, 1);
  assert_false(dt_remote_masks_geometry_validate(DT_MASKS_CIRCLE, hostile,
                                                  &error));
  _assert_error(&error, DT_REMOTE_ERR_INVALID_VALUE, hostile_name,
                "unknown_member");
  json_object_unref(hostile);

  JsonObject *valid =
    _geom("{\"center\":[0.5,0.5],\"radius\":0.1,\"border\":0.1}");
  assert_false(dt_remote_masks_geometry_validate(DT_MASKS_PATH, valid, &error));
  _assert_error(&error, DT_REMOTE_ERR_UNSUPPORTED_FIELD, NULL, NULL);
  assert_false(dt_remote_masks_geometry_validate(DT_MASKS_CIRCLE, NULL,
                                                  &error));
  _assert_error(&error, DT_REMOTE_ERR_INVALID_VALUE, "geometry",
                "expected_object");
  json_object_unref(valid);
}

static void test_circle_conversion_storage_bounds_and_atomic_output(void **state)
{
  (void)state;
  dt_remote_error_t *error = NULL;
  dt_masks_point_circle_t point = { 0 };

  JsonObject *minimum =
    _geom("{\"center\":[0.5,0.5],\"radius\":0.0005,\"border\":0.0005}");
  assert_true(dt_remote_masks_geometry_to_points(
    darktable.develop, DT_MASKS_CIRCLE, minimum, &point, &error));
  assert_null(error);
  assert_float_equal(point.radius, 0.0005, 1e-7);
  assert_float_equal(point.border, 0.0005, 1e-7);
  json_object_unref(minimum);

  JsonObject *maximum =
    _geom("{\"center\":[0.5,0.5],\"radius\":1,\"border\":1}");
  assert_true(dt_remote_masks_geometry_to_points(
    darktable.develop, DT_MASKS_CIRCLE, maximum, &point, &error));
  assert_null(error);
  assert_float_equal(point.radius, 1.0, 1e-6);
  assert_float_equal(point.border, 1.0, 1e-6);
  json_object_unref(maximum);

  memset(&point, 0x5a, sizeof(point));
  dt_masks_point_circle_t before = point;
  JsonObject *zero_border =
    _geom("{\"center\":[0.5,0.5],\"radius\":0.1,\"border\":0}");
  assert_false(dt_remote_masks_geometry_to_points(
    darktable.develop, DT_MASKS_CIRCLE, zero_border, &point, &error));
  _assert_error(&error, DT_REMOTE_ERR_INVALID_VALUE, "border", "out_of_range");
  assert_memory_equal(&point, &before, sizeof(point));
  json_object_unref(zero_border);

  JsonObject *too_large =
    _geom("{\"center\":[0.5,0.5],\"radius\":1.0001,\"border\":0.1}");
  assert_false(dt_remote_masks_geometry_to_points(
    darktable.develop, DT_MASKS_CIRCLE, too_large, &point, &error));
  _assert_error(&error, DT_REMOTE_ERR_INVALID_VALUE, "radius", "out_of_range");
  assert_memory_equal(&point, &before, sizeof(point));
  json_object_unref(too_large);
}

static void test_transformed_center_may_leave_wire_range(void **state)
{
  masks_fixture_t *fixture = *state;
  fixture->preview_pipe->processed_width = 2000;

  dt_remote_error_t *error = NULL;
  dt_masks_point_circle_t point = { 0 };
  JsonObject *geometry =
    _geom("{\"center\":[1,0.5],\"radius\":0.1,\"border\":0.03}");

  // The wire center is valid. With preview width twice raw width, the
  // transformed raw x coordinate is 2.0; raw points have no wire-space
  // [-0.5, 1.5] policy and remain valid when finite/float-storable.
  assert_true(dt_remote_masks_geometry_to_points(
    darktable.develop, DT_MASKS_CIRCLE, geometry, &point, &error));
  assert_null(error);
  assert_float_equal(point.center[0], 2.0, 1e-4);
  assert_float_equal(point.center[1], 0.5, 1e-4);

  json_object_unref(geometry);
}

static void test_successful_nonstorable_size_is_invalid_value(void **state)
{
  (void)state;
  dt_remote_error_t *error = NULL;
  dt_masks_point_circle_t point;
  memset(&point, 0x6b, sizeof(point));
  const dt_masks_point_circle_t before = point;
  JsonObject *geometry =
    _geom("{\"center\":[0.5,0.5],\"radius\":0.1,\"border\":0.03}");
  json_object_set_double_member(geometry, "radius", FLT_MAX);

  // FLT_MAX is accepted and float-storable on the wire. Scaling the probe
  // arms by the deterministic 1000px fixture overflows the transformed size,
  // while the transform helper itself returns TRUE. This is a storage-range
  // failure, not a transform-helper failure.
  assert_true(
    dt_remote_masks_geometry_validate(DT_MASKS_CIRCLE, geometry, &error));
  assert_null(error);
  assert_false(dt_remote_masks_geometry_to_points(
    darktable.develop, DT_MASKS_CIRCLE, geometry, &point, &error));
  _assert_error(&error, DT_REMOTE_ERR_INVALID_VALUE, "radius", "out_of_range");
  assert_memory_equal(&point, &before, sizeof(point));

  json_object_unref(geometry);
}

static void test_ellipse_conversion_storage_bounds_and_atomic_output(void **state)
{
  (void)state;
  dt_remote_error_t *error = NULL;
  dt_masks_point_ellipse_t point = { 0 };

  JsonObject *valid = _geom(
    "{\"center\":[0.5,0.5],\"radius\":[0.0005,1],\"rotation\":30,"
    "\"border\":0.0005,\"border_mode\":\"equidistant\"}");
  assert_true(dt_remote_masks_geometry_to_points(
    darktable.develop, DT_MASKS_ELLIPSE, valid, &point, &error));
  assert_null(error);
  assert_float_equal(point.radius[0], 0.0005, 1e-7);
  assert_float_equal(point.radius[1], 1.0, 1e-6);
  assert_float_equal(point.border, 0.0005, 1e-7);
  assert_int_equal(point.flags, DT_MASKS_ELLIPSE_EQUIDISTANT);
  json_object_unref(valid);

  memset(&point, 0x3c, sizeof(point));
  dt_masks_point_ellipse_t before = point;
  JsonObject *bad_proportional = _geom(
    "{\"center\":[0.5,0.5],\"radius\":[0.2,0.1],\"rotation\":0,"
    "\"border\":0,\"border_mode\":\"proportional\"}");
  assert_false(dt_remote_masks_geometry_to_points(
    darktable.develop, DT_MASKS_ELLIPSE, bad_proportional, &point, &error));
  _assert_error(&error, DT_REMOTE_ERR_INVALID_VALUE, "border", "out_of_range");
  assert_memory_equal(&point, &before, sizeof(point));
  json_object_unref(bad_proportional);

  JsonObject *bad_equidistant = _geom(
    "{\"center\":[0.5,0.5],\"radius\":[0.2,0.1],\"rotation\":0,"
    "\"border\":1.0001,\"border_mode\":\"equidistant\"}");
  assert_false(dt_remote_masks_geometry_to_points(
    darktable.develop, DT_MASKS_ELLIPSE, bad_equidistant, &point, &error));
  _assert_error(&error, DT_REMOTE_ERR_INVALID_VALUE, "border", "out_of_range");
  assert_memory_equal(&point, &before, sizeof(point));
  json_object_unref(bad_equidistant);
}

static void test_gradient_conversion_uses_exclusive_zero_compression(void **state)
{
  (void)state;
  dt_remote_error_t *error = NULL;
  dt_masks_point_gradient_t point = { 0 };

  JsonObject *below_shape_minimum =
    _geom("{\"anchor\":[0.5,0.5],\"rotation\":10,\"compression\":0.0001,"
          "\"steepness\":-0.75,\"curvature\":2}");
  assert_true(dt_remote_masks_geometry_to_points(
    darktable.develop, DT_MASKS_GRADIENT, below_shape_minimum, &point, &error));
  assert_null(error);
  assert_float_equal(point.compression, 0.0001, 1e-7);
  assert_float_equal(point.steepness, -0.75, 1e-6);
  assert_float_equal(point.curvature, 2.0, 1e-6);
  assert_int_equal(point.state, DT_MASKS_GRADIENT_STATE_SIGMOIDAL);
  json_object_unref(below_shape_minimum);

  memset(&point, 0x27, sizeof(point));
  dt_masks_point_gradient_t before = point;
  JsonObject *above_maximum =
    _geom("{\"anchor\":[0.5,0.5],\"rotation\":0,\"compression\":1.0001,"
          "\"steepness\":0,\"curvature\":0}");
  assert_false(dt_remote_masks_geometry_to_points(
    darktable.develop, DT_MASKS_GRADIENT, above_maximum, &point, &error));
  _assert_error(&error, DT_REMOTE_ERR_INVALID_VALUE, "compression",
                "out_of_range");
  assert_memory_equal(&point, &before, sizeof(point));
  json_object_unref(above_maximum);
}

static void test_circle_roundtrip_identity(void **state)
{
  (void)state;
  dt_remote_error_t *error = NULL;
  JsonObject *geometry =
    _geom("{\"center\":[0.5,0.5],\"radius\":0.1,\"border\":0.03}");
  dt_masks_point_circle_t point = { 0 };
  assert_true(dt_remote_masks_geometry_to_points(
    darktable.develop, DT_MASKS_CIRCLE, geometry, &point, &error));
  assert_null(error);
  json_object_unref(geometry);

  dt_masks_form_t form = { 0 };
  form.type = DT_MASKS_CIRCLE;
  form.points = g_list_append(NULL, &point);
  JsonNode *back =
    dt_remote_masks_points_to_geometry(darktable.develop, &form);
  JsonObject *object = _node_object(back);
  _assert_no_space(object);
  JsonArray *center = json_object_get_array_member(object, "center");
  assert_float_equal(json_array_get_double_element(center, 0), 0.5, 1e-3);
  assert_float_equal(json_array_get_double_element(center, 1), 0.5, 1e-3);
  assert_float_equal(json_object_get_double_member(object, "radius"), 0.1,
                     1e-3);
  assert_float_equal(json_object_get_double_member(object, "border"), 0.03,
                     1e-3);
  assert_string_equal(json_object_get_string_member(object, "size_mapping"),
                      "exact");
  json_node_unref(back);
  g_list_free(form.points);
}

static void test_ellipse_roundtrip_identity(void **state)
{
  (void)state;
  dt_remote_error_t *error = NULL;
  JsonObject *geometry = _geom(
    "{\"center\":[0.4,0.6],\"radius\":[0.2,0.1],\"rotation\":35,"
    "\"border\":0.04,\"border_mode\":\"proportional\"}");
  dt_masks_point_ellipse_t point = { 0 };
  assert_true(dt_remote_masks_geometry_to_points(
    darktable.develop, DT_MASKS_ELLIPSE, geometry, &point, &error));
  assert_null(error);
  json_object_unref(geometry);

  dt_masks_form_t form = { 0 };
  form.type = DT_MASKS_ELLIPSE;
  form.points = g_list_append(NULL, &point);
  JsonNode *back =
    dt_remote_masks_points_to_geometry(darktable.develop, &form);
  JsonObject *object = _node_object(back);
  _assert_no_space(object);
  JsonArray *center = json_object_get_array_member(object, "center");
  JsonArray *radius = json_object_get_array_member(object, "radius");
  assert_float_equal(json_array_get_double_element(center, 0), 0.4, 1e-3);
  assert_float_equal(json_array_get_double_element(center, 1), 0.6, 1e-3);
  assert_float_equal(json_array_get_double_element(radius, 0), 0.2, 1e-3);
  assert_float_equal(json_array_get_double_element(radius, 1), 0.1, 1e-3);
  assert_float_equal(json_object_get_double_member(object, "rotation"), 35,
                     1e-3);
  assert_float_equal(json_object_get_double_member(object, "border"), 0.04,
                     1e-3);
  assert_string_equal(json_object_get_string_member(object, "border_mode"),
                      "proportional");
  assert_string_equal(json_object_get_string_member(object, "size_mapping"),
                      "exact");
  json_node_unref(back);
  g_list_free(form.points);
}

static void test_gradient_roundtrip_identity(void **state)
{
  (void)state;
  dt_remote_error_t *error = NULL;
  JsonObject *geometry =
    _geom("{\"anchor\":[0.3,0.7],\"rotation\":-20,\"compression\":0.4,"
          "\"steepness\":0.25,\"curvature\":-1.5}");
  dt_masks_point_gradient_t point = { 0 };
  assert_true(dt_remote_masks_geometry_to_points(
    darktable.develop, DT_MASKS_GRADIENT, geometry, &point, &error));
  assert_null(error);
  json_object_unref(geometry);

  dt_masks_form_t form = { 0 };
  form.type = DT_MASKS_GRADIENT;
  form.points = g_list_append(NULL, &point);
  JsonNode *back =
    dt_remote_masks_points_to_geometry(darktable.develop, &form);
  JsonObject *object = _node_object(back);
  _assert_no_space(object);
  JsonArray *anchor = json_object_get_array_member(object, "anchor");
  assert_float_equal(json_array_get_double_element(anchor, 0), 0.3, 1e-3);
  assert_float_equal(json_array_get_double_element(anchor, 1), 0.7, 1e-3);
  assert_float_equal(json_object_get_double_member(object, "rotation"), -20,
                     1e-3);
  assert_float_equal(json_object_get_double_member(object, "compression"), 0.4,
                     1e-6);
  assert_float_equal(json_object_get_double_member(object, "steepness"), 0.25,
                     1e-6);
  assert_float_equal(json_object_get_double_member(object, "curvature"), -1.5,
                     1e-6);
  assert_string_equal(json_object_get_string_member(object, "size_mapping"),
                      "exact");
  json_node_unref(back);
  g_list_free(form.points);
}

static void test_raw_geometry_serializes_stored_values_verbatim(void **state)
{
  (void)state;
  dt_masks_point_circle_t circle = {
    .center = { -0.25f, 1.25f }, .radius = 0.2f, .border = 0.04f
  };
  dt_masks_form_t form = { .type = DT_MASKS_CIRCLE };
  form.points = g_list_append(NULL, &circle);
  JsonNode *node = dt_remote_masks_points_to_raw_geometry(&form);
  JsonObject *object = _node_object(node);
  _assert_no_space(object);
  JsonArray *center = json_object_get_array_member(object, "center");
  assert_float_equal(json_array_get_double_element(center, 0), -0.25, 1e-6);
  assert_float_equal(json_array_get_double_element(center, 1), 1.25, 1e-6);
  assert_float_equal(json_object_get_double_member(object, "radius"), 0.2,
                     1e-6);
  assert_false(json_object_has_member(object, "size_mapping"));
  json_node_unref(node);
  g_list_free(form.points);

  dt_masks_point_ellipse_t ellipse = {
    .center = { 0.2f, 0.3f },
    .radius = { 0.4f, 0.15f },
    .rotation = 72.0f,
    .border = 0.07f,
    .flags = DT_MASKS_ELLIPSE_PROPORTIONAL
  };
  form.type = DT_MASKS_ELLIPSE;
  form.points = g_list_append(NULL, &ellipse);
  node = dt_remote_masks_points_to_raw_geometry(&form);
  object = _node_object(node);
  JsonArray *radius = json_object_get_array_member(object, "radius");
  assert_float_equal(json_array_get_double_element(radius, 0), 0.4, 1e-6);
  assert_float_equal(json_array_get_double_element(radius, 1), 0.15, 1e-6);
  assert_float_equal(json_object_get_double_member(object, "rotation"), 72,
                     1e-6);
  assert_string_equal(json_object_get_string_member(object, "border_mode"),
                      "proportional");
  json_node_unref(node);
  g_list_free(form.points);

  dt_masks_point_gradient_t gradient = {
    .anchor = { 0.6f, 0.7f },
    .rotation = -12.0f,
    .compression = 0.3f,
    .steepness = 0.8f,
    .curvature = -0.9f,
    .state = DT_MASKS_GRADIENT_STATE_SIGMOIDAL
  };
  form.type = DT_MASKS_GRADIENT;
  form.points = g_list_append(NULL, &gradient);
  node = dt_remote_masks_points_to_raw_geometry(&form);
  object = _node_object(node);
  JsonArray *anchor = json_object_get_array_member(object, "anchor");
  assert_float_equal(json_array_get_double_element(anchor, 0), 0.6, 1e-6);
  assert_float_equal(json_object_get_double_member(object, "compression"), 0.3,
                     1e-6);
  assert_float_equal(json_object_get_double_member(object, "curvature"), -0.9,
                     1e-6);
  json_node_unref(node);
  g_list_free(form.points);
}

static void test_size_mapping_aggregates_border_probe(void **state)
{
  masks_fixture_t *fixture = *state;
  fixture->preview_pipe->processed_height = 500;

  // A zero radius has no directional spread and is exact. The nonzero border
  // is the only approximate size probe, so this freezes border aggregation.
  dt_masks_point_circle_t point = {
    .center = { 0.5f, 0.5f }, .radius = 0.0f, .border = 0.1f
  };
  dt_masks_form_t form = { .type = DT_MASKS_CIRCLE };
  form.points = g_list_append(NULL, &point);
  JsonNode *node =
    dt_remote_masks_points_to_geometry(darktable.develop, &form);
  JsonObject *object = _node_object(node);
  assert_string_equal(json_object_get_string_member(object, "size_mapping"),
                      "approximate");
  json_node_unref(node);
  g_list_free(form.points);
}

static void test_read_serializer_rejects_invalid_transform_output(void **state)
{
  masks_fixture_t *fixture = *state;
  fixture->preview_pipe->processed_width = 0;

  dt_masks_point_circle_t point = {
    .center = { 0.5f, 0.5f }, .radius = 0.1f, .border = 0.02f
  };
  dt_masks_form_t form = { .type = DT_MASKS_CIRCLE };
  form.points = g_list_append(NULL, &point);
  assert_null(dt_remote_masks_points_to_geometry(darktable.develop, &form));
  g_list_free(form.points);
}

static void test_unsupported_and_empty_serializers(void **state)
{
  (void)state;
  dt_masks_point_circle_t point = { 0 };
  dt_masks_form_t form = { .type = DT_MASKS_PATH };
  form.points = g_list_append(NULL, &point);
  assert_null(dt_remote_masks_points_to_geometry(darktable.develop, &form));
  assert_null(dt_remote_masks_points_to_raw_geometry(&form));
  g_list_free(form.points);
  form.points = NULL;
  assert_null(dt_remote_masks_points_to_geometry(darktable.develop, &form));
  assert_null(dt_remote_masks_points_to_raw_geometry(&form));
}

static void test_state_op_mapping_roundtrips_and_rejects_atomically(void **state)
{
  (void)state;
  static const struct
  {
    const char *name;
    int bit;
  } writable[] = {
    { "union", DT_MASKS_STATE_UNION },
    { "intersection", DT_MASKS_STATE_INTERSECTION },
    { "difference", DT_MASKS_STATE_DIFFERENCE },
    { "exclusion", DT_MASKS_STATE_EXCLUSION },
  };

  for(guint i = 0; i < G_N_ELEMENTS(writable); i++)
  {
    assert_string_equal(
      dt_remote_masks_state_op_string(
        DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW | writable[i].bit),
      writable[i].name);
    int parsed = -1;
    assert_true(
      dt_remote_masks_state_op_from_string(writable[i].name, &parsed));
    assert_int_equal(parsed, writable[i].bit);
  }

  assert_string_equal(
    dt_remote_masks_state_op_string(DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW),
    "union");
  assert_string_equal(
    dt_remote_masks_state_op_string(DT_MASKS_STATE_SUM), "sum");

  int unchanged = 0x13579;
  assert_false(dt_remote_masks_state_op_from_string("sum", &unchanged));
  assert_int_equal(unchanged, 0x13579);
  assert_false(dt_remote_masks_state_op_from_string("bogus", &unchanged));
  assert_int_equal(unchanged, 0x13579);
  assert_false(dt_remote_masks_state_op_from_string(NULL, &unchanged));
  assert_int_equal(unchanged, 0x13579);
  assert_false(dt_remote_masks_state_op_from_string("union", NULL));
}

static void test_list_reports_complete_shapes_and_membership(void **state)
{
  blend_fixture_t *fixture = *state;
  fixture->module->multi_priority = 3;

  dt_masks_form_t *circle = dt_masks_create(DT_MASKS_CIRCLE);
  assert_non_null(circle);
  circle->formid = 101;
  g_strlcpy(circle->name, "attached circle", sizeof(circle->name));
  dt_masks_point_circle_t *circle_point =
    g_malloc0(sizeof(dt_masks_point_circle_t));
  *circle_point = (dt_masks_point_circle_t){
    .center = { 0.5f, 0.5f },
    .radius = 0.1f,
    .border = 0.03f,
  };
  circle->points = g_list_append(circle->points, circle_point);
  fixture->dev.forms = g_list_append(fixture->dev.forms, circle);

  dt_masks_form_t *clone =
    dt_masks_create(DT_MASKS_CLONE | DT_MASKS_CIRCLE);
  assert_non_null(clone);
  clone->formid = 202;
  g_strlcpy(clone->name, "deferred clone", sizeof(clone->name));
  fixture->dev.forms = g_list_append(fixture->dev.forms, clone);

  dt_masks_form_t *brush = dt_masks_create(DT_MASKS_BRUSH);
  assert_non_null(brush);
  brush->formid = 303;
  g_strlcpy(brush->name, "deferred brush", sizeof(brush->name));
  fixture->dev.forms = g_list_append(fixture->dev.forms, brush);

  dt_masks_form_t *group = dt_masks_group_create_for_module(
    &fixture->dev, fixture->module, DT_MASKS_GROUP);
  assert_non_null(group);
  dt_masks_point_group_t *member =
    g_malloc0(sizeof(dt_masks_point_group_t));
  *member = (dt_masks_point_group_t){
    .formid = circle->formid,
    .parentid = group->formid,
    // A bottom member has no combine bit. Listing uses the documented
    // read default "union" while retaining inverse and opacity.
    .state = DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW
             | DT_MASKS_STATE_INVERSE,
    .opacity = 0.85f,
  };
  group->points = g_list_append(group->points, member);
  dt_masks_point_group_t *brush_member =
    g_malloc0(sizeof(dt_masks_point_group_t));
  *brush_member = (dt_masks_point_group_t){
    .formid = brush->formid,
    .parentid = group->formid,
    .state = DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW | DT_MASKS_STATE_SUM,
    .opacity = 0.55f,
  };
  group->points = g_list_append(group->points, brush_member);

  JsonNode *node = dt_remote_masks_list(&fixture->dev);
  JsonObject *root = _node_object(node);
  assert_int_equal(json_object_get_size(root), 1);
  JsonArray *shapes = json_object_get_array_member(root, "shapes");
  assert_non_null(shapes);
  assert_int_equal(json_array_get_length(shapes), 3);

  JsonObject *listed_circle = _find_shape(shapes, circle->formid);
  assert_non_null(listed_circle);
  assert_int_equal(json_object_get_size(listed_circle), 8);
  assert_int_equal(json_object_get_int_member(listed_circle, "id"), 101);
  assert_string_equal(json_object_get_string_member(listed_circle, "type"),
                      "circle");
  assert_string_equal(json_object_get_string_member(listed_circle, "name"),
                      "attached circle");
  assert_string_equal(json_object_get_string_member(listed_circle, "space"),
                      "preview");
  assert_true(json_object_get_boolean_member(listed_circle, "editable"));

  JsonObject *geometry =
    json_object_get_object_member(listed_circle, "geometry");
  assert_non_null(geometry);
  assert_int_equal(json_object_get_size(geometry), 4);
  _assert_no_space(geometry);
  JsonArray *center = json_object_get_array_member(geometry, "center");
  assert_int_equal(json_array_get_length(center), 2);
  assert_float_equal(json_array_get_double_element(center, 0), 0.5, 1e-4);
  assert_float_equal(json_array_get_double_element(center, 1), 0.5, 1e-4);
  assert_float_equal(json_object_get_double_member(geometry, "radius"), 0.1,
                     1e-4);
  assert_float_equal(json_object_get_double_member(geometry, "border"), 0.03,
                     1e-4);
  assert_string_equal(json_object_get_string_member(geometry, "size_mapping"),
                      "exact");

  JsonObject *raw_geometry =
    json_object_get_object_member(listed_circle, "raw_geometry");
  assert_non_null(raw_geometry);
  assert_int_equal(json_object_get_size(raw_geometry), 3);
  _assert_no_space(raw_geometry);
  JsonArray *raw_center =
    json_object_get_array_member(raw_geometry, "center");
  assert_int_equal(json_array_get_length(raw_center), 2);
  assert_float_equal(json_array_get_double_element(raw_center, 0), 0.5, 1e-6);
  assert_float_equal(json_array_get_double_element(raw_center, 1), 0.5, 1e-6);
  assert_float_equal(
    json_object_get_double_member(raw_geometry, "radius"), 0.1, 1e-6);
  assert_float_equal(
    json_object_get_double_member(raw_geometry, "border"), 0.03, 1e-6);

  JsonArray *used_by =
    json_object_get_array_member(listed_circle, "used_by");
  assert_non_null(used_by);
  assert_int_equal(json_array_get_length(used_by), 1);
  JsonObject *usage = json_array_get_object_element(used_by, 0);
  assert_int_equal(json_object_get_size(usage), 5);
  assert_string_equal(json_object_get_string_member(usage, "op"), "exposure");
  assert_int_equal(json_object_get_int_member(usage, "instance"), 3);
  JsonArray *member_state = json_object_get_array_member(usage, "state");
  assert_int_equal(json_array_get_length(member_state), 1);
  assert_string_equal(json_array_get_string_element(member_state, 0),
                      "union");
  assert_true(json_object_get_boolean_member(usage, "inverted"));
  assert_float_equal(json_object_get_double_member(usage, "opacity"), 0.85,
                     1e-4);

  JsonObject *listed_clone = _find_shape(shapes, clone->formid);
  assert_non_null(listed_clone);
  assert_int_equal(json_object_get_size(listed_clone), 6);
  assert_int_equal(json_object_get_int_member(listed_clone, "id"), 202);
  assert_string_equal(json_object_get_string_member(listed_clone, "type"),
                      "clone");
  assert_string_equal(json_object_get_string_member(listed_clone, "name"),
                      "deferred clone");
  assert_string_equal(json_object_get_string_member(listed_clone, "space"),
                      "preview");
  assert_false(json_object_get_boolean_member(listed_clone, "editable"));
  assert_false(json_object_has_member(listed_clone, "geometry"));
  assert_false(json_object_has_member(listed_clone, "raw_geometry"));
  JsonArray *clone_used_by =
    json_object_get_array_member(listed_clone, "used_by");
  assert_non_null(clone_used_by);
  assert_int_equal(json_array_get_length(clone_used_by), 0);

  JsonObject *listed_brush = _find_shape(shapes, brush->formid);
  assert_non_null(listed_brush);
  assert_int_equal(json_object_get_size(listed_brush), 6);
  assert_int_equal(json_object_get_int_member(listed_brush, "id"), 303);
  assert_string_equal(json_object_get_string_member(listed_brush, "type"),
                      "brush");
  assert_string_equal(json_object_get_string_member(listed_brush, "name"),
                      "deferred brush");
  assert_string_equal(json_object_get_string_member(listed_brush, "space"),
                      "preview");
  assert_false(json_object_get_boolean_member(listed_brush, "editable"));
  assert_false(json_object_has_member(listed_brush, "geometry"));
  assert_false(json_object_has_member(listed_brush, "raw_geometry"));
  JsonArray *brush_used_by =
    json_object_get_array_member(listed_brush, "used_by");
  assert_non_null(brush_used_by);
  assert_int_equal(json_array_get_length(brush_used_by), 1);
  JsonObject *brush_usage =
    json_array_get_object_element(brush_used_by, 0);
  assert_int_equal(json_object_get_size(brush_usage), 5);
  assert_string_equal(json_object_get_string_member(brush_usage, "op"),
                      "exposure");
  assert_int_equal(json_object_get_int_member(brush_usage, "instance"), 3);
  JsonArray *brush_state =
    json_object_get_array_member(brush_usage, "state");
  assert_int_equal(json_array_get_length(brush_state), 1);
  assert_string_equal(json_array_get_string_element(brush_state, 0), "sum");
  assert_false(json_object_get_boolean_member(brush_usage, "inverted"));
  assert_float_equal(
    json_object_get_double_member(brush_usage, "opacity"), 0.55, 1e-4);

  assert_null(_find_shape(shapes, group->formid));
  json_node_unref(node);
}

static void test_list_null_develop_contract(void **state)
{
  (void)state;
  assert_null(dt_remote_masks_list(NULL));
}

static void test_guard_cancels_direct_target(void **state)
{
  blend_fixture_t *fixture = *state;
  assert_non_null(fixture->dev.form_gui);

  dt_masks_form_t *form = dt_masks_create(DT_MASKS_CIRCLE);
  assert_non_null(form);
  form->formid = 202;
  fixture->dev.form_visible = form;
  fixture->dev.form_gui->formid = form->formid;

  dt_remote_masks_cancel_gui_edit_if_targeting(&fixture->dev, form);

  assert_null(fixture->dev.form_visible);
  assert_int_equal(fixture->dev.form_gui->formid, 0);
  dt_masks_free_form(form);
}

static void test_guard_cancels_group_targeting_member(void **state)
{
  blend_fixture_t *fixture = *state;
  assert_non_null(fixture->dev.form_gui);

  dt_masks_form_t *form = dt_masks_create(DT_MASKS_CIRCLE);
  dt_masks_form_t *group = dt_masks_create(DT_MASKS_GROUP);
  assert_non_null(form);
  assert_non_null(group);
  form->formid = 303;
  group->formid = 404;
  dt_masks_point_group_t *member =
    g_malloc0(sizeof(dt_masks_point_group_t));
  member->formid = form->formid;
  member->parentid = group->formid;
  group->points = g_list_append(group->points, member);
  fixture->dev.form_visible = group;
  fixture->dev.form_gui->formid = group->formid;

  dt_remote_masks_cancel_gui_edit_if_targeting(&fixture->dev, form);

  assert_null(fixture->dev.form_visible);
  assert_int_equal(fixture->dev.form_gui->formid, 0);
  dt_masks_free_form(group);
  dt_masks_free_form(form);
}

static void test_guard_leaves_unrelated_edit_untouched(void **state)
{
  blend_fixture_t *fixture = *state;
  assert_non_null(fixture->dev.form_gui);

  dt_masks_form_t *visible = dt_masks_create(DT_MASKS_CIRCLE);
  dt_masks_form_t *other = dt_masks_create(DT_MASKS_ELLIPSE);
  assert_non_null(visible);
  assert_non_null(other);
  visible->formid = 505;
  other->formid = 606;
  fixture->dev.form_visible = visible;
  fixture->dev.form_gui->formid = visible->formid;

  dt_remote_masks_cancel_gui_edit_if_targeting(&fixture->dev, other);

  assert_ptr_equal(fixture->dev.form_visible, visible);
  assert_int_equal(fixture->dev.form_gui->formid, visible->formid);
  fixture->dev.form_visible = NULL;
  fixture->dev.form_gui->formid = 0;
  dt_masks_free_form(visible);
  dt_masks_free_form(other);
}

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

static void test_create_unattached_preserves_requested_unique_name(void **state)
{
  blend_fixture_t *fixture = *state;
  JsonObject *geometry =
    _geom("{\"center\":[0.25,0.75],\"radius\":0.1,\"border\":0.02}");
  dt_remote_error_t *error = NULL;
  dt_mask_id_t first_id = INVALID_MASKID;
  dt_mask_id_t second_id = INVALID_MASKID;

  assert_true(dt_remote_masks_create(&fixture->dev, DT_MASKS_CIRCLE,
                                     geometry, "subject", NULL,
                                     &first_id, &error));
  assert_null(error);
  assert_true(dt_is_valid_maskid(first_id));
  dt_masks_form_t *first =
    dt_masks_get_from_id(&fixture->dev, first_id);
  assert_non_null(first);
  assert_string_equal(first->name, "subject");
  assert_int_equal(first->type, DT_MASKS_CIRCLE);
  assert_int_equal(g_list_length(first->points), 1);
  const dt_masks_point_circle_t *point = first->points->data;
  assert_float_equal(point->center[0], 0.25, 1e-6);
  assert_float_equal(point->center[1], 0.75, 1e-6);
  assert_float_equal(point->radius, 0.1, 1e-6);
  assert_float_equal(point->border, 0.02, 1e-6);

  assert_true(dt_remote_masks_create(&fixture->dev, DT_MASKS_CIRCLE,
                                     geometry, "subject", NULL,
                                     &second_id, &error));
  assert_null(error);
  assert_true(dt_is_valid_maskid(second_id));
  assert_int_not_equal(second_id, first_id);
  dt_masks_form_t *second =
    dt_masks_get_from_id(&fixture->dev, second_id);
  assert_non_null(second);
  assert_string_not_equal(second->name, first->name);
  assert_true(g_str_has_prefix(second->name, "subject"));
  assert_int_equal(fixture->module->blend_params->mask_id, NO_MASKID);

  json_object_unref(geometry);
}

static void test_duplicate_requested_name_keeps_valid_utf8(void **state)
{
  blend_fixture_t *fixture = *state;
  JsonObject *geometry =
    _geom("{\"center\":[0.5,0.5],\"radius\":0.1,\"border\":0.02}");
  char name[128];
  memset(name, 'a', 123);
  name[123] = (char)0xf0;
  name[124] = (char)0x9f;
  name[125] = (char)0x98;
  name[126] = (char)0x80;
  name[127] = '\0';
  assert_true(g_utf8_validate(name, -1, NULL));

  dt_remote_error_t *error = NULL;
  dt_mask_id_t first_id = INVALID_MASKID;
  dt_mask_id_t second_id = INVALID_MASKID;
  assert_true(dt_remote_masks_create(&fixture->dev, DT_MASKS_CIRCLE,
                                     geometry, name, NULL,
                                     &first_id, &error));
  assert_null(error);
  assert_true(dt_remote_masks_create(&fixture->dev, DT_MASKS_CIRCLE,
                                     geometry, name, NULL,
                                     &second_id, &error));
  assert_null(error);
  assert_int_not_equal(first_id, second_id);

  const dt_masks_form_t *second =
    dt_masks_get_from_id(&fixture->dev, second_id);
  assert_non_null(second);
  assert_true(g_utf8_validate(second->name, -1, NULL));

  json_object_unref(geometry);
}

static void test_create_attached_sets_group_and_mask_mode(void **state)
{
  blend_fixture_t *fixture = *state;
  fixture->module->blend_params->mask_mode =
    DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL;
  JsonObject *geometry =
    _geom("{\"center\":[0.5,0.5],\"radius\":0.2,\"border\":0.03}");
  dt_remote_error_t *error = NULL;
  dt_mask_id_t id = INVALID_MASKID;

  assert_true(dt_remote_masks_create(&fixture->dev, DT_MASKS_CIRCLE,
                                     geometry, NULL, fixture->module,
                                     &id, &error));
  assert_null(error);
  assert_true(dt_is_valid_maskid(id));
  assert_true(fixture->module->blend_params->mask_mode
              & DEVELOP_MASK_ENABLED);
  assert_true(fixture->module->blend_params->mask_mode
              & DEVELOP_MASK_MASK);
  assert_true(fixture->module->blend_params->mask_mode
              & DEVELOP_MASK_CONDITIONAL);

  dt_masks_form_t *group =
    dt_masks_get_from_id(&fixture->dev,
                         fixture->module->blend_params->mask_id);
  assert_non_null(group);
  assert_true(group->type & DT_MASKS_GROUP);
  assert_int_equal(g_list_length(group->points), 1);
  const dt_masks_point_group_t *member = group->points->data;
  assert_int_equal(member->formid, id);
  assert_int_equal(member->parentid, group->formid);
  assert_true(member->state & DT_MASKS_STATE_USE);
  assert_true(member->state & DT_MASKS_STATE_SHOW);
  assert_int_equal(member->state & DT_MASKS_STATE_OP, 0);

  json_object_unref(geometry);
}

static void test_create_rejects_overlong_name_without_mutation(void **state)
{
  blend_fixture_t *fixture = *state;
  JsonObject *geometry =
    _geom("{\"center\":[0.5,0.5],\"radius\":0.1,\"border\":0.02}");
  char name[129];
  memset(name, 'x', sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  const guint forms_before = g_list_length(fixture->dev.forms);
  dt_remote_error_t *error = NULL;
  dt_mask_id_t id = 777;

  assert_false(dt_remote_masks_create(&fixture->dev, DT_MASKS_CIRCLE,
                                      geometry, name, NULL, &id, &error));
  _assert_error(&error, DT_REMOTE_ERR_INVALID_VALUE, "name",
                "length_lt_128");
  assert_int_equal(id, 777);
  assert_int_equal(g_list_length(fixture->dev.forms), forms_before);

  json_object_unref(geometry);
}

static void test_create_rejects_unsupported_attach_target(void **state)
{
  blend_fixture_t *fixture = *state;
  JsonObject *geometry =
    _geom("{\"center\":[0.5,0.5],\"radius\":0.1,\"border\":0.02}");
  dt_iop_module_t unsupported = { 0 };
  dt_remote_error_t *error = NULL;
  const guint forms_before = g_list_length(fixture->dev.forms);

  assert_false(dt_remote_masks_create(&fixture->dev, DT_MASKS_CIRCLE,
                                      geometry, NULL, &unsupported,
                                      NULL, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_UNSUPPORTED_FIELD);
  dt_remote_error_free(error);
  assert_int_equal(g_list_length(fixture->dev.forms), forms_before);

  json_object_unref(geometry);
}

static void test_create_rejects_raster_attach_without_mutation(void **state)
{
  blend_fixture_t *fixture = *state;
  fixture->module->blend_params->mask_mode = DEVELOP_MASK_RASTER;
  JsonObject *geometry =
    _geom("{\"center\":[0.5,0.5],\"radius\":0.1,\"border\":0.02}");
  dt_remote_error_t *error = NULL;
  const guint forms_before = g_list_length(fixture->dev.forms);

  assert_false(dt_remote_masks_create(&fixture->dev, DT_MASKS_CIRCLE,
                                      geometry, NULL, fixture->module,
                                      NULL, &error));
  _assert_error(&error, DT_REMOTE_ERR_INVALID_VALUE, "op",
                "raster_unsupported");
  assert_int_equal(g_list_length(fixture->dev.forms), forms_before);
  assert_int_equal(fixture->module->blend_params->mask_mode,
                   DEVELOP_MASK_RASTER);

  json_object_unref(geometry);
}

static void test_update_replaces_atomically_and_reports_users(void **state)
{
  blend_fixture_t *fixture = *state;
  JsonObject *initial =
    _geom("{\"center\":[0.5,0.5],\"radius\":0.1,\"border\":0.02}");
  JsonObject *replacement =
    _geom("{\"center\":[0.3,0.7],\"radius\":0.2,\"border\":0.04}");
  dt_remote_error_t *error = NULL;
  dt_mask_id_t id = INVALID_MASKID;
  assert_true(dt_remote_masks_create(&fixture->dev, DT_MASKS_CIRCLE,
                                     initial, "before", fixture->module,
                                     &id, &error));
  assert_null(error);

  int affects = -1;
  assert_true(dt_remote_masks_update(&fixture->dev, id, replacement,
                                     "after", &affects, &error));
  assert_null(error);
  assert_int_equal(affects, 1);
  dt_masks_form_t *form = dt_masks_get_from_id(&fixture->dev, id);
  assert_non_null(form);
  assert_string_equal(form->name, "after");
  assert_int_equal(g_list_length(form->points), 1);
  const dt_masks_point_circle_t *point = form->points->data;
  assert_float_equal(point->center[0], 0.3, 1e-6);
  assert_float_equal(point->center[1], 0.7, 1e-6);
  assert_float_equal(point->radius, 0.2, 1e-6);
  assert_float_equal(point->border, 0.04, 1e-6);

  dt_masks_point_circle_t before_invalid = *point;
  JsonObject *invalid =
    _geom("{\"center\":[0.1,0.2],\"radius\":0,\"border\":0.03}");
  affects = 999;
  assert_false(dt_remote_masks_update(&fixture->dev, id, invalid,
                                      "must not apply", &affects, &error));
  _assert_error(&error, DT_REMOTE_ERR_INVALID_VALUE, "radius",
                "must_be_positive");
  point = form->points->data;
  assert_memory_equal(point, &before_invalid, sizeof(before_invalid));
  assert_string_equal(form->name, "after");
  assert_int_equal(affects, 999);

  json_object_unref(invalid);
  json_object_unref(replacement);
  json_object_unref(initial);
}

static void test_update_unknown_id_is_not_found(void **state)
{
  blend_fixture_t *fixture = *state;
  JsonObject *geometry =
    _geom("{\"center\":[0.5,0.5],\"radius\":0.1,\"border\":0.02}");
  dt_remote_error_t *error = NULL;
  int affects = -1;

  assert_false(dt_remote_masks_update(&fixture->dev, 99999, geometry,
                                      NULL, &affects, &error));
  _assert_error(&error, DT_REMOTE_ERR_NOT_FOUND, "id", NULL);
  assert_int_equal(affects, -1);

  json_object_unref(geometry);
}

static void test_delete_removes_membership_and_clears_only_drawn_bit(void **state)
{
  blend_fixture_t *fixture = *state;
  fixture->module->multi_priority = 4;
  fixture->module->blend_params->mask_mode =
    DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL;
  JsonObject *geometry =
    _geom("{\"center\":[0.5,0.5],\"radius\":0.1,\"border\":0.02}");
  dt_remote_error_t *error = NULL;
  dt_mask_id_t id = INVALID_MASKID;
  assert_true(dt_remote_masks_create(&fixture->dev, DT_MASKS_CIRCLE,
                                     geometry, NULL, fixture->module,
                                     &id, &error));
  assert_null(error);
  dt_masks_form_t *form = dt_masks_get_from_id(&fixture->dev, id);
  dt_masks_form_t *group =
    dt_masks_get_from_id(&fixture->dev,
                         fixture->module->blend_params->mask_id);
  assert_non_null(form);
  assert_non_null(group);

  JsonArray *removed_from = json_array_new();
  // dt_masks_form_remove() invokes the normal GUI history helper when it
  // removes the now-empty module group. In this headless fixture, mark the
  // call as a GUI update so that helper skips tag/thumbtable side effects;
  // the masks-history path under test still runs.
  DT_ENTER_GUI_UPDATE();
  const gboolean deleted =
    dt_remote_masks_delete(&fixture->dev, id, removed_from, &error);
  DT_LEAVE_GUI_UPDATE();
  assert_true(deleted);
  assert_null(error);
  assert_null(dt_masks_get_from_id(&fixture->dev, id));
  assert_false(dt_is_valid_maskid(
    fixture->module->blend_params->mask_id));
  assert_false(fixture->module->blend_params->mask_mode
               & DEVELOP_MASK_MASK);
  assert_true(fixture->module->blend_params->mask_mode
              & DEVELOP_MASK_ENABLED);
  assert_true(fixture->module->blend_params->mask_mode
              & DEVELOP_MASK_CONDITIONAL);
  assert_int_equal(json_array_get_length(removed_from), 1);
  JsonObject *removed = json_array_get_object_element(removed_from, 0);
  assert_string_equal(json_object_get_string_member(removed, "op"),
                      "exposure");
  assert_int_equal(json_object_get_int_member(removed, "instance"), 4);

  json_array_unref(removed_from);
  json_object_unref(geometry);
  dt_masks_free_form(group);
  dt_masks_free_form(form);
}

static void test_delete_unknown_id_is_not_found(void **state)
{
  blend_fixture_t *fixture = *state;
  JsonArray *removed_from = json_array_new();
  dt_remote_error_t *error = NULL;

  assert_false(dt_remote_masks_delete(&fixture->dev, 99999, removed_from,
                                      &error));
  _assert_error(&error, DT_REMOTE_ERR_NOT_FOUND, "id", NULL);
  assert_int_equal(json_array_get_length(removed_from), 0);

  json_array_unref(removed_from);
}

static void test_delete_rejects_engine_managed_group_id(void **state)
{
  blend_fixture_t *fixture = *state;
  JsonObject *geometry =
    _geom("{\"center\":[0.5,0.5],\"radius\":0.1,\"border\":0.02}");
  dt_remote_error_t *error = NULL;
  dt_mask_id_t shape_id = INVALID_MASKID;
  assert_true(dt_remote_masks_create(&fixture->dev, DT_MASKS_CIRCLE,
                                     geometry, NULL, fixture->module,
                                     &shape_id, &error));
  assert_null(error);
  const dt_mask_id_t group_id =
    fixture->module->blend_params->mask_id;
  assert_true(dt_is_valid_maskid(group_id));
  const uint32_t mode_before =
    fixture->module->blend_params->mask_mode;

  JsonArray *removed_from = json_array_new();
  DT_ENTER_GUI_UPDATE();
  const gboolean deleted =
    dt_remote_masks_delete(&fixture->dev, group_id, removed_from, &error);
  DT_LEAVE_GUI_UPDATE();

  assert_false(deleted);
  _assert_error(&error, DT_REMOTE_ERR_UNSUPPORTED_FIELD, "type",
                "group_internal");
  assert_non_null(dt_masks_get_from_id(&fixture->dev, shape_id));
  assert_non_null(dt_masks_get_from_id(&fixture->dev, group_id));
  assert_int_equal(fixture->module->blend_params->mask_id, group_id);
  assert_int_equal(fixture->module->blend_params->mask_mode, mode_before);
  assert_int_equal(json_array_get_length(removed_from), 0);

  json_array_unref(removed_from);
  json_object_unref(geometry);
}

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

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_group_create_symbol_is_public),
    cmocka_unit_test_setup_teardown(
      test_core_group_contains_form_is_transitive_and_cycle_safe,
      blend_test_setup, blend_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_core_referencing_modules_flattens_nested_and_shared_paths,
      blend_test_setup, blend_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_core_full_delete_keeps_nested_sibling_and_retires_leaf,
      blend_test_setup, blend_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_core_full_delete_prunes_shared_empty_ancestors,
      blend_test_setup, blend_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_core_full_delete_rejects_group_without_mutation,
      blend_test_setup, blend_test_teardown),
    cmocka_unit_test(test_type_mapping_and_composite_precedence),
    cmocka_unit_test(test_validate_circle_policy),
    cmocka_unit_test(test_validate_ellipse_policy),
    cmocka_unit_test(test_validate_gradient_policy_and_integer_coercion),
    cmocka_unit_test(
      test_validate_rejects_nonfinite_overflow_unknown_and_unsupported),
    cmocka_unit_test_setup_teardown(
      test_circle_conversion_storage_bounds_and_atomic_output,
      masks_test_setup, masks_test_teardown),
    cmocka_unit_test_setup_teardown(test_transformed_center_may_leave_wire_range,
                                    masks_test_setup, masks_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_successful_nonstorable_size_is_invalid_value,
      masks_test_setup, masks_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_ellipse_conversion_storage_bounds_and_atomic_output,
      masks_test_setup, masks_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_gradient_conversion_uses_exclusive_zero_compression,
      masks_test_setup, masks_test_teardown),
    cmocka_unit_test_setup_teardown(test_circle_roundtrip_identity,
                                    masks_test_setup, masks_test_teardown),
    cmocka_unit_test_setup_teardown(test_ellipse_roundtrip_identity,
                                    masks_test_setup, masks_test_teardown),
    cmocka_unit_test_setup_teardown(test_gradient_roundtrip_identity,
                                    masks_test_setup, masks_test_teardown),
    cmocka_unit_test(test_raw_geometry_serializes_stored_values_verbatim),
    cmocka_unit_test_setup_teardown(test_size_mapping_aggregates_border_probe,
                                    masks_test_setup, masks_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_read_serializer_rejects_invalid_transform_output,
      masks_test_setup, masks_test_teardown),
    cmocka_unit_test_setup_teardown(test_unsupported_and_empty_serializers,
                                    masks_test_setup, masks_test_teardown),
    cmocka_unit_test(
      test_state_op_mapping_roundtrips_and_rejects_atomically),
    cmocka_unit_test_setup_teardown(
      test_list_reports_complete_shapes_and_membership,
      blend_test_setup, blend_test_teardown),
    cmocka_unit_test(test_list_null_develop_contract),
    cmocka_unit_test_setup_teardown(
      test_guard_cancels_direct_target,
      blend_test_setup, blend_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_guard_cancels_group_targeting_member,
      blend_test_setup, blend_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_guard_leaves_unrelated_edit_untouched,
      blend_test_setup, blend_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_creation_ext_preserves_disabled_and_orders_snapshots,
      blend_test_setup, blend_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_creation_wrapper_keeps_gui_default_naming,
      blend_test_setup, blend_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_create_unattached_preserves_requested_unique_name,
      blend_test_setup, blend_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_duplicate_requested_name_keeps_valid_utf8,
      blend_test_setup, blend_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_create_attached_sets_group_and_mask_mode,
      blend_test_setup, blend_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_create_rejects_overlong_name_without_mutation,
      blend_test_setup, blend_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_create_rejects_unsupported_attach_target,
      blend_test_setup, blend_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_create_rejects_raster_attach_without_mutation,
      blend_test_setup, blend_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_update_replaces_atomically_and_reports_users,
      blend_test_setup, blend_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_update_unknown_id_is_not_found,
      blend_test_setup, blend_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_delete_removes_membership_and_clears_only_drawn_bit,
      blend_test_setup, blend_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_delete_unknown_id_is_not_found,
      blend_test_setup, blend_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_delete_rejects_engine_managed_group_id,
      blend_test_setup, blend_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_new_mask_history_forces_distinct_snapshots,
      blend_test_setup, blend_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_new_mask_history_resolves_global_mask_manager,
      blend_test_setup, blend_test_teardown),
  };
  return cmocka_run_group_tests(tests, harness_group_setup,
                                harness_group_teardown);
}
