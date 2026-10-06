// SPDX-License-Identifier: GPL-2.0-only
#include "overlay.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output_layout.h>
#include <scenefx/types/wlr_scene.h>
#include "common/lab-scene-rect.h"
#include "common/macros.h"
#include "config/rcxml.h"
#include "labwc.h"
#include "output.h"
#include "protocols/singularity-tiling.h"
#include "regions.h"
#include "theme.h"
#include "view.h"
#include "protocols/singularity-snap.h"

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

#define GROW_SEED_FRACTION 0.12

static void
place_rect(struct overlay *overlay, struct wlr_box box)
{
	if (!overlay->rect) {
		return;
	}
	box.width = MAX(1, box.width);
	box.height = MAX(1, box.height);
	overlay->box = box;
	lab_scene_rect_set_size(overlay->rect, box.width, box.height);
	wlr_scene_node_set_position(&overlay->rect->tree->node, box.x, box.y);
}

static void
handle_grow_update(struct motion_spring_anim *anim, double value)
{
	struct overlay *overlay = wl_container_of(anim, overlay, grow);
	double t = overlay->grow_range > 0.0 ? value / overlay->grow_range : 1.0;
	struct wlr_box from = overlay->grow_from;
	struct wlr_box to = overlay->grow_to;
	place_rect(overlay, (struct wlr_box){
		.x = (int)lround(from.x + (to.x - from.x) * t),
		.y = (int)lround(from.y + (to.y - from.y) * t),
		.width = (int)lround(from.width + (to.width - from.width) * t),
		.height = (int)lround(from.height + (to.height - from.height) * t),
	});
}

static struct wlr_box
cursor_seed_box(struct seat *seat, const struct wlr_box *target)
{
	int size = MAX(8, (int)lround(MIN(target->width, target->height)
		* GROW_SEED_FRACTION));
	size = MIN(size, MIN(target->width, target->height));
	int x = (int)lround(seat->cursor->x) - size / 2;
	int y = (int)lround(seat->cursor->y) - size / 2;
	x = MAX(target->x, MIN(x, target->x + target->width - size));
	y = MAX(target->y, MIN(y, target->y + target->height - size));
	return (struct wlr_box){ .x = x, .y = y, .width = size, .height = size };
}

static void
finish_animation(struct overlay *overlay)
{
	motion_timed_cancel(&overlay->animation);
	if (overlay->rect) {
		overlay->opacity = 1.0f;
		lab_scene_rect_set_opacity(overlay->rect, 1.0f);
	}
	if (overlay->outgoing_rect) {
		wlr_scene_node_destroy(&overlay->outgoing_rect->tree->node);
	}
}

static void
handle_animation_update(struct motion_timed *timed, double eased, double linear)
{
	struct overlay *overlay = wl_container_of(timed, overlay, animation);
	if (overlay->rect) {
		overlay->opacity = overlay->animation_opacity_from
			+ (1.0f - overlay->animation_opacity_from) * (float)linear;
		lab_scene_rect_set_opacity(overlay->rect, overlay->opacity);
	}
	if (overlay->outgoing_rect) {
		overlay->outgoing_opacity =
			overlay->animation_outgoing_from * (1.0f - (float)linear);
		lab_scene_rect_set_opacity(overlay->outgoing_rect,
			overlay->outgoing_opacity);
	}
}

static void
handle_animation_done(struct motion_timed *timed)
{
	struct overlay *overlay = wl_container_of(timed, overlay, animation);
	finish_animation(overlay);
}

static void
start_animation(struct seat *seat)
{
	struct overlay *overlay = &seat->overlay;
	overlay->animation_opacity_from = overlay->rect ? overlay->opacity : 1.0f;
	overlay->animation_outgoing_from = overlay->outgoing_rect
		? overlay->outgoing_opacity : 0.0f;
	overlay->animation.update = handle_animation_update;
	overlay->animation.done = handle_animation_done;
	motion_timed_start_custom(&overlay->animation, MOTION_SNAP_PREVIEW,
		motion_enabled() ? motion_duration_ms(MOTION_SNAP_PREVIEW) : 0,
		MOTION_CURVE_LINEAR);
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

	/*
	 * By default the overlay is drawn below the dragged window.
	 * With <snapping><overlay><placement>above</placement> it is
	 * drawn above instead.
	 */
	struct wlr_scene_tree *parent = rc.snap_overlay_on_top
		? server.cycle_preview_tree : view->scene_tree->node.parent;

	seat->overlay.rect =
		lab_scene_rect_create(parent, &opts);
	seat->overlay.rect_destroy.notify = handle_rect_destroy;
	wl_signal_add(&seat->overlay.rect->tree->node.events.destroy,
		&seat->overlay.rect_destroy);
	seat->overlay.opacity = 0.0f;
	lab_scene_rect_set_opacity(seat->overlay.rect, 0.0f);
	start_animation(seat);

	struct wlr_scene_node *node = &seat->overlay.rect->tree->node;
	if (rc.snap_overlay_on_top) {
		wlr_scene_node_raise_to_top(node);
	} else {
		wlr_scene_node_place_below(node, &view->scene_tree->node);
	}
	struct overlay *overlay = &seat->overlay;
	overlay->grow_to = *box;
	if (!motion_enabled()) {
		place_rect(overlay, *box);
		return;
	}
	overlay->grow_from = overlay->outgoing_rect
		? overlay->outgoing_box : cursor_seed_box(seat, box);
	overlay->grow.update = handle_grow_update;
	overlay->grow.done = NULL;
	overlay->grow.clamp = true;
	struct wlr_box from = overlay->grow_from;
	overlay->grow_range = MAX(1.0, MAX(MAX(abs(box->x - from.x),
		abs(box->y - from.y)), MAX(abs(box->width - from.width),
		abs(box->height - from.height))));
	place_rect(overlay, from);
	motion_spring_start(&overlay->grow, MOTION_SPRING_GENTLE,
		0.0, overlay->grow_range, 0.0);
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

	struct wlr_box snap_box;
	if (singularity_snap_get_zone_preview(&snap_box)) {
		show_tiling_drop_overlay(seat, &snap_box);
		return;
	}
	if (singularity_snap_cursor_in_picker()) {
		overlay_finish(seat);
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
		motion_spring_cancel(&seat->overlay.grow);
		seat->overlay.outgoing_rect = seat->overlay.rect;
		seat->overlay.outgoing_box = seat->overlay.box;
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
}
