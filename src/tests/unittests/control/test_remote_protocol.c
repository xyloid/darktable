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
 * cmocka unit tests for the remote-edit JSON dispatcher
 * (src/control/remote_protocol.c).
 *
 * Fixed request/response pairs live in fixtures/, authored verbatim from
 * docs/superpowers/specs/2026-07-05-darktable-mcp-protocol-reference.md;
 * they are also meant to be reused by the Python sidecar's client tests
 * (plan step 5). Malformed-input edge cases that are not really "protocol
 * shape" (an overlong string, a non-finite number, ...) are built inline
 * with JsonBuilder instead of as fixture files.
 *
 * These tests never touch darktable.develop: the four read-only
 * remote_edit calls the handlers make are replaced with canned stubs via
 * dt_remote_protocol_set_calls() (the test seam remote_protocol.c
 * exposes for exactly this reason). No socket, no GTK, no running
 * darktable -- fixtures go straight into dt_remote_protocol_dispatch().
 *
 * Please see README.md for more detailed documentation.
 */
#include <float.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <cmocka.h>
#include <json-glib/json-glib.h>

#include "../util/assert.h"

#include "common/darktable.h"
#include "control/remote_edit.h"
#include "control/remote_parameters.h"
#include "control/remote_protocol.h"
#include "control/remote_server.h"  // session/pending definitions + the real
                                    // async lifecycle, for the finish_preview
                                    // completion tests (fake session, no sockets)

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

#ifndef TEST_FIXTURE_DIR
#error "TEST_FIXTURE_DIR must be defined by CMakeLists.txt"
#endif

/* ---------------------------------------------------------------------- */
/* fixture loading + structural JSON comparison                            */
/* ---------------------------------------------------------------------- */

static JsonNode *_load_fixture(const char *filename)
{
  gchar *path = g_build_filename(TEST_FIXTURE_DIR, filename, NULL);
  JsonParser *parser = json_parser_new();
  GError *gerror = NULL;
  if(!json_parser_load_from_file(parser, path, &gerror))
  {
    fprintf(stderr, "failed to load fixture '%s': %s\n", path, gerror ? gerror->message : "?");
    g_clear_error(&gerror);
    fail_msg("could not load fixture %s", path);
  }
  // json_parser_get_root() is owned by the parser; copy it out so the
  // caller can drop the parser without invalidating the node.
  JsonNode *node = json_node_copy(json_parser_get_root(parser));
  g_object_unref(parser);
  g_free(path);
  return node;
}

static gboolean _json_equal(JsonNode *a, JsonNode *b);

static gboolean _json_array_equal(JsonArray *a, JsonArray *b)
{
  guint na = json_array_get_length(a);
  guint nb = json_array_get_length(b);
  if(na != nb) return FALSE;
  for(guint i = 0; i < na; i++)
    if(!_json_equal(json_array_get_element(a, i), json_array_get_element(b, i))) return FALSE;
  return TRUE;
}

static gboolean _json_object_equal(JsonObject *a, JsonObject *b)
{
  if(json_object_get_size(a) != json_object_get_size(b)) return FALSE;

  GList *keys = json_object_get_members(a);
  gboolean ok = TRUE;
  for(GList *l = keys; l && ok; l = l->next)
  {
    const char *key = l->data;
    if(!json_object_has_member(b, key)) { ok = FALSE; break; }
    if(!_json_equal(json_object_get_member(a, key), json_object_get_member(b, key))) { ok = FALSE; break; }
  }
  g_list_free(keys);
  return ok;
}

// Structural equality, not textual: object member order and int-vs-double
// literal spelling (800 vs 800.0) are not meaningful wire differences.
static gboolean _json_equal(JsonNode *a, JsonNode *b)
{
  if(!a || !b) return a == b;

  JsonNodeType ta = json_node_get_node_type(a);
  JsonNodeType tb = json_node_get_node_type(b);
  if(ta != tb) return FALSE;

  switch(ta)
  {
    case JSON_NODE_NULL: return TRUE;
    case JSON_NODE_ARRAY: return _json_array_equal(json_node_get_array(a), json_node_get_array(b));
    case JSON_NODE_OBJECT: return _json_object_equal(json_node_get_object(a), json_node_get_object(b));
    case JSON_NODE_VALUE:
    {
      GType gta = json_node_get_value_type(a);
      GType gtb = json_node_get_value_type(b);
      gboolean a_num = (gta == G_TYPE_INT64 || gta == G_TYPE_DOUBLE);
      gboolean b_num = (gtb == G_TYPE_INT64 || gtb == G_TYPE_DOUBLE);
      // dt_remote_state_t's exif fields are C `float`, not `double` (see
      // remote_edit.h) -- comparing their double-promoted values needs
      // float32-scale tolerance, not exact/near-exact double equality.
      if(a_num && b_num) return fabs(json_node_get_double(a) - json_node_get_double(b)) < 1e-4;
      if(gta != gtb) return FALSE;
      if(gta == G_TYPE_STRING) return g_strcmp0(json_node_get_string(a), json_node_get_string(b)) == 0;
      if(gta == G_TYPE_BOOLEAN) return json_node_get_boolean(a) == json_node_get_boolean(b);
      return FALSE;
    }
    default: return FALSE;
  }
}

// Runs one request fixture through the dispatcher and asserts the result
// is structurally identical to the matching response fixture.
static void _assert_dispatch_matches(const char *request_file, const char *response_file)
{
  JsonNode *request_node = _load_fixture(request_file);
  assert_true(JSON_NODE_HOLDS_OBJECT(request_node));
  JsonObject *request = json_node_get_object(request_node);

  JsonNode *expected = _load_fixture(response_file);

  JsonNode *actual = dt_remote_protocol_dispatch(request, NULL);
  assert_non_null(actual);

  if(!_json_equal(actual, expected))
  {
    JsonGenerator *gen = json_generator_new();
    json_generator_set_pretty(gen, TRUE);
    json_generator_set_root(gen, actual);
    gchar *actual_str = json_generator_to_data(gen, NULL);
    json_generator_set_root(gen, expected);
    gchar *expected_str = json_generator_to_data(gen, NULL);
    fprintf(stderr, "expected:\n%s\nactual:\n%s\n", expected_str, actual_str);
    g_free(actual_str);
    g_free(expected_str);
    g_object_unref(gen);
    fail_msg("dispatch result for %s did not match %s", request_file, response_file);
  }

  json_node_unref(actual);
  json_node_unref(request_node);
  json_node_unref(expected);
}

/* ---------------------------------------------------------------------- */
/* canned remote_edit stubs                                                */
/* ---------------------------------------------------------------------- */

// takes ownership of `message` (pass a g_strdup()/g_strdup_printf() result).
static dt_remote_error_t *_make_error(dt_remote_error_code_t code, char *message)
{
  dt_remote_error_t *err = g_malloc0(sizeof(dt_remote_error_t));
  err->code = code;
  err->message = message;
  return err;
}

static gboolean stub_get_state_darkroom(dt_remote_state_t **out, dt_remote_error_t **error)
{
  (void)error;
  dt_remote_state_t *s = g_malloc0(sizeof(dt_remote_state_t));
  s->view = g_strdup("darkroom");
  s->has_image = TRUE;
  s->image_id = 172;
  s->image_filename = g_strdup("IMG_4021.CR3");
  s->width = 6000;
  s->height = 4000;
  s->maker = g_strdup("Canon");
  s->model = g_strdup("EOS R6");
  s->lens = g_strdup("RF35mm F1.8");
  s->iso = 800;
  s->aperture = 2.8f;
  s->exposure_time = 0.005f;
  s->focal_length = 35.0f;
  s->revision = 31;
  *out = s;
  return TRUE;
}

static gboolean stub_get_state_no_image(dt_remote_state_t **out, dt_remote_error_t **error)
{
  (void)error;
  dt_remote_state_t *s = g_malloc0(sizeof(dt_remote_state_t));
  s->view = g_strdup("lighttable");
  s->has_image = FALSE;
  s->revision = 0;
  *out = s;
  return TRUE;
}

static gboolean stub_get_state_revision31_no_image(dt_remote_state_t **out, dt_remote_error_t **error)
{
  (void)error;
  dt_remote_state_t *s = g_malloc0(sizeof(dt_remote_state_t));
  s->view = g_strdup("darkroom");
  s->has_image = TRUE;
  s->revision = 31;
  *out = s;
  return TRUE;
}

static gboolean stub_list_modules_one_exposure(GPtrArray **out, dt_remote_error_t **error)
{
  (void)error;
  GPtrArray *arr = g_ptr_array_new_with_free_func(dt_remote_module_free);
  dt_remote_module_t *m = g_malloc0(sizeof(dt_remote_module_t));
  m->op = g_strdup("exposure");
  m->instance = 0;
  m->instance_name = g_strdup("");
  m->display_name = g_strdup("exposure");
  m->enabled = TRUE;
  m->deprecated = FALSE;
  m->supports_multiple_instances = TRUE;
  g_ptr_array_add(arr, m);
  *out = arr;
  return TRUE;
}

static gboolean stub_list_modules_not_in_darkroom(GPtrArray **out, dt_remote_error_t **error)
{
  (void)out;
  if(error) *error = _make_error(DT_REMOTE_ERR_NOT_IN_DARKROOM, g_strdup("not in darkroom view"));
  return FALSE;
}

static dt_remote_field_t *_make_field(const char *name, const char *desc, const char *type_name,
                                      gboolean writable)
{
  dt_remote_field_t *f = g_malloc0(sizeof(dt_remote_field_t));
  f->name = (char *)name;
  f->description = (char *)desc;
  f->type_name = (char *)type_name;
  f->writable = writable;
  return f;
}

static gboolean stub_get_module_schema_exposure(const char *op, dt_remote_module_schema_t **out,
                                                dt_remote_error_t **error)
{
  (void)op;
  (void)error;
  dt_remote_module_schema_t *schema = g_malloc0(sizeof(dt_remote_module_schema_t));
  schema->op = g_strdup("exposure");
  schema->display_name = g_strdup("exposure");
  schema->params_version = 7;
  schema->deprecated = FALSE;
  schema->supports_multiple_instances = TRUE;
  schema->fields = g_ptr_array_new_with_free_func(dt_remote_field_free);

  dt_remote_field_t *exposure = _make_field("exposure", "exposure correction", "float", TRUE);
  exposure->has_range = TRUE;
  exposure->minimum = -18.0;
  exposure->maximum = 18.0;
  exposure->has_default = TRUE;
  exposure->default_value.type = DT_REMOTE_VALUE_FLOAT;
  exposure->default_value.v.f = 0.0;
  g_ptr_array_add(schema->fields, exposure);

  dt_remote_field_t *mode = _make_field("mode", "mode", "enum", TRUE);
  mode->has_default = TRUE;
  mode->default_value.type = DT_REMOTE_VALUE_ENUM;
  mode->default_value.v.e.value = 0;
  mode->default_value.v.e.name = g_strdup("EXPOSURE_MODE_MANUAL");
  mode->enum_values = g_ptr_array_new_with_free_func(g_free);
  dt_remote_enum_value_t *manual = g_malloc0(sizeof(dt_remote_enum_value_t));
  manual->name = (char *)"EXPOSURE_MODE_MANUAL";
  manual->value = 0;
  manual->description = (char *)"manual";
  g_ptr_array_add(mode->enum_values, manual);
  dt_remote_enum_value_t *deflicker = g_malloc0(sizeof(dt_remote_enum_value_t));
  deflicker->name = (char *)"EXPOSURE_MODE_DEFLICKER";
  deflicker->value = 1;
  deflicker->description = (char *)"automatic";
  g_ptr_array_add(mode->enum_values, deflicker);
  g_ptr_array_add(schema->fields, mode);

  *out = schema;
  return TRUE;
}

static gboolean stub_get_module_schema_unknown(const char *op, dt_remote_module_schema_t **out,
                                               dt_remote_error_t **error)
{
  assert_non_null(out);
  assert_null(*out);
  if(error) *error = _make_error(DT_REMOTE_ERR_UNKNOWN_MODULE, g_strdup_printf("unknown module '%s'", op));
  return FALSE;
}

static gboolean stub_get_module_schema_internal(const char *op, dt_remote_module_schema_t **out,
                                                dt_remote_error_t **error)
{
  (void)op;
  assert_non_null(out);
  assert_null(*out);
  if(error)
    *error = _make_error(DT_REMOTE_ERR_INTERNAL,
                         g_strdup("curve registry does not match module introspection"));
  return FALSE;
}

static gboolean stub_get_module_params_exposure(const dt_remote_module_ref_t *ref, GPtrArray **out,
                                                GHashTable **semantic_out, dt_remote_error_t **error)
{
  (void)ref;
  (void)error;
  assert_non_null(out);
  assert_null(*out);
  assert_non_null(semantic_out);
  assert_null(*semantic_out);
  GPtrArray *arr = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);

  dt_remote_patch_entry_t *e1 = g_malloc0(sizeof(dt_remote_patch_entry_t));
  e1->name = g_strdup("exposure");
  e1->value.type = DT_REMOTE_VALUE_FLOAT;
  e1->value.v.f = 0.5;
  g_ptr_array_add(arr, e1);

  dt_remote_patch_entry_t *e2 = g_malloc0(sizeof(dt_remote_patch_entry_t));
  e2->name = g_strdup("black");
  e2->value.type = DT_REMOTE_VALUE_FLOAT;
  e2->value.v.f = 0.0;
  g_ptr_array_add(arr, e2);

  dt_remote_patch_entry_t *e3 = g_malloc0(sizeof(dt_remote_patch_entry_t));
  e3->name = g_strdup("mode");
  e3->value.type = DT_REMOTE_VALUE_ENUM;
  e3->value.v.e.value = 0;
  e3->value.v.e.name = g_strdup("EXPOSURE_MODE_MANUAL");
  g_ptr_array_add(arr, e3);

  *out = arr;
  *semantic_out = g_hash_table_new(g_str_hash, g_str_equal);
  return TRUE;
}

// A read whose float values include a stored NaN -- temperature's `various`
// coefficient is NAN by design on every RGB camera (temperature.c:159, and
// the "the fourth is usually NAN for RGB" comment), so the serializer must
// map non-finite reads to a wire-legal value instead of letting json-glib
// emit a bare `nan` token that no strict JSON parser accepts.
static gboolean stub_get_module_params_nan_float(const dt_remote_module_ref_t *ref, GPtrArray **out,
                                                 GHashTable **semantic_out, dt_remote_error_t **error)
{
  (void)ref;
  (void)error;
  assert_non_null(out);
  assert_null(*out);
  assert_non_null(semantic_out);
  assert_null(*semantic_out);
  GPtrArray *arr = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);

  dt_remote_patch_entry_t *e1 = g_malloc0(sizeof(dt_remote_patch_entry_t));
  e1->name = g_strdup("red");
  e1->value.type = DT_REMOTE_VALUE_FLOAT;
  e1->value.v.f = 2.5;
  g_ptr_array_add(arr, e1);

  dt_remote_patch_entry_t *e2 = g_malloc0(sizeof(dt_remote_patch_entry_t));
  e2->name = g_strdup("various");
  e2->value.type = DT_REMOTE_VALUE_FLOAT;
  e2->value.v.f = NAN;
  g_ptr_array_add(arr, e2);

  *out = arr;
  *semantic_out = g_hash_table_new(g_str_hash, g_str_equal);
  return TRUE;
}

static gboolean stub_get_module_params_unknown_module(const dt_remote_module_ref_t *ref, GPtrArray **out,
                                                      GHashTable **semantic_out, dt_remote_error_t **error)
{
  assert_non_null(out);
  assert_null(*out);
  assert_non_null(semantic_out);
  assert_null(*semantic_out);
  if(error)
    *error = _make_error(DT_REMOTE_ERR_UNKNOWN_MODULE, g_strdup_printf("unknown module '%s'", ref->op));
  return FALSE;
}

static gboolean stub_get_module_params_internal(const dt_remote_module_ref_t *ref, GPtrArray **out,
                                                GHashTable **semantic_out, dt_remote_error_t **error)
{
  (void)ref;
  assert_non_null(out);
  assert_null(*out);
  assert_non_null(semantic_out);
  assert_null(*semantic_out);
  if(error)
    *error = _make_error(DT_REMOTE_ERR_INTERNAL,
                         g_strdup("curve registry does not match live module introspection"));
  return FALSE;
}

/* --- rgbcurve semantic read-path stubs (milestone 2, task 7) ------------- */

static const char *const RGBCURVE_SEMANTIC_IDS[] = { "curve.master", "curve.red", "curve.green",
                                                     "curve.blue" };

static dt_remote_parameter_condition_t *_make_condition(const char *field,
                                                        dt_remote_predicate_operator_t op,
                                                        const char *enum_name)
{
  dt_remote_parameter_condition_t *condition = g_new0(dt_remote_parameter_condition_t, 1);
  condition->field = g_strdup(field);
  condition->op = op;
  condition->enum_name = g_strdup(enum_name);
  return condition;
}

// One hand-built rgbcurve curve schema, mirroring what the real registry
// adapter produces (remote_curve_registry.c): [0,1]^2 normalized domain,
// 2-20 points, spacing 0.0025 strictly-greater, optional boundaries, all
// three interpolations, conditionally writable on curve_autoscale.
static dt_remote_curve_schema_t *_make_rgbcurve_curve_schema(const char *name, const char *display_name,
                                                             dt_remote_predicate_operator_t op)
{
  dt_remote_curve_schema_t *schema = g_new0(dt_remote_curve_schema_t, 1);
  schema->name = g_strdup(name);
  schema->display_name = g_strdup(display_name);
  schema->x = (dt_remote_curve_axis_t){ .minimum = 0.0, .maximum = 1.0, .unit = g_strdup("normalized") };
  schema->y = (dt_remote_curve_axis_t){ .minimum = 0.0, .maximum = 1.0, .unit = g_strdup("normalized") };
  schema->minimum_points = 2;
  schema->maximum_points = 20;
  schema->minimum_x_spacing = 0.0025;
  schema->adjacent_spacing_rule = DT_REMOTE_SPACING_GREATER_THAN;
  schema->strict_x_order = TRUE;
  schema->boundary_point_policy = DT_REMOTE_CURVE_BOUNDARY_POINTS_OPTIONAL;
  schema->interpolation_mask = (1u << DT_REMOTE_CURVE_CUBIC_SPLINE) | (1u << DT_REMOTE_CURVE_CATMULL_ROM)
                               | (1u << DT_REMOTE_CURVE_MONOTONE_HERMITE);
  schema->default_interpolation = DT_REMOTE_CURVE_MONOTONE_HERMITE;
  schema->writability = DT_REMOTE_WRITABLE_CONDITIONAL;
  schema->active_when = _make_condition("curve_autoscale", op, "DT_S_SCALE_MANUAL_RGB");
  schema->writable_when = _make_condition("curve_autoscale", op, "DT_S_SCALE_MANUAL_RGB");
  return schema;
}

static dt_remote_field_t *_make_represented_array_field(const char *name)
{
  dt_remote_field_t *field = _make_field(name, "", "array", FALSE);
  field->represented_by = g_ptr_array_new_with_free_func(g_free);
  for(guint i = 0; i < G_N_ELEMENTS(RGBCURVE_SEMANTIC_IDS); i++)
    g_ptr_array_add(field->represented_by, g_strdup(RGBCURVE_SEMANTIC_IDS[i]));
  return field;
}

static gboolean stub_get_module_schema_rgbcurve(const char *op, dt_remote_module_schema_t **out,
                                                dt_remote_error_t **error)
{
  (void)op;
  (void)error;
  dt_remote_module_schema_t *schema = g_malloc0(sizeof(dt_remote_module_schema_t));
  schema->op = g_strdup("rgbcurve");
  schema->display_name = g_strdup("rgb curve");
  schema->params_version = 1;
  schema->deprecated = FALSE;
  schema->supports_multiple_instances = TRUE;
  schema->fields = g_ptr_array_new_with_free_func(dt_remote_field_free);

  g_ptr_array_add(schema->fields, _make_represented_array_field("curve_nodes"));
  g_ptr_array_add(schema->fields, _make_represented_array_field("curve_num_nodes"));
  g_ptr_array_add(schema->fields, _make_represented_array_field("curve_type"));

  dt_remote_field_t *autoscale = _make_field("curve_autoscale", "mode", "enum", TRUE);
  autoscale->has_default = TRUE;
  autoscale->default_value.type = DT_REMOTE_VALUE_ENUM;
  autoscale->default_value.v.e.value = 0;
  autoscale->default_value.v.e.name = g_strdup("DT_S_SCALE_AUTOMATIC_RGB");
  autoscale->enum_values = g_ptr_array_new_with_free_func(g_free);
  dt_remote_enum_value_t *automatic = g_malloc0(sizeof(dt_remote_enum_value_t));
  automatic->name = (char *)"DT_S_SCALE_AUTOMATIC_RGB";
  automatic->value = 0;
  automatic->description = (char *)"RGB, linked channels";
  g_ptr_array_add(autoscale->enum_values, automatic);
  dt_remote_enum_value_t *manual = g_malloc0(sizeof(dt_remote_enum_value_t));
  manual->name = (char *)"DT_S_SCALE_MANUAL_RGB";
  manual->value = 1;
  manual->description = (char *)"RGB, independent channels";
  g_ptr_array_add(autoscale->enum_values, manual);
  g_ptr_array_add(schema->fields, autoscale);

  dt_remote_field_t *compensate = _make_field("compensate_middle_grey", "compensate middle gray",
                                              "bool", TRUE);
  compensate->has_default = TRUE;
  compensate->default_value.type = DT_REMOTE_VALUE_BOOL;
  compensate->default_value.v.b = FALSE;
  g_ptr_array_add(schema->fields, compensate);

  schema->semantic_fields = g_ptr_array_new_with_free_func(dt_remote_semantic_schema_free);
  g_ptr_array_add(schema->semantic_fields,
                  dt_remote_semantic_schema_wrap_curve(
                    _make_rgbcurve_curve_schema("curve.master", "master", DT_REMOTE_PREDICATE_NE)));
  g_ptr_array_add(schema->semantic_fields,
                  dt_remote_semantic_schema_wrap_curve(
                    _make_rgbcurve_curve_schema("curve.red", "R", DT_REMOTE_PREDICATE_EQ)));
  g_ptr_array_add(schema->semantic_fields,
                  dt_remote_semantic_schema_wrap_curve(
                    _make_rgbcurve_curve_schema("curve.green", "G", DT_REMOTE_PREDICATE_EQ)));
  g_ptr_array_add(schema->semantic_fields,
                  dt_remote_semantic_schema_wrap_curve(
                    _make_rgbcurve_curve_schema("curve.blue", "B", DT_REMOTE_PREDICATE_EQ)));

  *out = schema;
  return TRUE;
}

static dt_remote_curve_value_t *_make_identity_curve_value(const char *name, gboolean active)
{
  dt_remote_curve_value_t *value = g_new0(dt_remote_curve_value_t, 1);
  value->name = g_strdup(name);
  value->points = g_array_new(FALSE, FALSE, sizeof(dt_remote_curve_point_t));
  const dt_remote_curve_point_t p0 = { 0.0, 0.0 };
  const dt_remote_curve_point_t p1 = { 1.0, 1.0 };
  g_array_append_val(value->points, p0);
  g_array_append_val(value->points, p1);
  value->interpolation = DT_REMOTE_CURVE_MONOTONE_HERMITE;
  value->active = active;
  value->effective = active;
  value->writable_now = active;
  return value;
}

// Automatic-RGB mode: all four semantic IDs are present (never omitted when
// inactive -- milestone spec resolved decision 4); only curve.master is
// active/writable.
static gboolean stub_get_module_params_rgbcurve(const dt_remote_module_ref_t *ref, GPtrArray **out,
                                                GHashTable **semantic_out, dt_remote_error_t **error)
{
  (void)ref;
  (void)error;
  GPtrArray *arr = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);

  dt_remote_patch_entry_t *autoscale = g_malloc0(sizeof(dt_remote_patch_entry_t));
  autoscale->name = g_strdup("curve_autoscale");
  autoscale->value.type = DT_REMOTE_VALUE_ENUM;
  autoscale->value.v.e.value = 0;
  autoscale->value.v.e.name = g_strdup("DT_S_SCALE_AUTOMATIC_RGB");
  g_ptr_array_add(arr, autoscale);

  dt_remote_patch_entry_t *compensate = g_malloc0(sizeof(dt_remote_patch_entry_t));
  compensate->name = g_strdup("compensate_middle_grey");
  compensate->value.type = DT_REMOTE_VALUE_BOOL;
  compensate->value.v.b = FALSE;
  g_ptr_array_add(arr, compensate);

  GHashTable *semantic =
    g_hash_table_new_full(g_str_hash, g_str_equal, g_free, dt_remote_semantic_value_free);
  g_hash_table_insert(semantic, g_strdup("curve.master"),
                      dt_remote_semantic_value_wrap_curve(_make_identity_curve_value("curve.master", TRUE)));
  g_hash_table_insert(semantic, g_strdup("curve.red"),
                      dt_remote_semantic_value_wrap_curve(_make_identity_curve_value("curve.red", FALSE)));
  g_hash_table_insert(semantic, g_strdup("curve.green"),
                      dt_remote_semantic_value_wrap_curve(_make_identity_curve_value("curve.green", FALSE)));
  g_hash_table_insert(semantic, g_strdup("curve.blue"),
                      dt_remote_semantic_value_wrap_curve(_make_identity_curve_value("curve.blue", FALSE)));

  *out = arr;
  if(semantic_out) *semantic_out = semantic;
  else g_hash_table_unref(semantic);
  return TRUE;
}

static gboolean stub_list_modules_one_rgbcurve(GPtrArray **out, dt_remote_error_t **error)
{
  (void)error;
  GPtrArray *arr = g_ptr_array_new_with_free_func(dt_remote_module_free);
  dt_remote_module_t *m = g_malloc0(sizeof(dt_remote_module_t));
  m->op = g_strdup("rgbcurve");
  m->instance = 0;
  m->instance_name = g_strdup("");
  m->display_name = g_strdup("rgb curve");
  m->enabled = TRUE;
  m->deprecated = FALSE;
  m->supports_multiple_instances = TRUE;
  g_ptr_array_add(arr, m);
  *out = arr;
  return TRUE;
}

/* --- semantic vector schema/value stubs (milestone 4, task 4) ----------- */

// One hand-built vector schema per subtype -- covers the three wire shapes
// _vector_schema_to_json emits: PLAIN (no color_space/ordering), COLOR
// (color_space, no ordering), LEVELS (ordering, no color_space). Mirrors
// what a real registry adapter would produce (remote_vector_registry.c).
static dt_remote_vector_schema_t *_make_vector_schema(const char *name, const char *display_name,
                                                       dt_remote_vector_subtype_t subtype,
                                                       const char *color_space,
                                                       double minimum_gap,
                                                       const char *const *component_names,
                                                       guint component_count,
                                                       dt_remote_parameter_condition_t *writable_when)
{
  dt_remote_vector_schema_t *schema = g_new0(dt_remote_vector_schema_t, 1);
  schema->name = g_strdup(name);
  schema->display_name = g_strdup(display_name);
  schema->subtype = subtype;
  schema->color_space = color_space ? g_strdup(color_space) : NULL;
  schema->strictly_increasing = (subtype == DT_REMOTE_VECTOR_LEVELS);
  schema->minimum_gap = minimum_gap;
  schema->writability = writable_when ? DT_REMOTE_WRITABLE_CONDITIONAL : DT_REMOTE_WRITABLE_NOW;
  schema->writable_when = writable_when;
  schema->components =
    g_array_sized_new(FALSE, FALSE, sizeof(dt_remote_vector_component_schema_t), component_count);
  for(guint i = 0; i < component_count; i++)
  {
    dt_remote_vector_component_schema_t c = { .name = g_strdup(component_names[i]),
                                              .minimum = 0.0, .maximum = 1.0 };
    g_array_append_val(schema->components, c);
  }
  return schema;
}

static gboolean stub_get_module_schema_vector(const char *op, dt_remote_module_schema_t **out,
                                              dt_remote_error_t **error)
{
  (void)op;
  (void)error;
  dt_remote_module_schema_t *schema = g_malloc0(sizeof(dt_remote_module_schema_t));
  schema->op = g_strdup("borders");
  schema->display_name = g_strdup("framing");
  schema->params_version = 4;
  schema->deprecated = FALSE;
  schema->supports_multiple_instances = FALSE;
  schema->fields = g_ptr_array_new_with_free_func(dt_remote_field_free);

  dt_remote_field_t *color_field = _make_field("color", "", "array", FALSE);
  color_field->represented_by = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(color_field->represented_by, g_strdup("color"));
  g_ptr_array_add(schema->fields, color_field);

  dt_remote_field_t *size = _make_field("size", "border size", "float", TRUE);
  size->has_range = TRUE;
  size->minimum = 0.0;
  size->maximum = 0.5;
  size->has_default = TRUE;
  size->default_value.type = DT_REMOTE_VALUE_FLOAT;
  size->default_value.v.f = 0.1;
  g_ptr_array_add(schema->fields, size);

  schema->semantic_fields = g_ptr_array_new_with_free_func(dt_remote_semantic_schema_free);

  static const char *const lift_components[] = { "factor", "red" };
  g_ptr_array_add(schema->semantic_fields,
                  dt_remote_semantic_schema_wrap_vector(
                    _make_vector_schema("lift", "Lift", DT_REMOTE_VECTOR_PLAIN, NULL, 0.0,
                                        lift_components, G_N_ELEMENTS(lift_components), NULL)));

  static const char *const color_components[] = { "red", "green", "blue" };
  g_ptr_array_add(schema->semantic_fields,
                  dt_remote_semantic_schema_wrap_vector(
                    _make_vector_schema("color", "Border color", DT_REMOTE_VECTOR_COLOR, "display_rgb",
                                        0.0, color_components, G_N_ELEMENTS(color_components), NULL)));

  static const char *const levels_components[] = { "black", "midpoint", "white" };
  g_ptr_array_add(schema->semantic_fields,
                  dt_remote_semantic_schema_wrap_vector(
                    _make_vector_schema("levels.linked", "Levels", DT_REMOTE_VECTOR_LEVELS, NULL,
                                        (double)FLT_EPSILON, levels_components,
                                        G_N_ELEMENTS(levels_components),
                                        _make_condition("autoscale", DT_REMOTE_PREDICATE_EQ,
                                                       "LINKED_CHANNELS"))));

  *out = schema;
  return TRUE;
}

static dt_remote_vector_value_t *_make_vector_value(const char *name, const double *values, guint count,
                                                     gboolean active)
{
  dt_remote_vector_value_t *value = g_new0(dt_remote_vector_value_t, 1);
  value->name = g_strdup(name);
  value->values = g_array_sized_new(FALSE, FALSE, sizeof(double), count);
  g_array_append_vals(value->values, values, count);
  value->active = active;
  value->effective = active;
  value->writable_now = active;
  return value;
}

static gboolean stub_get_module_params_vector(const dt_remote_module_ref_t *ref, GPtrArray **out,
                                              GHashTable **semantic_out, dt_remote_error_t **error)
{
  (void)ref;
  (void)error;
  GPtrArray *arr = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);

  dt_remote_patch_entry_t *size = g_malloc0(sizeof(dt_remote_patch_entry_t));
  size->name = g_strdup("size");
  size->value.type = DT_REMOTE_VALUE_FLOAT;
  size->value.v.f = 0.1;
  g_ptr_array_add(arr, size);

  GHashTable *semantic =
    g_hash_table_new_full(g_str_hash, g_str_equal, g_free, dt_remote_semantic_value_free);
  static const double color_values[] = { 1.0, 1.0, 1.0 };
  g_hash_table_insert(semantic, g_strdup("color"),
                      dt_remote_semantic_value_wrap_vector(
                        _make_vector_value("color", color_values, G_N_ELEMENTS(color_values), TRUE)));

  *out = arr;
  if(semantic_out) *semantic_out = semantic;
  else g_hash_table_unref(semantic);
  return TRUE;
}

static gboolean stub_list_modules_one_borders(GPtrArray **out, dt_remote_error_t **error)
{
  (void)error;
  GPtrArray *arr = g_ptr_array_new_with_free_func(dt_remote_module_free);
  dt_remote_module_t *m = g_malloc0(sizeof(dt_remote_module_t));
  m->op = g_strdup("borders");
  m->instance = 0;
  m->instance_name = g_strdup("");
  m->display_name = g_strdup("framing");
  m->enabled = TRUE;
  m->deprecated = FALSE;
  m->supports_multiple_instances = FALSE;
  g_ptr_array_add(arr, m);
  *out = arr;
  return TRUE;
}

/* --- semantic band schema/value stubs (milestone 5, task 4) ------------- */

// Hand-built band schemas covering the wire shapes _band_schema_to_json
// emits: a FIXED-policy set (no min_gap/x_shared_with members, current "x"
// array present, unconditionally writable) and an INTERIOR-policy set
// (min_gap + x_shared_with present, no current "x" -- the NULL x a
// list_schema-produced schema carries -- plus a writable_when condition).
// Mirrors what a real registry adapter would produce
// (remote_band_registry.c), fictional entries included, exactly as the
// vector stub above hangs PLAIN/COLOR/LEVELS entries off one op.
static dt_remote_band_schema_t *_make_band_schema(const char *name, const char *display_name,
                                                   dt_remote_band_x_policy_t x_policy,
                                                   double minimum_gap, const char *x_shared_with,
                                                   const double *x, guint count,
                                                   dt_remote_parameter_condition_t *writable_when)
{
  dt_remote_band_schema_t *schema = g_new0(dt_remote_band_schema_t, 1);
  schema->name = g_strdup(name);
  schema->display_name = g_strdup(display_name);
  schema->count = count;
  schema->y_minimum = 0.0;
  schema->y_maximum = 1.0;
  schema->x_policy = x_policy;
  schema->minimum_gap = minimum_gap;
  schema->x_shared_with = x_shared_with ? g_strdup(x_shared_with) : NULL;
  if(x)
  {
    schema->x = g_array_sized_new(FALSE, FALSE, sizeof(double), count);
    g_array_append_vals(schema->x, x, count);
  }
  schema->writability = writable_when ? DT_REMOTE_WRITABLE_CONDITIONAL : DT_REMOTE_WRITABLE_NOW;
  schema->writable_when = writable_when;
  return schema;
}

static const double s_band_default_x[] = { 0.0, 0.2, 0.4, 0.6, 0.8, 1.0 };

static gboolean stub_get_module_schema_band(const char *op, dt_remote_module_schema_t **out,
                                            dt_remote_error_t **error)
{
  (void)op;
  (void)error;
  dt_remote_module_schema_t *schema = g_malloc0(sizeof(dt_remote_module_schema_t));
  schema->op = g_strdup("lowlight");
  schema->display_name = g_strdup("lowlight vision");
  schema->params_version = 1;
  schema->deprecated = FALSE;
  schema->supports_multiple_instances = FALSE;
  schema->fields = g_ptr_array_new_with_free_func(dt_remote_field_free);

  dt_remote_field_t *blueness = _make_field("blueness", "blue shift", "float", TRUE);
  blueness->has_range = TRUE;
  blueness->minimum = 0.0;
  blueness->maximum = 100.0;
  blueness->has_default = TRUE;
  blueness->default_value.type = DT_REMOTE_VALUE_FLOAT;
  blueness->default_value.v.f = 0.0;
  g_ptr_array_add(schema->fields, blueness);

  dt_remote_field_t *transition_x = _make_field("transition_x", "", "array", FALSE);
  transition_x->represented_by = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(transition_x->represented_by, g_strdup("bands.transition"));
  g_ptr_array_add(schema->fields, transition_x);

  dt_remote_field_t *transition_y = _make_field("transition_y", "", "array", FALSE);
  transition_y->represented_by = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(transition_y->represented_by, g_strdup("bands.transition"));
  g_ptr_array_add(schema->fields, transition_y);

  schema->semantic_fields = g_ptr_array_new_with_free_func(dt_remote_semantic_schema_free);
  g_ptr_array_add(schema->semantic_fields,
                  dt_remote_semantic_schema_wrap_band(
                    _make_band_schema("bands.transition", "Transition", DT_REMOTE_BAND_X_FIXED, 0.0,
                                      NULL, s_band_default_x, G_N_ELEMENTS(s_band_default_x), NULL)));
  g_ptr_array_add(schema->semantic_fields,
                  dt_remote_semantic_schema_wrap_band(
                    _make_band_schema("bands.shadow", "Shadow response", DT_REMOTE_BAND_X_INTERIOR,
                                      0.001, "bands.highlight", NULL, 6,
                                      _make_condition("mode", DT_REMOTE_PREDICATE_EQ, "ADVANCED"))));

  *out = schema;
  return TRUE;
}

static dt_remote_band_value_t *_make_band_value(const char *name, const double *y, const double *x,
                                                 guint count)
{
  dt_remote_band_value_t *value = g_new0(dt_remote_band_value_t, 1);
  value->name = g_strdup(name);
  value->y = g_array_sized_new(FALSE, FALSE, sizeof(double), count);
  g_array_append_vals(value->y, y, count);
  value->x = g_array_sized_new(FALSE, FALSE, sizeof(double), count);
  g_array_append_vals(value->x, x, count);
  value->active = TRUE;
  value->effective = TRUE;
  value->writable_now = TRUE;
  return value;
}

static gboolean stub_get_module_params_band(const dt_remote_module_ref_t *ref, GPtrArray **out,
                                            GHashTable **semantic_out, dt_remote_error_t **error)
{
  (void)ref;
  (void)error;
  GPtrArray *arr = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);

  dt_remote_patch_entry_t *blueness = g_malloc0(sizeof(dt_remote_patch_entry_t));
  blueness->name = g_strdup("blueness");
  blueness->value.type = DT_REMOTE_VALUE_FLOAT;
  blueness->value.v.f = 0.0;
  g_ptr_array_add(arr, blueness);

  GHashTable *semantic =
    g_hash_table_new_full(g_str_hash, g_str_equal, g_free, dt_remote_semantic_value_free);
  static const double transition_y[] = { 0.5, 0.5, 0.5, 0.5, 0.5, 0.5 };
  g_hash_table_insert(semantic, g_strdup("bands.transition"),
                      dt_remote_semantic_value_wrap_band(
                        _make_band_value("bands.transition", transition_y, s_band_default_x,
                                         G_N_ELEMENTS(transition_y))));

  *out = arr;
  if(semantic_out) *semantic_out = semantic;
  else g_hash_table_unref(semantic);
  return TRUE;
}

static gboolean stub_list_modules_one_lowlight(GPtrArray **out, dt_remote_error_t **error)
{
  (void)error;
  GPtrArray *arr = g_ptr_array_new_with_free_func(dt_remote_module_free);
  dt_remote_module_t *m = g_malloc0(sizeof(dt_remote_module_t));
  m->op = g_strdup("lowlight");
  m->instance = 0;
  m->instance_name = g_strdup("");
  m->display_name = g_strdup("lowlight vision");
  m->enabled = TRUE;
  m->deprecated = FALSE;
  m->supports_multiple_instances = FALSE;
  g_ptr_array_add(arr, m);
  *out = arr;
  return TRUE;
}

/* --- semantic quantity schema/value stubs (milestone 6, task 4) --------- */

// Hand-built quantity schema covering the wire shapes _quantity_schema_to_json
// emits: "wb.temperature" over the real "temperature" module's red/green/
// blue/various coefficient scalars -- a two-component ("temperature"/
// "tint") derived quantity, unconditionally writable, one component
// carrying a unit ("kelvin") and one not, exactly the "unit present only
// when non-NULL" shape the wire vocabulary requires. Mirrors what a real
// registry adapter would produce (remote_quantity_registry.c, Task 5),
// exactly as the band stub above hangs a fictional entry off "lowlight".
static dt_remote_quantity_schema_component_t *_make_quantity_schema_component(
  const char *name, const char *unit, double minimum, double maximum)
{
  dt_remote_quantity_schema_component_t *c = g_new0(dt_remote_quantity_schema_component_t, 1);
  c->name = g_strdup(name);
  c->unit = unit ? g_strdup(unit) : NULL;
  c->minimum = minimum;
  c->maximum = maximum;
  return c;
}

static dt_remote_quantity_schema_t *_make_quantity_schema(const char *name, const char *display_name)
{
  dt_remote_quantity_schema_t *schema = g_new0(dt_remote_quantity_schema_t, 1);
  schema->name = g_strdup(name);
  schema->display_name = g_strdup(display_name);
  schema->components = g_ptr_array_new(); // manually freed element-by-element, see dt_remote_quantity_schema_free
  g_ptr_array_add(schema->components,
                  _make_quantity_schema_component("temperature", "kelvin", 3000.0, 25000.0));
  g_ptr_array_add(schema->components, _make_quantity_schema_component("tint", NULL, 0.1, 8.0));
  schema->derived = TRUE;
  schema->writability = DT_REMOTE_WRITABLE_NOW;
  return schema;
}

static gboolean stub_get_module_schema_quantity(const char *op, dt_remote_module_schema_t **out,
                                                dt_remote_error_t **error)
{
  (void)op;
  (void)error;
  dt_remote_module_schema_t *schema = g_malloc0(sizeof(dt_remote_module_schema_t));
  schema->op = g_strdup("temperature");
  schema->display_name = g_strdup("white balance");
  schema->params_version = 4;
  schema->deprecated = FALSE;
  schema->supports_multiple_instances = FALSE;
  schema->fields = g_ptr_array_new_with_free_func(dt_remote_field_free);

  // All four of temperature's coefficient scalars -- the "coefficient
  // coexistence" exception: every one stays writable:true while also
  // carrying represented_by.
  static const char *const coeffs[] = { "red", "green", "blue", "various" };
  for(guint i = 0; i < G_N_ELEMENTS(coeffs); i++)
  {
    dt_remote_field_t *f = _make_field(coeffs[i], "", "float", TRUE);
    f->represented_by = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(f->represented_by, g_strdup("wb.temperature"));
    g_ptr_array_add(schema->fields, f);
  }

  schema->semantic_fields = g_ptr_array_new_with_free_func(dt_remote_semantic_schema_free);
  g_ptr_array_add(schema->semantic_fields,
                  dt_remote_semantic_schema_wrap_quantity(
                    _make_quantity_schema("wb.temperature", "White Balance")));

  *out = schema;
  return TRUE;
}

static dt_remote_quantity_value_t *_make_quantity_value(const char *name, const char *const *names,
                                                        const double *values, guint n)
{
  dt_remote_quantity_value_t *v = g_new0(dt_remote_quantity_value_t, 1);
  v->name = g_strdup(name);
  v->values = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_quantity_component_value_free);
  for(guint i = 0; i < n; i++)
  {
    dt_remote_quantity_component_value_t *c = g_new0(dt_remote_quantity_component_value_t, 1);
    c->name = g_strdup(names[i]);
    c->value = values[i];
    g_ptr_array_add(v->values, c);
  }
  v->active = TRUE;
  v->effective = TRUE;
  v->writable_now = TRUE;
  return v;
}

static gboolean stub_get_module_params_quantity(const dt_remote_module_ref_t *ref, GPtrArray **out,
                                                GHashTable **semantic_out, dt_remote_error_t **error)
{
  (void)ref;
  (void)error;
  GPtrArray *arr = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);

  static const struct { const char *name; float value; } coeffs[] = {
    { "red", 1.0f }, { "green", 1.0f }, { "blue", 1.0f }, { "various", 1.0f },
  };
  for(guint i = 0; i < G_N_ELEMENTS(coeffs); i++)
  {
    dt_remote_patch_entry_t *e = g_malloc0(sizeof(dt_remote_patch_entry_t));
    e->name = g_strdup(coeffs[i].name);
    e->value.type = DT_REMOTE_VALUE_FLOAT;
    e->value.v.f = coeffs[i].value;
    g_ptr_array_add(arr, e);
  }

  GHashTable *semantic =
    g_hash_table_new_full(g_str_hash, g_str_equal, g_free, dt_remote_semantic_value_free);
  static const char *const component_names[] = { "temperature", "tint" };
  static const double component_values[] = { 5500.0, 1.2 };
  g_hash_table_insert(semantic, g_strdup("wb.temperature"),
                      dt_remote_semantic_value_wrap_quantity(
                        _make_quantity_value("wb.temperature", component_names, component_values, 2)));

  *out = arr;
  if(semantic_out) *semantic_out = semantic;
  else g_hash_table_unref(semantic);
  return TRUE;
}

// Same shape, but the read hook produced a NaN tint. Reachable through a
// legal write: zeroing all three RGB coefficients (each has $MIN: 0.0)
// makes temperature's XYZ sum zero, and _XYZ_to_temperature's tint clamps
// use ordered comparisons that NaN sails through.
static gboolean stub_get_module_params_quantity_nan(const dt_remote_module_ref_t *ref, GPtrArray **out,
                                                    GHashTable **semantic_out, dt_remote_error_t **error)
{
  (void)ref;
  (void)error;
  GPtrArray *arr = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);
  GHashTable *semantic =
    g_hash_table_new_full(g_str_hash, g_str_equal, g_free, dt_remote_semantic_value_free);
  static const char *const component_names[] = { "temperature", "tint" };
  static const double component_values[] = { 5500.0, NAN };
  g_hash_table_insert(semantic, g_strdup("wb.temperature"),
                      dt_remote_semantic_value_wrap_quantity(
                        _make_quantity_value("wb.temperature", component_names, component_values, 2)));

  *out = arr;
  if(semantic_out) *semantic_out = semantic;
  else g_hash_table_unref(semantic);
  return TRUE;
}

static gboolean stub_list_modules_one_temperature(GPtrArray **out, dt_remote_error_t **error)
{
  (void)error;
  GPtrArray *arr = g_ptr_array_new_with_free_func(dt_remote_module_free);
  dt_remote_module_t *m = g_malloc0(sizeof(dt_remote_module_t));
  m->op = g_strdup("temperature");
  m->instance = 0;
  m->instance_name = g_strdup("");
  m->display_name = g_strdup("white balance");
  m->enabled = TRUE;
  m->deprecated = FALSE;
  m->supports_multiple_instances = FALSE;
  g_ptr_array_add(arr, m);
  *out = arr;
  return TRUE;
}

/* --- set_module_params stubs (plan step 7) ------------------------------ */

// Like stub_get_module_schema_exposure but with the "black" field the wire
// contract's set_module_params example patches alongside "exposure" -- kept
// separate so the get_module_schema fixture diff stays untouched.
static gboolean stub_get_module_primitive_schema_exposure_full(const char *op,
                                                               dt_remote_module_schema_t **out,
                                                               dt_remote_error_t **error)
{
  dt_remote_module_schema_t *schema = NULL;
  if(!stub_get_module_schema_exposure(op, &schema, error)) return FALSE;

  dt_remote_field_t *black = _make_field("black", "black level correction", "float", TRUE);
  black->has_range = TRUE;
  black->minimum = -0.1;
  black->maximum = 0.1;
  black->has_default = TRUE;
  black->default_value.type = DT_REMOTE_VALUE_FLOAT;
  black->default_value.v.f = 0.0;
  g_ptr_array_add(schema->fields, black);

  // a known-but-not-writable field, for the unsupported_field path.
  g_ptr_array_add(schema->fields, _make_field("curve", "tone curve points", "array", FALSE));

  *out = schema;
  return TRUE;
}

// filmicrgb's "version" field: known to the schema (present, per the
// "never omitted" rule) but forced writable:false by the per-op denylist
// (dt_remote_denylist_for_op(), Task 1) -- exercises the same
// unsupported_field handler path as
// stub_get_module_primitive_schema_exposure_full's "curve", using a real
// denylisted op/field pair instead of a naturally non-scalar type.
static gboolean stub_get_module_primitive_schema_filmicrgb_denylisted(
  const char *op, dt_remote_module_schema_t **out, dt_remote_error_t **error)
{
  (void)error;
  dt_remote_module_schema_t *schema = g_malloc0(sizeof(dt_remote_module_schema_t));
  schema->op = g_strdup(op);
  schema->display_name = g_strdup("filmic rgb");
  schema->params_version = 6;
  schema->deprecated = FALSE;
  schema->supports_multiple_instances = FALSE;
  schema->fields = g_ptr_array_new_with_free_func(dt_remote_field_free);

  g_ptr_array_add(schema->fields, _make_field("version", "color science", "enum", FALSE));

  *out = schema;
  return TRUE;
}

static const dt_remote_patch_entry_t *_find_patch_entry(const dt_remote_patch_t *patch, const char *name)
{
  for(guint i = 0; patch->scalar_values && i < patch->scalar_values->len; i++)
  {
    const dt_remote_patch_entry_t *e = g_ptr_array_index(patch->scalar_values, i);
    if(!g_strcmp0(e->name, name)) return e;
  }
  return NULL;
}

static dt_remote_patch_entry_t *_make_result_entry(const char *name, dt_remote_value_t value)
{
  dt_remote_patch_entry_t *e = g_malloc0(sizeof(dt_remote_patch_entry_t));
  e->name = g_strdup(name);
  e->value = value;
  return e;
}

// Success stub for the wire-contract example: asserts the handler decoded
// the request into exactly the neutral patch the engine must receive
// (values, expected_revision, enable tri-state), then answers with a canned
// "read back from live state" result matching set_module_params_response.json.
static gboolean stub_set_module_params_contract(const dt_remote_module_ref_t *ref,
                                                const dt_remote_patch_t *patch,
                                                const uint64_t *expected_revision,
                                                dt_remote_mutation_result_t **out,
                                                dt_remote_error_t **error)
{
  (void)error;
  assert_string_equal(ref->op, "exposure");
  assert_int_equal(ref->instance, 0);

  assert_non_null(patch);
  assert_non_null(patch->scalar_values);
  assert_int_equal(patch->scalar_values->len, 2);
  const dt_remote_patch_entry_t *exposure = _find_patch_entry(patch, "exposure");
  const dt_remote_patch_entry_t *black = _find_patch_entry(patch, "black");
  assert_non_null(exposure);
  assert_non_null(black);
  assert_int_equal(exposure->value.type, DT_REMOTE_VALUE_FLOAT);
  assert_float_equal(exposure->value.v.f, 0.7, 1e-9);
  assert_int_equal(black->value.type, DT_REMOTE_VALUE_FLOAT);
  assert_float_equal(black->value.v.f, -0.002, 1e-9);

  assert_non_null(expected_revision);
  assert_int_equal((int)*expected_revision, 31);
  assert_true(patch->has_enable);
  assert_true(patch->enable);

  dt_remote_mutation_result_t *result = g_malloc0(sizeof(dt_remote_mutation_result_t));
  result->op = g_strdup("exposure");
  result->instance = 0;
  result->instance_name = g_strdup("");
  result->enabled = TRUE;
  result->values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);
  g_ptr_array_add(result->values, _make_result_entry("exposure", (dt_remote_value_t){
    .type = DT_REMOTE_VALUE_FLOAT, .v.f = 0.7 }));
  g_ptr_array_add(result->values, _make_result_entry("black", (dt_remote_value_t){
    .type = DT_REMOTE_VALUE_FLOAT, .v.f = -0.002 }));
  result->revision = 32;

  *out = result;
  return TRUE;
}

// Asserts the handler resolved the enum member (by stable name or by
// integer -- the two request spellings both funnel into this) to a fully
// formed {value, name} pair, and that expected_revision/enable were absent.
static gboolean stub_set_module_params_enum_deflicker(const dt_remote_module_ref_t *ref,
                                                      const dt_remote_patch_t *patch,
                                                      const uint64_t *expected_revision,
                                                      dt_remote_mutation_result_t **out,
                                                      dt_remote_error_t **error)
{
  (void)error;
  assert_string_equal(ref->op, "exposure");
  assert_null(expected_revision);
  assert_false(patch->has_enable);

  assert_int_equal(patch->scalar_values->len, 1);
  const dt_remote_patch_entry_t *mode = _find_patch_entry(patch, "mode");
  assert_non_null(mode);
  assert_int_equal(mode->value.type, DT_REMOTE_VALUE_ENUM);
  assert_int_equal(mode->value.v.e.value, 1);
  assert_string_equal(mode->value.v.e.name, "EXPOSURE_MODE_DEFLICKER");

  dt_remote_mutation_result_t *result = g_malloc0(sizeof(dt_remote_mutation_result_t));
  result->op = g_strdup("exposure");
  result->instance = 0;
  result->instance_name = g_strdup("");
  result->enabled = TRUE;
  result->values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);
  g_ptr_array_add(result->values, _make_result_entry("mode", (dt_remote_value_t){
    .type = DT_REMOTE_VALUE_ENUM, .v.e = { 1, g_strdup("EXPOSURE_MODE_DEFLICKER") } }));
  result->revision = 33;

  *out = result;
  return TRUE;
}

static gboolean stub_set_module_params_invalid_value(const dt_remote_module_ref_t *ref,
                                                     const dt_remote_patch_t *patch,
                                                     const uint64_t *expected_revision,
                                                     dt_remote_mutation_result_t **out,
                                                     dt_remote_error_t **error)
{
  (void)ref;
  (void)patch;
  (void)expected_revision;
  (void)out;
  if(error)
    *error = _make_error(DT_REMOTE_ERR_INVALID_VALUE,
                         g_strdup("value out of range for field 'exposure'"));
  return FALSE;
}

static gboolean stub_set_module_params_revision_conflict(const dt_remote_module_ref_t *ref,
                                                         const dt_remote_patch_t *patch,
                                                         const uint64_t *expected_revision,
                                                         dt_remote_mutation_result_t **out,
                                                         dt_remote_error_t **error)
{
  (void)ref;
  (void)patch;
  (void)out;
  assert_non_null(expected_revision);
  if(error)
    *error = _make_error(DT_REMOTE_ERR_REVISION_CONFLICT,
                         g_strdup_printf("expected revision %d does not match current state",
                                         (int)*expected_revision));
  return FALSE;
}

// unknown_instance comes from the engine (schema resolution already
// succeeded for the op; only the live darkroom knows which multi-instances
// exist), so unlike unknown_module this stub *is* reached.
static gboolean stub_set_module_params_unknown_instance(const dt_remote_module_ref_t *ref,
                                                        const dt_remote_patch_t *patch,
                                                        const uint64_t *expected_revision,
                                                        dt_remote_mutation_result_t **out,
                                                        dt_remote_error_t **error)
{
  (void)patch;
  (void)expected_revision;
  (void)out;
  assert_non_null(ref);
  if(error)
    *error = _make_error(DT_REMOTE_ERR_UNKNOWN_INSTANCE,
                         g_strdup_printf("module '%s' has no instance %d", ref->op, ref->instance));
  return FALSE;
}

// For handler-side rejection paths (unknown field, bad value shape, bad
// params): the engine must never be reached -- whole-patch atomicity starts
// at the protocol boundary.
static gboolean stub_set_module_params_must_not_be_called(const dt_remote_module_ref_t *ref,
                                                          const dt_remote_patch_t *patch,
                                                          const uint64_t *expected_revision,
                                                          dt_remote_mutation_result_t **out,
                                                          dt_remote_error_t **error)
{
  (void)ref;
  (void)patch;
  (void)expected_revision;
  (void)out;
  (void)error;
  fail_msg("set_module_params engine call must not be reached for a request the handler rejects");
  return FALSE;
}

/* --- set_module_params semantic curve stubs (milestone 2 Task 9) -------- */

// The set handler resolves scalar values through the *primitive* schema
// seam, and every semantic curve request here carries empty scalar values --
// a minimal fieldless primitive schema is the honest stub.
static gboolean stub_get_module_primitive_schema_rgbcurve(const char *op,
                                                          dt_remote_module_schema_t **out,
                                                          dt_remote_error_t **error)
{
  (void)error;
  dt_remote_module_schema_t *schema = g_malloc0(sizeof(dt_remote_module_schema_t));
  schema->op = g_strdup(op);
  schema->display_name = g_strdup("rgb curve");
  schema->params_version = 1;
  schema->supports_multiple_instances = TRUE;
  schema->fields = g_ptr_array_new_with_free_func(dt_remote_field_free);
  *out = schema;
  return TRUE;
}

// The four points of the curve-design SS Mutation request example, shared
// between the request-decode assertions and the canned read-back result.
static const dt_remote_curve_point_t CURVE_MUTATION_POINTS[] = {
  { 0.0, 0.0 }, { 0.25, 0.18 }, { 0.75, 0.82 }, { 1.0, 1.0 }
};

static dt_remote_curve_value_t *_make_written_master_curve_value(void)
{
  dt_remote_curve_value_t *value = g_new0(dt_remote_curve_value_t, 1);
  value->name = g_strdup("curve.master");
  value->points = g_array_new(FALSE, FALSE, sizeof(dt_remote_curve_point_t));
  g_array_append_vals(value->points, CURVE_MUTATION_POINTS, G_N_ELEMENTS(CURVE_MUTATION_POINTS));
  value->interpolation = DT_REMOTE_CURVE_MONOTONE_HERMITE;
  value->active = TRUE;
  value->effective = TRUE;
  value->writable_now = TRUE;
  return value;
}

// Success stub for the curve mutation wire-contract example: asserts the
// handler decoded semantic_values into exactly the neutral semantic patch
// the engine must receive (one curve entry, four points, explicit
// interpolation, no scalar entries), then answers with a canned "read back
// from projected state" result matching set_module_params_curve_response.json.
static gboolean stub_set_module_params_curve_master(const dt_remote_module_ref_t *ref,
                                                    const dt_remote_patch_t *patch,
                                                    const uint64_t *expected_revision,
                                                    dt_remote_mutation_result_t **out,
                                                    dt_remote_error_t **error)
{
  (void)error;
  assert_string_equal(ref->op, "rgbcurve");
  assert_int_equal(ref->instance, 0);

  assert_non_null(patch);
  assert_true(!patch->scalar_values || patch->scalar_values->len == 0);
  assert_non_null(patch->semantic_values);
  assert_int_equal(patch->semantic_values->len, 1);

  const dt_remote_semantic_patch_t *semantic = g_ptr_array_index(patch->semantic_values, 0);
  assert_int_equal(semantic->class_id, DT_REMOTE_PARAMETER_CURVE);
  assert_string_equal(semantic->value.curve.name, "curve.master");
  assert_non_null(semantic->value.curve.points);
  assert_int_equal(semantic->value.curve.points->len, G_N_ELEMENTS(CURVE_MUTATION_POINTS));
  for(guint i = 0; i < G_N_ELEMENTS(CURVE_MUTATION_POINTS); i++)
  {
    const dt_remote_curve_point_t p =
      g_array_index(semantic->value.curve.points, dt_remote_curve_point_t, i);
    assert_float_equal(p.x, CURVE_MUTATION_POINTS[i].x, 1e-12);
    assert_float_equal(p.y, CURVE_MUTATION_POINTS[i].y, 1e-12);
  }
  assert_true(semantic->value.curve.has_interpolation);
  assert_int_equal(semantic->value.curve.interpolation, DT_REMOTE_CURVE_MONOTONE_HERMITE);

  assert_non_null(expected_revision);
  assert_int_equal((int)*expected_revision, 31);
  assert_true(patch->has_enable);
  assert_true(patch->enable);

  dt_remote_mutation_result_t *result = g_malloc0(sizeof(dt_remote_mutation_result_t));
  result->op = g_strdup("rgbcurve");
  result->instance = 0;
  result->instance_name = g_strdup("");
  result->enabled = TRUE;
  result->values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);
  result->semantic_values =
    g_hash_table_new_full(g_str_hash, g_str_equal, g_free, dt_remote_semantic_value_free);
  g_hash_table_insert(result->semantic_values, g_strdup("curve.master"),
                      dt_remote_semantic_value_wrap_curve(_make_written_master_curve_value()));
  result->revision = 32;

  *out = result;
  return TRUE;
}

// Engine-produced validator rejection (curve design SS Validation algorithm
// item 6): the parse layer forwards the two-point patch, the engine answers
// invalid_value with the structured details object the validator attaches --
// the dispatcher must map details_json onto the wire "details" member.
static gboolean stub_set_module_params_curve_invalid_spacing(const dt_remote_module_ref_t *ref,
                                                             const dt_remote_patch_t *patch,
                                                             const uint64_t *expected_revision,
                                                             dt_remote_mutation_result_t **out,
                                                             dt_remote_error_t **error)
{
  (void)ref;
  (void)expected_revision;
  (void)out;
  assert_non_null(patch->semantic_values);
  assert_int_equal(patch->semantic_values->len, 1);
  const dt_remote_semantic_patch_t *semantic = g_ptr_array_index(patch->semantic_values, 0);
  assert_int_equal(semantic->value.curve.points->len, 2);
  assert_false(semantic->value.curve.has_interpolation);

  if(error)
  {
    *error = _make_error(DT_REMOTE_ERR_INVALID_VALUE,
                         g_strdup("curve 'curve.master' points 0 and 1 are 0.002 apart; "
                                  "minimum is 0.0025"));
    (*error)->details_json =
      g_strdup("{\"parameter\":\"curve.master\",\"point_index\":1,\"constraint\":\"adjacent_spacing\"}");
  }
  return FALSE;
}

// Unknown semantic IDs are an engine rejection, not a parse rejection: the
// protocol layer has no descriptor knowledge, so "curve.alpha" must reach
// the engine and come back as unknown_field.
static gboolean stub_set_module_params_curve_unknown_id(const dt_remote_module_ref_t *ref,
                                                        const dt_remote_patch_t *patch,
                                                        const uint64_t *expected_revision,
                                                        dt_remote_mutation_result_t **out,
                                                        dt_remote_error_t **error)
{
  (void)ref;
  (void)expected_revision;
  (void)out;
  assert_non_null(patch->semantic_values);
  assert_int_equal(patch->semantic_values->len, 1);
  const dt_remote_semantic_patch_t *semantic = g_ptr_array_index(patch->semantic_values, 0);
  assert_string_equal(semantic->value.curve.name, "curve.alpha");

  if(error)
    *error = _make_error(DT_REMOTE_ERR_UNKNOWN_FIELD,
                         g_strdup("unknown semantic curve 'curve.alpha'"));
  return FALSE;
}

/* --- set_module_params semantic vector stubs (milestone 4 Task 2) ------- */

// Success stub for the vector parse-accept test: asserts the handler decoded
// semantic_values into exactly one DT_REMOTE_PARAMETER_VECTOR entry with the
// four components from the request, in order, preserved as doubles. The
// result is deliberately minimal (no semantic readback) -- serializing a
// vector value/schema back onto the wire is milestone 4 Task 4's job, not
// the parser's.
static gboolean stub_set_module_params_vector_capture(const dt_remote_module_ref_t *ref,
                                                       const dt_remote_patch_t *patch,
                                                       const uint64_t *expected_revision,
                                                       dt_remote_mutation_result_t **out,
                                                       dt_remote_error_t **error)
{
  (void)error;
  (void)expected_revision;
  assert_non_null(patch);
  assert_non_null(patch->semantic_values);
  assert_int_equal(patch->semantic_values->len, 1);

  const dt_remote_semantic_patch_t *semantic = g_ptr_array_index(patch->semantic_values, 0);
  assert_int_equal(semantic->class_id, DT_REMOTE_PARAMETER_VECTOR);
  assert_string_equal(semantic->value.vector.name, "vector.example");
  assert_non_null(semantic->value.vector.values);

  static const double expected[] = { 1.0, 1.1, 1.0, 0.95 };
  assert_int_equal(semantic->value.vector.values->len, (int)G_N_ELEMENTS(expected));
  for(guint i = 0; i < G_N_ELEMENTS(expected); i++)
    assert_float_equal(g_array_index(semantic->value.vector.values, double, i), expected[i], 1e-12);

  dt_remote_mutation_result_t *result = g_malloc0(sizeof(dt_remote_mutation_result_t));
  result->op = g_strdup(ref->op);
  result->instance = ref->instance;
  result->instance_name = g_strdup("");
  result->enabled = TRUE;
  result->values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);
  result->revision = 1;
  *out = result;
  return TRUE;
}

// double-domain proof stub (curve design rationale, mirrored for vectors):
// a component that would narrow cleanly to float (2.00000001 -> 2.0f) must
// survive the parser as its un-narrowed double -- validating in float
// domain would let a slightly out-of-range number round back into range
// and pass.
static gboolean stub_set_module_params_vector_precision(const dt_remote_module_ref_t *ref,
                                                         const dt_remote_patch_t *patch,
                                                         const uint64_t *expected_revision,
                                                         dt_remote_mutation_result_t **out,
                                                         dt_remote_error_t **error)
{
  (void)error;
  (void)expected_revision;
  assert_non_null(patch);
  assert_non_null(patch->semantic_values);
  assert_int_equal(patch->semantic_values->len, 1);

  const dt_remote_semantic_patch_t *semantic = g_ptr_array_index(patch->semantic_values, 0);
  assert_int_equal(semantic->class_id, DT_REMOTE_PARAMETER_VECTOR);
  assert_non_null(semantic->value.vector.values);
  assert_int_equal(semantic->value.vector.values->len, 1);
  assert_true(g_array_index(semantic->value.vector.values, double, 0) > 2.0);

  dt_remote_mutation_result_t *result = g_malloc0(sizeof(dt_remote_mutation_result_t));
  result->op = g_strdup(ref->op);
  result->instance = ref->instance;
  result->instance_name = g_strdup("");
  result->enabled = TRUE;
  result->values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);
  result->revision = 1;
  *out = result;
  return TRUE;
}

/* --- set_module_params semantic bands stubs (milestone 5 Task 2) -------- */

// Success stub for the bands parse-accept test (no "x"): asserts the
// handler decoded semantic_values into exactly one DT_REMOTE_PARAMETER_BANDS
// entry with the six y samples from the request, in order, preserved as
// doubles, and "x" left NULL since the request omitted it.
static gboolean stub_set_module_params_bands_capture(const dt_remote_module_ref_t *ref,
                                                      const dt_remote_patch_t *patch,
                                                      const uint64_t *expected_revision,
                                                      dt_remote_mutation_result_t **out,
                                                      dt_remote_error_t **error)
{
  (void)error;
  (void)expected_revision;
  assert_non_null(patch);
  assert_non_null(patch->semantic_values);
  assert_int_equal(patch->semantic_values->len, 1);

  const dt_remote_semantic_patch_t *semantic = g_ptr_array_index(patch->semantic_values, 0);
  assert_int_equal(semantic->class_id, DT_REMOTE_PARAMETER_BANDS);
  assert_string_equal(semantic->value.bands.name, "bands.example");
  assert_non_null(semantic->value.bands.y);

  static const double expected[] = { 0.5, 0.5, 0.5, 0.5, 0.5, 0.5 };
  assert_int_equal(semantic->value.bands.y->len, (int)G_N_ELEMENTS(expected));
  for(guint i = 0; i < G_N_ELEMENTS(expected); i++)
    assert_float_equal(g_array_index(semantic->value.bands.y, double, i), expected[i], 1e-12);
  assert_null(semantic->value.bands.x);

  dt_remote_mutation_result_t *result = g_malloc0(sizeof(dt_remote_mutation_result_t));
  result->op = g_strdup(ref->op);
  result->instance = ref->instance;
  result->instance_name = g_strdup("");
  result->enabled = TRUE;
  result->values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);
  result->revision = 1;
  *out = result;
  return TRUE;
}

// Success stub for the bands parse-accept test with "x" present: asserts
// both y and x decode to six doubles each, in order.
static gboolean stub_set_module_params_bands_with_x_capture(const dt_remote_module_ref_t *ref,
                                                             const dt_remote_patch_t *patch,
                                                             const uint64_t *expected_revision,
                                                             dt_remote_mutation_result_t **out,
                                                             dt_remote_error_t **error)
{
  (void)error;
  (void)expected_revision;
  assert_non_null(patch);
  assert_non_null(patch->semantic_values);
  assert_int_equal(patch->semantic_values->len, 1);

  const dt_remote_semantic_patch_t *semantic = g_ptr_array_index(patch->semantic_values, 0);
  assert_int_equal(semantic->class_id, DT_REMOTE_PARAMETER_BANDS);
  assert_string_equal(semantic->value.bands.name, "bands.example");

  static const double expected_y[] = { 0.5, 0.5, 0.5, 0.5, 0.5, 0.5 };
  assert_non_null(semantic->value.bands.y);
  assert_int_equal(semantic->value.bands.y->len, (int)G_N_ELEMENTS(expected_y));
  for(guint i = 0; i < G_N_ELEMENTS(expected_y); i++)
    assert_float_equal(g_array_index(semantic->value.bands.y, double, i), expected_y[i], 1e-12);

  static const double expected_x[] = { 0.0, 0.2, 0.4, 0.6, 0.8, 1.0 };
  assert_non_null(semantic->value.bands.x);
  assert_int_equal(semantic->value.bands.x->len, (int)G_N_ELEMENTS(expected_x));
  for(guint i = 0; i < G_N_ELEMENTS(expected_x); i++)
    assert_float_equal(g_array_index(semantic->value.bands.x, double, i), expected_x[i], 1e-12);

  dt_remote_mutation_result_t *result = g_malloc0(sizeof(dt_remote_mutation_result_t));
  result->op = g_strdup(ref->op);
  result->instance = ref->instance;
  result->instance_name = g_strdup("");
  result->enabled = TRUE;
  result->values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);
  result->revision = 1;
  *out = result;
  return TRUE;
}

// double-domain proof stub (curve/vector design rationale, mirrored for
// bands): a sample that would narrow cleanly to float (0.50000001 -> 0.5f)
// must survive the parser as its un-narrowed double.
static gboolean stub_set_module_params_bands_precision(const dt_remote_module_ref_t *ref,
                                                        const dt_remote_patch_t *patch,
                                                        const uint64_t *expected_revision,
                                                        dt_remote_mutation_result_t **out,
                                                        dt_remote_error_t **error)
{
  (void)error;
  (void)expected_revision;
  assert_non_null(patch);
  assert_non_null(patch->semantic_values);
  assert_int_equal(patch->semantic_values->len, 1);

  const dt_remote_semantic_patch_t *semantic = g_ptr_array_index(patch->semantic_values, 0);
  assert_int_equal(semantic->class_id, DT_REMOTE_PARAMETER_BANDS);
  assert_non_null(semantic->value.bands.y);
  assert_int_equal(semantic->value.bands.y->len, 1);
  assert_true(g_array_index(semantic->value.bands.y, double, 0) > 0.5);

  dt_remote_mutation_result_t *result = g_malloc0(sizeof(dt_remote_mutation_result_t));
  result->op = g_strdup(ref->op);
  result->instance = ref->instance;
  result->instance_name = g_strdup("");
  result->enabled = TRUE;
  result->values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);
  result->revision = 1;
  *out = result;
  return TRUE;
}

/* --- set_module_params semantic quantity stubs (milestone 6 Task 1) ---- */

// Success stub for the quantity parse-accept test: asserts the handler
// decoded semantic_values into exactly one DT_REMOTE_PARAMETER_QUANTITY
// entry with the two named components from the request, in wire order,
// preserved as doubles.
static gboolean stub_set_module_params_quantity_capture(const dt_remote_module_ref_t *ref,
                                                         const dt_remote_patch_t *patch,
                                                         const uint64_t *expected_revision,
                                                         dt_remote_mutation_result_t **out,
                                                         dt_remote_error_t **error)
{
  (void)error;
  (void)expected_revision;
  assert_non_null(patch);
  assert_non_null(patch->semantic_values);
  assert_int_equal(patch->semantic_values->len, 1);

  const dt_remote_semantic_patch_t *semantic = g_ptr_array_index(patch->semantic_values, 0);
  assert_int_equal(semantic->class_id, DT_REMOTE_PARAMETER_QUANTITY);
  assert_string_equal(semantic->value.quantity.name, "wb.temperature");
  assert_non_null(semantic->value.quantity.values);
  assert_int_equal(semantic->value.quantity.values->len, 2);

  const dt_remote_quantity_component_value_t *c0 =
    g_ptr_array_index(semantic->value.quantity.values, 0);
  assert_string_equal(c0->name, "temperature");
  assert_float_equal(c0->value, 5500.0, 1e-12);

  const dt_remote_quantity_component_value_t *c1 =
    g_ptr_array_index(semantic->value.quantity.values, 1);
  assert_string_equal(c1->name, "tint");
  assert_float_equal(c1->value, 1.0, 1e-12);

  dt_remote_mutation_result_t *result = g_malloc0(sizeof(dt_remote_mutation_result_t));
  result->op = g_strdup(ref->op);
  result->instance = ref->instance;
  result->instance_name = g_strdup("");
  result->enabled = TRUE;
  result->values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);
  result->revision = 1;
  *out = result;
  return TRUE;
}

// double-domain proof stub (curve/vector/bands design rationale, mirrored
// for quantity): a component value that would narrow cleanly to float
// (5500.00000001 -> 5500.0f) must survive the parser as its un-narrowed
// double.
static gboolean stub_set_module_params_quantity_precision(const dt_remote_module_ref_t *ref,
                                                           const dt_remote_patch_t *patch,
                                                           const uint64_t *expected_revision,
                                                           dt_remote_mutation_result_t **out,
                                                           dt_remote_error_t **error)
{
  (void)error;
  (void)expected_revision;
  assert_non_null(patch);
  assert_non_null(patch->semantic_values);
  assert_int_equal(patch->semantic_values->len, 1);

  const dt_remote_semantic_patch_t *semantic = g_ptr_array_index(patch->semantic_values, 0);
  assert_int_equal(semantic->class_id, DT_REMOTE_PARAMETER_QUANTITY);
  assert_non_null(semantic->value.quantity.values);
  assert_int_equal(semantic->value.quantity.values->len, 1);

  const dt_remote_quantity_component_value_t *c0 =
    g_ptr_array_index(semantic->value.quantity.values, 0);
  assert_string_equal(c0->name, "temperature");
  assert_true(c0->value > 5500.0);

  dt_remote_mutation_result_t *result = g_malloc0(sizeof(dt_remote_mutation_result_t));
  result->op = g_strdup(ref->op);
  result->instance = ref->instance;
  result->instance_name = g_strdup("");
  result->enabled = TRUE;
  result->values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);
  result->revision = 1;
  *out = result;
  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* hello                                                                    */
/* ---------------------------------------------------------------------- */

// darktable_version and pid are host/build-specific, so hello's success
// response is checked field-by-field against the running process rather
// than diffed against a static fixture (hello_response.json is kept for
// shape documentation and Python reuse, mirroring how the protocol
// reference itself shows "darktable_version": "5.x" illustratively).
static void test_hello_success(void **state)
{
  (void)state;
  JsonNode *request_node = _load_fixture("hello_request.json");
  JsonObject *request = json_node_get_object(request_node);

  JsonNode *actual = dt_remote_protocol_dispatch(request, NULL);
  assert_non_null(actual);
  JsonObject *resp = json_node_get_object(actual);

  assert_int_equal(json_object_get_int_member(resp, "id"), 1);
  assert_true(json_object_get_boolean_member(resp, "ok"));

  JsonObject *result = json_object_get_object_member(resp, "result");
  assert_int_equal(json_object_get_int_member(result, "protocol_version"), DT_REMOTE_PROTOCOL_VERSION);
  assert_string_equal(json_object_get_string_member(result, "darktable_version"), darktable_package_version);
  assert_int_equal(json_object_get_int_member(result, "pid"), (gint64)getpid());
  // "params" (step 7) + "semantic_params"/"curve_params" (milestone 2:
  // capability-gated semantic curve read/write) + "vector_params"
  // (milestone 4: capability-gated semantic vector read/write) +
  // "band_params" (milestone 5: capability-gated semantic band read/write) +
  // "quantity_params" (milestone 6: capability-gated semantic quantity
  // read/write) + "instances"/"history" (step 8) + "preview" (step 9) +
  // "scopes" (step 10) -- the full capability set.
  JsonArray *caps = json_object_get_array_member(result, "capabilities");
  assert_int_equal(json_array_get_length(caps), 10);
  assert_string_equal(json_array_get_string_element(caps, 0), "params");
  assert_string_equal(json_array_get_string_element(caps, 1), "semantic_params");
  assert_string_equal(json_array_get_string_element(caps, 2), "curve_params");
  assert_string_equal(json_array_get_string_element(caps, 3), "vector_params");
  assert_string_equal(json_array_get_string_element(caps, 4), "band_params");
  assert_string_equal(json_array_get_string_element(caps, 5), "quantity_params");
  assert_string_equal(json_array_get_string_element(caps, 6), "instances");
  assert_string_equal(json_array_get_string_element(caps, 7), "history");
  assert_string_equal(json_array_get_string_element(caps, 8), "preview");
  assert_string_equal(json_array_get_string_element(caps, 9), "scopes");

  json_node_unref(actual);
  json_node_unref(request_node);
}

static void test_hello_error_bad_protocol_version(void **state)
{
  (void)state;
  _assert_dispatch_matches("hello_error_bad_protocol_version_request.json",
                           "hello_error_bad_protocol_version_response.json");
}

static void test_hello_error_missing_token(void **state)
{
  (void)state;
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "id");
  json_builder_add_int_value(b, 1);
  json_builder_set_member_name(b, "method");
  json_builder_add_string_value(b, "hello");
  json_builder_set_member_name(b, "params");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "protocol_version");
  json_builder_add_int_value(b, 1);
  json_builder_set_member_name(b, "client");
  json_builder_add_string_value(b, "test");
  json_builder_end_object(b);
  json_builder_end_object(b);
  JsonNode *req = json_builder_get_root(b);
  g_object_unref(b);

  JsonNode *actual = dt_remote_protocol_dispatch(json_node_get_object(req), NULL);
  assert_non_null(actual);
  JsonObject *resp = json_node_get_object(actual);
  assert_false(json_object_get_boolean_member(resp, "ok"));
  assert_string_equal(json_object_get_string_member(json_object_get_object_member(resp, "error"), "code"),
                      "invalid_value");

  json_node_unref(actual);
  json_node_unref(req);
}

static void test_hello_error_unknown_key(void **state)
{
  (void)state;
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "id");
  json_builder_add_int_value(b, 1);
  json_builder_set_member_name(b, "method");
  json_builder_add_string_value(b, "hello");
  json_builder_set_member_name(b, "params");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "protocol_version");
  json_builder_add_int_value(b, 1);
  json_builder_set_member_name(b, "token");
  json_builder_add_string_value(b, "t");
  json_builder_set_member_name(b, "client");
  json_builder_add_string_value(b, "c");
  json_builder_set_member_name(b, "surprise");
  json_builder_add_int_value(b, 1);
  json_builder_end_object(b);
  json_builder_end_object(b);
  JsonNode *req = json_builder_get_root(b);
  g_object_unref(b);

  JsonNode *actual = dt_remote_protocol_dispatch(json_node_get_object(req), NULL);
  assert_non_null(actual);
  JsonObject *resp = json_node_get_object(actual);
  assert_false(json_object_get_boolean_member(resp, "ok"));
  JsonObject *error = json_object_get_object_member(resp, "error");
  assert_string_equal(json_object_get_string_member(error, "code"), "invalid_value");
  assert_true(strstr(json_object_get_string_member(error, "message"), "surprise") != NULL);

  json_node_unref(actual);
  json_node_unref(req);
}

/* ---------------------------------------------------------------------- */
/* get_state                                                                */
/* ---------------------------------------------------------------------- */

static void test_get_state_darkroom(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .get_state = stub_get_state_darkroom };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("get_state_request.json", "get_state_response_darkroom.json");
  dt_remote_protocol_set_calls(NULL);
}

static void test_get_state_no_image(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .get_state = stub_get_state_no_image };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("get_state_request.json", "get_state_response_no_image.json");
  dt_remote_protocol_set_calls(NULL);
}

// binding: get_state has no invalid_value row in the error-codes-by-method
// matrix -- garbage params must be silently ignored, not rejected.
static void test_get_state_ignores_garbage_params(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .get_state = stub_get_state_no_image };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("get_state_ignores_garbage_params_request.json", "get_state_response_no_image.json");
  dt_remote_protocol_set_calls(NULL);
}

/* ---------------------------------------------------------------------- */
/* list_modules                                                             */
/* ---------------------------------------------------------------------- */

static void test_list_modules_success(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .list_modules = stub_list_modules_one_exposure,
    .get_state = stub_get_state_revision31_no_image,
  };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("list_modules_request.json", "list_modules_response.json");
  dt_remote_protocol_set_calls(NULL);
}

static void test_list_modules_error_not_in_darkroom(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .list_modules = stub_list_modules_not_in_darkroom };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("list_modules_request.json", "list_modules_error_not_in_darkroom_response.json");
  dt_remote_protocol_set_calls(NULL);
}

// binding: list_modules shares get_state's empty invalid_value row.
static void test_list_modules_ignores_garbage_params(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .list_modules = stub_list_modules_one_exposure,
    .get_state = stub_get_state_revision31_no_image,
  };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("list_modules_ignores_garbage_params_request.json", "list_modules_response.json");
  dt_remote_protocol_set_calls(NULL);
}

/* ---------------------------------------------------------------------- */
/* get_module_schema                                                        */
/* ---------------------------------------------------------------------- */

static void test_get_module_schema_success(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .get_module_schema = stub_get_module_schema_exposure };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("get_module_schema_request.json", "get_module_schema_response.json");
  dt_remote_protocol_set_calls(NULL);
}

static void test_get_module_schema_error_unknown_module(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .get_module_schema = stub_get_module_schema_unknown };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("get_module_schema_error_unknown_module_request.json",
                           "get_module_schema_error_unknown_module_response.json");
  dt_remote_protocol_set_calls(NULL);
}

static void test_get_module_schema_error_internal_is_error_envelope(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .get_module_schema = stub_get_module_schema_internal };
  dt_remote_protocol_set_calls(&calls);

  JsonNode *request_node = _load_fixture("get_module_schema_request.json");
  JsonNode *actual = dt_remote_protocol_dispatch(json_node_get_object(request_node), NULL);
  assert_non_null(actual);

  JsonObject *response = json_node_get_object(actual);
  assert_false(json_object_get_boolean_member(response, "ok"));
  assert_false(json_object_has_member(response, "result"));
  JsonObject *error = json_object_get_object_member(response, "error");
  assert_non_null(error);
  assert_string_equal(json_object_get_string_member(error, "code"), "internal");
  assert_true(strstr(json_object_get_string_member(error, "message"), "curve registry") != NULL);

  json_node_unref(actual);
  json_node_unref(request_node);
  dt_remote_protocol_set_calls(NULL);
}

// wrong JSON type for the whole `params` blob: the handler still requires
// "module", so it is reported as "missing" -- the dispatcher never even
// attempts to interpret the non-object params value as a module name.
static void test_get_module_schema_error_wrong_type_params(void **state)
{
  (void)state;
  _assert_dispatch_matches("error_wrong_type_params_request.json", "error_wrong_type_params_response.json");
}

static void test_get_module_schema_error_overlong_string(void **state)
{
  (void)state;
  GString *huge = g_string_new(NULL);
  for(int i = 0; i < 5000; i++) g_string_append_c(huge, 'x');

  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "id");
  json_builder_add_int_value(b, 10);
  json_builder_set_member_name(b, "method");
  json_builder_add_string_value(b, "get_module_schema");
  json_builder_set_member_name(b, "params");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "module");
  json_builder_add_string_value(b, huge->str);
  json_builder_end_object(b);
  json_builder_end_object(b);
  JsonNode *req = json_builder_get_root(b);
  g_object_unref(b);
  g_string_free(huge, TRUE);

  JsonNode *actual = dt_remote_protocol_dispatch(json_node_get_object(req), NULL);
  assert_non_null(actual);
  JsonObject *resp = json_node_get_object(actual);
  assert_false(json_object_get_boolean_member(resp, "ok"));
  assert_string_equal(json_object_get_string_member(json_object_get_object_member(resp, "error"), "code"),
                      "invalid_value");

  json_node_unref(actual);
  json_node_unref(req);
}

static void test_get_module_schema_error_unknown_key(void **state)
{
  (void)state;
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "id");
  json_builder_add_int_value(b, 11);
  json_builder_set_member_name(b, "method");
  json_builder_add_string_value(b, "get_module_schema");
  json_builder_set_member_name(b, "params");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "module");
  json_builder_add_string_value(b, "exposure");
  json_builder_set_member_name(b, "bogus");
  json_builder_add_int_value(b, 1);
  json_builder_end_object(b);
  json_builder_end_object(b);
  JsonNode *req = json_builder_get_root(b);
  g_object_unref(b);

  JsonNode *actual = dt_remote_protocol_dispatch(json_node_get_object(req), NULL);
  assert_non_null(actual);
  JsonObject *resp = json_node_get_object(actual);
  assert_false(json_object_get_boolean_member(resp, "ok"));
  JsonObject *error = json_object_get_object_member(resp, "error");
  assert_string_equal(json_object_get_string_member(error, "code"), "invalid_value");
  assert_true(strstr(json_object_get_string_member(error, "message"), "bogus") != NULL);

  json_node_unref(actual);
  json_node_unref(req);
}

/* ---------------------------------------------------------------------- */
/* get_module_params                                                        */
/* ---------------------------------------------------------------------- */

static void test_get_module_params_success(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_params = stub_get_module_params_exposure,
    .list_modules = stub_list_modules_one_exposure,
    .get_state = stub_get_state_revision31_no_image,
  };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("get_module_params_request.json", "get_module_params_response.json");
  dt_remote_protocol_set_calls(NULL);
}

// A stored non-finite float (temperature's NaN `various`) serializes as
// JSON null, keeping the emitted document parseable by strict JSON
// parsers -- json-glib would otherwise generate a bare `nan` token, which
// the Python sidecar rejects as malformed and tears the connection down.
// Finite siblings are unaffected.
static void test_get_module_params_nan_float_serializes_as_null(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_params = stub_get_module_params_nan_float,
    .list_modules = stub_list_modules_one_exposure,
    .get_state = stub_get_state_revision31_no_image,
  };
  dt_remote_protocol_set_calls(&calls);

  JsonNode *request_node = _load_fixture("get_module_params_request.json");
  JsonNode *actual = dt_remote_protocol_dispatch(json_node_get_object(request_node), NULL);
  assert_non_null(actual);

  JsonObject *response = json_node_get_object(actual);
  assert_true(json_object_get_boolean_member(response, "ok"));
  JsonObject *result = json_object_get_object_member(response, "result");
  JsonObject *values = json_object_get_object_member(result, "values");
  assert_true(json_object_has_member(values, "various"));
  assert_true(json_node_is_null(json_object_get_member(values, "various")));
  assert_float_equal(json_object_get_double_member(values, "red"), 2.5, 0.0);

  // The whole response generates to a strictly-valid JSON document: a
  // json-glib parse of the generated text round-trips (a bare `nan` token
  // would fail here exactly as it fails the sidecar's json.loads).
  JsonGenerator *gen = json_generator_new();
  json_generator_set_root(gen, actual);
  gchar *text = json_generator_to_data(gen, NULL);
  g_object_unref(gen);
  assert_null(strstr(text, "nan"));
  JsonParser *parser = json_parser_new();
  GError *parse_error = NULL;
  assert_true(json_parser_load_from_data(parser, text, -1, &parse_error));
  assert_null(parse_error);
  g_object_unref(parser);
  g_free(text);

  json_node_unref(actual);
  json_node_unref(request_node);
  dt_remote_protocol_set_calls(NULL);
}

static void test_get_module_params_error_unknown_module(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .get_module_params = stub_get_module_params_unknown_module };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("get_module_params_error_unknown_module_request.json",
                           "get_module_params_error_unknown_module_response.json");
  dt_remote_protocol_set_calls(NULL);
}

static void test_get_module_params_error_internal_is_error_envelope(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .get_module_params = stub_get_module_params_internal };
  dt_remote_protocol_set_calls(&calls);

  JsonNode *request_node = _load_fixture("get_module_params_request.json");
  JsonNode *actual = dt_remote_protocol_dispatch(json_node_get_object(request_node), NULL);
  assert_non_null(actual);

  JsonObject *response = json_node_get_object(actual);
  assert_false(json_object_get_boolean_member(response, "ok"));
  assert_false(json_object_has_member(response, "result"));
  JsonObject *error = json_object_get_object_member(response, "error");
  assert_non_null(error);
  assert_string_equal(json_object_get_string_member(error, "code"), "internal");
  assert_true(strstr(json_object_get_string_member(error, "message"), "curve registry") != NULL);

  json_node_unref(actual);
  json_node_unref(request_node);
}

/* --- semantic curve read path (milestone 2, task 7) ---------------------- */

// rgbcurve advertises its four semantic curves in an optional
// `semantic_fields` schema member (shape: curve design SS Schema response,
// including represented_by on the native array fields). Ops without a
// registry adapter -- exercised by test_get_module_schema_success above,
// whose exposure fixture is untouched -- emit no such member.
static void test_get_module_schema_rgbcurve_semantic_fields(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .get_module_schema = stub_get_module_schema_rgbcurve };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("get_module_schema_rgbcurve_request.json",
                           "get_module_schema_rgbcurve_response.json");
  dt_remote_protocol_set_calls(NULL);
}

// rgbcurve's four semantic values ride on get_module_params as an optional
// `semantic_values` member (shape: curve design SS Value response): all
// four IDs present, active/writable_now resolved per mode, points as
// {"x","y"} objects, uppercase interpolation names.
static void test_get_module_params_rgbcurve_semantic_values(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_params = stub_get_module_params_rgbcurve,
    .list_modules = stub_list_modules_one_rgbcurve,
    .get_state = stub_get_state_revision31_no_image,
  };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("get_module_params_rgbcurve_request.json",
                           "get_module_params_rgbcurve_response.json");
  dt_remote_protocol_set_calls(NULL);
}

/* --- semantic vector schema/value path (milestone 4, task 4) ------------ */

// borders' hand-built vector schema (subtypes PLAIN/COLOR/LEVELS) rides on
// get_module_schema as `semantic_fields` entries tagged `"class":"vector"`:
// "subtype" always present, "components" with per-component name/minimum/
// maximum always present, "color_space" only for the COLOR entry,
// "ordering" only for the LEVELS entry.
static void test_get_module_schema_vector_semantic_fields(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .get_module_schema = stub_get_module_schema_vector };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("get_module_schema_vector_request.json",
                           "get_module_schema_vector_response.json");
  dt_remote_protocol_set_calls(NULL);
}

// borders' "color" vector value rides on get_module_params as an optional
// `semantic_values` member: class/active/effective/writable_now/values.
static void test_get_module_params_vector_semantic_values(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_params = stub_get_module_params_vector,
    .list_modules = stub_list_modules_one_borders,
    .get_state = stub_get_state_revision31_no_image,
  };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("get_module_params_vector_request.json",
                           "get_module_params_vector_response.json");
  dt_remote_protocol_set_calls(NULL);
}

/* --- semantic band schema/value path (milestone 5, task 4) --------------- */

// lowlight's hand-built band schemas ride on get_module_schema as
// `semantic_fields` entries tagged `"class":"bands"`: "count", "y_range"
// {minimum,maximum} and "x_policy" always present; "min_gap" and
// "x_shared_with" only for the INTERIOR entry; current "x" positions only
// when the schema carries them.
static void test_get_module_schema_band_semantic_fields(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .get_module_schema = stub_get_module_schema_band };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("get_module_schema_band_request.json",
                           "get_module_schema_band_response.json");
  dt_remote_protocol_set_calls(NULL);
}

// lowlight's "bands.transition" band value rides on get_module_params as an
// optional `semantic_values` member: class/active/effective/writable_now
// plus the y and x sample arrays (doubles).
static void test_get_module_params_band_semantic_values(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_params = stub_get_module_params_band,
    .list_modules = stub_list_modules_one_lowlight,
    .get_state = stub_get_state_revision31_no_image,
  };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("get_module_params_band_request.json",
                           "get_module_params_band_response.json");
  dt_remote_protocol_set_calls(NULL);
}

/* --- semantic quantity schema/value path (milestone 6, task 4) ---------- */

// "wb.temperature"'s hand-built quantity schema rides on get_module_schema
// as a `semantic_fields` entry tagged `"class":"quantity"`: "derived" and a
// "components" array always present, each component carrying "unit" only
// when non-NULL; the four coefficient scalars (red/green/blue/various)
// stay writable:true while gaining `represented_by: ["wb.temperature"]` --
// the coexistence exception.
static void test_get_module_schema_quantity_semantic_fields(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .get_module_schema = stub_get_module_schema_quantity };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("get_module_schema_quantity_request.json",
                           "get_module_schema_quantity_response.json");
  dt_remote_protocol_set_calls(NULL);
}

// "wb.temperature"'s quantity value rides on get_module_params as an
// optional `semantic_values` member: class/active/effective/writable_now
// plus the "values" object keyed by component name (doubles).
static void test_get_module_params_quantity_semantic_values(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_params = stub_get_module_params_quantity,
    .list_modules = stub_list_modules_one_temperature,
    .get_state = stub_get_state_revision31_no_image,
  };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("get_module_params_quantity_request.json",
                           "get_module_params_quantity_response.json");
  dt_remote_protocol_set_calls(NULL);
}

// A non-finite quantity component value serializes as JSON null, exactly
// like the scalar path (test_get_module_params_nan_float_serializes_as_null
// above): the read hook's output is not engine-validated, and json-glib
// would emit a bare `nan` token that kills the connection. Finite sibling
// components are unaffected.
static void test_get_module_params_quantity_nan_component_serializes_as_null(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_params = stub_get_module_params_quantity_nan,
    .list_modules = stub_list_modules_one_temperature,
    .get_state = stub_get_state_revision31_no_image,
  };
  dt_remote_protocol_set_calls(&calls);

  JsonNode *request_node = _load_fixture("get_module_params_quantity_request.json");
  JsonNode *actual = dt_remote_protocol_dispatch(json_node_get_object(request_node), NULL);
  assert_non_null(actual);

  JsonObject *response = json_node_get_object(actual);
  assert_true(json_object_get_boolean_member(response, "ok"));
  JsonObject *result = json_object_get_object_member(response, "result");
  JsonObject *semantic = json_object_get_object_member(result, "semantic_values");
  JsonObject *wb = json_object_get_object_member(semantic, "wb.temperature");
  JsonObject *values = json_object_get_object_member(wb, "values");
  assert_true(json_object_has_member(values, "tint"));
  assert_true(json_node_is_null(json_object_get_member(values, "tint")));
  assert_float_equal(json_object_get_double_member(values, "temperature"), 5500.0, 0.0);

  // The whole response generates to a strictly-valid JSON document.
  JsonGenerator *gen = json_generator_new();
  json_generator_set_root(gen, actual);
  gchar *text = json_generator_to_data(gen, NULL);
  g_object_unref(gen);
  assert_null(strstr(text, "nan"));
  JsonParser *parser = json_parser_new();
  GError *parse_error = NULL;
  assert_true(json_parser_load_from_data(parser, text, -1, &parse_error));
  assert_null(parse_error);
  g_object_unref(parser);
  g_free(text);

  json_node_unref(actual);
  json_node_unref(request_node);
  dt_remote_protocol_set_calls(NULL);
}

// "instance": 1e400 parses as a JSON double literal that overflows to
// +Infinity -- exactly the non-finite-number case the protocol reference
// requires rejecting with invalid_value, even though "instance" is
// documented as an integer field.
static void test_get_module_params_error_non_finite_instance(void **state)
{
  (void)state;
  const char *json_text =
    "{\"id\":12,\"method\":\"get_module_params\",\"params\":{\"module\":\"exposure\",\"instance\":1e400}}";
  JsonParser *parser = json_parser_new();
  assert_true(json_parser_load_from_data(parser, json_text, -1, NULL));
  JsonObject *request = json_node_get_object(json_parser_get_root(parser));

  JsonNode *actual = dt_remote_protocol_dispatch(request, NULL);
  assert_non_null(actual);
  JsonObject *resp = json_node_get_object(actual);
  assert_false(json_object_get_boolean_member(resp, "ok"));
  assert_string_equal(json_object_get_string_member(json_object_get_object_member(resp, "error"), "code"),
                      "invalid_value");

  json_node_unref(actual);
  g_object_unref(parser);
}

// integer fields reject fractional values, even when finite.
static void test_get_module_params_error_fractional_instance(void **state)
{
  (void)state;
  const char *json_text =
    "{\"id\":13,\"method\":\"get_module_params\",\"params\":{\"module\":\"exposure\",\"instance\":0.5}}";
  JsonParser *parser = json_parser_new();
  assert_true(json_parser_load_from_data(parser, json_text, -1, NULL));
  JsonObject *request = json_node_get_object(json_parser_get_root(parser));

  JsonNode *actual = dt_remote_protocol_dispatch(request, NULL);
  assert_non_null(actual);
  JsonObject *resp = json_node_get_object(actual);
  assert_false(json_object_get_boolean_member(resp, "ok"));
  assert_string_equal(json_object_get_string_member(json_object_get_object_member(resp, "error"), "code"),
                      "invalid_value");

  json_node_unref(actual);
  g_object_unref(parser);
}

/* ---------------------------------------------------------------------- */
/* set_module_params (plan step 7)                                          */
/* ---------------------------------------------------------------------- */

// Dispatches an inline JSON request text and returns the response node
// (caller unrefs). For malformed-input cases that aren't wire-contract
// shapes worth a fixture file, mirroring the get_module_params tests above.
static JsonNode *_dispatch_inline(const char *json_text)
{
  JsonParser *parser = json_parser_new();
  assert_true(json_parser_load_from_data(parser, json_text, -1, NULL));
  JsonObject *request = json_node_get_object(json_parser_get_root(parser));
  JsonNode *actual = dt_remote_protocol_dispatch(request, NULL);
  assert_non_null(actual);
  g_object_unref(parser);
  return actual;
}

static void _assert_inline_error(const char *json_text, const char *expected_code)
{
  JsonNode *actual = _dispatch_inline(json_text);
  JsonObject *resp = json_node_get_object(actual);
  assert_false(json_object_get_boolean_member(resp, "ok"));
  assert_string_equal(json_object_get_string_member(json_object_get_object_member(resp, "error"), "code"),
                      expected_code);
  json_node_unref(actual);
}

// The wire-contract example round trip: the request fixture is decoded into
// the exact neutral patch (stub-asserted), and the stub's read-back result
// is serialized into the exact response fixture.
static void test_set_module_params_success(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_exposure_full,
    .set_module_params = stub_set_module_params_contract,
  };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("set_module_params_request.json", "set_module_params_response.json");
  dt_remote_protocol_set_calls(NULL);
}

// Scalar mutation needs only primitive field metadata. A failure in the
// semantic schema registry must therefore remain isolated from this handler:
// the primitive lookup and mutation engine both still run to success.
static void test_set_module_params_semantic_schema_failure_does_not_block_primitive_mutation(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_schema = stub_get_module_schema_internal,
    .get_module_primitive_schema = stub_get_module_primitive_schema_exposure_full,
    .set_module_params = stub_set_module_params_contract,
  };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("set_module_params_request.json", "set_module_params_response.json");
  dt_remote_protocol_set_calls(NULL);
}

// enum by stable name and by integer representation (plan step 7's test
// list): both spellings must reach the engine as the same resolved
// {value, name} pair -- stub_set_module_params_enum_deflicker asserts it.
static void test_set_module_params_enum_by_name(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_exposure_full,
    .set_module_params = stub_set_module_params_enum_deflicker,
  };
  dt_remote_protocol_set_calls(&calls);

  JsonNode *actual = _dispatch_inline(
    "{\"id\":30,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"exposure\",\"values\":{\"mode\":\"EXPOSURE_MODE_DEFLICKER\"}}}");
  JsonObject *resp = json_node_get_object(actual);
  assert_true(json_object_get_boolean_member(resp, "ok"));
  JsonObject *result = json_object_get_object_member(resp, "result");
  assert_string_equal(json_object_get_string_member(json_object_get_object_member(result, "values"), "mode"),
                      "EXPOSURE_MODE_DEFLICKER");
  json_node_unref(actual);

  dt_remote_protocol_set_calls(NULL);
}

static void test_set_module_params_enum_by_int(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_exposure_full,
    .set_module_params = stub_set_module_params_enum_deflicker,
  };
  dt_remote_protocol_set_calls(&calls);

  JsonNode *actual = _dispatch_inline(
    "{\"id\":31,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"exposure\",\"values\":{\"mode\":1}}}");
  JsonObject *resp = json_node_get_object(actual);
  assert_true(json_object_get_boolean_member(resp, "ok"));
  JsonObject *result = json_object_get_object_member(resp, "result");
  assert_string_equal(json_object_get_string_member(json_object_get_object_member(result, "values"), "mode"),
                      "EXPOSURE_MODE_DEFLICKER");
  json_node_unref(actual);

  dt_remote_protocol_set_calls(NULL);
}

// invalid_value surfaced from the engine (range checks live in
// remote_edit's pure core, not the handler): mapped 1:1 onto the wire
// envelope, retryable false.
static void test_set_module_params_error_invalid_value(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_exposure_full,
    .set_module_params = stub_set_module_params_invalid_value,
  };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("set_module_params_error_invalid_value_request.json",
                           "set_module_params_error_invalid_value_response.json");
  dt_remote_protocol_set_calls(NULL);
}

// revision_conflict is the one retryable domain error -- the response
// fixture pins "retryable": true.
static void test_set_module_params_error_revision_conflict(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_exposure_full,
    .set_module_params = stub_set_module_params_revision_conflict,
  };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("set_module_params_error_revision_conflict_request.json",
                           "set_module_params_error_revision_conflict_response.json");
  dt_remote_protocol_set_calls(NULL);
}

// unknown_field is rejected at the protocol boundary (schema lookup), and
// the engine must never be reached.
static void test_set_module_params_error_unknown_field(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_exposure_full,
    .set_module_params = stub_set_module_params_must_not_be_called,
  };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("set_module_params_error_unknown_field_request.json",
                           "set_module_params_error_unknown_field_response.json");
  dt_remote_protocol_set_calls(NULL);
}

// a known field whose schema row says writable:false -> unsupported_field,
// engine never reached.
static void test_set_module_params_error_unsupported_field(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_exposure_full,
    .set_module_params = stub_set_module_params_must_not_be_called,
  };
  dt_remote_protocol_set_calls(&calls);
  _assert_inline_error(
    "{\"id\":32,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"exposure\",\"values\":{\"curve\":1.0}}}",
    "unsupported_field");
  dt_remote_protocol_set_calls(NULL);
}

// The shared-fixture pair for a real per-op-denylisted field (filmicrgb's
// "version", plan step 2/Task 2): same unsupported_field path as the
// synthetic "curve" case above, engine never reached, but with the exact
// request/response fixtures the Python sidecar's client tests reuse
// byte-identically (tools/mcp/tests/test_tools.py::
// test_set_module_params_denylisted_field_surfaces_hint).
static void test_set_module_params_error_denylisted_field(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_filmicrgb_denylisted,
    .set_module_params = stub_set_module_params_must_not_be_called,
  };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("set_module_params_error_denylisted_field_request.json",
                           "set_module_params_error_denylisted_field_response.json");
  dt_remote_protocol_set_calls(NULL);
}

// handler-side shape rejections: every one must leave the engine untouched.
static void test_set_module_params_error_bad_shapes(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_exposure_full,
    .set_module_params = stub_set_module_params_must_not_be_called,
  };
  dt_remote_protocol_set_calls(&calls);

  // missing values
  _assert_inline_error(
    "{\"id\":33,\"method\":\"set_module_params\",\"params\":{\"module\":\"exposure\"}}",
    "invalid_value");
  // empty values
  _assert_inline_error(
    "{\"id\":34,\"method\":\"set_module_params\",\"params\":{\"module\":\"exposure\",\"values\":{}}}",
    "invalid_value");
  // values not an object
  _assert_inline_error(
    "{\"id\":35,\"method\":\"set_module_params\",\"params\":{\"module\":\"exposure\",\"values\":3}}",
    "invalid_value");
  // wrong JSON type for a float field
  _assert_inline_error(
    "{\"id\":36,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"exposure\",\"values\":{\"exposure\":\"bright\"}}}",
    "invalid_value");
  // non-finite float value (1e400 overflows to +Inf on parse)
  _assert_inline_error(
    "{\"id\":43,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"exposure\",\"values\":{\"exposure\":1e400}}}",
    "invalid_value");
  // unknown enum member name
  _assert_inline_error(
    "{\"id\":37,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"exposure\",\"values\":{\"mode\":\"NO_SUCH_MODE\"}}}",
    "invalid_value");
  // unknown enum integer value
  _assert_inline_error(
    "{\"id\":38,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"exposure\",\"values\":{\"mode\":99}}}",
    "invalid_value");
  // enable must be a boolean
  _assert_inline_error(
    "{\"id\":39,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"exposure\",\"values\":{\"exposure\":0.5},\"enable\":1}}",
    "invalid_value");
  // negative expected_revision
  _assert_inline_error(
    "{\"id\":40,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"exposure\",\"values\":{\"exposure\":0.5},\"expected_revision\":-1}}",
    "invalid_value");
  // unknown top-level key (strict-params rule)
  _assert_inline_error(
    "{\"id\":41,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"exposure\",\"values\":{\"exposure\":0.5},\"bogus\":true}}",
    "invalid_value");

  dt_remote_protocol_set_calls(NULL);
}

// an unknown module fails at the schema-resolution step with the same
// error unknown ops produce everywhere else.
static void test_set_module_params_error_unknown_module(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_schema_unknown,
    .set_module_params = stub_set_module_params_must_not_be_called,
  };
  dt_remote_protocol_set_calls(&calls);
  _assert_inline_error(
    "{\"id\":42,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"nonexistent_op\",\"values\":{\"exposure\":0.5}}}",
    "unknown_module");
  dt_remote_protocol_set_calls(NULL);
}

// an existing module but a nonexistent multi-instance fails inside the
// engine; the dispatcher maps DT_REMOTE_ERR_UNKNOWN_INSTANCE onto the
// "unknown_instance" wire envelope.
static void test_set_module_params_error_unknown_instance(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_exposure_full,
    .set_module_params = stub_set_module_params_unknown_instance,
  };
  dt_remote_protocol_set_calls(&calls);
  _assert_inline_error(
    "{\"id\":43,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"exposure\",\"instance\":3,\"values\":{\"exposure\":0.5}}}",
    "unknown_instance");
  dt_remote_protocol_set_calls(NULL);
}

/* --- set_module_params semantic_values (milestone 2 Task 9) ------------- */

// The curve-design SS Mutation request example round trip: the request
// fixture (empty scalar values, one semantic curve entry) is decoded into
// the exact neutral patch (stub-asserted), and the stub's read-back result
// serializes semantic_values into the exact response fixture.
static void test_set_module_params_curve_success(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_rgbcurve,
    .set_module_params = stub_set_module_params_curve_master,
  };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("set_module_params_curve_request.json",
                           "set_module_params_curve_response.json");
  dt_remote_protocol_set_calls(NULL);
}

// The validator's structured details (parameter/point_index/constraint) must
// survive onto the wire error envelope.
static void test_set_module_params_curve_error_invalid_spacing(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_rgbcurve,
    .set_module_params = stub_set_module_params_curve_invalid_spacing,
  };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("set_module_params_error_curve_invalid_spacing_request.json",
                           "set_module_params_error_curve_invalid_spacing_response.json");
  dt_remote_protocol_set_calls(NULL);
}

static void test_set_module_params_curve_error_unknown_id(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_rgbcurve,
    .set_module_params = stub_set_module_params_curve_unknown_id,
  };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("set_module_params_error_curve_unknown_id_request.json",
                           "set_module_params_error_curve_unknown_id_response.json");
  dt_remote_protocol_set_calls(NULL);
}

// Parse-layer semantic_values shape rejections (curve design
// SS Serialization rules): every one must leave the engine untouched --
// whole-patch atomicity starts at the protocol boundary.
static void test_set_module_params_semantic_bad_shapes(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_rgbcurve,
    .set_module_params = stub_set_module_params_must_not_be_called,
  };
  dt_remote_protocol_set_calls(&calls);

  // semantic_values not an object
  _assert_inline_error(
    "{\"id\":50,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},\"semantic_values\":3}}",
    "invalid_value");
  // semantic_values empty: with values also empty, nothing changes state
  _assert_inline_error(
    "{\"id\":51,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},\"semantic_values\":{}}}",
    "invalid_value");
  // entry not an object
  _assert_inline_error(
    "{\"id\":52,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"curve.master\":7}}}",
    "invalid_value");
  // class is required
  _assert_inline_error(
    "{\"id\":53,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"curve.master\":{\"points\":[{\"x\":0.0,\"y\":0.0},{\"x\":1.0,\"y\":1.0}]}}}}",
    "invalid_value");
  // unsupported class name
  _assert_inline_error(
    "{\"id\":54,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"curve.master\":{\"class\":\"levels\","
    "\"points\":[{\"x\":0.0,\"y\":0.0},{\"x\":1.0,\"y\":1.0}]}}}}",
    "invalid_value");
  // points is required
  _assert_inline_error(
    "{\"id\":55,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"curve.master\":{\"class\":\"curve\"}}}}",
    "invalid_value");
  // points must be an array
  _assert_inline_error(
    "{\"id\":56,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"curve.master\":{\"class\":\"curve\",\"points\":true}}}}",
    "invalid_value");
  // point object missing y
  _assert_inline_error(
    "{\"id\":57,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"curve.master\":{\"class\":\"curve\",\"points\":[{\"x\":0.5}]}}}}",
    "invalid_value");
  // point object with a member beyond exactly {x, y}
  _assert_inline_error(
    "{\"id\":58,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"curve.master\":{\"class\":\"curve\","
    "\"points\":[{\"x\":0.0,\"y\":0.0,\"z\":0.0},{\"x\":1.0,\"y\":1.0}]}}}}",
    "invalid_value");
  // non-finite coordinate (1e400 overflows to +Inf on parse)
  _assert_inline_error(
    "{\"id\":59,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"curve.master\":{\"class\":\"curve\","
    "\"points\":[{\"x\":1e400,\"y\":0.0},{\"x\":1.0,\"y\":1.0}]}}}}",
    "invalid_value");
  // non-numeric coordinate
  _assert_inline_error(
    "{\"id\":60,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"curve.master\":{\"class\":\"curve\","
    "\"points\":[{\"x\":\"left\",\"y\":0.0},{\"x\":1.0,\"y\":1.0}]}}}}",
    "invalid_value");
  // unknown entry member
  _assert_inline_error(
    "{\"id\":61,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"curve.master\":{\"class\":\"curve\","
    "\"points\":[{\"x\":0.0,\"y\":0.0},{\"x\":1.0,\"y\":1.0}],\"smoothing\":1}}}}",
    "invalid_value");
  // unknown interpolation name
  _assert_inline_error(
    "{\"id\":62,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"curve.master\":{\"class\":\"curve\","
    "\"points\":[{\"x\":0.0,\"y\":0.0},{\"x\":1.0,\"y\":1.0}],"
    "\"interpolation\":\"QUADRATIC\"}}}}",
    "invalid_value");

  // request-size cap: 65 points is rejected before the engine (the
  // per-descriptor maximum_points bound is the engine's business; this is
  // the protocol layer's flat pre-engine limit of 64)
  GString *oversized = g_string_new(
    "{\"id\":63,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"curve.master\":{\"class\":\"curve\",\"points\":[");
  for(int i = 0; i < 65; i++)
    g_string_append_printf(oversized, "%s{\"x\":%.6f,\"y\":0.5}", i ? "," : "", i / 64.0);
  g_string_append(oversized, "]}}}}");
  _assert_inline_error(oversized->str, "invalid_value");
  g_string_free(oversized, TRUE);

  dt_remote_protocol_set_calls(NULL);
}

/* --- set_module_params semantic_values vector class (milestone 4 Task 2) */

// accept: {"class":"vector","values":[1.0,1.1,1.0,0.95]} decodes into a
// DT_REMOTE_PARAMETER_VECTOR patch with all four components preserved, in
// order, as doubles, and the semantic name kept.
static void test_semantic_vector_entry_parses(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_rgbcurve,
    .set_module_params = stub_set_module_params_vector_capture,
  };
  dt_remote_protocol_set_calls(&calls);

  JsonNode *actual = _dispatch_inline(
    "{\"id\":100,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"vector.example\":{\"class\":\"vector\","
    "\"values\":[1.0,1.1,1.0,0.95]}}}}");
  JsonObject *resp = json_node_get_object(actual);
  assert_true(json_object_get_boolean_member(resp, "ok"));
  json_node_unref(actual);

  dt_remote_protocol_set_calls(NULL);
}

// reject: 'values' member missing, and 'values' present but not an array.
static void test_semantic_vector_requires_values_array(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_rgbcurve,
    .set_module_params = stub_set_module_params_must_not_be_called,
  };
  dt_remote_protocol_set_calls(&calls);

  // values missing entirely
  _assert_inline_error(
    "{\"id\":101,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"vector.example\":{\"class\":\"vector\"}}}}",
    "invalid_value");
  // values present but not an array
  _assert_inline_error(
    "{\"id\":102,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"vector.example\":{\"class\":\"vector\",\"values\":true}}}}",
    "invalid_value");

  dt_remote_protocol_set_calls(NULL);
}

// reject: every non-numeric/non-finite component spelling -- a non-numeric
// string, a boolean, overflow-to-Infinity in both directions (1e400/-1e400
// have no finite double representation; a bare Infinity/NaN token is not
// valid JSON, so overflow is the wire-representable spelling -- curve
// point parsing uses the same trick), and a null element.
static void test_semantic_vector_rejects_non_finite_components(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_rgbcurve,
    .set_module_params = stub_set_module_params_must_not_be_called,
  };
  dt_remote_protocol_set_calls(&calls);

  _assert_inline_error(
    "{\"id\":103,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"vector.example\":{\"class\":\"vector\",\"values\":[1.0,\"x\"]}}}}",
    "invalid_value");
  _assert_inline_error(
    "{\"id\":104,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"vector.example\":{\"class\":\"vector\",\"values\":[1.0,true]}}}}",
    "invalid_value");
  _assert_inline_error(
    "{\"id\":105,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"vector.example\":{\"class\":\"vector\",\"values\":[1e400]}}}}",
    "invalid_value");
  _assert_inline_error(
    "{\"id\":106,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"vector.example\":{\"class\":\"vector\",\"values\":[-1e400]}}}}",
    "invalid_value");
  _assert_inline_error(
    "{\"id\":107,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"vector.example\":{\"class\":\"vector\",\"values\":[1.0,null]}}}}",
    "invalid_value");

  dt_remote_protocol_set_calls(NULL);
}

// reject: 9 components exceeds DT_REMOTE_VECTOR_WIRE_COMPONENT_CAP (8) -- the
// protocol layer's flat pre-engine limit, mirroring the curve point cap.
static void test_semantic_vector_rejects_oversized_component_list(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_rgbcurve,
    .set_module_params = stub_set_module_params_must_not_be_called,
  };
  dt_remote_protocol_set_calls(&calls);

  GString *oversized = g_string_new(
    "{\"id\":108,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"vector.example\":{\"class\":\"vector\",\"values\":[");
  for(int i = 0; i < 9; i++)
    g_string_append_printf(oversized, "%s%.6f", i ? "," : "", i / 8.0);
  g_string_append(oversized, "]}}}}");
  _assert_inline_error(oversized->str, "invalid_value");
  g_string_free(oversized, TRUE);

  dt_remote_protocol_set_calls(NULL);
}

// reject: a member beyond exactly {class, values} -- here "points", a
// curve-flavored member, leaking onto a vector entry.
static void test_semantic_vector_rejects_unknown_members(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_rgbcurve,
    .set_module_params = stub_set_module_params_must_not_be_called,
  };
  dt_remote_protocol_set_calls(&calls);

  _assert_inline_error(
    "{\"id\":109,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"vector.example\":{\"class\":\"vector\","
    "\"values\":[1.0,1.1],\"points\":[{\"x\":0.0,\"y\":0.0}]}}}}",
    "invalid_value");

  dt_remote_protocol_set_calls(NULL);
}

// reject: two entries with the same semantic ID, one curve-shaped and one
// vector-shaped. JSON object member names are unique -- the underlying
// JSON parser deduplicates literal duplicate keys during parse, keeping
// only the last occurrence -- so the curve entry never survives to be seen
// by _parse_semantic_values at all; "duplicate-ID detection" is therefore
// structural (see the comment above _parse_semantic_values), not code this
// branch adds. The surviving (vector) entry is deliberately malformed so
// the test can observe whole-patch atomicity: nothing from the discarded
// curve entry, and nothing from the malformed survivor, is produced.
static void test_semantic_duplicate_ids_rejected_across_classes(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_rgbcurve,
    .set_module_params = stub_set_module_params_must_not_be_called,
  };
  dt_remote_protocol_set_calls(&calls);

  _assert_inline_error(
    "{\"id\":110,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"shared.name\":{\"class\":\"curve\","
    "\"points\":[{\"x\":0.0,\"y\":0.0},{\"x\":1.0,\"y\":1.0}]},"
    "\"shared.name\":{\"class\":\"vector\",\"values\":[\"oops\"]}}}}",
    "invalid_value");

  dt_remote_protocol_set_calls(NULL);
}

// double-domain proof: a component that would narrow cleanly to float
// (2.00000001 -> 2.0f, since the difference is well under float's ULP near
// 2.0) must survive parsing as its un-narrowed double.
static void test_semantic_vector_preserves_double_precision(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_rgbcurve,
    .set_module_params = stub_set_module_params_vector_precision,
  };
  dt_remote_protocol_set_calls(&calls);

  JsonNode *actual = _dispatch_inline(
    "{\"id\":111,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"vector.example\":{\"class\":\"vector\","
    "\"values\":[2.00000001]}}}}");
  JsonObject *resp = json_node_get_object(actual);
  assert_true(json_object_get_boolean_member(resp, "ok"));
  json_node_unref(actual);

  dt_remote_protocol_set_calls(NULL);
}

/* --- set_module_params semantic_values bands class (milestone 5 Task 2) */

// accept: {"class":"bands","y":[0.5,0.5,0.5,0.5,0.5,0.5]} decodes into a
// DT_REMOTE_PARAMETER_BANDS patch with all six y samples preserved, in
// order, as doubles, x left NULL, and the semantic name kept.
static void test_semantic_band_entry_parses(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_rgbcurve,
    .set_module_params = stub_set_module_params_bands_capture,
  };
  dt_remote_protocol_set_calls(&calls);

  JsonNode *actual = _dispatch_inline(
    "{\"id\":112,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"bands.example\":{\"class\":\"bands\","
    "\"y\":[0.5,0.5,0.5,0.5,0.5,0.5]}}}}");
  JsonObject *resp = json_node_get_object(actual);
  assert_true(json_object_get_boolean_member(resp, "ok"));
  json_node_unref(actual);

  dt_remote_protocol_set_calls(NULL);
}

// accept: same y, plus "x":[0.0,0.2,0.4,0.6,0.8,1.0] -- x is kept as six
// doubles alongside y.
static void test_semantic_band_entry_with_x_parses(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_rgbcurve,
    .set_module_params = stub_set_module_params_bands_with_x_capture,
  };
  dt_remote_protocol_set_calls(&calls);

  JsonNode *actual = _dispatch_inline(
    "{\"id\":113,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"bands.example\":{\"class\":\"bands\","
    "\"y\":[0.5,0.5,0.5,0.5,0.5,0.5],"
    "\"x\":[0.0,0.2,0.4,0.6,0.8,1.0]}}}}");
  JsonObject *resp = json_node_get_object(actual);
  assert_true(json_object_get_boolean_member(resp, "ok"));
  json_node_unref(actual);

  dt_remote_protocol_set_calls(NULL);
}

// reject: 'y' member missing, and 'y' present but not an array.
static void test_semantic_band_requires_y_array(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_rgbcurve,
    .set_module_params = stub_set_module_params_must_not_be_called,
  };
  dt_remote_protocol_set_calls(&calls);

  // y missing entirely
  _assert_inline_error(
    "{\"id\":114,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"bands.example\":{\"class\":\"bands\"}}}}",
    "invalid_value");
  // y present but not an array
  _assert_inline_error(
    "{\"id\":115,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"bands.example\":{\"class\":\"bands\",\"y\":true}}}}",
    "invalid_value");

  dt_remote_protocol_set_calls(NULL);
}

// reject: every non-numeric/non-finite y-sample spelling -- a non-numeric
// string, a boolean, overflow-to-Infinity in both directions (1e400/-1e400
// have no finite double representation), and a null element -- mirroring
// the vector-class component rejections above.
static void test_semantic_band_rejects_non_finite_samples(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_rgbcurve,
    .set_module_params = stub_set_module_params_must_not_be_called,
  };
  dt_remote_protocol_set_calls(&calls);

  _assert_inline_error(
    "{\"id\":116,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"bands.example\":{\"class\":\"bands\",\"y\":[0.5,\"x\"]}}}}",
    "invalid_value");
  _assert_inline_error(
    "{\"id\":117,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"bands.example\":{\"class\":\"bands\",\"y\":[0.5,true]}}}}",
    "invalid_value");
  _assert_inline_error(
    "{\"id\":118,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"bands.example\":{\"class\":\"bands\",\"y\":[1e400]}}}}",
    "invalid_value");
  _assert_inline_error(
    "{\"id\":119,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"bands.example\":{\"class\":\"bands\",\"y\":[-1e400]}}}}",
    "invalid_value");
  _assert_inline_error(
    "{\"id\":120,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"bands.example\":{\"class\":\"bands\",\"y\":[0.5,null]}}}}",
    "invalid_value");

  dt_remote_protocol_set_calls(NULL);
}

// reject: 9 samples exceeds DT_REMOTE_BAND_WIRE_SAMPLE_CAP (8) -- the
// protocol layer's flat pre-engine limit, mirroring the vector component
// cap; and an empty y array, which is never valid regardless of the cap.
static void test_semantic_band_rejects_oversized_sample_list(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_rgbcurve,
    .set_module_params = stub_set_module_params_must_not_be_called,
  };
  dt_remote_protocol_set_calls(&calls);

  GString *oversized = g_string_new(
    "{\"id\":121,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"bands.example\":{\"class\":\"bands\",\"y\":[");
  for(int i = 0; i < 9; i++)
    g_string_append_printf(oversized, "%s%.6f", i ? "," : "", i / 8.0);
  g_string_append(oversized, "]}}}}");
  _assert_inline_error(oversized->str, "invalid_value");
  g_string_free(oversized, TRUE);

  _assert_inline_error(
    "{\"id\":122,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"bands.example\":{\"class\":\"bands\",\"y\":[]}}}}",
    "invalid_value");

  dt_remote_protocol_set_calls(NULL);
}

// reject: "x" present with a length different from "y"'s.
static void test_semantic_band_rejects_mismatched_x_length(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_rgbcurve,
    .set_module_params = stub_set_module_params_must_not_be_called,
  };
  dt_remote_protocol_set_calls(&calls);

  _assert_inline_error(
    "{\"id\":123,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"bands.example\":{\"class\":\"bands\","
    "\"y\":[0.5,0.5,0.5],\"x\":[0.0,1.0]}}}}",
    "invalid_value");

  dt_remote_protocol_set_calls(NULL);
}

// reject: a member beyond exactly {class, y[, x]} -- here "points", a
// curve-flavored member, leaking onto a bands entry.
static void test_semantic_band_rejects_unknown_members(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_rgbcurve,
    .set_module_params = stub_set_module_params_must_not_be_called,
  };
  dt_remote_protocol_set_calls(&calls);

  _assert_inline_error(
    "{\"id\":124,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"bands.example\":{\"class\":\"bands\","
    "\"y\":[0.5,0.5],\"points\":[{\"x\":0.0,\"y\":0.0}]}}}}",
    "invalid_value");

  dt_remote_protocol_set_calls(NULL);
}

// double-domain proof: a y sample that would narrow cleanly to float
// (0.50000001 -> 0.5f, since the difference is well under float's ULP near
// 0.5) must survive parsing as its un-narrowed double.
static void test_semantic_band_preserves_double_precision(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_rgbcurve,
    .set_module_params = stub_set_module_params_bands_precision,
  };
  dt_remote_protocol_set_calls(&calls);

  JsonNode *actual = _dispatch_inline(
    "{\"id\":125,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"bands.example\":{\"class\":\"bands\","
    "\"y\":[0.50000001]}}}}");
  JsonObject *resp = json_node_get_object(actual);
  assert_true(json_object_get_boolean_member(resp, "ok"));
  json_node_unref(actual);

  dt_remote_protocol_set_calls(NULL);
}

/* --- set_module_params semantic_values quantity class (milestone 6 Task 1) */

// accept: {"class":"quantity","values":{"temperature":5500.0,"tint":1.0}}
// decodes into a DT_REMOTE_PARAMETER_QUANTITY patch with two component
// values, in wire order, preserved as doubles, and the semantic name kept.
static void test_semantic_quantity_entry_parses(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_rgbcurve,
    .set_module_params = stub_set_module_params_quantity_capture,
  };
  dt_remote_protocol_set_calls(&calls);

  JsonNode *actual = _dispatch_inline(
    "{\"id\":126,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"wb.temperature\":{\"class\":\"quantity\","
    "\"values\":{\"temperature\":5500.0,\"tint\":1.0}}}}}");
  JsonObject *resp = json_node_get_object(actual);
  assert_true(json_object_get_boolean_member(resp, "ok"));
  json_node_unref(actual);

  dt_remote_protocol_set_calls(NULL);
}

// reject: 'values' member missing, present but not an object, and present
// as an empty object -- an object with zero component members is never
// valid regardless of the cap.
static void test_semantic_quantity_requires_values_object(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_rgbcurve,
    .set_module_params = stub_set_module_params_must_not_be_called,
  };
  dt_remote_protocol_set_calls(&calls);

  // values missing entirely
  _assert_inline_error(
    "{\"id\":127,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"wb.temperature\":{\"class\":\"quantity\"}}}}",
    "invalid_value");
  // values present but not an object
  _assert_inline_error(
    "{\"id\":128,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"wb.temperature\":{\"class\":\"quantity\",\"values\":true}}}}",
    "invalid_value");
  // values present as an empty object
  _assert_inline_error(
    "{\"id\":129,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"wb.temperature\":{\"class\":\"quantity\",\"values\":{}}}}}",
    "invalid_value");

  dt_remote_protocol_set_calls(NULL);
}

// reject: every non-numeric/non-finite component-value spelling -- a
// non-numeric string, a boolean, overflow-to-Infinity in both directions
// (1e400/-1e400 have no finite double representation), and a null member --
// mirroring the vector/bands-class component rejections above.
static void test_semantic_quantity_rejects_non_finite_components(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_rgbcurve,
    .set_module_params = stub_set_module_params_must_not_be_called,
  };
  dt_remote_protocol_set_calls(&calls);

  _assert_inline_error(
    "{\"id\":130,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"wb.temperature\":{\"class\":\"quantity\","
    "\"values\":{\"temperature\":\"hot\"}}}}}",
    "invalid_value");
  _assert_inline_error(
    "{\"id\":131,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"wb.temperature\":{\"class\":\"quantity\","
    "\"values\":{\"temperature\":true}}}}}",
    "invalid_value");
  _assert_inline_error(
    "{\"id\":132,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"wb.temperature\":{\"class\":\"quantity\","
    "\"values\":{\"temperature\":1e400}}}}}",
    "invalid_value");
  _assert_inline_error(
    "{\"id\":133,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"wb.temperature\":{\"class\":\"quantity\","
    "\"values\":{\"temperature\":-1e400}}}}}",
    "invalid_value");
  _assert_inline_error(
    "{\"id\":134,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"wb.temperature\":{\"class\":\"quantity\","
    "\"values\":{\"temperature\":null}}}}}",
    "invalid_value");

  dt_remote_protocol_set_calls(NULL);
}

// reject: a values object with 9 members exceeds
// DT_REMOTE_QUANTITY_WIRE_COMPONENT_CAP (8) -- the protocol layer's flat
// pre-engine limit, mirroring the vector component cap and the bands sample
// cap.
static void test_semantic_quantity_rejects_oversized_component_object(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_rgbcurve,
    .set_module_params = stub_set_module_params_must_not_be_called,
  };
  dt_remote_protocol_set_calls(&calls);

  GString *oversized = g_string_new(
    "{\"id\":135,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"wb.temperature\":{\"class\":\"quantity\",\"values\":{");
  for(int i = 0; i < 9; i++)
    g_string_append_printf(oversized, "%s\"c%d\":%.6f", i ? "," : "", i, i / 8.0);
  g_string_append(oversized, "}}}}}");
  _assert_inline_error(oversized->str, "invalid_value");
  g_string_free(oversized, TRUE);

  dt_remote_protocol_set_calls(NULL);
}

// reject: a member beyond exactly {class, values} -- here "points", a
// curve-flavored member, leaking onto a quantity entry.
static void test_semantic_quantity_rejects_unknown_members(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_rgbcurve,
    .set_module_params = stub_set_module_params_must_not_be_called,
  };
  dt_remote_protocol_set_calls(&calls);

  _assert_inline_error(
    "{\"id\":136,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"wb.temperature\":{\"class\":\"quantity\","
    "\"values\":{\"temperature\":5500.0,\"tint\":1.0},"
    "\"points\":[{\"x\":0.0,\"y\":0.0}]}}}}",
    "invalid_value");

  dt_remote_protocol_set_calls(NULL);
}

// double-domain proof: a component value that would narrow cleanly to float
// (5500.00000001 -> 5500.0f, since the difference is well under float's ULP
// near 5500.0) must survive parsing as its un-narrowed double.
static void test_semantic_quantity_preserves_double_precision(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_rgbcurve,
    .set_module_params = stub_set_module_params_quantity_precision,
  };
  dt_remote_protocol_set_calls(&calls);

  JsonNode *actual = _dispatch_inline(
    "{\"id\":137,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"rgbcurve\",\"values\":{},"
    "\"semantic_values\":{\"wb.temperature\":{\"class\":\"quantity\","
    "\"values\":{\"temperature\":5500.00000001}}}}}");
  JsonObject *resp = json_node_get_object(actual);
  assert_true(json_object_get_boolean_member(resp, "ok"));
  json_node_unref(actual);

  dt_remote_protocol_set_calls(NULL);
}

// The activation invariant (review-fixes plan Task 5): a hello that
// advertises curve_params and a semantic_values mutation that fails
// unknown-key validation must never coexist. Read the capabilities off the
// real hello response, then drive the wire-contract curve request through
// the same dispatcher.
static void test_hello_curve_params_implies_semantic_values_accepted(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_primitive_schema = stub_get_module_primitive_schema_rgbcurve,
    .set_module_params = stub_set_module_params_curve_master,
  };
  dt_remote_protocol_set_calls(&calls);

  JsonNode *hello_request_node = _load_fixture("hello_request.json");
  JsonNode *hello_response = dt_remote_protocol_dispatch(json_node_get_object(hello_request_node), NULL);
  assert_non_null(hello_response);
  JsonObject *hello_result =
    json_object_get_object_member(json_node_get_object(hello_response), "result");
  JsonArray *caps = json_object_get_array_member(hello_result, "capabilities");
  gboolean advertises_curve_params = FALSE;
  for(guint i = 0; i < json_array_get_length(caps); i++)
    if(!g_strcmp0(json_array_get_string_element(caps, i), "curve_params"))
      advertises_curve_params = TRUE;
  json_node_unref(hello_response);
  json_node_unref(hello_request_node);
  assert_true(advertises_curve_params);

  JsonNode *request_node = _load_fixture("set_module_params_curve_request.json");
  JsonNode *actual = dt_remote_protocol_dispatch(json_node_get_object(request_node), NULL);
  assert_non_null(actual);
  assert_true(json_object_get_boolean_member(json_node_get_object(actual), "ok"));
  json_node_unref(actual);
  json_node_unref(request_node);

  dt_remote_protocol_set_calls(NULL);
}

/* ---------------------------------------------------------------------- */
/* set_module_enabled / reset_module / create_module_instance /            */
/* get_history / undo  (plan step 8)                                        */
/* ---------------------------------------------------------------------- */

/* --- set_module_enabled --- */

// Success stub for the wire-contract example: asserts the handler decoded
// the request into (ref, enabled, expected_revision) exactly, then answers
// with a canned read-back result matching set_module_enabled_response.json.
static gboolean stub_set_module_enabled_ok(const dt_remote_module_ref_t *ref, gboolean enabled,
                                           const uint64_t *expected_revision,
                                           dt_remote_mutation_result_t **out,
                                           dt_remote_error_t **error)
{
  (void)error;
  assert_string_equal(ref->op, "exposure");
  assert_int_equal(ref->instance, 0);
  assert_true(enabled);
  assert_non_null(expected_revision);
  assert_int_equal((int)*expected_revision, 31);

  dt_remote_mutation_result_t *result = g_malloc0(sizeof(dt_remote_mutation_result_t));
  result->op = g_strdup("exposure");
  result->instance = 0;
  result->instance_name = g_strdup("");
  result->enabled = TRUE;
  result->values = NULL;  // enable carries no values
  result->revision = 32;
  *out = result;
  return TRUE;
}

static gboolean stub_set_module_enabled_conflict(const dt_remote_module_ref_t *ref, gboolean enabled,
                                                 const uint64_t *expected_revision,
                                                 dt_remote_mutation_result_t **out,
                                                 dt_remote_error_t **error)
{
  (void)ref;
  (void)enabled;
  (void)out;
  assert_non_null(expected_revision);
  if(error)
    *error = _make_error(DT_REMOTE_ERR_REVISION_CONFLICT,
                         g_strdup_printf("expected revision %d does not match current state",
                                         (int)*expected_revision));
  return FALSE;
}

static gboolean stub_set_module_enabled_must_not_be_called(const dt_remote_module_ref_t *ref,
                                                           gboolean enabled,
                                                           const uint64_t *expected_revision,
                                                           dt_remote_mutation_result_t **out,
                                                           dt_remote_error_t **error)
{
  (void)ref;
  (void)enabled;
  (void)expected_revision;
  (void)out;
  (void)error;
  fail_msg("set_module_enabled engine must not be reached for a request the handler rejects");
  return FALSE;
}

static void test_set_module_enabled_success(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .set_module_enabled = stub_set_module_enabled_ok };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("set_module_enabled_request.json", "set_module_enabled_response.json");
  dt_remote_protocol_set_calls(NULL);
}

static void test_set_module_enabled_error_revision_conflict(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .set_module_enabled = stub_set_module_enabled_conflict };
  dt_remote_protocol_set_calls(&calls);
  // retryable:true is pinned by the wire envelope for revision_conflict.
  JsonNode *actual = _dispatch_inline(
    "{\"id\":60,\"method\":\"set_module_enabled\","
    "\"params\":{\"module\":\"exposure\",\"enabled\":false,\"expected_revision\":30}}");
  JsonObject *resp = json_node_get_object(actual);
  assert_false(json_object_get_boolean_member(resp, "ok"));
  JsonObject *err = json_object_get_object_member(resp, "error");
  assert_string_equal(json_object_get_string_member(err, "code"), "revision_conflict");
  assert_true(json_object_get_boolean_member(err, "retryable"));
  json_node_unref(actual);
  dt_remote_protocol_set_calls(NULL);
}

static void test_set_module_enabled_error_bad_shapes(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .set_module_enabled = stub_set_module_enabled_must_not_be_called
  };
  dt_remote_protocol_set_calls(&calls);
  // missing required 'enabled'
  _assert_inline_error(
    "{\"id\":61,\"method\":\"set_module_enabled\",\"params\":{\"module\":\"exposure\"}}",
    "invalid_value");
  // 'enabled' must be a boolean
  _assert_inline_error(
    "{\"id\":62,\"method\":\"set_module_enabled\","
    "\"params\":{\"module\":\"exposure\",\"enabled\":1}}",
    "invalid_value");
  // missing 'module'
  _assert_inline_error(
    "{\"id\":63,\"method\":\"set_module_enabled\",\"params\":{\"enabled\":true}}",
    "invalid_value");
  // unknown top-level key (strict-params rule)
  _assert_inline_error(
    "{\"id\":64,\"method\":\"set_module_enabled\","
    "\"params\":{\"module\":\"exposure\",\"enabled\":true,\"bogus\":1}}",
    "invalid_value");
  // negative expected_revision
  _assert_inline_error(
    "{\"id\":65,\"method\":\"set_module_enabled\","
    "\"params\":{\"module\":\"exposure\",\"enabled\":true,\"expected_revision\":-1}}",
    "invalid_value");
  dt_remote_protocol_set_calls(NULL);
}

static gboolean stub_set_module_enabled_unknown_module(const dt_remote_module_ref_t *ref,
                                                       gboolean enabled,
                                                       const uint64_t *expected_revision,
                                                       dt_remote_mutation_result_t **out,
                                                       dt_remote_error_t **error)
{
  (void)enabled;
  (void)expected_revision;
  (void)out;
  if(error)
    *error = _make_error(DT_REMOTE_ERR_UNKNOWN_MODULE,
                         g_strdup_printf("unknown module '%s'", ref->op));
  return FALSE;
}

// an unknown op fails inside the engine; the dispatcher maps
// DT_REMOTE_ERR_UNKNOWN_MODULE onto the "unknown_module" wire envelope.
static void test_set_module_enabled_error_unknown_module(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .set_module_enabled = stub_set_module_enabled_unknown_module
  };
  dt_remote_protocol_set_calls(&calls);
  _assert_inline_error(
    "{\"id\":69,\"method\":\"set_module_enabled\","
    "\"params\":{\"module\":\"nonexistent_op\",\"enabled\":true}}",
    "unknown_module");
  dt_remote_protocol_set_calls(NULL);
}

/* --- reset_module --- */

static gboolean stub_reset_module_ok(const dt_remote_module_ref_t *ref,
                                     const uint64_t *expected_revision,
                                     dt_remote_mutation_result_t **out, dt_remote_error_t **error)
{
  (void)error;
  assert_string_equal(ref->op, "exposure");
  assert_int_equal(ref->instance, 0);
  assert_non_null(expected_revision);
  assert_int_equal((int)*expected_revision, 31);

  dt_remote_mutation_result_t *result = g_malloc0(sizeof(dt_remote_mutation_result_t));
  result->op = g_strdup("exposure");
  result->instance = 0;
  result->instance_name = g_strdup("");
  result->enabled = TRUE;
  result->values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);
  g_ptr_array_add(result->values, _make_result_entry("exposure", (dt_remote_value_t){
    .type = DT_REMOTE_VALUE_FLOAT, .v.f = 0.0 }));
  g_ptr_array_add(result->values, _make_result_entry("black", (dt_remote_value_t){
    .type = DT_REMOTE_VALUE_FLOAT, .v.f = 0.0 }));
  result->revision = 32;
  *out = result;
  return TRUE;
}

static gboolean stub_reset_module_conflict(const dt_remote_module_ref_t *ref,
                                           const uint64_t *expected_revision,
                                           dt_remote_mutation_result_t **out,
                                           dt_remote_error_t **error)
{
  (void)ref;
  (void)out;
  assert_non_null(expected_revision);
  if(error)
    *error = _make_error(DT_REMOTE_ERR_REVISION_CONFLICT,
                         g_strdup_printf("expected revision %d does not match current state",
                                         (int)*expected_revision));
  return FALSE;
}

static gboolean stub_reset_module_must_not_be_called(const dt_remote_module_ref_t *ref,
                                                     const uint64_t *expected_revision,
                                                     dt_remote_mutation_result_t **out,
                                                     dt_remote_error_t **error)
{
  (void)ref;
  (void)expected_revision;
  (void)out;
  (void)error;
  fail_msg("reset_module engine must not be reached for a request the handler rejects");
  return FALSE;
}

static void test_reset_module_success(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .reset_module = stub_reset_module_ok };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("reset_module_request.json", "reset_module_response.json");
  dt_remote_protocol_set_calls(NULL);
}

static void test_reset_module_error_revision_conflict(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .reset_module = stub_reset_module_conflict };
  dt_remote_protocol_set_calls(&calls);
  JsonNode *actual = _dispatch_inline(
    "{\"id\":66,\"method\":\"reset_module\","
    "\"params\":{\"module\":\"exposure\",\"expected_revision\":30}}");
  JsonObject *resp = json_node_get_object(actual);
  assert_false(json_object_get_boolean_member(resp, "ok"));
  JsonObject *err = json_object_get_object_member(resp, "error");
  assert_string_equal(json_object_get_string_member(err, "code"), "revision_conflict");
  assert_true(json_object_get_boolean_member(err, "retryable"));
  json_node_unref(actual);
  dt_remote_protocol_set_calls(NULL);
}

static void test_reset_module_error_bad_shapes(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .reset_module = stub_reset_module_must_not_be_called };
  dt_remote_protocol_set_calls(&calls);
  // missing 'module'
  _assert_inline_error("{\"id\":67,\"method\":\"reset_module\",\"params\":{}}", "invalid_value");
  // unknown top-level key
  _assert_inline_error(
    "{\"id\":68,\"method\":\"reset_module\",\"params\":{\"module\":\"exposure\",\"bogus\":1}}",
    "invalid_value");
  dt_remote_protocol_set_calls(NULL);
}

static gboolean stub_reset_module_unknown_instance(const dt_remote_module_ref_t *ref,
                                                   const uint64_t *expected_revision,
                                                   dt_remote_mutation_result_t **out,
                                                   dt_remote_error_t **error)
{
  (void)expected_revision;
  (void)out;
  if(error)
    *error = _make_error(DT_REMOTE_ERR_UNKNOWN_INSTANCE,
                         g_strdup_printf("module '%s' has no instance %d", ref->op, ref->instance));
  return FALSE;
}

// an existing module but a nonexistent multi-instance fails inside the
// engine; the dispatcher maps DT_REMOTE_ERR_UNKNOWN_INSTANCE onto the
// "unknown_instance" wire envelope.
static void test_reset_module_error_unknown_instance(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .reset_module = stub_reset_module_unknown_instance
  };
  dt_remote_protocol_set_calls(&calls);
  _assert_inline_error(
    "{\"id\":72,\"method\":\"reset_module\","
    "\"params\":{\"module\":\"exposure\",\"instance\":9}}",
    "unknown_instance");
  dt_remote_protocol_set_calls(NULL);
}

/* --- create_module_instance --- */

static gboolean stub_create_instance_ok(const dt_remote_module_ref_t *ref, gboolean copy_params,
                                        const uint64_t *expected_revision,
                                        dt_remote_mutation_result_t **out, dt_remote_error_t **error)
{
  (void)error;
  assert_string_equal(ref->op, "exposure");
  assert_int_equal(ref->instance, 0);  // source_instance
  assert_false(copy_params);
  assert_non_null(expected_revision);
  assert_int_equal((int)*expected_revision, 31);

  dt_remote_mutation_result_t *result = g_malloc0(sizeof(dt_remote_mutation_result_t));
  result->op = g_strdup("exposure");
  result->instance = 1;  // the NEW instance's multi_priority
  result->instance_name = g_strdup("1");
  result->enabled = TRUE;
  result->values = NULL;  // create carries no values
  result->revision = 33;
  *out = result;
  return TRUE;
}

static gboolean stub_create_instance_copy_params_true(const dt_remote_module_ref_t *ref,
                                                      gboolean copy_params,
                                                      const uint64_t *expected_revision,
                                                      dt_remote_mutation_result_t **out,
                                                      dt_remote_error_t **error)
{
  (void)ref;
  (void)expected_revision;
  (void)error;
  assert_true(copy_params);  // the sole point of this stub

  dt_remote_mutation_result_t *result = g_malloc0(sizeof(dt_remote_mutation_result_t));
  result->op = g_strdup("exposure");
  result->instance = 1;
  result->instance_name = g_strdup("1");
  result->enabled = TRUE;
  result->values = NULL;
  result->revision = 33;
  *out = result;
  return TRUE;
}

static gboolean stub_create_instance_not_supported(const dt_remote_module_ref_t *ref,
                                                   gboolean copy_params,
                                                   const uint64_t *expected_revision,
                                                   dt_remote_mutation_result_t **out,
                                                   dt_remote_error_t **error)
{
  (void)copy_params;
  (void)expected_revision;
  (void)out;
  if(error)
    *error = _make_error(DT_REMOTE_ERR_INSTANCE_NOT_SUPPORTED,
                         g_strdup_printf("module '%s' does not support multiple instances", ref->op));
  return FALSE;
}

static gboolean stub_create_instance_unknown_instance(const dt_remote_module_ref_t *ref,
                                                      gboolean copy_params,
                                                      const uint64_t *expected_revision,
                                                      dt_remote_mutation_result_t **out,
                                                      dt_remote_error_t **error)
{
  (void)copy_params;
  (void)expected_revision;
  (void)out;
  if(error)
    *error = _make_error(DT_REMOTE_ERR_UNKNOWN_INSTANCE,
                         g_strdup_printf("module '%s' has no instance %d", ref->op, ref->instance));
  return FALSE;
}

static gboolean stub_create_instance_must_not_be_called(const dt_remote_module_ref_t *ref,
                                                        gboolean copy_params,
                                                        const uint64_t *expected_revision,
                                                        dt_remote_mutation_result_t **out,
                                                        dt_remote_error_t **error)
{
  (void)ref;
  (void)copy_params;
  (void)expected_revision;
  (void)out;
  (void)error;
  fail_msg("create_module_instance engine must not be reached for a request the handler rejects");
  return FALSE;
}

static void test_create_module_instance_success(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .create_module_instance = stub_create_instance_ok };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("create_module_instance_request.json",
                           "create_module_instance_response.json");
  dt_remote_protocol_set_calls(NULL);
}

// copy_params defaults to false when absent, and threads through as true
// when set -- both spellings are checked by their respective stubs.
static void test_create_module_instance_copy_params_true(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .create_module_instance = stub_create_instance_copy_params_true
  };
  dt_remote_protocol_set_calls(&calls);
  JsonNode *actual = _dispatch_inline(
    "{\"id\":70,\"method\":\"create_module_instance\","
    "\"params\":{\"module\":\"exposure\",\"copy_params\":true}}");
  JsonObject *resp = json_node_get_object(actual);
  assert_true(json_object_get_boolean_member(resp, "ok"));
  json_node_unref(actual);
  dt_remote_protocol_set_calls(NULL);
}

static void test_create_module_instance_error_instance_not_supported(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .create_module_instance = stub_create_instance_not_supported
  };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("create_module_instance_error_instance_not_supported_request.json",
                           "create_module_instance_error_instance_not_supported_response.json");
  dt_remote_protocol_set_calls(NULL);
}

static void test_create_module_instance_error_unknown_instance(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .create_module_instance = stub_create_instance_unknown_instance
  };
  dt_remote_protocol_set_calls(&calls);
  _assert_inline_error(
    "{\"id\":71,\"method\":\"create_module_instance\","
    "\"params\":{\"module\":\"exposure\",\"source_instance\":9}}",
    "unknown_instance");
  dt_remote_protocol_set_calls(NULL);
}

static void test_create_module_instance_error_bad_shapes(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .create_module_instance = stub_create_instance_must_not_be_called
  };
  dt_remote_protocol_set_calls(&calls);
  // missing 'module'
  _assert_inline_error(
    "{\"id\":72,\"method\":\"create_module_instance\",\"params\":{}}", "invalid_value");
  // copy_params must be a boolean
  _assert_inline_error(
    "{\"id\":73,\"method\":\"create_module_instance\","
    "\"params\":{\"module\":\"exposure\",\"copy_params\":1}}",
    "invalid_value");
  // unknown top-level key
  _assert_inline_error(
    "{\"id\":74,\"method\":\"create_module_instance\","
    "\"params\":{\"module\":\"exposure\",\"bogus\":true}}",
    "invalid_value");
  dt_remote_protocol_set_calls(NULL);
}

/* --- get_history --- */

// captured across a dispatch so the clamping tests can assert the exact
// limit the handler passed the engine (single-threaded test process).
static int g_captured_history_limit = -1;

static gboolean stub_get_history_one(int limit, GPtrArray **out, uint64_t *revision,
                                     dt_remote_error_t **error)
{
  (void)error;
  g_captured_history_limit = limit;

  GPtrArray *items = g_ptr_array_new_with_free_func(dt_remote_history_item_free);
  dt_remote_history_item_t *item = g_malloc0(sizeof(dt_remote_history_item_t));
  item->seq = 12;
  item->op = g_strdup("exposure");
  item->instance = 0;
  item->display_name = g_strdup("exposure");
  item->instance_name = g_strdup("");
  item->enabled = TRUE;
  g_ptr_array_add(items, item);

  if(revision) *revision = 33;
  *out = items;
  return TRUE;
}

static void test_get_history_success(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .get_history = stub_get_history_one };
  dt_remote_protocol_set_calls(&calls);
  g_captured_history_limit = -1;
  _assert_dispatch_matches("get_history_request.json", "get_history_response.json");
  assert_int_equal(g_captured_history_limit, 20);  // fixture asks for 20
  dt_remote_protocol_set_calls(NULL);
}

// limit absent -> default 20; >100 -> clamped to 100; <1 -> clamped to 1.
static void test_get_history_limit_clamping(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .get_history = stub_get_history_one };
  dt_remote_protocol_set_calls(&calls);

  g_captured_history_limit = -1;
  json_node_unref(_dispatch_inline("{\"id\":80,\"method\":\"get_history\",\"params\":{}}"));
  assert_int_equal(g_captured_history_limit, 20);

  g_captured_history_limit = -1;
  json_node_unref(_dispatch_inline(
    "{\"id\":81,\"method\":\"get_history\",\"params\":{\"limit\":500}}"));
  assert_int_equal(g_captured_history_limit, 100);

  g_captured_history_limit = -1;
  json_node_unref(_dispatch_inline(
    "{\"id\":82,\"method\":\"get_history\",\"params\":{\"limit\":0}}"));
  assert_int_equal(g_captured_history_limit, 1);

  g_captured_history_limit = -1;
  json_node_unref(_dispatch_inline(
    "{\"id\":83,\"method\":\"get_history\",\"params\":{\"limit\":-5}}"));
  assert_int_equal(g_captured_history_limit, 1);

  dt_remote_protocol_set_calls(NULL);
}

// no binary param-blob keys leak into the history items.
static void test_get_history_items_carry_no_param_blobs(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .get_history = stub_get_history_one };
  dt_remote_protocol_set_calls(&calls);

  JsonNode *actual = _dispatch_inline("{\"id\":84,\"method\":\"get_history\",\"params\":{}}");
  JsonObject *resp = json_node_get_object(actual);
  JsonObject *result = json_object_get_object_member(resp, "result");
  JsonArray *items = json_object_get_array_member(result, "items");
  JsonObject *item0 = json_array_get_object_element(items, 0);

  // exactly the six model-metadata keys, nothing that could carry params.
  assert_int_equal(json_object_get_size(item0), 6);
  assert_true(json_object_has_member(item0, "seq"));
  assert_true(json_object_has_member(item0, "op"));
  assert_true(json_object_has_member(item0, "instance"));
  assert_true(json_object_has_member(item0, "display_name"));
  assert_true(json_object_has_member(item0, "instance_name"));
  assert_true(json_object_has_member(item0, "enabled"));
  assert_false(json_object_has_member(item0, "params"));
  assert_false(json_object_has_member(item0, "values"));

  json_node_unref(actual);
  dt_remote_protocol_set_calls(NULL);
}

static void test_get_history_error_bad_shapes(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .get_history = stub_get_history_one };
  dt_remote_protocol_set_calls(&calls);
  // limit must be an integer
  _assert_inline_error(
    "{\"id\":85,\"method\":\"get_history\",\"params\":{\"limit\":\"lots\"}}", "invalid_value");
  // fractional limit rejected
  _assert_inline_error(
    "{\"id\":86,\"method\":\"get_history\",\"params\":{\"limit\":1.5}}", "invalid_value");
  // unknown top-level key
  _assert_inline_error(
    "{\"id\":87,\"method\":\"get_history\",\"params\":{\"limit\":20,\"bogus\":1}}", "invalid_value");
  dt_remote_protocol_set_calls(NULL);
}

/* --- undo --- */

static gboolean stub_undo_ok(uint64_t expected_revision, uint64_t *revision, dt_remote_error_t **error)
{
  (void)error;
  assert_int_equal((int)expected_revision, 33);
  if(revision) *revision = 34;
  return TRUE;
}

static gboolean stub_undo_conflict(uint64_t expected_revision, uint64_t *revision,
                                   dt_remote_error_t **error)
{
  (void)revision;
  if(error)
    *error = _make_error(DT_REMOTE_ERR_REVISION_CONFLICT,
                         g_strdup_printf("expected revision %d does not match current state",
                                         (int)expected_revision));
  return FALSE;
}

static gboolean stub_undo_must_not_be_called(uint64_t expected_revision, uint64_t *revision,
                                             dt_remote_error_t **error)
{
  (void)expected_revision;
  (void)revision;
  (void)error;
  fail_msg("undo engine must not be reached for a request the handler rejects");
  return FALSE;
}

static void test_undo_success(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .undo = stub_undo_ok };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("undo_request.json", "undo_response.json");
  dt_remote_protocol_set_calls(NULL);
}

static void test_undo_error_revision_conflict(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .undo = stub_undo_conflict };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("undo_error_revision_conflict_request.json",
                           "undo_error_revision_conflict_response.json");
  dt_remote_protocol_set_calls(NULL);
}

// expected_revision is REQUIRED for undo (compare-and-undo, no unconditional
// form) -- absent surfaces invalid_value and the engine is never reached.
static void test_undo_error_bad_shapes(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .undo = stub_undo_must_not_be_called };
  dt_remote_protocol_set_calls(&calls);
  // missing required expected_revision
  _assert_inline_error("{\"id\":90,\"method\":\"undo\",\"params\":{}}", "invalid_value");
  // negative expected_revision
  _assert_inline_error(
    "{\"id\":91,\"method\":\"undo\",\"params\":{\"expected_revision\":-1}}", "invalid_value");
  // unknown top-level key
  _assert_inline_error(
    "{\"id\":92,\"method\":\"undo\",\"params\":{\"expected_revision\":5,\"steps\":2}}",
    "invalid_value");
  dt_remote_protocol_set_calls(NULL);
}

/* ---------------------------------------------------------------------- */
/* render_preview (plan step 9) -- the first asynchronous method           */
/* ---------------------------------------------------------------------- */

// -- fake async transport (records begin/queue/abort traffic; the fake
// pending is static storage, never freed) --------------------------------

static int g_async_begin_calls;
static gint64 g_async_begin_request_id;
static int g_async_abort_calls;
static int g_queue_preview_calls;
static dt_remote_preview_request_t g_queued_req;
static int g_queued_max_px;
static int g_queued_quality;
static gboolean g_queue_preview_result;
static dt_remote_pending_t g_fake_pending;

static dt_remote_pending_t *fake_async_begin(dt_remote_session_t *session, gint64 request_id)
{
  (void)session;
  g_async_begin_calls++;
  g_async_begin_request_id = request_id;
  memset(&g_fake_pending, 0, sizeof(g_fake_pending));
  g_fake_pending.request_id = request_id;
  return &g_fake_pending;
}

static void fake_async_abort(dt_remote_pending_t *pending)
{
  assert_ptr_equal(pending, &g_fake_pending);
  g_async_abort_calls++;
}

static void fake_async_complete(dt_remote_pending_t *pending, JsonNode *response)
{
  (void)pending;
  if(response) json_node_unref(response);
  fail_msg("complete must not be reached from dispatch itself");
}

static gboolean fake_queue_preview(dt_remote_pending_t *pending,
                                   const dt_remote_preview_request_t *req,
                                   int max_px, int quality)
{
  assert_ptr_equal(pending, &g_fake_pending);
  g_queue_preview_calls++;
  g_queued_req = *req;
  g_queued_max_px = max_px;
  g_queued_quality = quality;
  return g_queue_preview_result;
}

// compute_scopes' half of the same fake transport (step 10): records the
// queued request so the deferral/clamping tests can assert on it.
static int g_queue_scopes_calls;
static dt_remote_scopes_request_t g_queued_scopes_req;
static gboolean g_queue_scopes_result;

static gboolean fake_queue_scopes(dt_remote_pending_t *pending,
                                  const dt_remote_scopes_request_t *req)
{
  assert_ptr_equal(pending, &g_fake_pending);
  g_queue_scopes_calls++;
  g_queued_scopes_req = *req;
  return g_queue_scopes_result;
}

static void _install_fake_async(gboolean queue_result)
{
  g_async_begin_calls = 0;
  g_async_begin_request_id = -1;
  g_async_abort_calls = 0;
  g_queue_preview_calls = 0;
  g_queued_max_px = g_queued_quality = 0;
  memset(&g_queued_req, 0, sizeof(g_queued_req));
  g_queue_preview_result = queue_result;
  g_queue_scopes_calls = 0;
  memset(&g_queued_scopes_req, 0, sizeof(g_queued_scopes_req));
  g_queue_scopes_result = queue_result;

  static const dt_remote_protocol_async_t ops = {
    .begin = fake_async_begin,
    .abort = fake_async_abort,
    .complete = fake_async_complete,
    .queue_preview = fake_queue_preview,
    .queue_scopes = fake_queue_scopes,
  };
  dt_remote_protocol_set_async(&ops);
}

static gboolean stub_render_preview_prepare_ok(dt_remote_preview_request_t *out,
                                               dt_remote_error_t **error)
{
  (void)error;
  out->imgid = 172;
  out->revision = 34;
  return TRUE;
}

static gboolean stub_render_preview_prepare_not_in_darkroom(dt_remote_preview_request_t *out,
                                                            dt_remote_error_t **error)
{
  (void)out;
  if(error) *error = _make_error(DT_REMOTE_ERR_NOT_IN_DARKROOM, g_strdup("no darkroom view is active"));
  return FALSE;
}

// like _dispatch_inline, but a NULL return (async deferral) is legal
static JsonNode *_dispatch_inline_maybe_deferred(const char *json_text)
{
  JsonParser *parser = json_parser_new();
  assert_true(json_parser_load_from_data(parser, json_text, -1, NULL));
  JsonObject *request = json_node_get_object(json_parser_get_root(parser));
  JsonNode *actual = dt_remote_protocol_dispatch(request, NULL);
  g_object_unref(parser);
  return actual;
}

// The deferral contract end-to-end through the seams: dispatch registers
// the pending with the request id, the handler clamps/threads the params
// and the prepared request into the queued job, and dispatch returns NULL
// (the transport then leaves pending_requests to the completion).
static void test_render_preview_defers_with_defaults(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .render_preview_prepare = stub_render_preview_prepare_ok };
  dt_remote_protocol_set_calls(&calls);
  _install_fake_async(TRUE);

  JsonNode *resp = _dispatch_inline_maybe_deferred("{\"id\":9,\"method\":\"render_preview\"}");
  assert_null(resp);  // deferred

  assert_int_equal(g_async_begin_calls, 1);
  assert_int_equal((int)g_async_begin_request_id, 9);
  assert_int_equal(g_queue_preview_calls, 1);
  assert_int_equal(g_queued_max_px, 1024);  // wire default
  assert_int_equal(g_queued_quality, 85);   // wire default
  assert_int_equal((int)g_queued_req.imgid, 172);
  assert_int_equal((int)g_queued_req.revision, 34);  // revision captured at prepare
  assert_int_equal(g_async_abort_calls, 0);

  dt_remote_protocol_set_async(NULL);
  dt_remote_protocol_set_calls(NULL);
}

static void test_render_preview_request_fixture_defers(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .render_preview_prepare = stub_render_preview_prepare_ok };
  dt_remote_protocol_set_calls(&calls);
  _install_fake_async(TRUE);

  JsonNode *request_node = _load_fixture("render_preview_request.json");
  JsonNode *resp = dt_remote_protocol_dispatch(json_node_get_object(request_node), NULL);
  assert_null(resp);

  assert_int_equal((int)g_async_begin_request_id, 9);
  assert_int_equal(g_queued_max_px, 1024);
  assert_int_equal(g_queued_quality, 85);

  json_node_unref(request_node);
  dt_remote_protocol_set_async(NULL);
  dt_remote_protocol_set_calls(NULL);
}

// out-of-range integers are clamped (never rejected): 10 -> 64,
// 5000 -> 2048; quality 10 -> 50, 200 -> 95
static void test_render_preview_clamps_out_of_range_params(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .render_preview_prepare = stub_render_preview_prepare_ok };
  dt_remote_protocol_set_calls(&calls);
  _install_fake_async(TRUE);

  assert_null(_dispatch_inline_maybe_deferred(
    "{\"id\":10,\"method\":\"render_preview\",\"params\":{\"max_px\":10,\"quality\":10}}"));
  assert_int_equal(g_queued_max_px, 64);
  assert_int_equal(g_queued_quality, 50);

  assert_null(_dispatch_inline_maybe_deferred(
    "{\"id\":11,\"method\":\"render_preview\",\"params\":{\"max_px\":5000,\"quality\":200}}"));
  assert_int_equal(g_queued_max_px, 2048);
  assert_int_equal(g_queued_quality, 95);

  assert_int_equal(g_queue_preview_calls, 2);
  assert_int_equal(g_async_abort_calls, 0);

  dt_remote_protocol_set_async(NULL);
  dt_remote_protocol_set_calls(NULL);
}

// Wrong-shaped params fail synchronously with invalid_value: the pending
// dispatch pre-registered is aborted (exactly once), nothing is queued,
// and prepare (the history flush) is never reached where params are bad.
static void test_render_preview_error_bad_shapes(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .render_preview_prepare = stub_render_preview_prepare_ok };
  dt_remote_protocol_set_calls(&calls);
  _install_fake_async(TRUE);

  // non-integer max_px
  _assert_inline_error(
    "{\"id\":20,\"method\":\"render_preview\",\"params\":{\"max_px\":\"big\"}}", "invalid_value");
  // fractional max_px
  _assert_inline_error(
    "{\"id\":21,\"method\":\"render_preview\",\"params\":{\"max_px\":1024.5}}", "invalid_value");
  // non-integer quality
  _assert_inline_error(
    "{\"id\":22,\"method\":\"render_preview\",\"params\":{\"quality\":\"good\"}}", "invalid_value");
  // unknown key (strict-params rule)
  _assert_inline_error(
    "{\"id\":23,\"method\":\"render_preview\",\"params\":{\"max_px\":512,\"format\":\"png\"}}",
    "invalid_value");

  assert_int_equal(g_async_begin_calls, 4);
  assert_int_equal(g_async_abort_calls, 4);  // one abort per synchronous outcome
  assert_int_equal(g_queue_preview_calls, 0);

  dt_remote_protocol_set_async(NULL);
  dt_remote_protocol_set_calls(NULL);
}

// prepare failing (not in darkroom) surfaces synchronously through the
// standard envelope; the pending is aborted and no job is queued.
static void test_render_preview_error_not_in_darkroom(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls =
    { .render_preview_prepare = stub_render_preview_prepare_not_in_darkroom };
  dt_remote_protocol_set_calls(&calls);
  _install_fake_async(TRUE);

  _assert_inline_error("{\"id\":30,\"method\":\"render_preview\"}", "not_in_darkroom");

  assert_int_equal(g_async_abort_calls, 1);
  assert_int_equal(g_queue_preview_calls, 0);

  dt_remote_protocol_set_async(NULL);
  dt_remote_protocol_set_calls(NULL);
}

// queueing failure maps to preview_failed (retryable -- the job system
// hiccuped, not the request), with the pending aborted.
static void test_render_preview_error_queue_failure(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .render_preview_prepare = stub_render_preview_prepare_ok };
  dt_remote_protocol_set_calls(&calls);
  _install_fake_async(FALSE);  // queue_preview reports failure

  JsonNode *resp = _dispatch_inline_maybe_deferred("{\"id\":31,\"method\":\"render_preview\"}");
  assert_non_null(resp);
  JsonObject *obj = json_node_get_object(resp);
  assert_false(json_object_get_boolean_member(obj, "ok"));
  JsonObject *error = json_object_get_object_member(obj, "error");
  assert_string_equal(json_object_get_string_member(error, "code"), "preview_failed");
  assert_true(json_object_get_boolean_member(error, "retryable"));
  json_node_unref(resp);

  assert_int_equal(g_queue_preview_calls, 1);
  assert_int_equal(g_async_abort_calls, 1);

  dt_remote_protocol_set_async(NULL);
  dt_remote_protocol_set_calls(NULL);
}

// No session (production async table, NULL session -> begin returns NULL):
// params still validate, then the handler fails cleanly with internal --
// never a crash, never a queued job.
static void test_render_preview_without_transport_is_internal_error(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .render_preview_prepare = stub_render_preview_prepare_ok };
  dt_remote_protocol_set_calls(&calls);
  dt_remote_protocol_set_async(NULL);  // production table

  _assert_inline_error("{\"id\":32,\"method\":\"render_preview\"}", "internal");

  dt_remote_protocol_set_calls(NULL);
}

// -- response shaping (pure; matched against the shared wire fixtures) ----

static const char RENDER_PREVIEW_STUB_BYTES[] = "stub-jpeg-bytes-for-render-preview-fixture";

static void test_build_preview_response_success_matches_fixture(void **state)
{
  (void)state;
  dt_remote_preview_t preview = {
    .jpeg = (uint8_t *)RENDER_PREVIEW_STUB_BYTES,
    .jpeg_len = strlen(RENDER_PREVIEW_STUB_BYTES),
    .width = 1024,
    .height = 683,
    .revision = 34,
  };

  JsonNode *actual = dt_remote_protocol_build_preview_response(9, &preview, NULL);
  JsonNode *expected = _load_fixture("render_preview_response.json");
  assert_true(_json_equal(actual, expected));

  // and the fixture's data member really is the base64 of the stub bytes
  JsonObject *result = json_object_get_object_member(json_node_get_object(actual), "result");
  gsize decoded_len = 0;
  guchar *decoded = g_base64_decode(json_object_get_string_member(result, "data"), &decoded_len);
  assert_int_equal((int)decoded_len, (int)strlen(RENDER_PREVIEW_STUB_BYTES));
  assert_memory_equal(decoded, RENDER_PREVIEW_STUB_BYTES, decoded_len);
  g_free(decoded);

  json_node_unref(actual);
  json_node_unref(expected);
}

static void test_build_preview_response_error_matches_fixture(void **state)
{
  (void)state;
  dt_remote_error_t *err = _make_error(DT_REMOTE_ERR_PREVIEW_FAILED, g_strdup("pixelpipe export failed"));

  JsonNode *actual = dt_remote_protocol_build_preview_response(9, NULL, err);
  JsonNode *expected = _load_fixture("render_preview_error_preview_failed_response.json");
  assert_true(_json_equal(actual, expected));  // pins retryable: true

  json_node_unref(actual);
  json_node_unref(expected);
  dt_remote_error_free(err);
}

// an over-cap preview becomes the request_too_large envelope (retryable
// false per the reference's table) -- the client lowers max_px and retries
static void test_build_preview_response_too_large_matches_fixture(void **state)
{
  (void)state;
  const size_t huge_len = (size_t)DT_REMOTE_MAX_FRAME;  // inflates ~4/3 over the cap
  dt_remote_preview_t preview = {
    .jpeg = g_malloc0(huge_len),
    .jpeg_len = huge_len,
    .width = 2048,
    .height = 2048,
    .revision = 34,
  };

  JsonNode *actual = dt_remote_protocol_build_preview_response(9, &preview, NULL);
  JsonNode *expected = _load_fixture("render_preview_error_request_too_large_response.json");
  assert_true(_json_equal(actual, expected));  // pins retryable: false

  json_node_unref(actual);
  json_node_unref(expected);
  g_free(preview.jpeg);
}

static void test_preview_fits_frame_boundaries(void **state)
{
  (void)state;
  assert_true(dt_remote_protocol_preview_fits_frame(100));
  assert_true(dt_remote_protocol_preview_fits_frame(12000000));    // ~12 MB: inflates to ~16 MB, fits
  assert_false(dt_remote_protocol_preview_fits_frame(12582912));   // 12 MiB: inflates past the cap
  assert_false(dt_remote_protocol_preview_fits_frame((size_t)DT_REMOTE_MAX_FRAME));
}

// -- completion against a real (fake-transport-free) session --------------

// The same fake-session technique as test_remote_server.c: writing=TRUE
// parks outgoing frames on write_queue for inspection, and closing tests
// preset io_refs=1 so releasing the pending never reaches the real
// session destructor (which needs live GIO objects).
static dt_remote_session_t *_fake_session(void)
{
  dt_remote_session_t *s = g_new0(dt_remote_session_t, 1);
  s->auth_state = DT_REMOTE_SESSION_READY;
  s->io_cancellable = g_cancellable_new();
  s->write_queue = g_queue_new();
  s->pendings = g_ptr_array_new();
  s->writing = TRUE;
  return s;
}

static void _fake_session_free(dt_remote_session_t *s)
{
  while(!g_queue_is_empty(s->write_queue)) g_bytes_unref(g_queue_pop_head(s->write_queue));
  g_queue_free(s->write_queue);
  g_ptr_array_free(s->pendings, TRUE);
  g_object_unref(s->io_cancellable);
  g_free(s);
}

// asserts exactly one frame is queued, strips its 4-byte length prefix, and
// returns the parsed payload object (owned by *parser_out; unref when done)
static JsonObject *_parse_single_queued_frame(dt_remote_session_t *s, JsonParser **parser_out)
{
  assert_int_equal((int)g_queue_get_length(s->write_queue), 1);
  GBytes *framed = g_queue_peek_head(s->write_queue);
  gsize len = 0;
  const guint8 *data = g_bytes_get_data(framed, &len);
  assert_true(len > 4);
  JsonParser *parser = json_parser_new();
  assert_true(json_parser_load_from_data(parser, (const gchar *)data + 4, (gssize)(len - 4), NULL));
  *parser_out = parser;
  return json_node_get_object(json_parser_get_root(parser));
}

// current_revision seam stubs for the completion-time coherence check: the
// preview fixtures are stamped revision 34, so _coherent returns 34 (no
// drift, success) and _drifted returns something else (state changed during
// the render, retryable failure).
static uint64_t stub_current_revision_coherent(void)
{
  return 34;
}

static uint64_t stub_current_revision_drifted(void)
{
  return 35;
}

// the full happy-path completion: job result -> framed success response on
// the session's write path, with the async bookkeeping torn down once
static void test_finish_preview_sends_framed_response(void **state)
{
  (void)state;
  dt_remote_protocol_set_async(NULL);  // the real async transport
  // revision unchanged since the stamp -> the coherence check passes
  dt_remote_protocol_calls_t calls = { .current_revision = stub_current_revision_coherent };
  dt_remote_protocol_set_calls(&calls);

  dt_remote_session_t *s = _fake_session();
  s->pending_requests = 1;
  dt_remote_pending_t *pending = dt_remote_async_begin(s, 9);

  dt_remote_preview_t *preview = g_malloc0(sizeof(dt_remote_preview_t));
  preview->jpeg = g_malloc(strlen(RENDER_PREVIEW_STUB_BYTES));
  memcpy(preview->jpeg, RENDER_PREVIEW_STUB_BYTES, strlen(RENDER_PREVIEW_STUB_BYTES));
  preview->jpeg_len = strlen(RENDER_PREVIEW_STUB_BYTES);
  preview->width = 1024;
  preview->height = 683;
  preview->revision = 34;

  dt_remote_protocol_finish_preview(pending, preview, NULL);  // takes ownership

  assert_int_equal((int)g_queue_get_length(s->write_queue), 1);
  GBytes *framed = g_queue_peek_head(s->write_queue);
  gsize len = 0;
  const guint8 *data = g_bytes_get_data(framed, &len);
  assert_true(len > 4);

  JsonParser *parser = json_parser_new();
  assert_true(json_parser_load_from_data(parser, (const gchar *)data + 4, (gssize)(len - 4), NULL));
  JsonNode *expected = _load_fixture("render_preview_response.json");
  assert_true(_json_equal(json_parser_get_root(parser), expected));
  json_node_unref(expected);
  g_object_unref(parser);

  assert_int_equal(s->pending_requests, 0);
  assert_int_equal(s->io_refs, 0);
  assert_int_equal((int)s->pendings->len, 0);

  _fake_session_free(s);
  dt_remote_protocol_set_calls(NULL);
}

// revision-coherence race (internals §8): if the live revision drifted from
// the stamp while the render was in flight, the completion discards the
// rendered payload and answers a RETRYABLE preview_failed -- never revision R
// with revision-R+1 pixels. The JPEG buffer must be released, not sent.
static void test_finish_preview_revision_drift_fails_retryable(void **state)
{
  (void)state;
  dt_remote_protocol_set_async(NULL);  // the real async transport
  dt_remote_protocol_calls_t calls = { .current_revision = stub_current_revision_drifted };
  dt_remote_protocol_set_calls(&calls);

  dt_remote_session_t *s = _fake_session();
  s->pending_requests = 1;
  dt_remote_pending_t *pending = dt_remote_async_begin(s, 9);

  dt_remote_preview_t *preview = g_malloc0(sizeof(dt_remote_preview_t));
  preview->jpeg = g_malloc(1024 * 1024);  // must be discarded, never encoded/sent
  preview->jpeg_len = 1024 * 1024;
  preview->width = 1024;
  preview->height = 683;
  preview->revision = 34;  // stamp; stub_current_revision_drifted() returns 35

  dt_remote_protocol_finish_preview(pending, preview, NULL);  // takes ownership

  JsonParser *parser = NULL;
  JsonObject *resp = _parse_single_queued_frame(s, &parser);
  assert_int_equal((int)json_object_get_int_member(resp, "id"), 9);
  assert_false(json_object_get_boolean_member(resp, "ok"));
  JsonObject *error = json_object_get_object_member(resp, "error");
  assert_string_equal(json_object_get_string_member(error, "code"), "preview_failed");
  assert_true(json_object_get_boolean_member(error, "retryable"));
  g_object_unref(parser);

  assert_int_equal(s->pending_requests, 0);
  assert_int_equal(s->io_refs, 0);
  assert_int_equal((int)s->pendings->len, 0);

  _fake_session_free(s);
  dt_remote_protocol_set_calls(NULL);
}

// the converse: revision unchanged since the stamp -> the render is coherent
// and the success frame is sent (this is the same coherent path the happy
// path above pins, asserted here explicitly against the drift case).
static void test_finish_preview_no_drift_succeeds(void **state)
{
  (void)state;
  dt_remote_protocol_set_async(NULL);
  dt_remote_protocol_calls_t calls = { .current_revision = stub_current_revision_coherent };
  dt_remote_protocol_set_calls(&calls);

  dt_remote_session_t *s = _fake_session();
  s->pending_requests = 1;
  dt_remote_pending_t *pending = dt_remote_async_begin(s, 9);

  dt_remote_preview_t *preview = g_malloc0(sizeof(dt_remote_preview_t));
  preview->jpeg = g_malloc(strlen(RENDER_PREVIEW_STUB_BYTES));
  memcpy(preview->jpeg, RENDER_PREVIEW_STUB_BYTES, strlen(RENDER_PREVIEW_STUB_BYTES));
  preview->jpeg_len = strlen(RENDER_PREVIEW_STUB_BYTES);
  preview->width = 1024;
  preview->height = 683;
  preview->revision = 34;  // matches stub_current_revision_coherent()

  dt_remote_protocol_finish_preview(pending, preview, NULL);

  JsonParser *parser = NULL;
  JsonObject *resp = _parse_single_queued_frame(s, &parser);
  assert_true(json_object_get_boolean_member(resp, "ok"));
  JsonObject *result = json_object_get_object_member(resp, "result");
  assert_int_equal((int)json_object_get_int_member(result, "revision"), 34);
  g_object_unref(parser);

  assert_int_equal(s->pending_requests, 0);
  assert_int_equal(s->io_refs, 0);
  assert_int_equal((int)s->pendings->len, 0);

  _fake_session_free(s);
  dt_remote_protocol_set_calls(NULL);
}

// cancellation-on-disconnect: the completion of a job whose session
// closed must release the result buffers and the pending WITHOUT writing
// to the closing session (the buffers' release is verified structurally
// here and by the leak sanitizer)
static void test_finish_preview_cancelled_releases_without_writing(void **state)
{
  (void)state;
  dt_remote_protocol_set_async(NULL);

  dt_remote_session_t *s = _fake_session();
  s->io_refs = 1;  // the in-flight read every real closing session has
  s->pending_requests = 1;
  dt_remote_pending_t *pending = dt_remote_async_begin(s, 9);

  // the disconnect path: close fires the pending's cancellable
  dt_remote_session_request_close(s);
  assert_true(g_cancellable_is_cancelled(pending->cancellable));

  dt_remote_preview_t *preview = g_malloc0(sizeof(dt_remote_preview_t));
  preview->jpeg = g_malloc(1024 * 1024);  // a result buffer that must be released
  preview->jpeg_len = 1024 * 1024;
  preview->width = 1024;
  preview->height = 683;
  preview->revision = 34;

  dt_remote_protocol_finish_preview(pending, preview, NULL);

  assert_int_equal((int)g_queue_get_length(s->write_queue), 0);  // nothing written
  assert_int_equal(s->pending_requests, 0);                      // still balanced
  assert_int_equal(s->io_refs, 1);                               // only the preset read ref left
  assert_int_equal((int)s->pendings->len, 0);                    // pending released

  _fake_session_free(s);
}

/* ---------------------------------------------------------------------- */
/* compute_scopes (plan step 10) -- the second asynchronous method         */
/* ---------------------------------------------------------------------- */

static gboolean stub_scopes_prepare_ok(dt_remote_error_t **error)
{
  (void)error;
  return TRUE;
}

static gboolean stub_scopes_prepare_not_in_darkroom(dt_remote_error_t **error)
{
  if(error) *error = _make_error(DT_REMOTE_ERR_NOT_IN_DARKROOM, g_strdup("no darkroom view is active"));
  return FALSE;
}

// deferral with defaults: include_summary TRUE, include_bins FALSE,
// include_images TRUE, image_size 512 (wire contract), and the async
// lifecycle mirrors render_preview exactly (begin with the request id,
// queue once, NULL return, no abort).
static void test_compute_scopes_defers_with_defaults(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .scopes_prepare = stub_scopes_prepare_ok };
  dt_remote_protocol_set_calls(&calls);
  _install_fake_async(TRUE);

  JsonNode *resp = _dispatch_inline_maybe_deferred(
    "{\"id\":40,\"method\":\"compute_scopes\",\"params\":{\"scopes\":[\"histogram\"]}}");
  assert_null(resp);   // deferred

  assert_int_equal(g_async_begin_calls, 1);
  assert_int_equal(g_async_begin_request_id, 40);
  assert_int_equal(g_queue_scopes_calls, 1);
  assert_int_equal(g_async_abort_calls, 0);

  assert_true(g_queued_scopes_req.want_histogram);
  assert_false(g_queued_scopes_req.want_waveform);
  assert_false(g_queued_scopes_req.want_parade);
  assert_false(g_queued_scopes_req.want_vectorscope);
  assert_true(g_queued_scopes_req.include_summary);
  assert_false(g_queued_scopes_req.include_bins);
  assert_true(g_queued_scopes_req.include_images);
  assert_int_equal(g_queued_scopes_req.image_size, 512);

  dt_remote_protocol_set_calls(NULL);
  dt_remote_protocol_set_async(NULL);
}

static void test_compute_scopes_request_fixture_defers(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .scopes_prepare = stub_scopes_prepare_ok };
  dt_remote_protocol_set_calls(&calls);
  _install_fake_async(TRUE);

  JsonNode *request_node = _load_fixture("compute_scopes_request.json");
  JsonNode *resp = dt_remote_protocol_dispatch(json_node_get_object(request_node), NULL);
  assert_null(resp);   // deferred

  assert_int_equal(g_queue_scopes_calls, 1);
  assert_true(g_queued_scopes_req.want_histogram);
  assert_true(g_queued_scopes_req.want_waveform);
  assert_true(g_queued_scopes_req.want_parade);
  assert_true(g_queued_scopes_req.want_vectorscope);
  assert_int_equal(g_queued_scopes_req.image_size, 512);

  json_node_unref(request_node);
  dt_remote_protocol_set_calls(NULL);
  dt_remote_protocol_set_async(NULL);
}

// image_size is clamped to [128, 1024], not rejected (wire contract).
static void test_compute_scopes_clamps_image_size(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .scopes_prepare = stub_scopes_prepare_ok };
  dt_remote_protocol_set_calls(&calls);
  _install_fake_async(TRUE);

  assert_null(_dispatch_inline_maybe_deferred(
    "{\"id\":41,\"method\":\"compute_scopes\","
    "\"params\":{\"scopes\":[\"waveform\"],\"image_size\":10}}"));
  assert_int_equal(g_queued_scopes_req.image_size, 128);

  assert_null(_dispatch_inline_maybe_deferred(
    "{\"id\":42,\"method\":\"compute_scopes\","
    "\"params\":{\"scopes\":[\"waveform\"],\"image_size\":5000}}"));
  assert_int_equal(g_queued_scopes_req.image_size, 1024);

  assert_int_equal(g_queue_scopes_calls, 2);

  dt_remote_protocol_set_calls(NULL);
  dt_remote_protocol_set_async(NULL);
}

// the validation matrix: missing/empty/duplicate/unknown scopes, wrong
// shapes for every param, strict unknown-key rejection -- all invalid_value,
// none reaching the queue.
static void test_compute_scopes_validation_matrix(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .scopes_prepare = stub_scopes_prepare_ok };
  dt_remote_protocol_set_calls(&calls);
  _install_fake_async(TRUE);

  // scopes: missing / not an array / empty / non-string entry
  _assert_inline_error("{\"id\":50,\"method\":\"compute_scopes\",\"params\":{}}", "invalid_value");
  _assert_inline_error(
    "{\"id\":51,\"method\":\"compute_scopes\",\"params\":{\"scopes\":\"histogram\"}}", "invalid_value");
  _assert_inline_error(
    "{\"id\":52,\"method\":\"compute_scopes\",\"params\":{\"scopes\":[]}}", "invalid_value");
  _assert_inline_error(
    "{\"id\":53,\"method\":\"compute_scopes\",\"params\":{\"scopes\":[7]}}", "invalid_value");

  // unknown scope name; duplicate scope name (protocol reference: both invalid_value)
  _assert_inline_error(
    "{\"id\":54,\"method\":\"compute_scopes\",\"params\":{\"scopes\":[\"histogramme\"]}}",
    "invalid_value");
  _assert_inline_error(
    "{\"id\":55,\"method\":\"compute_scopes\","
    "\"params\":{\"scopes\":[\"histogram\",\"histogram\"]}}", "invalid_value");

  // wrong types for the optional params
  _assert_inline_error(
    "{\"id\":56,\"method\":\"compute_scopes\","
    "\"params\":{\"scopes\":[\"histogram\"],\"include_summary\":\"yes\"}}", "invalid_value");
  _assert_inline_error(
    "{\"id\":57,\"method\":\"compute_scopes\","
    "\"params\":{\"scopes\":[\"histogram\"],\"include_bins\":1}}", "invalid_value");
  _assert_inline_error(
    "{\"id\":58,\"method\":\"compute_scopes\","
    "\"params\":{\"scopes\":[\"histogram\"],\"include_images\":\"no\"}}", "invalid_value");
  _assert_inline_error(
    "{\"id\":59,\"method\":\"compute_scopes\","
    "\"params\":{\"scopes\":[\"histogram\"],\"image_size\":512.5}}", "invalid_value");

  // strict params: unknown key
  _assert_inline_error(
    "{\"id\":60,\"method\":\"compute_scopes\","
    "\"params\":{\"scopes\":[\"histogram\"],\"format\":\"png\"}}", "invalid_value");

  assert_int_equal(g_queue_scopes_calls, 0);
  // every validation failure aborts the pre-registered pending
  assert_int_equal(g_async_begin_calls, g_async_abort_calls);

  dt_remote_protocol_set_calls(NULL);
  dt_remote_protocol_set_async(NULL);
}

static void test_compute_scopes_error_not_in_darkroom(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .scopes_prepare = stub_scopes_prepare_not_in_darkroom };
  dt_remote_protocol_set_calls(&calls);
  _install_fake_async(TRUE);

  _assert_inline_error(
    "{\"id\":61,\"method\":\"compute_scopes\",\"params\":{\"scopes\":[\"histogram\"]}}",
    "not_in_darkroom");
  assert_int_equal(g_queue_scopes_calls, 0);
  assert_int_equal(g_async_abort_calls, 1);

  dt_remote_protocol_set_calls(NULL);
  dt_remote_protocol_set_async(NULL);
}

// queueing failure maps to scope_failed, retryable TRUE (the ratified
// unconditional rule)
static void test_compute_scopes_error_queue_failure(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .scopes_prepare = stub_scopes_prepare_ok };
  dt_remote_protocol_set_calls(&calls);
  _install_fake_async(FALSE);  // queue_scopes reports failure

  JsonNode *resp = _dispatch_inline_maybe_deferred(
    "{\"id\":62,\"method\":\"compute_scopes\",\"params\":{\"scopes\":[\"histogram\"]}}");
  assert_non_null(resp);
  JsonObject *obj = json_node_get_object(resp);
  assert_false(json_object_get_boolean_member(obj, "ok"));
  JsonObject *error = json_object_get_object_member(obj, "error");
  assert_string_equal(json_object_get_string_member(error, "code"), "scope_failed");
  assert_true(json_object_get_boolean_member(error, "retryable"));
  json_node_unref(resp);

  assert_int_equal(g_queue_scopes_calls, 1);
  assert_int_equal(g_async_abort_calls, 1);

  dt_remote_protocol_set_calls(NULL);
  dt_remote_protocol_set_async(NULL);
}

static void test_compute_scopes_without_transport_is_internal_error(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .scopes_prepare = stub_scopes_prepare_ok };
  dt_remote_protocol_set_calls(&calls);
  dt_remote_protocol_set_async(NULL);  // real transport: begin(NULL session) -> NULL pending

  _assert_inline_error(
    "{\"id\":63,\"method\":\"compute_scopes\",\"params\":{\"scopes\":[\"histogram\"]}}", "internal");

  dt_remote_protocol_set_calls(NULL);
}

// -- response shaping (pure) ----------------------------------------------

static const char SCOPES_WAVE_STUB[] = "stub-png-bytes-for-scopes-waveform";
static const char SCOPES_PARADE_STUB[] = "stub-png-bytes-for-scopes-parade";
static const char SCOPES_VEC_STUB[] = "stub-png-bytes-for-scopes-vectorscope";

// a canned result carrying the design-spec example numbers; string/buffer
// fields point at static storage (build_scopes_response only borrows)
static dt_remote_scopes_result_t _canned_scopes_result(void)
{
  dt_remote_scopes_result_t r = { 0 };
  r.revision = 34;
  r.source = (char *)"final_preview";
  r.color_profile = (char *)"linear Rec2020 RGB";
  r.roi = (char *)"full_image";
  r.has_histogram = TRUE;
  r.has_histogram_summary = TRUE;
  r.histogram_summary.bins = 256;
  r.histogram_summary.black_clip_fraction = 0.0012;
  r.histogram_summary.white_clip_fraction = 0.0041;
  r.histogram_summary.p01 = 0.02;
  r.histogram_summary.p50 = 0.41;
  r.histogram_summary.p99 = 0.98;
  r.histogram_summary.mean_red = 0.46;
  r.histogram_summary.mean_green = 0.42;
  r.histogram_summary.mean_blue = 0.37;
  // all four scopes requested with include_images -> one key each, images
  // present (matches compute_scopes_request.json / _response.json)
  r.want_waveform = TRUE;
  r.want_parade = TRUE;
  r.want_vectorscope = TRUE;
  r.image_size = 512;
  r.waveform.present = TRUE;
  r.waveform.png = (uint8_t *)SCOPES_WAVE_STUB;
  r.waveform.png_len = strlen(SCOPES_WAVE_STUB);
  r.waveform.width = 360;
  r.waveform.height = 256;
  r.parade.present = TRUE;
  r.parade.png = (uint8_t *)SCOPES_PARADE_STUB;
  r.parade.png_len = strlen(SCOPES_PARADE_STUB);
  r.parade.width = 1080;
  r.parade.height = 256;
  r.vectorscope.present = TRUE;
  r.vectorscope.png = (uint8_t *)SCOPES_VEC_STUB;
  r.vectorscope.png_len = strlen(SCOPES_VEC_STUB);
  r.vectorscope.width = 512;
  r.vectorscope.height = 512;
  return r;
}

static void test_build_scopes_response_success_matches_fixture(void **state)
{
  (void)state;
  dt_remote_scopes_result_t r = _canned_scopes_result();

  JsonNode *actual = dt_remote_protocol_build_scopes_response(40, &r, NULL);
  JsonNode *expected = _load_fixture("compute_scopes_response.json");
  assert_true(_json_equal(actual, expected));

  json_node_unref(actual);
  json_node_unref(expected);
}

// include_bins: 256 normalized bins per channel under histogram.channel_bins
static void test_build_scopes_response_bins_shape(void **state)
{
  (void)state;
  dt_remote_scopes_result_t r = { 0 };
  r.revision = 7;
  r.has_histogram = TRUE;
  r.has_histogram_bins = TRUE;
  for(int b = 0; b < DT_SCOPES_HISTOGRAM_BINS; b++)
  {
    r.histogram_bins[0][b] = 1.0f;
    r.histogram_bins[1][b] = 0.5f;
    r.histogram_bins[2][b] = 0.0f;
  }

  JsonNode *actual = dt_remote_protocol_build_scopes_response(41, &r, NULL);
  JsonObject *resp = json_node_get_object(actual);
  assert_true(json_object_get_boolean_member(resp, "ok"));
  JsonObject *result = json_object_get_object_member(resp, "result");
  JsonObject *hist = json_object_get_object_member(result, "histogram");
  assert_int_equal((int)json_object_get_int_member(hist, "bins"), 256);
  JsonObject *bins = json_object_get_object_member(hist, "channel_bins");
  JsonArray *red = json_object_get_array_member(bins, "red");
  JsonArray *green = json_object_get_array_member(bins, "green");
  JsonArray *blue = json_object_get_array_member(bins, "blue");
  assert_int_equal((int)json_array_get_length(red), 256);
  assert_int_equal((int)json_array_get_length(green), 256);
  assert_int_equal((int)json_array_get_length(blue), 256);
  assert_true(json_array_get_double_element(red, 0) > 0.99);
  assert_true(json_array_get_double_element(green, 0) > 0.49);
  assert_true(json_array_get_double_element(blue, 0) < 0.01);

  json_node_unref(actual);
}

// finding 3: an image scope requested with include_images=false still emits
// its key -- a minimal {"image_size": N} metadata object (no "image"), so
// the response contains one key per requested scope (protocol reference).
static void test_build_scopes_response_imageless_keys(void **state)
{
  (void)state;
  dt_remote_scopes_result_t r = { 0 };
  r.revision = 12;
  r.want_waveform = TRUE;
  r.want_parade = TRUE;
  r.want_vectorscope = TRUE;
  r.image_size = 512;
  // no .present, no histogram -> only the three imageless image keys

  JsonNode *actual = dt_remote_protocol_build_scopes_response(42, &r, NULL);
  JsonObject *resp = json_node_get_object(actual);
  assert_true(json_object_get_boolean_member(resp, "ok"));
  JsonObject *result = json_object_get_object_member(resp, "result");

  const char *const keys[3] = { "waveform", "parade", "vectorscope" };
  for(int i = 0; i < 3; i++)
  {
    assert_true(json_object_has_member(result, keys[i]));
    JsonObject *o = json_object_get_object_member(result, keys[i]);
    assert_false(json_object_has_member(o, "image"));
    assert_int_equal((int)json_object_get_int_member(o, "image_size"), 512);
  }

  json_node_unref(actual);
}

// the empty-capture-slot error envelope: scope_failed, retryable pinned TRUE
static void test_build_scopes_response_error_matches_fixture(void **state)
{
  (void)state;
  dt_remote_error_t error = {
    .code = DT_REMOTE_ERR_SCOPE_FAILED,
    .message = (char *)"no preview buffer captured yet; retry after the preview updates",
  };

  JsonNode *actual = dt_remote_protocol_build_scopes_response(40, NULL, &error);
  JsonNode *expected = _load_fixture("compute_scopes_error_scope_failed_response.json");
  assert_true(_json_equal(actual, expected));

  json_node_unref(actual);
  json_node_unref(expected);
}

// over-cap composed response degrades to request_too_large (retryable false)
static void test_build_scopes_response_too_large(void **state)
{
  (void)state;
  dt_remote_scopes_result_t r = { 0 };
  r.revision = 34;
  const size_t huge = (size_t)DT_REMOTE_MAX_FRAME;  // inflates past the cap in base64
  r.waveform.present = TRUE;
  r.waveform.png = g_malloc0(huge);
  r.waveform.png_len = huge;
  r.waveform.width = 4096;
  r.waveform.height = 4096;

  JsonNode *actual = dt_remote_protocol_build_scopes_response(40, &r, NULL);
  JsonObject *resp = json_node_get_object(actual);
  assert_false(json_object_get_boolean_member(resp, "ok"));
  JsonObject *error = json_object_get_object_member(resp, "error");
  assert_string_equal(json_object_get_string_member(error, "code"), "request_too_large");
  assert_false(json_object_get_boolean_member(error, "retryable"));

  json_node_unref(actual);
  g_free(r.waveform.png);
}

// -- completion against a real (fake-transport-free) session --------------

// The deliberate contrast with render_preview: finish_scopes performs NO
// completion-time revision drift check. Install a calls table whose live
// revision (35) differs from the result's stamped revision (34): the
// response must still be the SUCCESS envelope carrying revision 34 -- all
// scopes derive from the one captured buffer and stay valid for its stamped
// revision (design spec). This is also the multi-scope revision-coherence
// test: one revision for the whole response.
static void test_finish_scopes_sends_framed_response_without_drift_check(void **state)
{
  (void)state;
  dt_remote_protocol_set_async(NULL);  // the real async transport
  dt_remote_protocol_calls_t calls = { .current_revision = stub_current_revision_drifted };
  dt_remote_protocol_set_calls(&calls);

  dt_remote_session_t *s = _fake_session();
  s->pending_requests = 1;
  dt_remote_pending_t *pending = dt_remote_async_begin(s, 40);

  // heap-allocated copy of the canned result (finish takes ownership)
  dt_remote_scopes_result_t *r = g_malloc0(sizeof(dt_remote_scopes_result_t));
  *r = _canned_scopes_result();
  r->source = g_strdup("final_preview");
  r->color_profile = g_strdup("linear Rec2020 RGB");
  r->roi = g_strdup("full_image");
  r->waveform.png = g_malloc(strlen(SCOPES_WAVE_STUB));
  memcpy(r->waveform.png, SCOPES_WAVE_STUB, strlen(SCOPES_WAVE_STUB));
  r->parade.png = g_malloc(strlen(SCOPES_PARADE_STUB));
  memcpy(r->parade.png, SCOPES_PARADE_STUB, strlen(SCOPES_PARADE_STUB));
  r->vectorscope.png = g_malloc(strlen(SCOPES_VEC_STUB));
  memcpy(r->vectorscope.png, SCOPES_VEC_STUB, strlen(SCOPES_VEC_STUB));

  dt_remote_protocol_finish_scopes(pending, r, NULL);  // takes ownership

  JsonParser *parser = NULL;
  JsonObject *resp = _parse_single_queued_frame(s, &parser);
  JsonNode *expected = _load_fixture("compute_scopes_response.json");
  assert_true(_json_equal(json_parser_get_root(parser), expected));
  // and explicitly: the response carries the STAMPED revision, success,
  // despite the live revision having moved on
  JsonObject *result = json_object_get_object_member(resp, "result");
  assert_int_equal((int)json_object_get_int_member(result, "revision"), 34);
  json_node_unref(expected);
  g_object_unref(parser);

  assert_int_equal(s->pending_requests, 0);
  assert_int_equal(s->io_refs, 0);
  assert_int_equal((int)s->pendings->len, 0);

  _fake_session_free(s);
  dt_remote_protocol_set_calls(NULL);
}

// cancellation-on-disconnect for scopes: release everything, write nothing
static void test_finish_scopes_cancelled_releases_without_writing(void **state)
{
  (void)state;
  dt_remote_protocol_set_async(NULL);

  dt_remote_session_t *s = _fake_session();
  s->io_refs = 1;
  s->pending_requests = 1;
  dt_remote_pending_t *pending = dt_remote_async_begin(s, 40);

  dt_remote_session_request_close(s);
  assert_true(g_cancellable_is_cancelled(pending->cancellable));

  dt_remote_scopes_result_t *r = g_malloc0(sizeof(dt_remote_scopes_result_t));
  r->revision = 34;
  r->source = g_strdup("final_preview");
  r->color_profile = g_strdup("linear Rec2020 RGB");
  r->roi = g_strdup("full_image");
  r->waveform.present = TRUE;
  r->waveform.png = g_malloc(1024 * 1024);  // a result buffer that must be released
  r->waveform.png_len = 1024 * 1024;

  dt_remote_protocol_finish_scopes(pending, r, NULL);

  assert_int_equal((int)g_queue_get_length(s->write_queue), 0);  // nothing written
  assert_int_equal(s->pending_requests, 0);
  assert_int_equal(s->io_refs, 1);
  assert_int_equal((int)s->pendings->len, 0);

  _fake_session_free(s);
}

// a job discarded before running (neither result nor error) answers a
// retryable scope_failed instead of leaving the request dangling
static void test_finish_scopes_discarded_job_fails_retryable(void **state)
{
  (void)state;
  dt_remote_protocol_set_async(NULL);

  dt_remote_session_t *s = _fake_session();
  s->pending_requests = 1;
  dt_remote_pending_t *pending = dt_remote_async_begin(s, 40);

  dt_remote_protocol_finish_scopes(pending, NULL, NULL);

  JsonParser *parser = NULL;
  JsonObject *resp = _parse_single_queued_frame(s, &parser);
  assert_false(json_object_get_boolean_member(resp, "ok"));
  JsonObject *error = json_object_get_object_member(resp, "error");
  assert_string_equal(json_object_get_string_member(error, "code"), "scope_failed");
  assert_true(json_object_get_boolean_member(error, "retryable"));
  g_object_unref(parser);

  assert_int_equal(s->pending_requests, 0);
  assert_int_equal(s->io_refs, 0);
  assert_int_equal((int)s->pendings->len, 0);

  _fake_session_free(s);
}

/* ---------------------------------------------------------------------- */
/* dispatch-level envelope validation                                      */
/* ---------------------------------------------------------------------- */

static void test_error_missing_id(void **state)
{
  (void)state;
  _assert_dispatch_matches("error_missing_id_request.json", "error_missing_id_response.json");
}

static void test_error_invalid_id_type(void **state)
{
  (void)state;
  // same envelope as missing id: an id that can't be interpreted as a
  // non-negative integer is treated identically to an absent one.
  _assert_dispatch_matches("error_invalid_id_type_request.json", "error_missing_id_response.json");
}

static void test_error_missing_method(void **state)
{
  (void)state;
  _assert_dispatch_matches("error_missing_method_request.json", "error_missing_method_response.json");
}

static void test_error_unknown_method(void **state)
{
  (void)state;
  _assert_dispatch_matches("error_unknown_method_request.json", "error_unknown_method_response.json");
}

// compute_scopes (the last unimplemented method) gained its handler in plan
// step 10, so a "not implemented" method no longer exists in the allowlist.
// A method this build does not know about surfaces through the dispatcher's
// existing unknown-method contract (invalid_value, non-retryable) -- this
// fixture pair documents that a plausible FUTURE method name gets exactly
// that treatment rather than a crash or a dangling request.
static void test_method_not_implemented_yet(void **state)
{
  (void)state;
  _assert_dispatch_matches("not_implemented_request.json", "not_implemented_response.json");
}

/* ---------------------------------------------------------------------- */
/* allowlist table                                                          */
/* ---------------------------------------------------------------------- */

static void test_allowlist_has_thirteen_methods_with_expected_flags(void **state)
{
  (void)state;

  static const char *const names[] = {
    "hello", "get_state", "list_modules", "get_module_schema", "get_module_params",
    "set_module_params", "set_module_enabled", "reset_module", "create_module_instance",
    "get_history", "undo", "render_preview", "compute_scopes",
  };
  for(size_t i = 0; i < G_N_ELEMENTS(names); i++)
    assert_non_null(dt_remote_protocol_lookup_method(names[i]));

  assert_null(dt_remote_protocol_lookup_method("does_not_exist"));

  const dt_remote_method_t *hello = dt_remote_protocol_lookup_method("hello");
  assert_false(hello->needs_darkroom);
  assert_false(hello->is_mutation);
  assert_false(hello->is_async);

  const dt_remote_method_t *set_params = dt_remote_protocol_lookup_method("set_module_params");
  assert_true(set_params->needs_darkroom);
  assert_true(set_params->is_mutation);
  assert_false(set_params->is_async);
  assert_non_null(set_params->handler);  // implemented in plan step 7

  const dt_remote_method_t *set_enabled = dt_remote_protocol_lookup_method("set_module_enabled");
  assert_true(set_enabled->is_mutation);
  assert_non_null(set_enabled->handler);  // implemented in plan step 8

  // get_history is a read (not a mutation); undo is a mutation. Both wired
  // in plan step 8.
  const dt_remote_method_t *history = dt_remote_protocol_lookup_method("get_history");
  assert_true(history->needs_darkroom);
  assert_false(history->is_mutation);
  assert_non_null(history->handler);

  const dt_remote_method_t *undo = dt_remote_protocol_lookup_method("undo");
  assert_true(undo->is_mutation);
  assert_non_null(undo->handler);

  const dt_remote_method_t *preview = dt_remote_protocol_lookup_method("render_preview");
  assert_true(preview->needs_darkroom);
  assert_false(preview->is_mutation);
  assert_true(preview->is_async);
  assert_non_null(preview->handler);  // implemented in plan step 9

  const dt_remote_method_t *scopes = dt_remote_protocol_lookup_method("compute_scopes");
  assert_true(scopes->needs_darkroom);
  assert_false(scopes->is_mutation);
  assert_true(scopes->is_async);
  assert_non_null(scopes->handler);  // implemented in plan step 10
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_hello_success),
    cmocka_unit_test(test_hello_error_bad_protocol_version),
    cmocka_unit_test(test_hello_error_missing_token),
    cmocka_unit_test(test_hello_error_unknown_key),

    cmocka_unit_test(test_get_state_darkroom),
    cmocka_unit_test(test_get_state_no_image),
    cmocka_unit_test(test_get_state_ignores_garbage_params),

    cmocka_unit_test(test_list_modules_success),
    cmocka_unit_test(test_list_modules_error_not_in_darkroom),
    cmocka_unit_test(test_list_modules_ignores_garbage_params),

    cmocka_unit_test(test_get_module_schema_success),
    cmocka_unit_test(test_get_module_schema_error_unknown_module),
    cmocka_unit_test(test_get_module_schema_error_internal_is_error_envelope),
    cmocka_unit_test(test_get_module_schema_error_wrong_type_params),
    cmocka_unit_test(test_get_module_schema_error_overlong_string),
    cmocka_unit_test(test_get_module_schema_error_unknown_key),

    cmocka_unit_test(test_get_module_params_success),
    cmocka_unit_test(test_get_module_params_nan_float_serializes_as_null),
    cmocka_unit_test(test_get_module_schema_rgbcurve_semantic_fields),
    cmocka_unit_test(test_get_module_params_rgbcurve_semantic_values),
    cmocka_unit_test(test_get_module_schema_vector_semantic_fields),
    cmocka_unit_test(test_get_module_params_vector_semantic_values),
    cmocka_unit_test(test_get_module_schema_band_semantic_fields),
    cmocka_unit_test(test_get_module_params_band_semantic_values),
    cmocka_unit_test(test_get_module_schema_quantity_semantic_fields),
    cmocka_unit_test(test_get_module_params_quantity_semantic_values),
    cmocka_unit_test(test_get_module_params_quantity_nan_component_serializes_as_null),
    cmocka_unit_test(test_get_module_params_error_unknown_module),
    cmocka_unit_test(test_get_module_params_error_internal_is_error_envelope),
    cmocka_unit_test(test_get_module_params_error_non_finite_instance),
    cmocka_unit_test(test_get_module_params_error_fractional_instance),

    cmocka_unit_test(test_set_module_params_success),
    cmocka_unit_test(test_set_module_params_semantic_schema_failure_does_not_block_primitive_mutation),
    cmocka_unit_test(test_set_module_params_enum_by_name),
    cmocka_unit_test(test_set_module_params_enum_by_int),
    cmocka_unit_test(test_set_module_params_error_invalid_value),
    cmocka_unit_test(test_set_module_params_error_revision_conflict),
    cmocka_unit_test(test_set_module_params_error_unknown_field),
    cmocka_unit_test(test_set_module_params_error_unsupported_field),
    cmocka_unit_test(test_set_module_params_error_denylisted_field),
    cmocka_unit_test(test_set_module_params_error_bad_shapes),
    cmocka_unit_test(test_set_module_params_error_unknown_module),
    cmocka_unit_test(test_set_module_params_error_unknown_instance),
    cmocka_unit_test(test_set_module_params_curve_success),
    cmocka_unit_test(test_set_module_params_curve_error_invalid_spacing),
    cmocka_unit_test(test_set_module_params_curve_error_unknown_id),
    cmocka_unit_test(test_set_module_params_semantic_bad_shapes),
    cmocka_unit_test(test_semantic_vector_entry_parses),
    cmocka_unit_test(test_semantic_vector_requires_values_array),
    cmocka_unit_test(test_semantic_vector_rejects_non_finite_components),
    cmocka_unit_test(test_semantic_vector_rejects_oversized_component_list),
    cmocka_unit_test(test_semantic_vector_rejects_unknown_members),
    cmocka_unit_test(test_semantic_duplicate_ids_rejected_across_classes),
    cmocka_unit_test(test_semantic_vector_preserves_double_precision),
    cmocka_unit_test(test_semantic_band_entry_parses),
    cmocka_unit_test(test_semantic_band_entry_with_x_parses),
    cmocka_unit_test(test_semantic_band_requires_y_array),
    cmocka_unit_test(test_semantic_band_rejects_non_finite_samples),
    cmocka_unit_test(test_semantic_band_rejects_oversized_sample_list),
    cmocka_unit_test(test_semantic_band_rejects_mismatched_x_length),
    cmocka_unit_test(test_semantic_band_rejects_unknown_members),
    cmocka_unit_test(test_semantic_band_preserves_double_precision),

    cmocka_unit_test(test_semantic_quantity_entry_parses),
    cmocka_unit_test(test_semantic_quantity_requires_values_object),
    cmocka_unit_test(test_semantic_quantity_rejects_non_finite_components),
    cmocka_unit_test(test_semantic_quantity_rejects_oversized_component_object),
    cmocka_unit_test(test_semantic_quantity_rejects_unknown_members),
    cmocka_unit_test(test_semantic_quantity_preserves_double_precision),

    cmocka_unit_test(test_hello_curve_params_implies_semantic_values_accepted),

    cmocka_unit_test(test_set_module_enabled_success),
    cmocka_unit_test(test_set_module_enabled_error_revision_conflict),
    cmocka_unit_test(test_set_module_enabled_error_bad_shapes),
    cmocka_unit_test(test_set_module_enabled_error_unknown_module),

    cmocka_unit_test(test_reset_module_success),
    cmocka_unit_test(test_reset_module_error_revision_conflict),
    cmocka_unit_test(test_reset_module_error_bad_shapes),
    cmocka_unit_test(test_reset_module_error_unknown_instance),

    cmocka_unit_test(test_create_module_instance_success),
    cmocka_unit_test(test_create_module_instance_copy_params_true),
    cmocka_unit_test(test_create_module_instance_error_instance_not_supported),
    cmocka_unit_test(test_create_module_instance_error_unknown_instance),
    cmocka_unit_test(test_create_module_instance_error_bad_shapes),

    cmocka_unit_test(test_get_history_success),
    cmocka_unit_test(test_get_history_limit_clamping),
    cmocka_unit_test(test_get_history_items_carry_no_param_blobs),
    cmocka_unit_test(test_get_history_error_bad_shapes),

    cmocka_unit_test(test_undo_success),
    cmocka_unit_test(test_undo_error_revision_conflict),
    cmocka_unit_test(test_undo_error_bad_shapes),

    cmocka_unit_test(test_render_preview_defers_with_defaults),
    cmocka_unit_test(test_render_preview_request_fixture_defers),
    cmocka_unit_test(test_render_preview_clamps_out_of_range_params),
    cmocka_unit_test(test_render_preview_error_bad_shapes),
    cmocka_unit_test(test_render_preview_error_not_in_darkroom),
    cmocka_unit_test(test_render_preview_error_queue_failure),
    cmocka_unit_test(test_render_preview_without_transport_is_internal_error),
    cmocka_unit_test(test_build_preview_response_success_matches_fixture),
    cmocka_unit_test(test_build_preview_response_error_matches_fixture),
    cmocka_unit_test(test_build_preview_response_too_large_matches_fixture),
    cmocka_unit_test(test_preview_fits_frame_boundaries),
    cmocka_unit_test(test_finish_preview_sends_framed_response),
    cmocka_unit_test(test_finish_preview_revision_drift_fails_retryable),
    cmocka_unit_test(test_finish_preview_no_drift_succeeds),
    cmocka_unit_test(test_finish_preview_cancelled_releases_without_writing),
    cmocka_unit_test(test_compute_scopes_defers_with_defaults),
    cmocka_unit_test(test_compute_scopes_request_fixture_defers),
    cmocka_unit_test(test_compute_scopes_clamps_image_size),
    cmocka_unit_test(test_compute_scopes_validation_matrix),
    cmocka_unit_test(test_compute_scopes_error_not_in_darkroom),
    cmocka_unit_test(test_compute_scopes_error_queue_failure),
    cmocka_unit_test(test_compute_scopes_without_transport_is_internal_error),
    cmocka_unit_test(test_build_scopes_response_success_matches_fixture),
    cmocka_unit_test(test_build_scopes_response_bins_shape),
    cmocka_unit_test(test_build_scopes_response_imageless_keys),
    cmocka_unit_test(test_build_scopes_response_error_matches_fixture),
    cmocka_unit_test(test_build_scopes_response_too_large),
    cmocka_unit_test(test_finish_scopes_sends_framed_response_without_drift_check),
    cmocka_unit_test(test_finish_scopes_cancelled_releases_without_writing),
    cmocka_unit_test(test_finish_scopes_discarded_job_fails_retryable),

    cmocka_unit_test(test_error_missing_id),
    cmocka_unit_test(test_error_invalid_id_type),
    cmocka_unit_test(test_error_missing_method),
    cmocka_unit_test(test_error_unknown_method),
    cmocka_unit_test(test_method_not_implemented_yet),

    cmocka_unit_test(test_allowlist_has_thirteen_methods_with_expected_flags),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
