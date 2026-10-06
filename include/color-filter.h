/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_COLOR_FILTER_H
#define LABWC_COLOR_FILTER_H

#include <stdbool.h>

struct wlr_box;
struct wlr_buffer;

/* Row-major 3x3 matrix with a trailing offset per row */
extern const float color_filter_invert[12];
extern const float color_filter_invert_lightness[12];

bool color_filter_active(void);
void color_filter_reconfigure(void);
void color_filter_apply(struct wlr_buffer *buffer, const struct wlr_box *box,
	const float matrix[static 12]);
void color_filter_draw(struct wlr_buffer *buffer);
void color_filter_update_direct_scanout(void);

#endif /* LABWC_COLOR_FILTER_H */
