/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_OVERLAY_H
#define LABWC_OVERLAY_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>
#include "common/edge.h"

struct seat;

struct overlay {
	struct lab_scene_rect *rect;
	float opacity;
	struct wl_listener rect_destroy;
	struct lab_scene_rect *outgoing_rect;
	float outgoing_opacity;
	struct wl_listener outgoing_rect_destroy;

	/* Represents currently shown or delayed overlay */
	struct {
		/* Region overlay */
		struct region *region;

		/* Snap-to-edge overlay */
		enum lab_edge edge;
		struct output *output;

		bool float_drop;

		bool tiling_drop;
		struct wlr_box tiling_drop_box;
	} active;

	/* For delayed snap-to-edge overlay */
	struct wl_event_source *timer;
	struct wl_event_source *animation_timer;
};

/*
 * Shows or updates an overlay when the grabbed window can be snapped to
 * a region or an output edge. Calls overlay_finish() otherwise.
 */
void overlay_update(struct seat *seat);

/* Destroys the overlay if it exists */
void overlay_finish(struct seat *seat);

#endif
