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

// Tier-1 blend base plus Tier-2 extensions for the darktable MCP
// remote-edit engine: a hand-written mini-introspection over
// dt_develop_blend_params_t (the struct is NOT introspected) plus pure JSON
// schema/read/patch helpers.
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

/** Parse any of the five non-raster transition targets to its bitfield:
 * "off" -> DEVELOP_MASK_DISABLED, "uniform" -> DEVELOP_MASK_ENABLED,
 * "parametric" -> ENABLED|CONDITIONAL, "drawn" -> ENABLED|MASK,
 * "drawn+parametric" -> ENABLED|MASK_CONDITIONAL. "raster", unknown
 * strings, NULL input, or NULL output return FALSE without writing *out.
 * This is a pure lexical parser; the state-preserving transition rules
 * (MASK ownership, raster rejection) live in
 * dt_remote_blend_mask_mode_transition. */
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

/* ---- Tier 2 / M-B: parametric (blendif) masks ---- */

/** one wire channel of a blend colorspace family: a stable ASCII name
 * (never a translated GUI label) plus its `_in`/`_out` blendif slots and
 * boost metadata. `boost_offset` is the STORAGE offset of the boost
 * value's GUI zero (-6.64385619 for Jz/Cz, 0 else); `marker_offset` is
 * the separate Lab a/b centering (0.5) used only in the non-normative
 * display hint. See the parametric-masks design SS Boost. */
typedef struct dt_remote_blendif_channel_t
{
  const char *name;
  dt_develop_blendif_channels_t slot_in;
  dt_develop_blendif_channels_t slot_out;
  gboolean boost_supported;
  float boost_offset;
  float marker_offset;
  float display_factor;   // non-normative display hint
  const char *display_unit;
} dt_remote_blendif_channel_t;

/** the NULL-terminated channel table for `csp`, or NULL when the family
 * has no parametric support (RAW / NONE). The order matches the GUI
 * tables entry-for-entry (bound by a parity test). */
const dt_remote_blendif_channel_t *
dt_remote_blendif_channels(dt_develop_blend_colorspace_t csp);

/** bitfield <-> per-slot views. `slot` is a usable storage slot 0..14;
 * slot 15 is DEVELOP_BLENDIF_unused, whose polarity bit aliases the legacy
 * DEVELOP_BLENDIF_active bit (31), and is rejected. `slot_pack` sets/clears
 * the enable bit (slot) and polarity bit (16+slot) and always strips bit 31. */
gboolean dt_remote_blendif_slot_enabled(uint32_t blendif, int slot);
gboolean dt_remote_blendif_slot_inverted(uint32_t blendif, int slot);
uint32_t dt_remote_blendif_slot_pack(uint32_t blendif, int slot,
                                     gboolean enabled, gboolean inverted);

/** the derived-enable rule (single source of truth): a slot is DISABLED
 * iff its markers are the full-span identity (m[1]==0 && m[2]==1). */
gboolean dt_remote_blendif_markers_enable(const float m[4]);

/** Apply the authoritative mask-mode transition table. Only ENABLED and
 * CONDITIONAL may change; MASK and every unknown bit are preserved.
 * On FALSE, `*constraint` is one of the static strings unknown_value,
 * drawn_via_attach_only, or raster_unsupported. */
gboolean dt_remote_blend_mask_mode_transition(uint32_t stored,
                                              const char *target,
                                              uint32_t *out,
                                              const char **constraint);

G_END_DECLS
