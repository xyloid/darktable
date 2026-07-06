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

#include "common/crypto_random.h"

#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <Security/SecRandom.h>
#else
// Linux (and other POSIX systems close enough to it -- darktable does not
// target other Unixes): getrandom(2) with a /dev/urandom fallback for
// kernels/sandboxes where the syscall is unavailable.
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#if defined(__has_include)
#if __has_include(<sys/random.h>)
#include <sys/random.h>
#define DT_HAVE_GETRANDOM 1
#endif
#endif
#endif

#if defined(_WIN32)

gboolean dt_crypto_random_bytes(void *buf, size_t len)
{
  if(len == 0) return TRUE;
  g_return_val_if_fail(buf != NULL, FALSE);

  // BCRYPT_USE_SYSTEM_PREFERRED_RNG: use the system CSPRNG without
  // requiring an explicit algorithm handle.
  const NTSTATUS status =
    BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  return status == 0 /* STATUS_SUCCESS */;
}

#elif defined(__APPLE__)

gboolean dt_crypto_random_bytes(void *buf, size_t len)
{
  if(len == 0) return TRUE;
  g_return_val_if_fail(buf != NULL, FALSE);

  return SecRandomCopyBytes(kSecRandomDefault, len, buf) == errSecSuccess;
}

#else

// Reads exactly `len` bytes from /dev/urandom, retrying on short reads and
// EINTR. Never blocks on entropy the way /dev/random can. Returns FALSE on
// any error (buffer contents unspecified in that case).
static gboolean _read_dev_urandom(void *buf, size_t len)
{
  int fd;
  do
  {
    fd = open("/dev/urandom", O_RDONLY);
  } while(fd < 0 && errno == EINTR);
  if(fd < 0) return FALSE;

  gboolean ok = TRUE;
  size_t got = 0;
  uint8_t *out = (uint8_t *)buf;
  while(got < len)
  {
    const ssize_t n = read(fd, out + got, len - got);
    if(n > 0) { got += (size_t)n; continue; }
    if(n < 0 && errno == EINTR) continue;
    ok = FALSE;  // 0 (EOF, shouldn't happen on a character device) or a hard error
    break;
  }

  close(fd);
  return ok && got == len;
}

gboolean dt_crypto_random_bytes(void *buf, size_t len)
{
  if(len == 0) return TRUE;
  g_return_val_if_fail(buf != NULL, FALSE);

#if defined(DT_HAVE_GETRANDOM)
  {
    size_t got = 0;
    uint8_t *out = (uint8_t *)buf;
    gboolean getrandom_usable = TRUE;
    while(got < len)
    {
      const ssize_t n = getrandom(out + got, len - got, 0);
      if(n > 0) { got += (size_t)n; continue; }
      if(n < 0 && errno == EINTR) continue;
      // ENOSYS (kernel too old) or any other failure: stop trying the
      // syscall and fall back to /dev/urandom below -- but only from a
      // clean slate (got == 0), so we do not mix two partial reads
      // together into a single buffer where either half could be a
      // predictable failure artifact.
      getrandom_usable = FALSE;
      break;
    }
    if(getrandom_usable && got == len) return TRUE;
    if(got != 0)
    {
      // Partial progress via getrandom() before it failed: do not hand
      // back a half-random, half-unspecified buffer as "success", and do
      // not silently splice in a second source either -- restart cleanly
      // via the /dev/urandom fallback below.
    }
  }
#endif

  return _read_dev_urandom(buf, len);
}

#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
