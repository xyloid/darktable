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

// Tier-3 drawn-mask engine surface (mask_shapes capability). Geometry
// (de)serialization treats all wire input as adversarial (first remote
// surface feeding variable structures into pipeline-adjacent code): every
// value is finiteness- and range-checked before any dt_masks_create call,
// and never reaches dev->forms unvalidated. Coordinate mapping is delegated
// to remote_transform.h. Every function taking a dt_develop_t/dt_iop_module_t
// runs on the GTK main thread.
//
// CAVEAT: remote creation and full deletion use the dev-parameterized
// extended core APIs; this surface does not call legacy
// dt_masks_form_remove(). Some underlying mask-core operations still consult
// the live darkroom singleton (notably creation ID deconfliction and image-size
// lookup), GUI edit cancellation only acts on that live context, and deletion
// explicitly rejects any other dev. Standalone tests must therefore repoint
// darktable.develop at their fixture.

#pragma once

#include "control/remote_edit.h"
#include "control/remote_transform.h"
#include "develop/masks.h"

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

struct dt_develop_t;
struct dt_iop_module_t;

typedef enum dt_remote_shape_kind_t
{
  DT_REMOTE_SHAPE_CIRCLE,
  DT_REMOTE_SHAPE_ELLIPSE,
  DT_REMOTE_SHAPE_GRADIENT,
  DT_REMOTE_SHAPE_UNSUPPORTED,
} dt_remote_shape_kind_t;

dt_remote_shape_kind_t dt_remote_masks_kind_from_type(dt_masks_type_t t);
const char *dt_remote_masks_type_string(dt_masks_type_t t);
gboolean dt_remote_masks_type_from_string(const char *s, dt_masks_type_t *out);

/** pure wire-space policy validation (Global Constraints "Validation
 * policy"): required members present, finite, in wire range; unknown members
 * rejected. No transform. FALSE + *error (DT_REMOTE_ERR_INVALID_VALUE, or
 * DT_REMOTE_ERR_UNSUPPORTED_FIELD for a non-editable type) on failure. */
gboolean dt_remote_masks_geometry_validate(dt_masks_type_t type, JsonObject *geom,
                                           dt_remote_error_t **error);

/** validate, back-transform (preview->raw), and fill `point_out` (a caller
 * struct of the type's size). Enforces raw storage clamps after transform.
 * Requires a fresh pipe — caller must have run ensure_fresh first. */
gboolean dt_remote_masks_geometry_to_points(struct dt_develop_t *dev,
                                            dt_masks_type_t type,
                                            JsonObject *geom,
                                            void *point_out,
                                            dt_remote_error_t **error);

/** stored raw points -> preview-normalized geometry object (with
 * "size_mapping"); NULL for unsupported/non-editable types or transform
 * failure. Caller owns. */
JsonNode *dt_remote_masks_points_to_geometry(struct dt_develop_t *dev,
                                             dt_masks_form_t *form);

/** stored raw points verbatim as a "raw_geometry" object; NULL for
 * unsupported types. Caller owns. */
JsonNode *dt_remote_masks_points_to_raw_geometry(dt_masks_form_t *form);

/** Stored member combine state -> wire operation. A member with no combine
 * bit reads as "union"; the existing brush-only SUM bit reads as "sum". */
const char *dt_remote_masks_state_op_string(int state);

/** Writable wire operation -> stored combine bit. "sum" is output-only.
 * FALSE leaves *op_bit_out unchanged. */
gboolean dt_remote_masks_state_op_from_string(const char *s,
                                              int *op_bit_out);

/** Serialize all non-group forms and their module memberships. Requires a
 * fresh pipe — the caller owns the ensure_fresh contract. Caller owns. */
JsonNode *dt_remote_masks_list(struct dt_develop_t *dev);

/** Cancel a live GUI edit of `form`, including an edit of a group that
 * contains it transitively. No-op unless `dev` is the live darkroom
 * develop context. Must run on the GTK main thread. */
void dt_remote_masks_cancel_gui_edit_if_targeting(struct dt_develop_t *dev,
                                                  dt_masks_form_t *form);

/** Create one editable fixed-size mask form, optionally attaching it to a
 * blending module. Geometry is fully validated and transformed before the
 * form enters dev->forms. The caller owns the pipe-freshness check. */
gboolean dt_remote_masks_create(struct dt_develop_t *dev,
                                dt_masks_type_t type,
                                JsonObject *geom,
                                const char *name_or_null,
                                struct dt_iop_module_t *attach_module_or_null,
                                dt_mask_id_t *new_id_out,
                                dt_remote_error_t **error);

/** Replace one editable form's geometry and optional name atomically.
 * `affects_out` receives the number of transitively referencing module
 * instances. The caller owns the pipe-freshness check. */
gboolean dt_remote_masks_update(struct dt_develop_t *dev,
                                dt_mask_id_t id,
                                JsonObject *geom,
                                const char *name_or_null,
                                int *affects_out,
                                dt_remote_error_t **error);

/** Permanently remove one form and all transitive module memberships.
 * `removed_from_out`, when non-NULL, receives {op,instance} objects. */
gboolean dt_remote_masks_delete(struct dt_develop_t *dev,
                                dt_mask_id_t id,
                                JsonArray *removed_from_out,
                                dt_remote_error_t **error);

/** Attach or detach one shape directly to/from a module's base group.
 * `state_or_null` defaults to union, `inverted` is -1 to leave unchanged,
 * and `opacity_or_null` leaves opacity unchanged. */
gboolean dt_remote_masks_set_attachment(struct dt_develop_t *dev,
                                        struct dt_iop_module_t *module,
                                        dt_mask_id_t shape_id,
                                        gboolean attached,
                                        const char *state_or_null,
                                        int inverted,
                                        const double *opacity_or_null,
                                        dt_remote_error_t **error);

G_END_DECLS
