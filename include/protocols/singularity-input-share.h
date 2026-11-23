/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_SINGULARITY_INPUT_SHARE_H
#define LABWC_SINGULARITY_INPUT_SHARE_H

#include <stdbool.h>
#include <stdint.h>

struct seat;
struct keyboard;
struct wlr_keyboard_key_event;
struct view;

void singularity_input_share_init(void);
bool singularity_input_share_active(void);
bool singularity_input_share_cross(struct seat *seat, double dx, double dy,
	uint32_t msec);
void singularity_input_share_motion(double dx, double dy);
void singularity_input_share_button(uint32_t button, bool pressed);
void singularity_input_share_axis(uint32_t axis, double value, int32_t discrete);
bool singularity_input_share_key(struct keyboard *keyboard,
	struct wlr_keyboard_key_event *event);
void singularity_input_share_cancel(void);
bool singularity_input_share_drop(struct view *view);
void singularity_input_share_drag_cancel(struct view *view);

#endif /* LABWC_SINGULARITY_INPUT_SHARE_H */
