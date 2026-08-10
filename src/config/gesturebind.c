// SPDX-License-Identifier: GPL-2.0-only
#include "config/gesturebind.h"
#include <assert.h>
#include <math.h>
#include "common/list.h"
#include "common/mem.h"
#include "config/rcxml.h"

struct gesturebind *
gesturebind_create(enum gesture_type type, uint32_t fingers,
	enum direction direction, double threshold)
{
	if (type == LAB_GESTURE_INVALID || fingers == 0
			|| direction == LAB_DIRECTION_INVALID || threshold <= 0) {
		return NULL;
	}

	struct gesturebind *bind = znew(*bind);
	bind->type = type;
	bind->fingers = fingers;
	bind->direction = direction;
	bind->threshold = threshold;
	wl_list_init(&bind->actions);
	wl_list_append(&rc.gesturebinds, &bind->link);
	return bind;
}

bool
gesturebind_the_same(struct gesturebind *a, struct gesturebind *b)
{
	assert(a && b);
	return a->type == b->type
		&& a->fingers == b->fingers
		&& a->direction == b->direction;
}

enum direction
gesture_direction_from_delta(double dx, double dy)
{
	if (fabs(dx) > fabs(dy)) {
		return dx < 0 ? LAB_DIRECTION_LEFT : LAB_DIRECTION_RIGHT;
	}
	return dy < 0 ? LAB_DIRECTION_UP : LAB_DIRECTION_DOWN;
}

bool
gesturebind_claims(enum gesture_type type, uint32_t fingers)
{
	struct gesturebind *bind;
	wl_list_for_each(bind, &rc.gesturebinds, link) {
		if (bind->type == type && bind->fingers == fingers) {
			return true;
		}
	}
	return false;
}

bool
gesturebind_claims_direction(enum gesture_type type, uint32_t fingers,
	enum direction direction)
{
	struct gesturebind *bind;
	wl_list_for_each(bind, &rc.gesturebinds, link) {
		if (bind->type == type && bind->fingers == fingers
				&& bind->direction == direction) {
			return true;
		}
	}
	return false;
}

struct gesturebind *
gesturebind_match_direction(enum gesture_type type, uint32_t fingers,
	enum direction direction, double dx, double dy)
{
	struct gesturebind *bind;
	wl_list_for_each(bind, &rc.gesturebinds, link) {
		double distance = 0;
		switch (direction) {
		case LAB_DIRECTION_LEFT:
			distance = -dx;
			break;
		case LAB_DIRECTION_RIGHT:
			distance = dx;
			break;
		case LAB_DIRECTION_UP:
			distance = -dy;
			break;
		case LAB_DIRECTION_DOWN:
			distance = dy;
			break;
		default:
			break;
		}
		if (bind->type == type && bind->fingers == fingers
				&& bind->direction == direction
				&& distance >= bind->threshold) {
			return bind;
		}
	}
	return NULL;
}

struct gesturebind *
gesturebind_match(enum gesture_type type, uint32_t fingers,
	double dx, double dy)
{
	return gesturebind_match_direction(type, fingers,
		gesture_direction_from_delta(dx, dy), dx, dy);
}
