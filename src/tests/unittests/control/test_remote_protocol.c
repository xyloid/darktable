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
#include "control/remote_protocol.h"

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
  (void)out;
  if(error) *error = _make_error(DT_REMOTE_ERR_UNKNOWN_MODULE, g_strdup_printf("unknown module '%s'", op));
  return FALSE;
}

static gboolean stub_get_module_params_exposure(const dt_remote_module_ref_t *ref, GPtrArray **out,
                                                dt_remote_error_t **error)
{
  (void)ref;
  (void)error;
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
  return TRUE;
}

static gboolean stub_get_module_params_unknown_module(const dt_remote_module_ref_t *ref, GPtrArray **out,
                                                      dt_remote_error_t **error)
{
  (void)out;
  if(error)
    *error = _make_error(DT_REMOTE_ERR_UNKNOWN_MODULE, g_strdup_printf("unknown module '%s'", ref->op));
  return FALSE;
}

/* --- set_module_params stubs (plan step 7) ------------------------------ */

// Like stub_get_module_schema_exposure but with the "black" field the wire
// contract's set_module_params example patches alongside "exposure" -- kept
// separate so the get_module_schema fixture diff stays untouched.
static gboolean stub_get_module_schema_exposure_full(const char *op, dt_remote_module_schema_t **out,
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
  JsonArray *caps = json_object_get_array_member(result, "capabilities");
  assert_int_equal(json_array_get_length(caps), 1);
  assert_string_equal(json_array_get_string_element(caps, 0), "params");

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

static void test_get_module_params_error_unknown_module(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = { .get_module_params = stub_get_module_params_unknown_module };
  dt_remote_protocol_set_calls(&calls);
  _assert_dispatch_matches("get_module_params_error_unknown_module_request.json",
                           "get_module_params_error_unknown_module_response.json");
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
    .get_module_schema = stub_get_module_schema_exposure_full,
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
    .get_module_schema = stub_get_module_schema_exposure_full,
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
    .get_module_schema = stub_get_module_schema_exposure_full,
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
    .get_module_schema = stub_get_module_schema_exposure_full,
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
    .get_module_schema = stub_get_module_schema_exposure_full,
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
    .get_module_schema = stub_get_module_schema_exposure_full,
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
    .get_module_schema = stub_get_module_schema_exposure_full,
    .set_module_params = stub_set_module_params_must_not_be_called,
  };
  dt_remote_protocol_set_calls(&calls);
  _assert_inline_error(
    "{\"id\":32,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"exposure\",\"values\":{\"curve\":1.0}}}",
    "unsupported_field");
  dt_remote_protocol_set_calls(NULL);
}

// handler-side shape rejections: every one must leave the engine untouched.
static void test_set_module_params_error_bad_shapes(void **state)
{
  (void)state;
  dt_remote_protocol_calls_t calls = {
    .get_module_schema = stub_get_module_schema_exposure_full,
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
    .get_module_schema = stub_get_module_schema_unknown,
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
    .get_module_schema = stub_get_module_schema_exposure_full,
    .set_module_params = stub_set_module_params_unknown_instance,
  };
  dt_remote_protocol_set_calls(&calls);
  _assert_inline_error(
    "{\"id\":43,\"method\":\"set_module_params\","
    "\"params\":{\"module\":\"exposure\",\"instance\":3,\"values\":{\"exposure\":0.5}}}",
    "unknown_instance");
  dt_remote_protocol_set_calls(NULL);
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
  assert_null(set_enabled->handler);  // not implemented until its own step (8)

  const dt_remote_method_t *preview = dt_remote_protocol_lookup_method("render_preview");
  assert_true(preview->needs_darkroom);
  assert_false(preview->is_mutation);
  assert_true(preview->is_async);
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
    cmocka_unit_test(test_get_module_schema_error_wrong_type_params),
    cmocka_unit_test(test_get_module_schema_error_overlong_string),
    cmocka_unit_test(test_get_module_schema_error_unknown_key),

    cmocka_unit_test(test_get_module_params_success),
    cmocka_unit_test(test_get_module_params_error_unknown_module),
    cmocka_unit_test(test_get_module_params_error_non_finite_instance),
    cmocka_unit_test(test_get_module_params_error_fractional_instance),

    cmocka_unit_test(test_set_module_params_success),
    cmocka_unit_test(test_set_module_params_enum_by_name),
    cmocka_unit_test(test_set_module_params_enum_by_int),
    cmocka_unit_test(test_set_module_params_error_invalid_value),
    cmocka_unit_test(test_set_module_params_error_revision_conflict),
    cmocka_unit_test(test_set_module_params_error_unknown_field),
    cmocka_unit_test(test_set_module_params_error_unsupported_field),
    cmocka_unit_test(test_set_module_params_error_bad_shapes),
    cmocka_unit_test(test_set_module_params_error_unknown_module),
    cmocka_unit_test(test_set_module_params_error_unknown_instance),

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
