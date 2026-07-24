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

#include "control/remote_masks.h"

#include "common/darktable.h"
#include "develop/develop.h"
#include "develop/imageop.h"

#include <float.h>
#include <math.h>
#include <string.h>

// Raw mask storage limits. Values outside these ranges are rejected instead
// of being silently clamped so the caller can distinguish its requested
// geometry from what darktable would otherwise store.
#define DTRM_SIZE_MIN 0.0005
#define DTRM_SIZE_MAX 1.0
#define DTRM_COMPRESSION_MAX 1.0
#define DTRM_CURVATURE_ABS 2.0
#define DTRM_CENTER_LO (-0.5)
#define DTRM_CENTER_HI 1.5

static gboolean _has_clone_semantics(const dt_masks_type_t type)
{
  return type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE);
}

static unsigned int _editable_base_count(const dt_masks_type_t type)
{
  return !!(type & DT_MASKS_CIRCLE)
         + !!(type & DT_MASKS_ELLIPSE)
         + !!(type & DT_MASKS_GRADIENT);
}

dt_remote_shape_kind_t dt_remote_masks_kind_from_type(const dt_masks_type_t type)
{
  if(type & DT_MASKS_GROUP) return DT_REMOTE_SHAPE_UNSUPPORTED;
  if(_has_clone_semantics(type)) return DT_REMOTE_SHAPE_UNSUPPORTED;
#ifdef HAVE_AI
  if(type & DT_MASKS_OBJECT) return DT_REMOTE_SHAPE_UNSUPPORTED;
#endif
  if(type & (DT_MASKS_PATH | DT_MASKS_BRUSH)) return DT_REMOTE_SHAPE_UNSUPPORTED;
  if(_editable_base_count(type) != 1) return DT_REMOTE_SHAPE_UNSUPPORTED;
  if(type & DT_MASKS_CIRCLE) return DT_REMOTE_SHAPE_CIRCLE;
  if(type & DT_MASKS_ELLIPSE) return DT_REMOTE_SHAPE_ELLIPSE;
  return DT_REMOTE_SHAPE_GRADIENT;
}

const char *dt_remote_masks_type_string(const dt_masks_type_t type)
{
  if(type & DT_MASKS_GROUP) return "group";
  if(_has_clone_semantics(type)) return "clone";
#ifdef HAVE_AI
  if(type & DT_MASKS_OBJECT) return "object";
#endif

  const unsigned int bases =
    !!(type & DT_MASKS_CIRCLE)
    + !!(type & DT_MASKS_ELLIPSE)
    + !!(type & DT_MASKS_GRADIENT)
    + !!(type & DT_MASKS_PATH)
    + !!(type & DT_MASKS_BRUSH);
  if(bases != 1) return "unsupported";
  if(type & DT_MASKS_CIRCLE) return "circle";
  if(type & DT_MASKS_ELLIPSE) return "ellipse";
  if(type & DT_MASKS_GRADIENT) return "gradient";
  if(type & DT_MASKS_PATH) return "path";
  return "brush";
}

gboolean dt_remote_masks_type_from_string(const char *s, dt_masks_type_t *out)
{
  if(!s || !out) return FALSE;
  if(!strcmp(s, "circle"))
  {
    *out = DT_MASKS_CIRCLE;
    return TRUE;
  }
  if(!strcmp(s, "ellipse"))
  {
    *out = DT_MASKS_ELLIPSE;
    return TRUE;
  }
  if(!strcmp(s, "gradient"))
  {
    *out = DT_MASKS_GRADIENT;
    return TRUE;
  }
  return FALSE;
}

static char *_details_json(const char *parameter, const char *constraint)
{
  JsonObject *details = json_object_new();
  json_object_set_string_member(details, "parameter", parameter);
  json_object_set_string_member(details, "constraint", constraint);

  JsonNode *root = json_node_new(JSON_NODE_OBJECT);
  json_node_take_object(root, details);
  JsonGenerator *generator = json_generator_new();
  json_generator_set_root(generator, root);
  char *json = json_generator_to_data(generator, NULL);
  g_object_unref(generator);
  json_node_unref(root);
  return json;
}

static gboolean _err(dt_remote_error_t **error,
                     const dt_remote_error_code_t code,
                     const char *parameter,
                     const char *constraint)
{
  if(error)
  {
    *error = dt_remote_error_new(code, "invalid geometry member '%s'",
                                 parameter);
    (*error)->details_json = _details_json(parameter, constraint);
  }
  return FALSE;
}

static gboolean _err_invalid(dt_remote_error_t **error,
                             const char *parameter,
                             const char *constraint)
{
  return _err(error, DT_REMOTE_ERR_INVALID_VALUE, parameter, constraint);
}

static gboolean _err_transform(dt_remote_error_t **error,
                               const char *parameter)
{
  return _err(error, DT_REMOTE_ERR_PREVIEW_FAILED, parameter,
              "transform_failed");
}

static gboolean _err_unsupported(dt_remote_error_t **error)
{
  if(error)
    *error = dt_remote_error_new(DT_REMOTE_ERR_UNSUPPORTED_FIELD,
                                 "shape type is not editable");
  return FALSE;
}

// JSON-GLib stores parsed integer and real numbers as distinct value types.
// Coerce both explicitly; json_node_get_double() does not safely coerce an
// INT64 node on all supported JSON-GLib versions.
static gboolean _node_num(JsonNode *node, double *value)
{
  if(!node || !value || json_node_get_node_type(node) != JSON_NODE_VALUE)
    return FALSE;

  const GType type = json_node_get_value_type(node);
  if(type == G_TYPE_DOUBLE)
  {
    *value = json_node_get_double(node);
    return TRUE;
  }
  if(type == G_TYPE_INT64)
  {
    *value = (double)json_node_get_int(node);
    return TRUE;
  }
  return FALSE;
}

static gboolean _float_representable(const double value)
{
  if(!isfinite(value) || value < -FLT_MAX || value > FLT_MAX) return FALSE;
  const float stored = (float)value;
  return value == 0.0 || stored != 0.0f;
}

static gboolean _num(JsonObject *object,
                     const char *member,
                     double *value,
                     dt_remote_error_t **error)
{
  if(!json_object_has_member(object, member))
    return _err_invalid(error, member, "missing_member");
  if(!_node_num(json_object_get_member(object, member), value))
    return _err_invalid(error, member, "not_a_number");
  if(!isfinite(*value))
    return _err_invalid(error, member, "not_finite");
  if(!_float_representable(*value))
    return _err_invalid(error, member, "not_float_representable");
  return TRUE;
}

static gboolean _point2(JsonObject *object,
                        const char *member,
                        double *x,
                        double *y,
                        dt_remote_error_t **error)
{
  if(!json_object_has_member(object, member))
    return _err_invalid(error, member, "missing_member");
  JsonNode *node = json_object_get_member(object, member);
  if(json_node_get_node_type(node) != JSON_NODE_ARRAY)
    return _err_invalid(error, member, "expected_pair");
  JsonArray *array = json_node_get_array(node);
  if(!array || json_array_get_length(array) != 2)
    return _err_invalid(error, member, "expected_pair");
  if(!_node_num(json_array_get_element(array, 0), x)
     || !_node_num(json_array_get_element(array, 1), y))
    return _err_invalid(error, member, "not_a_number");
  if(!isfinite(*x) || !isfinite(*y))
    return _err_invalid(error, member, "not_finite");
  if(!_float_representable(*x) || !_float_representable(*y))
    return _err_invalid(error, member, "not_float_representable");
  if(*x < DTRM_CENTER_LO || *x > DTRM_CENTER_HI
     || *y < DTRM_CENTER_LO || *y > DTRM_CENTER_HI)
    return _err_invalid(error, member, "out_of_canvas");
  return TRUE;
}

static gboolean _positive_pair(JsonObject *object,
                               const char *member,
                               double *a,
                               double *b,
                               dt_remote_error_t **error)
{
  if(!json_object_has_member(object, member))
    return _err_invalid(error, member, "missing_member");
  JsonNode *node = json_object_get_member(object, member);
  if(json_node_get_node_type(node) != JSON_NODE_ARRAY)
    return _err_invalid(error, member, "expected_pair");
  JsonArray *array = json_node_get_array(node);
  if(!array || json_array_get_length(array) != 2)
    return _err_invalid(error, member, "expected_pair");
  if(!_node_num(json_array_get_element(array, 0), a)
     || !_node_num(json_array_get_element(array, 1), b))
    return _err_invalid(error, member, "not_a_number");
  if(!isfinite(*a) || !isfinite(*b))
    return _err_invalid(error, member, "not_finite");
  if(!_float_representable(*a) || !_float_representable(*b))
    return _err_invalid(error, member, "not_float_representable");
  if(*a <= 0.0 || *b <= 0.0)
    return _err_invalid(error, member, "must_be_positive");
  return TRUE;
}

static gboolean _string(JsonObject *object,
                        const char *member,
                        const char **value,
                        dt_remote_error_t **error)
{
  if(!json_object_has_member(object, member))
    return _err_invalid(error, member, "missing_member");
  JsonNode *node = json_object_get_member(object, member);
  if(!node || json_node_get_node_type(node) != JSON_NODE_VALUE
     || json_node_get_value_type(node) != G_TYPE_STRING)
    return _err_invalid(error, member, "not_a_string");
  *value = json_node_get_string(node);
  if(!*value) return _err_invalid(error, member, "not_a_string");
  return TRUE;
}

static gboolean _reject_unknown(JsonObject *object,
                                const char *const *allowed,
                                dt_remote_error_t **error)
{
  GList *members = json_object_get_members(object);
  for(GList *member = members; member; member = member->next)
  {
    gboolean known = FALSE;
    for(const char *const *candidate = allowed; *candidate; candidate++)
    {
      if(!strcmp(member->data, *candidate))
      {
        known = TRUE;
        break;
      }
    }
    if(!known)
    {
      const gboolean result =
        _err_invalid(error, member->data, "unknown_member");
      g_list_free(members);
      return result;
    }
  }
  g_list_free(members);
  return TRUE;
}

gboolean dt_remote_masks_geometry_validate(const dt_masks_type_t type,
                                           JsonObject *geom,
                                           dt_remote_error_t **error)
{
  const dt_remote_shape_kind_t kind = dt_remote_masks_kind_from_type(type);
  if(kind == DT_REMOTE_SHAPE_UNSUPPORTED)
    return _err_unsupported(error);
  if(!geom)
    return _err_invalid(error, "geometry", "expected_object");

  double x = 0.0;
  double y = 0.0;
  double value = 0.0;
  switch(kind)
  {
    case DT_REMOTE_SHAPE_CIRCLE:
    {
      static const char *const allowed[] =
        { "center", "radius", "border", NULL };
      if(!_reject_unknown(geom, allowed, error)
         || !_point2(geom, "center", &x, &y, error)
         || !_num(geom, "radius", &value, error))
        return FALSE;
      if(value <= 0.0)
        return _err_invalid(error, "radius", "must_be_positive");
      if(!_num(geom, "border", &value, error)) return FALSE;
      if(value < 0.0)
        return _err_invalid(error, "border", "must_be_nonnegative");
      return TRUE;
    }

    case DT_REMOTE_SHAPE_ELLIPSE:
    {
      static const char *const allowed[] =
        { "center", "radius", "rotation", "border", "border_mode", NULL };
      double radius_a = 0.0;
      double radius_b = 0.0;
      const char *border_mode = NULL;
      if(!_reject_unknown(geom, allowed, error)
         || !_point2(geom, "center", &x, &y, error)
         || !_positive_pair(geom, "radius", &radius_a, &radius_b, error)
         || !_num(geom, "rotation", &value, error)
         || !_num(geom, "border", &value, error))
        return FALSE;
      if(value < 0.0)
        return _err_invalid(error, "border", "must_be_nonnegative");
      if(!_string(geom, "border_mode", &border_mode, error)) return FALSE;
      if(g_strcmp0(border_mode, "equidistant")
         && g_strcmp0(border_mode, "proportional"))
        return _err_invalid(error, "border_mode",
                            "must_be_equidistant_or_proportional");
      return TRUE;
    }

    case DT_REMOTE_SHAPE_GRADIENT:
    {
      static const char *const allowed[] =
        { "anchor", "rotation", "compression", "steepness", "curvature",
          NULL };
      if(!_reject_unknown(geom, allowed, error)
         || !_point2(geom, "anchor", &x, &y, error)
         || !_num(geom, "rotation", &value, error)
         || !_num(geom, "compression", &value, error))
        return FALSE;
      if(value <= 0.0)
        return _err_invalid(error, "compression", "must_be_positive");
      if(!_num(geom, "steepness", &value, error)
         || !_num(geom, "curvature", &value, error))
        return FALSE;
      if(fabs(value) > DTRM_CURVATURE_ABS)
        return _err_invalid(error, "curvature", "abs_gt_2");
      return TRUE;
    }

    case DT_REMOTE_SHAPE_UNSUPPORTED:
      break;
  }
  return _err_unsupported(error);
}

static double _member_num(JsonObject *object, const char *member)
{
  double value = 0.0;
  _node_num(json_object_get_member(object, member), &value);
  return value;
}

static void _member_pair(JsonObject *object,
                         const char *member,
                         double *a,
                         double *b)
{
  JsonArray *array = json_node_get_array(json_object_get_member(object, member));
  _node_num(json_array_get_element(array, 0), a);
  _node_num(json_array_get_element(array, 1), b);
}

static gboolean _in_storage_range(const double value,
                                  const double minimum,
                                  const double maximum)
{
  return isfinite(value) && value >= minimum && value <= maximum
         && _float_representable(value);
}

static gboolean _raw_center_ok(const double x, const double y)
{
  return _in_storage_range(x, DTRM_CENTER_LO, DTRM_CENTER_HI)
         && _in_storage_range(y, DTRM_CENTER_LO, DTRM_CENTER_HI);
}

gboolean dt_remote_masks_geometry_to_points(dt_develop_t *dev,
                                            const dt_masks_type_t type,
                                            JsonObject *geom,
                                            void *point_out,
                                            dt_remote_error_t **error)
{
  if(!point_out)
    return _err_invalid(error, "geometry", "missing_output");
  if(!dev)
  {
    if(error)
      *error = dt_remote_error_new(DT_REMOTE_ERR_INTERNAL,
                                   "no develop context");
    return FALSE;
  }
  if(!dt_remote_masks_geometry_validate(type, geom, error)) return FALSE;

  switch(dt_remote_masks_kind_from_type(type))
  {
    case DT_REMOTE_SHAPE_CIRCLE:
    {
      dt_masks_point_circle_t result = { 0 };
      double center_x = 0.0;
      double center_y = 0.0;
      double raw_x = 0.0;
      double raw_y = 0.0;
      double raw_radius = 0.0;
      double raw_border = 0.0;
      gboolean exact = FALSE;
      _member_pair(geom, "center", &center_x, &center_y);
      dt_remote_transform_preview_to_raw_point(dev, center_x, center_y,
                                               &raw_x, &raw_y);
      if(!isfinite(raw_x) || !isfinite(raw_y)
         || !_float_representable(raw_x) || !_float_representable(raw_y))
        return _err_transform(error, "center");
      if(!_raw_center_ok(raw_x, raw_y))
        return _err_invalid(error, "center", "out_of_range");
      if(!dt_remote_transform_preview_to_raw_size(
           dev, center_x, center_y, _member_num(geom, "radius"),
           &raw_radius, &exact))
        return _err_transform(error, "radius");
      if(!isfinite(raw_radius) || !_float_representable(raw_radius))
        return _err_transform(error, "radius");
      if(!_in_storage_range(raw_radius, DTRM_SIZE_MIN, DTRM_SIZE_MAX))
        return _err_invalid(error, "radius", "out_of_range");
      if(!dt_remote_transform_preview_to_raw_size(
           dev, center_x, center_y, _member_num(geom, "border"),
           &raw_border, &exact))
        return _err_transform(error, "border");
      if(!isfinite(raw_border) || !_float_representable(raw_border))
        return _err_transform(error, "border");
      if(!_in_storage_range(raw_border, DTRM_SIZE_MIN, DTRM_SIZE_MAX))
        return _err_invalid(error, "border", "out_of_range");

      result.center[0] = (float)raw_x;
      result.center[1] = (float)raw_y;
      result.radius = (float)raw_radius;
      result.border = (float)raw_border;
      memcpy(point_out, &result, sizeof(result));
      return TRUE;
    }

    case DT_REMOTE_SHAPE_ELLIPSE:
    {
      dt_masks_point_ellipse_t result = { 0 };
      double center_x = 0.0;
      double center_y = 0.0;
      double raw_x = 0.0;
      double raw_y = 0.0;
      double preview_radius_a = 0.0;
      double preview_radius_b = 0.0;
      double raw_radius_a = 0.0;
      double raw_radius_b = 0.0;
      double raw_border = 0.0;
      double raw_rotation = 0.0;
      gboolean exact = FALSE;
      _member_pair(geom, "center", &center_x, &center_y);
      _member_pair(geom, "radius", &preview_radius_a, &preview_radius_b);
      dt_remote_transform_preview_to_raw_point(dev, center_x, center_y,
                                               &raw_x, &raw_y);
      if(!isfinite(raw_x) || !isfinite(raw_y)
         || !_float_representable(raw_x) || !_float_representable(raw_y))
        return _err_transform(error, "center");
      if(!_raw_center_ok(raw_x, raw_y))
        return _err_invalid(error, "center", "out_of_range");
      if(!dt_remote_transform_preview_to_raw_size(
           dev, center_x, center_y, preview_radius_a, &raw_radius_a, &exact)
         || !dt_remote_transform_preview_to_raw_size(
           dev, center_x, center_y, preview_radius_b, &raw_radius_b, &exact))
        return _err_transform(error, "radius");
      if(!isfinite(raw_radius_a) || !isfinite(raw_radius_b)
         || !_float_representable(raw_radius_a)
         || !_float_representable(raw_radius_b))
        return _err_transform(error, "radius");
      if(!_in_storage_range(raw_radius_a, DTRM_SIZE_MIN, DTRM_SIZE_MAX)
         || !_in_storage_range(raw_radius_b, DTRM_SIZE_MIN, DTRM_SIZE_MAX))
        return _err_invalid(error, "radius", "out_of_range");
      if(!dt_remote_transform_preview_to_raw_angle(
           dev, center_x, center_y, _member_num(geom, "rotation"),
           &raw_rotation))
        return _err_transform(error, "rotation");
      if(!isfinite(raw_rotation) || !_float_representable(raw_rotation))
        return _err_transform(error, "rotation");

      const char *border_mode =
        json_object_get_string_member(geom, "border_mode");
      const gboolean proportional =
        !strcmp(border_mode, "proportional");
      if(proportional)
      {
        raw_border = _member_num(geom, "border");
      }
      else if(!dt_remote_transform_preview_to_raw_size(
                dev, center_x, center_y, _member_num(geom, "border"),
                &raw_border, &exact))
      {
        return _err_transform(error, "border");
      }
      if(!isfinite(raw_border) || !_float_representable(raw_border))
        return _err_transform(error, "border");
      if(!_in_storage_range(raw_border, DTRM_SIZE_MIN, DTRM_SIZE_MAX))
        return _err_invalid(error, "border", "out_of_range");

      result.center[0] = (float)raw_x;
      result.center[1] = (float)raw_y;
      result.radius[0] = (float)raw_radius_a;
      result.radius[1] = (float)raw_radius_b;
      result.rotation = (float)raw_rotation;
      result.border = (float)raw_border;
      result.flags = proportional ? DT_MASKS_ELLIPSE_PROPORTIONAL
                                  : DT_MASKS_ELLIPSE_EQUIDISTANT;
      memcpy(point_out, &result, sizeof(result));
      return TRUE;
    }

    case DT_REMOTE_SHAPE_GRADIENT:
    {
      dt_masks_point_gradient_t result = { 0 };
      double anchor_x = 0.0;
      double anchor_y = 0.0;
      double raw_x = 0.0;
      double raw_y = 0.0;
      double raw_rotation = 0.0;
      _member_pair(geom, "anchor", &anchor_x, &anchor_y);
      dt_remote_transform_preview_to_raw_point(dev, anchor_x, anchor_y,
                                               &raw_x, &raw_y);
      if(!isfinite(raw_x) || !isfinite(raw_y)
         || !_float_representable(raw_x) || !_float_representable(raw_y))
        return _err_transform(error, "anchor");
      if(!_raw_center_ok(raw_x, raw_y))
        return _err_invalid(error, "anchor", "out_of_range");
      if(!dt_remote_transform_preview_to_raw_angle(
           dev, anchor_x, anchor_y, _member_num(geom, "rotation"),
           &raw_rotation))
        return _err_transform(error, "rotation");
      if(!isfinite(raw_rotation) || !_float_representable(raw_rotation))
        return _err_transform(error, "rotation");

      const double compression = _member_num(geom, "compression");
      const double steepness = _member_num(geom, "steepness");
      const double curvature = _member_num(geom, "curvature");
      if(!_in_storage_range(compression, nextafter(0.0, 1.0),
                            DTRM_COMPRESSION_MAX)
         || (float)compression <= 0.0f)
        return _err_invalid(error, "compression", "out_of_range");

      result.anchor[0] = (float)raw_x;
      result.anchor[1] = (float)raw_y;
      result.rotation = (float)raw_rotation;
      result.compression = (float)compression;
      result.steepness = (float)steepness;
      result.curvature = (float)curvature;
      result.state = DT_MASKS_GRADIENT_STATE_SIGMOIDAL;
      memcpy(point_out, &result, sizeof(result));
      return TRUE;
    }

    case DT_REMOTE_SHAPE_UNSUPPORTED:
      return _err_unsupported(error);
  }
  return _err_unsupported(error);
}

static void _add_point2(JsonObject *object,
                        const char *member,
                        const double a,
                        const double b)
{
  JsonArray *array = json_array_new();
  json_array_add_double_element(array, a);
  json_array_add_double_element(array, b);
  json_object_set_array_member(object, member, array);
}

static JsonNode *_take_object(JsonObject *object)
{
  JsonNode *node = json_node_new(JSON_NODE_OBJECT);
  json_node_take_object(node, object);
  return node;
}

static gboolean _output_number_ok(const double value)
{
  return isfinite(value);
}

JsonNode *dt_remote_masks_points_to_geometry(dt_develop_t *dev,
                                             dt_masks_form_t *form)
{
  if(!dev || !form || !form->points) return NULL;
  const dt_remote_shape_kind_t kind =
    dt_remote_masks_kind_from_type(form->type);
  if(kind == DT_REMOTE_SHAPE_UNSUPPORTED) return NULL;

  JsonObject *object = json_object_new();
  gboolean exact = TRUE;
  double preview_x = 0.0;
  double preview_y = 0.0;
  double preview_value = 0.0;

  switch(kind)
  {
    case DT_REMOTE_SHAPE_CIRCLE:
    {
      const dt_masks_point_circle_t *circle = form->points->data;
      gboolean probe_exact = TRUE;
      dt_remote_transform_raw_to_preview_point(
        dev, circle->center[0], circle->center[1], &preview_x, &preview_y);
      if(!_output_number_ok(preview_x) || !_output_number_ok(preview_y))
        goto transform_failed;
      _add_point2(object, "center", preview_x, preview_y);
      if(!dt_remote_transform_raw_to_preview_size(
           dev, circle->center[0], circle->center[1], circle->radius,
           &preview_value, &probe_exact)
         || !_output_number_ok(preview_value))
        goto transform_failed;
      exact = exact && probe_exact;
      json_object_set_double_member(object, "radius", preview_value);
      if(!dt_remote_transform_raw_to_preview_size(
           dev, circle->center[0], circle->center[1], circle->border,
           &preview_value, &probe_exact)
         || !_output_number_ok(preview_value))
        goto transform_failed;
      exact = exact && probe_exact;
      json_object_set_double_member(object, "border", preview_value);
      break;
    }

    case DT_REMOTE_SHAPE_ELLIPSE:
    {
      const dt_masks_point_ellipse_t *ellipse = form->points->data;
      gboolean probe_exact = TRUE;
      double preview_rotation = 0.0;
      dt_remote_transform_raw_to_preview_point(
        dev, ellipse->center[0], ellipse->center[1], &preview_x, &preview_y);
      if(!_output_number_ok(preview_x) || !_output_number_ok(preview_y))
        goto transform_failed;
      _add_point2(object, "center", preview_x, preview_y);

      JsonArray *radius = json_array_new();
      if(!dt_remote_transform_raw_to_preview_size(
           dev, ellipse->center[0], ellipse->center[1], ellipse->radius[0],
           &preview_value, &probe_exact)
         || !_output_number_ok(preview_value))
      {
        json_array_unref(radius);
        goto transform_failed;
      }
      exact = exact && probe_exact;
      json_array_add_double_element(radius, preview_value);
      if(!dt_remote_transform_raw_to_preview_size(
           dev, ellipse->center[0], ellipse->center[1], ellipse->radius[1],
           &preview_value, &probe_exact)
         || !_output_number_ok(preview_value))
      {
        json_array_unref(radius);
        goto transform_failed;
      }
      exact = exact && probe_exact;
      json_array_add_double_element(radius, preview_value);
      json_object_set_array_member(object, "radius", radius);

      if(!dt_remote_transform_raw_to_preview_angle(
           dev, ellipse->center[0], ellipse->center[1], ellipse->rotation,
           &preview_rotation)
         || !_output_number_ok(preview_rotation))
        goto transform_failed;
      json_object_set_double_member(object, "rotation", preview_rotation);

      const gboolean proportional =
        ellipse->flags == DT_MASKS_ELLIPSE_PROPORTIONAL;
      json_object_set_string_member(object, "border_mode",
                                    proportional ? "proportional"
                                                 : "equidistant");
      if(proportional)
      {
        if(!_output_number_ok(ellipse->border)) goto transform_failed;
        json_object_set_double_member(object, "border", ellipse->border);
      }
      else
      {
        if(!dt_remote_transform_raw_to_preview_size(
             dev, ellipse->center[0], ellipse->center[1], ellipse->border,
             &preview_value, &probe_exact)
           || !_output_number_ok(preview_value))
          goto transform_failed;
        exact = exact && probe_exact;
        json_object_set_double_member(object, "border", preview_value);
      }
      break;
    }

    case DT_REMOTE_SHAPE_GRADIENT:
    {
      const dt_masks_point_gradient_t *gradient = form->points->data;
      double preview_rotation = 0.0;
      dt_remote_transform_raw_to_preview_point(
        dev, gradient->anchor[0], gradient->anchor[1], &preview_x, &preview_y);
      if(!_output_number_ok(preview_x) || !_output_number_ok(preview_y))
        goto transform_failed;
      _add_point2(object, "anchor", preview_x, preview_y);
      if(!dt_remote_transform_raw_to_preview_angle(
           dev, gradient->anchor[0], gradient->anchor[1],
           gradient->rotation, &preview_rotation)
         || !_output_number_ok(preview_rotation))
        goto transform_failed;
      if(!_output_number_ok(gradient->compression)
         || !_output_number_ok(gradient->steepness)
         || !_output_number_ok(gradient->curvature))
        goto transform_failed;
      json_object_set_double_member(object, "rotation", preview_rotation);
      json_object_set_double_member(object, "compression",
                                    gradient->compression);
      json_object_set_double_member(object, "steepness", gradient->steepness);
      json_object_set_double_member(object, "curvature", gradient->curvature);
      break;
    }

    case DT_REMOTE_SHAPE_UNSUPPORTED:
      goto transform_failed;
  }

  json_object_set_string_member(object, "size_mapping",
                                exact ? "exact" : "approximate");
  return _take_object(object);

transform_failed:
  json_object_unref(object);
  return NULL;
}

static gboolean _raw_pair_ok(const float pair[2])
{
  return isfinite(pair[0]) && isfinite(pair[1]);
}

JsonNode *dt_remote_masks_points_to_raw_geometry(dt_masks_form_t *form)
{
  if(!form || !form->points) return NULL;
  const dt_remote_shape_kind_t kind =
    dt_remote_masks_kind_from_type(form->type);
  if(kind == DT_REMOTE_SHAPE_UNSUPPORTED) return NULL;

  JsonObject *object = json_object_new();
  switch(kind)
  {
    case DT_REMOTE_SHAPE_CIRCLE:
    {
      const dt_masks_point_circle_t *circle = form->points->data;
      if(!_raw_pair_ok(circle->center)
         || !isfinite(circle->radius) || !isfinite(circle->border))
        goto invalid_raw;
      _add_point2(object, "center", circle->center[0], circle->center[1]);
      json_object_set_double_member(object, "radius", circle->radius);
      json_object_set_double_member(object, "border", circle->border);
      break;
    }

    case DT_REMOTE_SHAPE_ELLIPSE:
    {
      const dt_masks_point_ellipse_t *ellipse = form->points->data;
      if(!_raw_pair_ok(ellipse->center)
         || !_raw_pair_ok(ellipse->radius)
         || !isfinite(ellipse->rotation) || !isfinite(ellipse->border))
        goto invalid_raw;
      _add_point2(object, "center", ellipse->center[0], ellipse->center[1]);
      JsonArray *radius = json_array_new();
      json_array_add_double_element(radius, ellipse->radius[0]);
      json_array_add_double_element(radius, ellipse->radius[1]);
      json_object_set_array_member(object, "radius", radius);
      json_object_set_double_member(object, "rotation", ellipse->rotation);
      json_object_set_double_member(object, "border", ellipse->border);
      json_object_set_string_member(
        object, "border_mode",
        ellipse->flags == DT_MASKS_ELLIPSE_PROPORTIONAL
          ? "proportional"
          : "equidistant");
      break;
    }

    case DT_REMOTE_SHAPE_GRADIENT:
    {
      const dt_masks_point_gradient_t *gradient = form->points->data;
      if(!_raw_pair_ok(gradient->anchor)
         || !isfinite(gradient->rotation)
         || !isfinite(gradient->compression)
         || !isfinite(gradient->steepness)
         || !isfinite(gradient->curvature))
        goto invalid_raw;
      _add_point2(object, "anchor", gradient->anchor[0],
                  gradient->anchor[1]);
      json_object_set_double_member(object, "rotation", gradient->rotation);
      json_object_set_double_member(object, "compression",
                                    gradient->compression);
      json_object_set_double_member(object, "steepness", gradient->steepness);
      json_object_set_double_member(object, "curvature", gradient->curvature);
      break;
    }

    case DT_REMOTE_SHAPE_UNSUPPORTED:
      goto invalid_raw;
  }
  return _take_object(object);

invalid_raw:
  json_object_unref(object);
  return NULL;
}
