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

// Tier-1 blend surface for the darktable MCP remote-edit engine: a
// hand-written mini-introspection over dt_develop_blend_params_t (the
// struct is NOT introspected) plus pure JSON schema/read/patch helpers.
// Unlike remote_edit.h's neutral-type boundary, this layer deliberately
// speaks json-glib: the blend object's wire shape is bespoke (see the
// blend-settings design doc SS Wire contract), and both producers and the
// single consumer (remote_protocol.c) already live in JSON space.
//
// Threading: every function taking a dt_iop_module_t must run on the GTK
// main thread (same rule as remote_edit.h). The pure string/table helpers
// have no such constraint.

#pragma once

#include "control/remote_edit.h"
#include "develop/blend.h"

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

struct dt_iop_module_t;

/** stored mask_mode bitfield -> transition-appendix vocabulary:
 * "off", "uniform", "parametric", "drawn", "drawn+parametric", "raster"
 * (raster wins whenever the RASTER bit is set). Returns a static string. */
const char *dt_remote_blend_mask_mode_string(uint32_t mask_mode);

/** Tier-1 writable vocabulary only: "off" -> DEVELOP_MASK_DISABLED,
 * "uniform" -> DEVELOP_MASK_ENABLED. Anything else (including the
 * read-only compound strings) returns FALSE with *out untouched. */
gboolean dt_remote_blend_mask_mode_from_string(const char *s, uint32_t *out);

/** resolves a stored blend_cst to the effective blending colorspace:
 * a concrete stored space is returned verbatim; DEVELOP_BLEND_CS_NONE
 * (or garbage) resolves through
 * dt_develop_blend_default_module_blend_colorspace(module). */
dt_develop_blend_colorspace_t
dt_remote_blend_effective_colorspace(struct dt_iop_module_t *module, int32_t stored_cst);

/** the writable mode set for `csp`, as C enumerator names, in GUI order
 * (dt_develop_blend_mode_sections() expanded through
 * dt_develop_blend_mode_names[] tuple order -- the exact combobox
 * contents). Elements are static strings; caller frees only the array. */
GPtrArray *dt_remote_blend_mode_names_for_colorspace(dt_develop_blend_colorspace_t csp);

/** the per-instance blend schema object (design SS Schema), or NULL when
 * `module` is NULL or lacks IOP_FLAGS_SUPPORTS_BLENDING. Caller owns the
 * returned node (json_node_unref). */
JsonNode *dt_remote_blend_schema(struct dt_iop_module_t *module);

/** the current blend values object (design SS Read), or NULL when
 * `module` is NULL or lacks blending. Caller owns the node. */
JsonNode *dt_remote_blend_read(struct dt_iop_module_t *module);

/** validates `patch` (the request's "blend" member) and applies it to
 * `dst`, a caller-owned scratch copy of the live blend params. Never
 * touches the module. On failure returns FALSE with *error allocated
 * (DT_REMOTE_ERR_UNSUPPORTED_FIELD for unknown members / non-blending is
 * checked by the caller; DT_REMOTE_ERR_INVALID_VALUE otherwise, with
 * details_json {"parameter": "blend.<name>", "constraint": ...}) and
 * `dst` possibly partially written -- callers must treat `dst` as
 * poisoned on failure (the mutation path discards its scratch copy). */
gboolean dt_remote_blend_patch_apply(struct dt_iop_module_t *module,
                                     JsonObject *patch,
                                     dt_develop_blend_params_t *dst,
                                     dt_remote_error_t **error);

G_END_DECLS
