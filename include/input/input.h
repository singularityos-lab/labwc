/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_INPUT_H
#define LABWC_INPUT_H

#include <wayland-server-core.h>

struct wlr_pointer;

struct input {
	struct wlr_input_device *wlr_input_device;
	struct seat *seat;
	/* Set for pointer/touch devices */
	double scroll_factor;
	struct wl_listener destroy;
	struct wl_list link; /* seat.inputs */
	bool is_virtual;
	bool stands_for_hardware;
};

void input_handlers_init(struct seat *seat);
void input_handlers_finish(struct seat *seat);

bool input_virtual_blocked(void);
bool input_pointer_blocked(struct wlr_pointer *pointer);

#endif /* LABWC_INPUT_H */
