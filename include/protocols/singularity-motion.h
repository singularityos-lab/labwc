/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_SINGULARITY_MOTION_H
#define LABWC_SINGULARITY_MOTION_H

#include <stdbool.h>
#include <wlr/util/box.h>

struct output;

void singularity_motion_init(void);
bool singularity_motion_icon_box(const char *app_id, struct output *output,
	struct wlr_box *box);
bool singularity_motion_take_launch(const char *app_id);

#endif /* LABWC_SINGULARITY_MOTION_H */
