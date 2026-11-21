// SPDX-License-Identifier: GPL-2.0-only
#include <setjmp.h>
#include <stddef.h>
#include <stdarg.h>
#include <cmocka.h>
#include "common/panel-scale.h"

static void
test_unknown_size(void **state)
{
	assert_true(panel_default_scale(0, 0, 1920, 1080) == 1.0);
	assert_true(panel_default_scale(160, 90, 0, 0) == 1.0);
	assert_true(panel_default_scale(10, 10, 1920, 1080) == 1.0);
}

static void
test_phone(void **state)
{
	assert_true(panel_default_scale(71, 154, 1440, 3120) == 3.0);
	assert_true(panel_default_scale(68, 151, 1080, 2400) == 2.5);
}

static void
test_desktop(void **state)
{
	assert_true(panel_default_scale(344, 194, 1920, 1080) == 1.0);
	assert_true(panel_default_scale(301, 188, 1920, 1200) == 1.0);
	assert_true(panel_default_scale(597, 336, 3840, 2160) == 1.0);
	assert_true(panel_default_scale(302, 189, 2880, 1800) == 2.0);
	assert_true(panel_default_scale(1600, 900, 3840, 2160) == 1.0);
}

int
main(int argc, char **argv)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_unknown_size),
		cmocka_unit_test(test_phone),
		cmocka_unit_test(test_desktop),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
