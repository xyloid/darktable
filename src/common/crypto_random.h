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

// Cryptographically-secure random bytes, for the darktable MCP remote-edit
// session token (see the remote-edit internals design doc §7). Nothing at
// darktable's GLib 2.56 floor is documented as CSPRNG-backed --
// g_uuid_string_random() explicitly is not -- so this wraps the platform
// CSPRNG directly: getrandom(2) (falling back to /dev/urandom) on Linux,
// BCryptGenRandom on Windows, SecRandomCopyBytes on macOS.
//
// Threading: safe to call from any thread; every backend is either a
// stateless syscall or documented thread-safe by its platform.

#pragma once

#include <glib.h>
#include <stddef.h>

G_BEGIN_DECLS

/** fills buf[0..len) with cryptographically-secure random bytes from the
 * platform CSPRNG. `len == 0` trivially succeeds without touching `buf`
 * (which may then be NULL). On failure returns FALSE and leaves the
 * contents of `buf` unspecified -- callers must not use a buffer after a
 * FALSE return (in particular, never fall back to whatever was already
 * there, e.g. a zeroed token). */
gboolean dt_crypto_random_bytes(void *buf, size_t len);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
