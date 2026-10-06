// SPDX-License-Identifier: GPL-2.0-only
#include "view-animation.h"
#include <assert.h>
#include <math.h>
#include <wlr/render/allocator.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <scenefx/types/wlr_scene.h>
#include "common/mem.h"
#include "common/scene-helpers.h"
#include "config/rcxml.h"
#include "labwc.h"
#include "motion.h"
#include "output.h"
#include "protocols/singularity-motion.h"
#include "ssd.h"
#include "view.h"

#define MINIMIZED_SCALE 0.08f

enum view_animation_style {
	VIEW_ANIMATION_GEOMETRY,
	VIEW_ANIMATION_OPEN,
	VIEW_ANIMATION_CLOSE,
	VIEW_ANIMATION_MINIMIZE,
	VIEW_ANIMATION_RESTORE,
};

struct view_animation {
	struct view *view;
	struct wlr_scene_buffer *snapshot;
	struct wlr_scene_buffer *live;
	struct wlr_box live_box;
	uint32_t live_seq;
	double live_from;
	struct motion_timed timed;
	enum view_animation_style style;
	struct wlr_box from;
	struct wlr_box to;
	float opacity_from;
	float opacity_to;
	double fade_fraction;
};

static void
render_node(struct wlr_render_pass *pass,
		struct wlr_scene_node *node, int x, int y,
		struct wl_array *textures, bool root,
		struct wlr_scene_node *force_node, bool forced)
{
	if (node == force_node) {
		forced = true;
	}
	if (!root && !forced && !node->enabled) {
		return;
	}
	x += node->x;
	y += node->y;
	switch (node->type) {
	case WLR_SCENE_NODE_TREE: {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &tree->children, link) {
			render_node(pass, child, x, y, textures, false,
				force_node, forced);
		}
		break;
	}
	case WLR_SCENE_NODE_RECT: {
		struct wlr_scene_rect *rect = wlr_scene_rect_from_node(node);
		wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
			.box = {
				.x = x,
				.y = y,
				.width = rect->width,
				.height = rect->height,
			},
			.color = {
				.r = rect->color[0],
				.g = rect->color[1],
				.b = rect->color[2],
				.a = rect->color[3],
			},
		});
		break;
	}
	case WLR_SCENE_NODE_BUFFER: {
		struct wlr_scene_buffer *scene_buffer =
			wlr_scene_buffer_from_node(node);
		if (!scene_buffer->buffer) {
			break;
		}
		struct wlr_texture *texture = NULL;
		struct wlr_client_buffer *client_buffer =
			wlr_client_buffer_get(scene_buffer->buffer);
		if (client_buffer) {
			texture = client_buffer->texture;
		} else {
			texture = wlr_texture_from_buffer(
				server.renderer, scene_buffer->buffer);
			if (texture) {
				struct wlr_texture **item = wl_array_add(
					textures, sizeof(*item));
				if (!item) {
					wlr_texture_destroy(texture);
					texture = NULL;
				} else {
					*item = texture;
				}
			}
		}
		if (!texture) {
			break;
		}
		wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
			.texture = texture,
			.src_box = scene_buffer->src_box,
			.dst_box = {
				.x = x,
				.y = y,
				.width = scene_buffer->dst_width,
				.height = scene_buffer->dst_height,
			},
			.alpha = &scene_buffer->opacity,
			.transform = scene_buffer->transform,
			.filter_mode = scene_buffer->filter_mode,
		});
		break;
	}
	}
}

static struct wlr_buffer *
render_view_snapshot(struct view *view,
		struct wlr_color_transform *color_transform, bool include_unmapped)
{
	struct wlr_box box = ssd_max_extents(view);
	struct wlr_buffer *buffer = wlr_allocator_create_buffer(server.allocator,
		box.width, box.height,
		&view->output->wlr_output->swapchain->format);
	if (!buffer) {
		return NULL;
	}
	struct wlr_buffer_pass_options options = {
		.color_transform = color_transform,
	};
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server.renderer, buffer, color_transform ? &options : NULL);
	if (!pass) {
		wlr_buffer_drop(buffer);
		return NULL;
	}
	struct wl_array textures;
	wl_array_init(&textures);
	struct wlr_scene_node *force_node = include_unmapped && view->content_tree
		? &view->content_tree->node : NULL;
	render_node(pass, &view->scene_tree->node,
		-box.x, -box.y, &textures, true, force_node, false);
	bool submitted = wlr_render_pass_submit(pass);
	struct wlr_texture **texture;
	wl_array_for_each(texture, &textures) {
		wlr_texture_destroy(*texture);
	}
	wl_array_release(&textures);
	if (!submitted) {
		wlr_buffer_drop(buffer);
		return NULL;
	}
	return buffer;
}

static struct wlr_buffer *
render_snapshot(struct view *view, struct wlr_color_transform *color_transform)
{
	return render_view_snapshot(view, color_transform, false);
}

static void
finish_animation(struct view_animation *animation)
{
	struct view *view = animation->view;
	motion_timed_cancel(&animation->timed);
	if (animation->snapshot) {
		wlr_scene_node_destroy(&animation->snapshot->node);
	}
	if (animation->live) {
		wlr_scene_node_destroy(&animation->live->node);
	}
	if (view && view->animation == animation) {
		view->animation = NULL;
		if (view->scene_tree) {
			view_update_visibility(view);
		}
	}
	free(animation);
}

static double
lerp(double from, double to, double progress)
{
	return from + (to - from) * progress;
}

static double
clamp01(double value)
{
	return value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
}


static bool
geometry_content_ready(struct view_animation *animation)
{
	struct view *view = animation->view;
	if (!view || !view->surface || !view->mapped) {
		return false;
	}
	struct wlr_box extents = ssd_max_extents(view);
	return extents.width == animation->to.width
		&& extents.height == animation->to.height;
}

static void
update_live_content(struct view_animation *animation, double linear)
{
	struct view *view = animation->view;
	if (!geometry_content_ready(animation)) {
		return;
	}
	uint32_t seq = view->surface->current.seq;
	if (animation->live && animation->live_seq == seq) {
		return;
	}
	struct wlr_buffer *buffer = render_view_snapshot(view, NULL, true);
	if (!buffer) {
		return;
	}
	if (animation->live) {
		wlr_scene_buffer_set_buffer(animation->live, buffer);
	} else {
		animation->live = lab_wlr_scene_buffer_create(
			server.view_animation_tree, buffer);
		wlr_scene_node_place_below(&animation->live->node,
			&animation->snapshot->node);
		animation->live_from = MIN(linear, 0.9);
	}
	wlr_buffer_drop(buffer);
	animation->live_box = ssd_max_extents(view);
	animation->live_seq = seq;
}

static void
place_live_content(struct view_animation *animation,
		struct wlr_box rect, double linear)
{
	if (!animation->live) {
		wlr_scene_buffer_set_opacity(animation->snapshot, 1.0f);
		return;
	}
	struct wlr_box clip;
	if (!wlr_box_intersection(&clip, &rect, &animation->live_box)) {
		wlr_scene_node_set_enabled(&animation->live->node, false);
	} else {
		wlr_scene_node_set_enabled(&animation->live->node, true);
		wlr_scene_node_set_position(&animation->live->node,
			clip.x, clip.y);
		wlr_scene_buffer_set_source_box(animation->live, &(struct wlr_fbox){
			.x = clip.x - animation->live_box.x,
			.y = clip.y - animation->live_box.y,
			.width = clip.width,
			.height = clip.height,
		});
		wlr_scene_buffer_set_dest_size(animation->live,
			clip.width, clip.height);
	}
	double fade = clamp01((linear - animation->live_from)
		/ MAX(0.01, 1.0 - animation->live_from));
	wlr_scene_buffer_set_opacity(animation->snapshot, 1.0 - fade);
}

static void
handle_animation_update(struct motion_timed *timed, double eased, double linear)
{
	struct view_animation *animation =
		wl_container_of(timed, animation, timed);
	double x_progress = eased;
	double y_progress = eased;
	double opacity_progress = eased;
	switch (animation->style) {
	case VIEW_ANIMATION_OPEN:
	case VIEW_ANIMATION_RESTORE:
		opacity_progress = animation->fade_fraction > 0.0
			? clamp01(linear / animation->fade_fraction) : 1.0;
		break;
	case VIEW_ANIMATION_MINIMIZE:
		y_progress = 1.0 - pow(1.0 - eased, 1.6);
		opacity_progress = clamp01((eased - 0.6) / 0.4);
		break;
	case VIEW_ANIMATION_CLOSE:
	case VIEW_ANIMATION_GEOMETRY:
		break;
	}
	double width = MAX(1.0, lerp(animation->from.width,
		animation->to.width, x_progress));
	double height = MAX(1.0, lerp(animation->from.height,
		animation->to.height, x_progress));
	double center_x = lerp(animation->from.x + animation->from.width / 2.0,
		animation->to.x + animation->to.width / 2.0, x_progress);
	double center_y = lerp(animation->from.y + animation->from.height / 2.0,
		animation->to.y + animation->to.height / 2.0, y_progress);
	wlr_scene_node_set_position(&animation->snapshot->node,
		(int)lround(center_x - width / 2.0),
		(int)lround(center_y - height / 2.0));
	wlr_scene_buffer_set_dest_size(animation->snapshot,
		(int)lround(width), (int)lround(height));
	if (animation->style == VIEW_ANIMATION_GEOMETRY) {
		update_live_content(animation, linear);
		place_live_content(animation, (struct wlr_box){
			.x = (int)lround(center_x - width / 2.0),
			.y = (int)lround(center_y - height / 2.0),
			.width = (int)lround(width),
			.height = (int)lround(height),
		}, linear);
		return;
	}
	wlr_scene_buffer_set_opacity(animation->snapshot,
		lerp(animation->opacity_from, animation->opacity_to,
			opacity_progress));
}

static void
handle_animation_done(struct motion_timed *timed)
{
	struct view_animation *animation =
		wl_container_of(timed, animation, timed);
	finish_animation(animation);
}

static void
start_animation(struct view_animation *animation,
		enum view_animation_style style, enum motion_kind kind,
		struct wlr_box from, struct wlr_box to,
		float opacity_from, float opacity_to)
{
	if (from.width < 1 || from.height < 1
			|| to.width < 1 || to.height < 1) {
		finish_animation(animation);
		return;
	}
	animation->style = style;
	animation->from = from;
	animation->to = to;
	animation->opacity_from = opacity_from;
	animation->opacity_to = opacity_to;
	int total = motion_duration_ms(kind);
	int fade = motion_duration_ms(MOTION_WINDOW_OPEN_FADE);
	animation->fade_fraction = total > 0 ? MIN(1.0, (double)fade / total) : 0.0;
	animation->timed.update = handle_animation_update;
	animation->timed.done = handle_animation_done;
	handle_animation_update(&animation->timed, 0.0, 0.0);
	if (animation->view && !animation->view->minimized) {
		wlr_scene_node_set_enabled(
			&animation->view->scene_tree->node, false);
	}
	motion_timed_start(&animation->timed, kind);
}

static bool
view_can_animate(struct view *view)
{
	return rc.window_animations && view->mapped && view->content_tree
		&& output_is_usable(view->output)
		&& view->output->wlr_output->swapchain
		&& view->current.width >= 1 && view->current.height >= 1;
}

struct view_animation *
view_animation_create(struct view *view)
{
	assert(view);
	view_animation_cancel(view);
	if (!view_can_animate(view)) {
		return NULL;
	}
	struct wlr_buffer *buffer = render_snapshot(view, NULL);
	if (!buffer) {
		return NULL;
	}
	struct view_animation *animation = znew(*animation);
	animation->view = view;
	animation->from = ssd_max_extents(view);
	animation->snapshot = lab_wlr_scene_buffer_create(
		server.view_animation_tree, buffer);
	wlr_buffer_drop(buffer);
	view->animation = animation;
	return animation;
}

bool
view_animation_is_running(struct view *view)
{
	assert(view);
	return view->animation
		&& motion_timed_running(&view->animation->timed);
}

static struct wlr_box
fit_box(struct wlr_box window, struct wlr_box target)
{
	double scale = MIN((double)target.width / MAX(1, window.width),
		(double)target.height / MAX(1, window.height));
	struct wlr_box box = {
		.width = MAX(1, (int)lround(window.width * scale)),
		.height = MAX(1, (int)lround(window.height * scale)),
	};
	box.x = target.x + (target.width - box.width) / 2;
	box.y = target.y + (target.height - box.height) / 2;
	return box;
}

static struct wlr_box
scaled_box(struct wlr_box box, double scale)
{
	struct wlr_box scaled = {
		.width = MAX(1, (int)lround(box.width * scale)),
		.height = MAX(1, (int)lround(box.height * scale)),
	};
	scaled.x = box.x + (box.width - scaled.width) / 2;
	scaled.y = box.y + (box.height - scaled.height) / 2;
	return scaled;
}

static bool
view_icon_box(struct view *view, struct wlr_box *box)
{
	struct view *root = view->impl->get_root
		? view->impl->get_root(view) : view;
	if (root != view) {
		return false;
	}
	if (view->stage_target_valid) {
		*box = view->stage_target;
		return true;
	}
	return singularity_motion_icon_box(view->app_id, view->output, box);
}

void
view_animation_start_geometry(struct view_animation *animation,
	const struct wlr_box *target)
{
	if (!animation) {
		return;
	}
	struct border border = ssd_thickness(animation->view);
	struct wlr_box target_extents = {
		.x = target->x - border.left,
		.y = target->y - border.top,
		.width = target->width + border.left + border.right,
		.height = target->height + border.top + border.bottom,
	};
	start_animation(animation, VIEW_ANIMATION_GEOMETRY,
		MOTION_WINDOW_GEOMETRY, animation->from, target_extents,
		1.0f, 1.0f);
}

void
view_animation_start_minimize(struct view_animation *animation, bool minimized)
{
	if (!animation) {
		return;
	}
	struct wlr_box minimized_box;
	struct wlr_box icon;
	if (view_icon_box(animation->view, &icon)) {
		minimized_box = fit_box(animation->from, icon);
	} else {
		struct wlr_box usable =
			output_usable_area_in_layout_coords(animation->view->output);
		minimized_box = (struct wlr_box){
			.width = MAX(1, (int)(animation->from.width * MINIMIZED_SCALE)),
			.height = MAX(1, (int)(animation->from.height * MINIMIZED_SCALE)),
		};
		minimized_box.x = usable.x + (usable.width - minimized_box.width) / 2;
		minimized_box.y = usable.y + usable.height - minimized_box.height / 2;
	}
	if (minimized) {
		start_animation(animation, VIEW_ANIMATION_MINIMIZE,
			MOTION_WINDOW_MINIMIZE, animation->from, minimized_box,
			1.0f, 0.0f);
	} else {
		start_animation(animation, VIEW_ANIMATION_RESTORE,
			MOTION_WINDOW_RESTORE, minimized_box, animation->from,
			0.0f, 1.0f);
	}
}

void
view_animation_start_open(struct view_animation *animation)
{
	if (!animation) {
		return;
	}
	struct view *view = animation->view;
	struct wlr_box icon;
	struct wlr_box from;
	if (singularity_motion_take_launch(view->app_id)
			&& view_icon_box(view, &icon)) {
		from = fit_box(animation->from, icon);
	} else {
		from = scaled_box(animation->from, MOTION_ENTER_SCALE);
	}
	start_animation(animation, VIEW_ANIMATION_OPEN, MOTION_WINDOW_OPEN,
		from, animation->from, 0.0f, 1.0f);
}

void
view_animation_start_close(struct view *view)
{
	assert(view);
	if (!view_can_animate(view) || view->minimized || !view->scene_tree) {
		return;
	}
	int lx, ly;
	if (!wlr_scene_node_coords(&view->scene_tree->node, &lx, &ly)
			|| view_animation_is_running(view)) {
		return;
	}
	struct wlr_buffer *buffer = render_view_snapshot(view, NULL, true);
	if (!buffer) {
		return;
	}
	struct view_animation *animation = znew(*animation);
	animation->from = ssd_max_extents(view);
	animation->snapshot = lab_wlr_scene_buffer_create(
		server.view_animation_tree, buffer);
	wlr_buffer_drop(buffer);
	struct wlr_box icon;
	struct wlr_box to = view_icon_box(view, &icon)
		? fit_box(animation->from, icon)
		: scaled_box(animation->from, MOTION_ENTER_SCALE);
	start_animation(animation, VIEW_ANIMATION_CLOSE, MOTION_WINDOW_CLOSE,
		animation->from, to, 1.0f, 0.0f);
}

void
view_animation_cancel(struct view *view)
{
	assert(view);
	if (view->animation) {
		finish_animation(view->animation);
	}
}

void
view_close_gesture_set_progress(struct view *view, float progress)
{
	assert(view);
	progress = MAX(0.0f, MIN(1.0f, progress));
	view->close_gesture_progress = progress;
	if (progress == 0.0f || !view->mapped || !view->scene_tree
			|| !output_is_usable(view->output)) {
		if (view->close_gesture_snapshot) {
			wlr_scene_node_destroy(&view->close_gesture_snapshot->node);
			view->close_gesture_snapshot = NULL;
		}
		return;
	}
	if (!view->close_gesture_snapshot) {
		static const float grayscale_matrix[9] = {
			0.2126f, 0.7152f, 0.0722f,
			0.2126f, 0.7152f, 0.0722f,
			0.2126f, 0.7152f, 0.0722f,
		};
		struct wlr_color_transform *transform =
			wlr_color_transform_init_matrix(grayscale_matrix);
		if (!transform) {
			return;
		}
		struct wlr_buffer *buffer = render_snapshot(view, transform);
		wlr_color_transform_unref(transform);
		if (!buffer) {
			return;
		}
		view->close_gesture_snapshot = lab_wlr_scene_buffer_create(
			server.view_animation_tree, buffer);
		wlr_buffer_drop(buffer);
	}
	struct wlr_box box = ssd_max_extents(view);
	wlr_scene_node_set_position(&view->close_gesture_snapshot->node,
		box.x, box.y);
	wlr_scene_buffer_set_opacity(view->close_gesture_snapshot, progress);
}
