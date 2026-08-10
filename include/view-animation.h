/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_VIEW_ANIMATION_H
#define LABWC_VIEW_ANIMATION_H

#include <stdbool.h>
#include <wlr/util/box.h>

struct view;
struct view_animation;

struct view_animation *view_animation_create(struct view *view);
bool view_animation_is_running(struct view *view);
void view_animation_start_geometry(struct view_animation *animation,
	const struct wlr_box *target);
void view_animation_start_minimize(struct view_animation *animation,
	bool minimized);
void view_animation_start_open(struct view_animation *animation);
void view_animation_cancel(struct view *view);

#endif /* LABWC_VIEW_ANIMATION_H */
