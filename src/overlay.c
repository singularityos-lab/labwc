// SPDX-License-Identifier: GPL-2.0-only
#include "overlay.h"
#include <assert.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include "common/lab-scene-rect.h"
#include "config/rcxml.h"
#include "group.h"
#include "labwc.h"
#include "output.h"
#include "protocols/singularity-tiling.h"
#include "regions.h"
#include "theme.h"
#include "view.h"

#define OVERLAY_ANIMATION_INTERVAL_MS 16
#define OVERLAY_ANIMATION_STEP 0.1f

static void
handle_rect_destroy(struct wl_listener *listener, void *data)
{
	struct overlay *overlay = wl_container_of(listener, overlay, rect_destroy);
	wl_list_remove(&listener->link);
	listener->notify = NULL;
	overlay->rect = NULL;
	overlay->opacity = 0.0f;
}

static void
handle_outgoing_rect_destroy(struct wl_listener *listener, void *data)
{
	struct overlay *overlay =
		wl_container_of(listener, overlay, outgoing_rect_destroy);
	wl_list_remove(&listener->link);
	listener->notify = NULL;
	overlay->outgoing_rect = NULL;
	overlay->outgoing_opacity = 0.0f;
}

static void
finish_animation(struct overlay *overlay)
{
	if (overlay->rect) {
		overlay->opacity = 1.0f;
		lab_scene_rect_set_opacity(overlay->rect, 1.0f);
	}
	if (overlay->outgoing_rect) {
		wlr_scene_node_destroy(&overlay->outgoing_rect->tree->node);
	}
	if (overlay->animation_timer) {
		wl_event_source_remove(overlay->animation_timer);
		overlay->animation_timer = NULL;
	}
}

static int
handle_animation_timeout(void *data)
{
	struct seat *seat = data;
	struct overlay *overlay = &seat->overlay;
	bool running = false;

	if (overlay->rect && overlay->opacity < 1.0f) {
		overlay->opacity = MIN(1.0f,
			overlay->opacity + OVERLAY_ANIMATION_STEP);
		lab_scene_rect_set_opacity(overlay->rect, overlay->opacity);
		running = overlay->opacity < 1.0f;
	}
	if (overlay->outgoing_rect) {
		overlay->outgoing_opacity = MAX(0.0f,
			overlay->outgoing_opacity - OVERLAY_ANIMATION_STEP);
		lab_scene_rect_set_opacity(overlay->outgoing_rect,
			overlay->outgoing_opacity);
		if (overlay->outgoing_opacity == 0.0f) {
			wlr_scene_node_destroy(&overlay->outgoing_rect->tree->node);
			overlay->outgoing_rect = NULL;
		} else {
			running = true;
		}
	}

	if (running) {
		if (wl_event_source_timer_update(overlay->animation_timer,
				OVERLAY_ANIMATION_INTERVAL_MS) < 0) {
			finish_animation(overlay);
		}
	} else {
		wl_event_source_remove(overlay->animation_timer);
		overlay->animation_timer = NULL;
	}
	return 0;
}

static void
start_animation(struct seat *seat)
{
	if (!seat->overlay.animation_timer) {
		seat->overlay.animation_timer = wl_event_loop_add_timer(
			server.wl_event_loop, handle_animation_timeout, seat);
	}
	if (!seat->overlay.animation_timer
			|| wl_event_source_timer_update(seat->overlay.animation_timer,
				OVERLAY_ANIMATION_INTERVAL_MS) < 0) {
		finish_animation(&seat->overlay);
	}
}

static void
show_overlay(struct seat *seat, struct theme_snapping_overlay *overlay_theme,
		struct wlr_box *box)
{
	struct view *view = server.grabbed_view;
	assert(view);
	assert(!seat->overlay.rect);

	struct lab_scene_rect_options opts = {
		.width = box->width,
		.height = box->height,
	};
	if (overlay_theme->bg_enabled) {
		/* Create a filled rectangle */
		opts.bg_color = overlay_theme->bg_color;
	}
	float *border_colors[3] = {
		overlay_theme->border_color[0],
		overlay_theme->border_color[1],
		overlay_theme->border_color[2],
	};
	if (overlay_theme->border_enabled) {
		/* Create outlines */
		opts.border_colors = border_colors;
		opts.nr_borders = 3;
		opts.border_width = overlay_theme->border_width;
	}

	seat->overlay.rect =
		lab_scene_rect_create(view->scene_tree->node.parent, &opts);
	seat->overlay.rect_destroy.notify = handle_rect_destroy;
	wl_signal_add(&seat->overlay.rect->tree->node.events.destroy,
		&seat->overlay.rect_destroy);
	seat->overlay.opacity = 0.0f;
	lab_scene_rect_set_opacity(seat->overlay.rect, 0.0f);
	start_animation(seat);

	struct wlr_scene_node *node = &seat->overlay.rect->tree->node;
	wlr_scene_node_place_below(node, &view->scene_tree->node);
	wlr_scene_node_set_position(node, box->x, box->y);
}

static void
show_region_overlay(struct seat *seat, struct region *region)
{
	if (region == seat->overlay.active.region) {
		return;
	}
	overlay_finish(seat);
	seat->overlay.active.region = region;

	struct wlr_box geo = view_get_region_snap_box(NULL, region);
	show_overlay(seat, &rc.theme->snapping_overlay_region, &geo);
}

static void
show_float_drop_overlay(struct seat *seat)
{
	if (seat->overlay.active.float_drop) {
		return;
	}
	struct wlr_box box;
	if (!singularity_tiling_get_float_drop_box(server.grabbed_view, &box)) {
		return;
	}
	overlay_finish(seat);
	seat->overlay.active.float_drop = true;
	show_overlay(seat, &rc.theme->snapping_overlay_region, &box);
}

static void
show_tiling_drop_overlay(struct seat *seat, struct wlr_box *box)
{
	if (seat->overlay.active.tiling_drop
			&& wlr_box_equal(&seat->overlay.active.tiling_drop_box, box)) {
		return;
	}
	overlay_finish(seat);
	seat->overlay.active.tiling_drop = true;
	seat->overlay.active.tiling_drop_box = *box;
	show_overlay(seat, &rc.theme->snapping_overlay_region, box);
}

static void
show_group_drop_overlay(struct seat *seat, struct wlr_box *box)
{
	if (seat->overlay.active.group_drop
			&& wlr_box_equal(&seat->overlay.active.group_drop_box, box)) {
		return;
	}
	overlay_finish(seat);
	seat->overlay.active.group_drop = true;
	seat->overlay.active.group_drop_box = *box;
	show_overlay(seat, &rc.theme->snapping_overlay_region, box);
}

static bool
show_group_overlay_if_hovered(struct seat *seat)
{
	if (!server.grabbed_view
			|| server.input_mode != LAB_INPUT_STATE_MOVE) {
		return false;
	}
	struct view *target = view_group_drop_target(server.grabbed_view,
		seat->cursor->x, seat->cursor->y);
	if (!target) {
		return false;
	}
	struct wlr_box box = target->current;
	show_group_drop_overlay(seat, &box);
	return true;
}

static struct wlr_box
get_edge_snap_box(enum lab_edge edge, struct output *output)
{
	if (edge == LAB_EDGE_TOP && rc.snap_top_maximize) {
		return output_usable_area_in_layout_coords(output);
	} else {
		return view_get_edge_snap_box(NULL, output, edge);
	}
}

static int
handle_edge_overlay_timeout(void *data)
{
	struct seat *seat = data;
	assert(seat->overlay.active.edge != LAB_EDGE_NONE
		&& seat->overlay.active.output);
	struct wlr_box box = get_edge_snap_box(seat->overlay.active.edge,
		seat->overlay.active.output);
	show_overlay(seat, &rc.theme->snapping_overlay_edge, &box);
	return 0;
}

static bool
edge_has_adjacent_output_from_cursor(struct seat *seat, struct output *output,
		enum lab_edge edge)
{
	/* Allow only up/down/left/right */
	if (!lab_edge_is_cardinal(edge)) {
		return false;
	}
	/* Cast from enum lab_edge to enum wlr_direction is safe */
	return wlr_output_layout_adjacent_output(
		server.output_layout, (enum wlr_direction)edge,
		output->wlr_output, seat->cursor->x, seat->cursor->y);
}

static void
show_edge_overlay(struct seat *seat, enum lab_edge edge1, enum lab_edge edge2,
		struct output *output)
{
	if (!rc.snap_overlay_enabled) {
		return;
	}
	enum lab_edge edge = edge1 | edge2;
	if (seat->overlay.active.edge == edge
			&& seat->overlay.active.output == output) {
		return;
	}
	overlay_finish(seat);
	seat->overlay.active.edge = edge;
	seat->overlay.active.output = output;

	int delay;
	if (edge_has_adjacent_output_from_cursor(seat, output, edge1)) {
		delay = rc.snap_overlay_delay_inner;
	} else {
		delay = rc.snap_overlay_delay_outer;
	}

	if (delay > 0) {
		if (!seat->overlay.timer) {
			seat->overlay.timer = wl_event_loop_add_timer(
				server.wl_event_loop,
				handle_edge_overlay_timeout, seat);
		}
		/* Show overlay <snapping><preview><delay>ms later */
		wl_event_source_timer_update(seat->overlay.timer, delay);
	} else {
		/* Show overlay now */
		handle_edge_overlay_timeout(seat);
	}
}

void
overlay_update(struct seat *seat)
{
	if (show_group_overlay_if_hovered(seat)) {
		return;
	}

	if (singularity_tiling_scrolling_mode_enabled()) {
		if (server.grabbed_view
				&& server.grabbed_view->singularity_scrolling_tiled
				&& singularity_tiling_float_candidate(server.grabbed_view)) {
			show_float_drop_overlay(seat);
		} else {
			struct wlr_box box;
			if (server.grabbed_view
					&& singularity_tiling_get_drop_preview_box(&box)) {
				show_tiling_drop_overlay(seat, &box);
			} else {
				overlay_finish(seat);
			}
		}
		return;
	}

	/* Region-snapping overlay */
	if (regions_should_snap()) {
		struct region *region = regions_from_cursor();
		if (region) {
			show_region_overlay(seat, region);
			return;
		}
	}

	/* Edge-snapping overlay */
	struct output *output;
	enum lab_edge edge1, edge2;
	if (edge_from_cursor(seat, &output, &edge1, &edge2)) {
		show_edge_overlay(seat, edge1, edge2, output);
		return;
	}

	overlay_finish(seat);
}

void
overlay_finish(struct seat *seat)
{
	if (seat->overlay.timer) {
		wl_event_source_remove(seat->overlay.timer);
		seat->overlay.timer = NULL;
	}
	if (seat->overlay.rect) {
		if (seat->overlay.outgoing_rect) {
			wlr_scene_node_destroy(
				&seat->overlay.outgoing_rect->tree->node);
		}
		wl_list_remove(&seat->overlay.rect_destroy.link);
		seat->overlay.rect_destroy.notify = NULL;
		seat->overlay.outgoing_rect = seat->overlay.rect;
		seat->overlay.outgoing_opacity = seat->overlay.opacity;
		seat->overlay.outgoing_rect_destroy.notify =
			handle_outgoing_rect_destroy;
		wl_signal_add(&seat->overlay.outgoing_rect->tree->node.events.destroy,
			&seat->overlay.outgoing_rect_destroy);
		seat->overlay.rect = NULL;
		seat->overlay.opacity = 0.0f;
		start_animation(seat);
	}
	seat->overlay.active.region = NULL;
	seat->overlay.active.edge = LAB_EDGE_NONE;
	seat->overlay.active.output = NULL;
	seat->overlay.active.float_drop = false;
	seat->overlay.active.tiling_drop = false;
	seat->overlay.active.tiling_drop_box = (struct wlr_box){0};
	seat->overlay.active.group_drop = false;
	seat->overlay.active.group_drop_box = (struct wlr_box){0};
}
