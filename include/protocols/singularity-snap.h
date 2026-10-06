/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_SINGULARITY_SNAP_H
#define LABWC_SINGULARITY_SNAP_H

#include <stdbool.h>
#include <wlr/util/box.h>

struct view;
struct ssd_button;

void singularity_snap_init(void);
bool singularity_snap_enabled(void);
void singularity_snap_button_hovered(struct ssd_button *button);
void singularity_snap_button_pressed(void);
void singularity_snap_move_update(struct view *view);
bool singularity_snap_move_finish(struct view *view);
void singularity_snap_move_cancel(void);
bool singularity_snap_cursor_in_picker(void);
bool singularity_snap_get_zone_preview(struct wlr_box *box);

#endif /* LABWC_SINGULARITY_SNAP_H */
