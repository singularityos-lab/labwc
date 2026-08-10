// SPDX-License-Identifier: GPL-2.0-only
#include <setjmp.h>
#include <stddef.h>
#include <stdlib.h>
#include <cmocka.h>
#include "config/gesturebind.h"
#include "config/rcxml.h"

struct rcxml rc;

static void
free_bindings(void)
{
	struct gesturebind *bind, *tmp;
	wl_list_for_each_safe(bind, tmp, &rc.gesturebinds, link) {
		wl_list_remove(&bind->link);
		free(bind);
	}
}

static void
test_gesture_direction(void **state)
{
	assert_int_equal(gesture_direction_from_delta(-100, 20),
		LAB_DIRECTION_LEFT);
	assert_int_equal(gesture_direction_from_delta(100, -20),
		LAB_DIRECTION_RIGHT);
	assert_int_equal(gesture_direction_from_delta(20, -100),
		LAB_DIRECTION_UP);
	assert_int_equal(gesture_direction_from_delta(-20, 100),
		LAB_DIRECTION_DOWN);
}

static void
test_gesture_matching(void **state)
{
	wl_list_init(&rc.gesturebinds);
	struct gesturebind *up = gesturebind_create(LAB_GESTURE_SWIPE, 3,
		LAB_DIRECTION_UP, 80);
	struct gesturebind *left = gesturebind_create(LAB_GESTURE_SWIPE, 4,
		LAB_DIRECTION_LEFT, 40);

	assert_true(gesturebind_claims(LAB_GESTURE_SWIPE, 3));
	assert_true(gesturebind_claims(LAB_GESTURE_SWIPE, 4));
	assert_false(gesturebind_claims(LAB_GESTURE_SWIPE, 2));
	assert_true(gesturebind_claims_direction(LAB_GESTURE_SWIPE, 3,
		LAB_DIRECTION_UP));
	assert_false(gesturebind_claims_direction(LAB_GESTURE_SWIPE, 3,
		LAB_DIRECTION_DOWN));
	assert_ptr_equal(gesturebind_match(LAB_GESTURE_SWIPE, 3, 10, -100), up);
	assert_null(gesturebind_match(LAB_GESTURE_SWIPE, 3, 10, -79));
	assert_ptr_equal(gesturebind_match_direction(LAB_GESTURE_SWIPE, 3,
		LAB_DIRECTION_UP, 200, -100), up);
	assert_null(gesturebind_match_direction(LAB_GESTURE_SWIPE, 3,
		LAB_DIRECTION_UP, 0, -79));
	assert_null(gesturebind_match_direction(LAB_GESTURE_SWIPE, 3,
		LAB_DIRECTION_UP, 0, 100));
	assert_ptr_equal(gesturebind_match(LAB_GESTURE_SWIPE, 4, -50, 10), left);
	assert_null(gesturebind_match(LAB_GESTURE_SWIPE, 4, 50, 10));

	free_bindings();
}

int
main(int argc, char **argv)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_gesture_direction),
		cmocka_unit_test(test_gesture_matching),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
