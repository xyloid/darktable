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

#include "control/remote_parameters.h"

/** frees a single condition (its two owned strings and the struct
 * itself). NULL-safe. Internal: dt_remote_curve_schema_t and
 * dt_remote_vector_schema_t are the only owners of
 * dt_remote_parameter_condition_t instances in v1. */
static void dt_remote_parameter_condition_free(dt_remote_parameter_condition_t *condition)
{
  if(!condition) return;
  g_free(condition->field);
  g_free(condition->enum_name);
  g_free(condition);
}

void dt_remote_curve_schema_free(dt_remote_curve_schema_t *schema)
{
  if(!schema) return;
  g_free(schema->name);
  g_free(schema->display_name);
  g_free(schema->description);
  g_free(schema->x.unit);
  g_free(schema->y.unit);
  dt_remote_parameter_condition_free(schema->active_when);
  dt_remote_parameter_condition_free(schema->writable_when);
  dt_remote_parameter_condition_free(schema->periodic_when);
  g_free(schema);
}

void dt_remote_curve_value_free(dt_remote_curve_value_t *value)
{
  if(!value) return;
  g_free(value->name);
  if(value->points) g_array_unref(value->points);
  g_free(value);
}

void dt_remote_vector_schema_free(dt_remote_vector_schema_t *schema)
{
  if(!schema) return;
  g_free(schema->name);
  g_free(schema->display_name);
  g_free(schema->description);
  g_free(schema->color_space);
  if(schema->components)
  {
    for(guint i = 0; i < schema->components->len; i++)
      g_free(g_array_index(schema->components, dt_remote_vector_component_schema_t, i).name);
    g_array_unref(schema->components);
  }
  dt_remote_parameter_condition_free(schema->active_when);
  dt_remote_parameter_condition_free(schema->writable_when);
  g_free(schema);
}

void dt_remote_vector_value_free(dt_remote_vector_value_t *value)
{
  if(!value) return;
  g_free(value->name);
  if(value->values) g_array_unref(value->values);
  g_free(value);
}

void dt_remote_band_schema_free(dt_remote_band_schema_t *schema)
{
  if(!schema) return;
  g_free(schema->name);
  g_free(schema->display_name);
  g_free(schema->description);
  g_free(schema->x_shared_with);
  if(schema->x) g_array_unref(schema->x);
  dt_remote_parameter_condition_free(schema->active_when);
  dt_remote_parameter_condition_free(schema->writable_when);
  g_free(schema);
}

void dt_remote_band_value_free(dt_remote_band_value_t *value)
{
  if(!value) return;
  g_free(value->name);
  if(value->y) g_array_unref(value->y);
  if(value->x) g_array_unref(value->x);
  g_free(value);
}

void dt_remote_semantic_patch_free(gpointer patch_ptr)
{
  dt_remote_semantic_patch_t *patch = patch_ptr;
  if(!patch) return;

  switch(patch->class_id)
  {
    case DT_REMOTE_PARAMETER_CURVE:
      g_free(patch->value.curve.name);
      if(patch->value.curve.points) g_array_unref(patch->value.curve.points);
      break;

    case DT_REMOTE_PARAMETER_VECTOR:
      g_free(patch->value.vector.name);
      if(patch->value.vector.values) g_array_unref(patch->value.vector.values);
      break;

    case DT_REMOTE_PARAMETER_BANDS:
      g_free(patch->value.bands.name);
      if(patch->value.bands.y) g_array_unref(patch->value.bands.y);
      if(patch->value.bands.x) g_array_unref(patch->value.bands.x);
      break;

    default:
      break;
  }

  g_free(patch);
}

dt_remote_semantic_schema_t *dt_remote_semantic_schema_wrap_curve(dt_remote_curve_schema_t *s)
{
  dt_remote_semantic_schema_t *wrapper = g_malloc0(sizeof(dt_remote_semantic_schema_t));
  wrapper->class_id = DT_REMOTE_PARAMETER_CURVE;
  wrapper->u.curve = s;
  return wrapper;
}

dt_remote_semantic_value_t *dt_remote_semantic_value_wrap_curve(dt_remote_curve_value_t *v)
{
  dt_remote_semantic_value_t *wrapper = g_malloc0(sizeof(dt_remote_semantic_value_t));
  wrapper->class_id = DT_REMOTE_PARAMETER_CURVE;
  wrapper->u.curve = v;
  return wrapper;
}

dt_remote_semantic_schema_t *dt_remote_semantic_schema_wrap_vector(dt_remote_vector_schema_t *s)
{
  dt_remote_semantic_schema_t *wrapper = g_malloc0(sizeof(dt_remote_semantic_schema_t));
  wrapper->class_id = DT_REMOTE_PARAMETER_VECTOR;
  wrapper->u.vector = s;
  return wrapper;
}

dt_remote_semantic_value_t *dt_remote_semantic_value_wrap_vector(dt_remote_vector_value_t *v)
{
  dt_remote_semantic_value_t *wrapper = g_malloc0(sizeof(dt_remote_semantic_value_t));
  wrapper->class_id = DT_REMOTE_PARAMETER_VECTOR;
  wrapper->u.vector = v;
  return wrapper;
}

dt_remote_semantic_schema_t *dt_remote_semantic_schema_wrap_band(dt_remote_band_schema_t *s)
{
  dt_remote_semantic_schema_t *wrapper = g_malloc0(sizeof(dt_remote_semantic_schema_t));
  wrapper->class_id = DT_REMOTE_PARAMETER_BANDS;
  wrapper->u.bands = s;
  return wrapper;
}

dt_remote_semantic_value_t *dt_remote_semantic_value_wrap_band(dt_remote_band_value_t *v)
{
  dt_remote_semantic_value_t *wrapper = g_malloc0(sizeof(dt_remote_semantic_value_t));
  wrapper->class_id = DT_REMOTE_PARAMETER_BANDS;
  wrapper->u.bands = v;
  return wrapper;
}

void dt_remote_semantic_schema_free(gpointer schema_ptr)
{
  dt_remote_semantic_schema_t *schema = schema_ptr;
  if(!schema) return;

  switch(schema->class_id)
  {
    case DT_REMOTE_PARAMETER_CURVE:
      dt_remote_curve_schema_free(schema->u.curve);
      break;

    case DT_REMOTE_PARAMETER_VECTOR:
      dt_remote_vector_schema_free(schema->u.vector);
      break;

    case DT_REMOTE_PARAMETER_BANDS:
      dt_remote_band_schema_free(schema->u.bands);
      break;

    default:
      break;
  }

  g_free(schema);
}

void dt_remote_semantic_value_free(gpointer value_ptr)
{
  dt_remote_semantic_value_t *value = value_ptr;
  if(!value) return;

  switch(value->class_id)
  {
    case DT_REMOTE_PARAMETER_CURVE:
      dt_remote_curve_value_free(value->u.curve);
      break;

    case DT_REMOTE_PARAMETER_VECTOR:
      dt_remote_vector_value_free(value->u.vector);
      break;

    case DT_REMOTE_PARAMETER_BANDS:
      dt_remote_band_value_free(value->u.bands);
      break;

    default:
      break;
  }

  g_free(value);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
