/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_GESTUREBIND_H
#define LABWC_GESTUREBIND_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-util.h>
#include "config/mousebind.h"

enum gesture_type {
	LAB_GESTURE_INVALID = 0,
	LAB_GESTURE_SWIPE,
};

struct gesturebind {
	enum gesture_type type;
	uint32_t fingers;
	enum direction direction;
	double threshold;
	struct wl_list actions; /* struct action.link */
	struct wl_list link;    /* struct rcxml.gesturebinds */
};

struct gesturebind *gesturebind_create(enum gesture_type type,
	uint32_t fingers, enum direction direction, double threshold);
bool gesturebind_the_same(struct gesturebind *a, struct gesturebind *b);
enum direction gesture_direction_from_delta(double dx, double dy);
bool gesturebind_claims(enum gesture_type type, uint32_t fingers);
bool gesturebind_claims_direction(enum gesture_type type, uint32_t fingers,
	enum direction direction);
struct gesturebind *gesturebind_match_direction(enum gesture_type type,
	uint32_t fingers, enum direction direction, double dx, double dy);
struct gesturebind *gesturebind_match(enum gesture_type type,
	uint32_t fingers, double dx, double dy);

#endif /* LABWC_GESTUREBIND_H */
