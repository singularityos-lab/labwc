// SPDX-License-Identifier: GPL-2.0-only
#include "view-animation.h"
#include <assert.h>
#include <wlr/render/allocator.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include "common/mem.h"
#include "common/scene-helpers.h"
#include "config/rcxml.h"
#include "labwc.h"
#include "output.h"
#include "ssd.h"
#include "view.h"

#define VIEW_ANIMATION_INTERVAL_MS 16
#define VIEW_ANIMATION_FRAMES 12
#define MINIMIZED_SCALE 0.08f
#define OPEN_SCALE 0.96f

struct view_animation {
	struct view *view;
	struct wlr_scene_buffer *snapshot;
	struct wl_event_source *timer;
	struct wlr_box from;
	struct wlr_box to;
	float opacity_from;
	float opacity_to;
	int frame;
};

static void
render_node(struct wlr_render_pass *pass,
		struct wlr_scene_node *node, int x, int y,
		struct wl_array *textures, bool root)
{
	if (!root && !node->enabled) {
		return;
	}
	x += node->x;
	y += node->y;
	switch (node->type) {
	case WLR_SCENE_NODE_TREE: {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &tree->children, link) {
			render_node(pass, child, x, y, textures, false);
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
render_snapshot(struct view *view)
{
	struct wlr_box box = ssd_max_extents(view);
	struct wlr_buffer *buffer = wlr_allocator_create_buffer(server.allocator,
		box.width, box.height,
		&view->output->wlr_output->swapchain->format);
	if (!buffer) {
		return NULL;
	}
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server.renderer, buffer, NULL);
	if (!pass) {
		wlr_buffer_drop(buffer);
		return NULL;
	}
	struct wl_array textures;
	wl_array_init(&textures);
	render_node(pass, &view->scene_tree->node,
		-box.x, -box.y, &textures, true);
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

static void
finish_animation(struct view_animation *animation)
{
	struct view *view = animation->view;
	if (animation->timer) {
		wl_event_source_remove(animation->timer);
	}
	if (animation->snapshot) {
		wlr_scene_node_destroy(&animation->snapshot->node);
	}
	if (view->animation == animation) {
		view->animation = NULL;
		if (view->scene_tree) {
			view_update_visibility(view);
		}
	}
	free(animation);
}

static float
ease_in_out_cubic(float progress)
{
	if (progress < 0.5f) {
		return 4.0f * progress * progress * progress;
	}
	float offset = -2.0f * progress + 2.0f;
	return 1.0f - offset * offset * offset / 2.0f;
}

static int
interpolate(int from, int to, float progress)
{
	return from + (int)((to - from) * progress);
}

static void
update_animation(struct view_animation *animation, float progress)
{
	progress = ease_in_out_cubic(progress);
	int width = MAX(1, interpolate(animation->from.width,
		animation->to.width, progress));
	int height = MAX(1, interpolate(animation->from.height,
		animation->to.height, progress));
	wlr_scene_node_set_position(&animation->snapshot->node,
		interpolate(animation->from.x, animation->to.x, progress),
		interpolate(animation->from.y, animation->to.y, progress));
	wlr_scene_buffer_set_dest_size(animation->snapshot, width, height);
	wlr_scene_buffer_set_opacity(animation->snapshot,
		animation->opacity_from
		+ (animation->opacity_to - animation->opacity_from) * progress);
}

static int
handle_animation_timeout(void *data)
{
	struct view_animation *animation = data;
	animation->frame++;
	float progress = (float)animation->frame / VIEW_ANIMATION_FRAMES;
	update_animation(animation, MIN(1.0f, progress));
	if (animation->frame >= VIEW_ANIMATION_FRAMES) {
		finish_animation(animation);
		return 0;
	}
	if (wl_event_source_timer_update(animation->timer,
			VIEW_ANIMATION_INTERVAL_MS) < 0) {
		finish_animation(animation);
	}
	return 0;
}

static void
start_animation(struct view_animation *animation, struct wlr_box from,
	struct wlr_box to, float opacity_from, float opacity_to)
{
	if (from.width < 1 || from.height < 1
			|| to.width < 1 || to.height < 1) {
		finish_animation(animation);
		return;
	}
	animation->from = from;
	animation->to = to;
	animation->opacity_from = opacity_from;
	animation->opacity_to = opacity_to;
	update_animation(animation, 0.0f);
	if (!animation->view->minimized) {
		wlr_scene_node_set_enabled(
			&animation->view->scene_tree->node, false);
	}
	animation->timer = wl_event_loop_add_timer(server.wl_event_loop,
		handle_animation_timeout, animation);
	if (!animation->timer
			|| wl_event_source_timer_update(animation->timer,
				VIEW_ANIMATION_INTERVAL_MS) < 0) {
		finish_animation(animation);
	}
}

struct view_animation *
view_animation_create(struct view *view)
{
	assert(view);
	view_animation_cancel(view);
	if (!rc.window_animations || !view->mapped || !view->content_tree
			|| !output_is_usable(view->output)
			|| !view->output->wlr_output->swapchain
			|| view->current.width < 1 || view->current.height < 1) {
		return NULL;
	}
	struct wlr_buffer *buffer = render_snapshot(view);
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
	return view->animation && view->animation->timer;
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
	start_animation(animation, animation->from, target_extents, 1.0f, 1.0f);
}

void
view_animation_start_minimize(struct view_animation *animation, bool minimized)
{
	if (!animation) {
		return;
	}
	struct wlr_box usable =
		output_usable_area_in_layout_coords(animation->view->output);
	struct wlr_box minimized_box = {
		.width = MAX(1, (int)(animation->from.width * MINIMIZED_SCALE)),
		.height = MAX(1, (int)(animation->from.height * MINIMIZED_SCALE)),
	};
	minimized_box.x = usable.x + (usable.width - minimized_box.width) / 2;
	minimized_box.y = usable.y + usable.height - minimized_box.height / 2;
	if (minimized) {
		start_animation(animation, animation->from, minimized_box,
			1.0f, 0.0f);
	} else {
		start_animation(animation, minimized_box, animation->from,
			0.0f, 1.0f);
	}
}

void
view_animation_start_open(struct view_animation *animation)
{
	if (!animation) {
		return;
	}
	struct wlr_box from = {
		.width = MAX(1, (int)(animation->from.width * OPEN_SCALE)),
		.height = MAX(1, (int)(animation->from.height * OPEN_SCALE)),
	};
	from.x = animation->from.x + (animation->from.width - from.width) / 2;
	from.y = animation->from.y + (animation->from.height - from.height) / 2;
	start_animation(animation, from, animation->from, 0.0f, 1.0f);
}

void
view_animation_cancel(struct view *view)
{
	assert(view);
	if (view->animation) {
		finish_animation(view->animation);
	}
}
