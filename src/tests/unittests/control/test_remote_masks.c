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
 * cmocka unit tests for the Tier-3 drawn-mask surface (M-C).
 *  - Task 1: linkage of the exported dt_masks_group_create_for_module.
 *  - Tasks 3-6 append geometry/validation/CRUD/attach/guard sections.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "develop/masks.h"

#include <glib.h>

// Task 1: the symbol must be linkable (public, non-static). We only assert
// the declaration compiles and the pointer is non-NULL; behavioral tests
// that need a live dev/module arrive in Task 3 with the dt_init harness.
static void test_group_create_symbol_is_public(void **state)
{
  (void)state;
  void (*p)(void) = (void (*)(void))dt_masks_group_create_for_module;
  assert_non_null(p);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_group_create_symbol_is_public),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
