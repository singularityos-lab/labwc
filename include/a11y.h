/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_A11Y_H
#define LABWC_A11Y_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-protocol.h>

struct wlr_surface;

void a11y_init(void);
void a11y_finish(void);
void a11y_reconfigure(void);

void a11y_locate_pointer(void);
void a11y_flash(struct wlr_surface *surface);

/* Returns true when the button event is consumed */
bool a11y_pointer_button(uint32_t button, enum wl_pointer_button_state state,
	uint32_t time_msec);
void a11y_pointer_motion(double dx, double dy);

#endif /* LABWC_A11Y_H */
