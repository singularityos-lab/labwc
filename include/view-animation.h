/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_VIEW_ANIMATION_H
#define LABWC_VIEW_ANIMATION_H

#include <stdbool.h>
#include <wlr/util/box.h>

struct view;
struct wlr_buffer;
struct view_animation;
struct view_card;

enum view_card_action {
	VIEW_CARD_SHOW = 0,
	VIEW_CARD_MINIMIZE,
};

struct view_animation *view_animation_create(struct view *view);
bool view_animation_is_running(struct view *view);
void view_animation_start_geometry(struct view_animation *animation,
	const struct wlr_box *target);
void view_animation_start_minimize(struct view_animation *animation,
	bool minimized);
void view_animation_start_open(struct view_animation *animation);
void view_animation_start_close(struct view *view);
void view_animation_cancel(struct view *view);
void view_close_gesture_set_progress(struct view *view, float progress);
struct wlr_buffer *view_render_grayscale_snapshot(struct view *view, float scale);

struct view_card *view_card_create(struct view *view,
	void (*done)(void *data), void *data);
void view_card_set_rect(struct view_card *card, struct wlr_box box, int radius);
void view_card_release(struct view_card *card, enum view_card_action action,
	struct wlr_box box, int radius, double velocity_x, double velocity_y);
void view_card_destroy(struct view_card *card);
void view_card_view_gone(struct view *view);

#endif /* LABWC_VIEW_ANIMATION_H */
