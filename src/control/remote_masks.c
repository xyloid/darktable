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
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
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
  if(constraint)
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
      if(!dt_remote_transform_preview_to_raw_size(
           dev, center_x, center_y, _member_num(geom, "radius"),
           &raw_radius, &exact))
        return _err_transform(error, "radius");
      if(!_in_storage_range(raw_radius, DTRM_SIZE_MIN, DTRM_SIZE_MAX))
        return _err_invalid(error, "radius", "out_of_range");
      if(!dt_remote_transform_preview_to_raw_size(
           dev, center_x, center_y, _member_num(geom, "border"),
           &raw_border, &exact))
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
      if(!dt_remote_transform_preview_to_raw_size(
           dev, center_x, center_y, preview_radius_a, &raw_radius_a, &exact)
         || !dt_remote_transform_preview_to_raw_size(
           dev, center_x, center_y, preview_radius_b, &raw_radius_b, &exact))
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

const char *dt_remote_masks_state_op_string(const int state)
{
  if(state & DT_MASKS_STATE_SUM) return "sum";
  if(state & DT_MASKS_STATE_INTERSECTION) return "intersection";
  if(state & DT_MASKS_STATE_DIFFERENCE) return "difference";
  if(state & DT_MASKS_STATE_EXCLUSION) return "exclusion";
  return "union";
}

gboolean dt_remote_masks_state_op_from_string(const char *s,
                                              int *op_bit_out)
{
  if(!s || !op_bit_out) return FALSE;

  int op_bit = 0;
  if(!strcmp(s, "union"))
    op_bit = DT_MASKS_STATE_UNION;
  else if(!strcmp(s, "intersection"))
    op_bit = DT_MASKS_STATE_INTERSECTION;
  else if(!strcmp(s, "difference"))
    op_bit = DT_MASKS_STATE_DIFFERENCE;
  else if(!strcmp(s, "exclusion"))
    op_bit = DT_MASKS_STATE_EXCLUSION;
  else
    return FALSE;

  *op_bit_out = op_bit;
  return TRUE;
}

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

    dt_masks_form_t *group =
      dt_masks_get_from_id(dev, module->blend_params->mask_id);
    if(!group || !(group->type & DT_MASKS_GROUP)) continue;

    for(GList *points = group->points;
        points;
        points = g_list_next(points))
    {
      const dt_masks_point_group_t *member = points->data;
      if(!member || member->formid != id) continue;

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
}

JsonNode *dt_remote_masks_list(dt_develop_t *dev)
{
  if(!dev) return NULL;

  JsonObject *root = json_object_new();
  JsonArray *shapes = json_array_new();
  for(GList *forms = dev->forms; forms; forms = g_list_next(forms))
  {
    dt_masks_form_t *form = forms->data;
    if(!form || (form->type & DT_MASKS_GROUP)) continue;

    JsonObject *shape = json_object_new();
    json_object_set_int_member(shape, "id", form->formid);
    json_object_set_string_member(
      shape, "type", dt_remote_masks_type_string(form->type));
    json_object_set_string_member(shape, "name", form->name);
    json_object_set_string_member(shape, "space", "preview");

    const gboolean editable =
      dt_remote_masks_kind_from_type(form->type)
      != DT_REMOTE_SHAPE_UNSUPPORTED;
    json_object_set_boolean_member(shape, "editable", editable);
    if(editable)
    {
      JsonNode *geometry =
        dt_remote_masks_points_to_geometry(dev, form);
      if(geometry)
        json_object_set_member(shape, "geometry", geometry);
      JsonNode *raw_geometry =
        dt_remote_masks_points_to_raw_geometry(form);
      if(raw_geometry)
        json_object_set_member(shape, "raw_geometry", raw_geometry);
    }

    JsonArray *used_by = json_array_new();
    _append_used_by(dev, form->formid, used_by);
    json_object_set_array_member(shape, "used_by", used_by);
    json_array_add_object_element(shapes, shape);
  }

  json_object_set_array_member(root, "shapes", shapes);
  return _take_object(root);
}

static gboolean _operation_error(dt_remote_error_t **error,
                                 const dt_remote_error_code_t code,
                                 const char *message,
                                 const char *parameter,
                                 const char *constraint)
{
  if(error)
  {
    *error = dt_remote_error_new(code, "%s", message);
    if(parameter)
      (*error)->details_json = _details_json(parameter, constraint);
  }
  return FALSE;
}

static gboolean _name_is_valid(const char *name,
                               dt_remote_error_t **error)
{
  if(!name || !*name) return TRUE;
  if(strlen(name) < sizeof(((dt_masks_form_t *)0)->name)) return TRUE;
  return _operation_error(error, DT_REMOTE_ERR_INVALID_VALUE,
                          "mask name must be fewer than 128 bytes",
                          "name", "length_lt_128");
}

static size_t _point_size(const dt_masks_type_t type)
{
  switch(dt_remote_masks_kind_from_type(type))
  {
    case DT_REMOTE_SHAPE_CIRCLE:
      return sizeof(dt_masks_point_circle_t);
    case DT_REMOTE_SHAPE_ELLIPSE:
      return sizeof(dt_masks_point_ellipse_t);
    case DT_REMOTE_SHAPE_GRADIENT:
      return sizeof(dt_masks_point_gradient_t);
    case DT_REMOTE_SHAPE_UNSUPPORTED:
      return 0;
  }
  return 0;
}

static gboolean _module_accepts_drawn_masks(const dt_iop_module_t *module)
{
  if(!module || !module->flags || !module->blend_params) return FALSE;
  const int flags = module->flags();
  return (flags & IOP_FLAGS_SUPPORTS_BLENDING)
         && !(flags & IOP_FLAGS_NO_MASKS);
}

void dt_remote_masks_cancel_gui_edit_if_targeting(dt_develop_t *dev,
                                                  dt_masks_form_t *form)
{
  if(!dev || !form || dev != darktable.develop) return;

  const dt_masks_form_t *visible = dev->form_visible;
  if(!visible) return;

  const gboolean hit =
    visible->formid == form->formid
    || (dev->form_gui && dev->form_gui->formid == form->formid);
  gboolean group_hit = FALSE;
  if(!hit && (visible->type & DT_MASKS_GROUP))
  {
    for(GList *points = visible->points;
        points;
        points = g_list_next(points))
    {
      const dt_masks_point_group_t *member = points->data;
      if(member && member->formid == form->formid)
      {
        group_hit = TRUE;
        break;
      }
    }
  }

  if(hit || group_hit) dt_masks_change_form_gui(NULL);
}

gboolean dt_remote_masks_create(dt_develop_t *dev,
                                const dt_masks_type_t type,
                                JsonObject *geom,
                                const char *name_or_null,
                                dt_iop_module_t *attach_module,
                                dt_mask_id_t *new_id_out,
                                dt_remote_error_t **error)
{
  if(!dev)
    return _operation_error(error, DT_REMOTE_ERR_INTERNAL,
                            "no develop context", NULL, NULL);

  const size_t point_size = _point_size(type);
  if(point_size == 0)
    return _operation_error(error, DT_REMOTE_ERR_UNSUPPORTED_FIELD,
                            "type is not creatable", "type", NULL);
  if(!_name_is_valid(name_or_null, error)) return FALSE;

  if(attach_module && !_module_accepts_drawn_masks(attach_module))
    return _operation_error(error, DT_REMOTE_ERR_UNSUPPORTED_FIELD,
                            "module does not support drawn masks",
                            "op", "masks_unsupported");
  if(attach_module
     && (attach_module->blend_params->mask_mode & DEVELOP_MASK_RASTER))
    return _operation_error(error, DT_REMOTE_ERR_INVALID_VALUE,
                            "module has a raster mask",
                            "op", "raster_unsupported");

  // Transform into detached storage first: no invalid request reaches
  // dt_masks_create(), much less dev->forms.
  void *point = malloc(point_size);
  if(!point)
    return _operation_error(error, DT_REMOTE_ERR_INTERNAL,
                            "failed to allocate mask point", NULL, NULL);
  if(!dt_remote_masks_geometry_to_points(dev, type, geom, point, error))
  {
    free(point);
    return FALSE;
  }

  dt_masks_form_t *form = dt_masks_create(type);
  if(!form)
  {
    free(point);
    return _operation_error(error, DT_REMOTE_ERR_INTERNAL,
                            "failed to allocate mask shape", NULL, NULL);
  }
  form->points = g_list_append(form->points, point);

  const dt_masks_form_creation_options_t options = {
    .requested_name = name_or_null,
    .preserve_module_enabled = TRUE,
    .mask_mode_to_add =
      attach_module ? DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK : 0,
  };
  dt_masks_gui_form_save_creation_ext(dev, attach_module, form, NULL,
                                      &options);
  if(new_id_out) *new_id_out = form->formid;
  return TRUE;
}

gboolean dt_remote_masks_update(dt_develop_t *dev,
                                const dt_mask_id_t id,
                                JsonObject *geom,
                                const char *name_or_null,
                                int *affects_out,
                                dt_remote_error_t **error)
{
  if(!dev)
    return _operation_error(error, DT_REMOTE_ERR_INTERNAL,
                            "no develop context", NULL, NULL);

  dt_masks_form_t *form = dt_masks_get_from_id(dev, id);
  if(!form)
    return _operation_error(error, DT_REMOTE_ERR_NOT_FOUND,
                            "no mask shape with that id", "id", NULL);

  const size_t point_size = _point_size(form->type);
  if(point_size == 0)
    return _operation_error(error, DT_REMOTE_ERR_UNSUPPORTED_FIELD,
                            "shape is not editable", "type", NULL);
  if(!_name_is_valid(name_or_null, error)) return FALSE;
  if(!form->points || form->points->next || !form->points->data)
    return _operation_error(error, DT_REMOTE_ERR_INTERNAL,
                            "editable mask shape has invalid point storage",
                            NULL, NULL);

  // Validate and transform into scratch storage before touching the live
  // shape. A rejected replacement leaves geometry and name unchanged.
  void *scratch = malloc(point_size);
  if(!scratch)
    return _operation_error(error, DT_REMOTE_ERR_INTERNAL,
                            "failed to allocate mask point", NULL, NULL);
  if(!dt_remote_masks_geometry_to_points(dev, form->type, geom,
                                         scratch, error))
  {
    free(scratch);
    return FALSE;
  }

  dt_remote_masks_cancel_gui_edit_if_targeting(dev, form);
  memcpy(form->points->data, scratch, point_size);
  free(scratch);
  if(name_or_null && *name_or_null)
    g_strlcpy(form->name, name_or_null, sizeof(form->name));

  int affects = 0;
  for(GList *iops = dev->iop; iops; iops = g_list_next(iops))
  {
    dt_iop_module_t *module = iops->data;
    if(!_module_accepts_drawn_masks(module)) continue;
    dt_masks_form_t *group =
      dt_masks_get_from_id(dev, module->blend_params->mask_id);
    if(!group || !(group->type & DT_MASKS_GROUP)) continue;
    for(GList *points = group->points;
        points;
        points = g_list_next(points))
    {
      const dt_masks_point_group_t *member = points->data;
      if(member && member->formid == id)
      {
        affects++;
        break;
      }
    }
  }

  if(affects_out) *affects_out = affects;
  dt_dev_add_masks_history_item(dev, NULL, TRUE);
  return TRUE;
}

static gboolean _group_has_other_members(const dt_masks_form_t *group,
                                         const dt_mask_id_t id)
{
  if(!group) return FALSE;
  for(GList *points = group->points;
      points;
      points = g_list_next(points))
  {
    const dt_masks_point_group_t *member = points->data;
    if(member && member->formid != id) return TRUE;
  }
  return FALSE;
}

gboolean dt_remote_masks_delete(dt_develop_t *dev,
                                const dt_mask_id_t id,
                                JsonArray *removed_from_out,
                                dt_remote_error_t **error)
{
  if(!dev)
    return _operation_error(error, DT_REMOTE_ERR_INTERNAL,
                            "no develop context", NULL, NULL);
  if(dev != darktable.develop)
    return _operation_error(error, DT_REMOTE_ERR_INTERNAL,
                            "mask deletion requires the live develop context",
                            NULL, NULL);

  dt_masks_form_t *form = dt_masks_get_from_id(dev, id);
  if(!form)
    return _operation_error(error, DT_REMOTE_ERR_NOT_FOUND,
                            "no mask shape with that id", "id", NULL);
  if(form->type & DT_MASKS_GROUP)
    return _operation_error(error, DT_REMOTE_ERR_UNSUPPORTED_FIELD,
                            "group forms are engine-managed",
                            "type", "group_internal");

  dt_remote_masks_cancel_gui_edit_if_targeting(dev, form);

  GPtrArray *references = g_ptr_array_new();
  for(GList *iops = dev->iop; iops; iops = g_list_next(iops))
  {
    dt_iop_module_t *module = iops->data;
    if(!_module_accepts_drawn_masks(module)) continue;
    dt_masks_form_t *group =
      dt_masks_get_from_id(dev, module->blend_params->mask_id);
    if(!group || !(group->type & DT_MASKS_GROUP)) continue;
    for(GList *points = group->points;
        points;
        points = g_list_next(points))
    {
      const dt_masks_point_group_t *member = points->data;
      if(member && member->formid == id)
      {
        g_ptr_array_add(references, module);
        // Clear before dt_masks_form_remove commits its internal history
        // snapshots. This preserves the final mask_mode across reload/undo.
        if(!_group_has_other_members(group, id))
          module->blend_params->mask_mode &= ~DEVELOP_MASK_MASK;
        break;
      }
    }
  }

  // NULL/NULL is the masks core's full-delete path. It removes the form
  // from every module and deletes groups that become empty.
  dt_masks_form_remove(NULL, NULL, form);

  for(guint i = 0; i < references->len; i++)
  {
    dt_iop_module_t *module = g_ptr_array_index(references, i);
    if(removed_from_out)
    {
      JsonObject *removed = json_object_new();
      json_object_set_string_member(removed, "op", module->op);
      json_object_set_int_member(removed, "instance",
                                 module->multi_priority);
      json_array_add_object_element(removed_from_out, removed);
    }

    // Defensive postcondition for malformed/duplicate group membership.
    if(!dt_masks_get_from_id(dev, module->blend_params->mask_id))
      module->blend_params->mask_mode &= ~DEVELOP_MASK_MASK;
  }

  g_ptr_array_unref(references);
  return TRUE;
}
