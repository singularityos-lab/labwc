// SPDX-License-Identifier: GPL-2.0-only
#include <setjmp.h>
#include <stddef.h>
#include <stdlib.h>
#include <cmocka.h>
#include "config/tablet-tool.h"

static void
test_parse(void **state)
{
	double curve[4] = {0};
	assert_true(tablet_parse_pressure_curve("0.25,0.1,0.75,0.9", curve));
	assert_float_equal(curve[0], 0.25, 1e-9);
	assert_float_equal(curve[3], 0.9, 1e-9);
	assert_true(tablet_parse_pressure_curve(" 0 , 0 , 1 , 1 ", curve));
	assert_false(tablet_parse_pressure_curve("0,0,1", curve));
	assert_false(tablet_parse_pressure_curve("0,0,1,1.5", curve));
	assert_false(tablet_parse_pressure_curve("0,0,1,1,x", curve));
	assert_false(tablet_parse_pressure_curve(NULL, curve));
}

static void
test_apply(void **state)
{
	const double linear[4] = {0.0, 0.0, 1.0, 1.0};
	const double soft[4] = {0.0, 0.4, 0.4, 1.0};
	const double firm[4] = {0.4, 0.0, 1.0, 0.6};
	for (int i = 0; i <= 10; i++) {
		double p = i / 10.0;
		assert_float_equal(tablet_pressure_curve_apply(linear, p), p, 1e-6);
		assert_true(tablet_pressure_curve_apply(soft, p) >= p - 1e-9);
		assert_true(tablet_pressure_curve_apply(firm, p) <= p + 1e-9);
	}
	assert_float_equal(tablet_pressure_curve_apply(soft, 0.0), 0.0, 1e-9);
	assert_float_equal(tablet_pressure_curve_apply(soft, 1.0), 1.0, 1e-9);
	assert_float_equal(tablet_pressure_curve_apply(soft, -0.5), 0.0, 1e-9);
	assert_float_equal(tablet_pressure_curve_apply(soft, 2.0), 1.0, 1e-9);
	double last = 0.0;
	for (int i = 0; i <= 100; i++) {
		double v = tablet_pressure_curve_apply(firm, i / 100.0);
		assert_true(v >= last - 1e-9);
		last = v;
	}
}

int
main(int argc, char **argv)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_parse),
		cmocka_unit_test(test_apply),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
