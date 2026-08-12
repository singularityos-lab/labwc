// SPDX-License-Identifier: GPL-2.0-only
#include "protocols/singularity-gesture.h"
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include "common/macros.h"
#include "labwc.h"
#include "output.h"
#include "singularity-gesture-unstable-v1-protocol.h"
#include "view.h"
#include "view-animation.h"

#define REVEAL_EDGE_SIZE 28
#define REVEAL_GESTURE_DISTANCE 0.35
#define REVEAL_ANIMATION_FRAMES 12
#define REVEAL_ANIMATION_INTERVAL_MS 16

struct singularity_gesture_manager {
	struct wl_global *global;
	struct wl_list resources;
};

static struct singularity_gesture_manager *gesture_manager;

struct reveal_view {
	struct wl_list link;
	struct view *view;
	struct wl_listener destroy;
	int from_x;
	int from_y;
	int to_x;
	int to_y;
};

static struct {
	struct wl_list views;
	struct wl_event_source *timer;
	double progress;
	double gesture_start_progress;
	double gesture_start_scale;
	double settle_from;
	double settle_to;
	int frame;
	bool initialized;
} desktop_reveal;

static void
reveal_view_destroy(struct wl_listener *listener, void *data)
{
	struct reveal_view *entry = wl_container_of(listener, entry, destroy);
	wl_list_remove(&entry->destroy.link);
	wl_list_remove(&entry->link);
	free(entry);
	if (wl_list_empty(&desktop_reveal.views)) {
		if (desktop_reveal.timer) {
			wl_event_source_remove(desktop_reveal.timer);
			desktop_reveal.timer = NULL;
		}
		desktop_reveal.progress = 0.0;
	}
}

static void
desktop_reveal_apply(double progress)
{
	struct reveal_view *entry;
	wl_list_for_each(entry, &desktop_reveal.views, link) {
		int x = entry->from_x
			+ (int)((entry->to_x - entry->from_x) * progress);
		int y = entry->from_y
			+ (int)((entry->to_y - entry->from_y) * progress);
		wlr_scene_node_set_position(&entry->view->scene_tree->node, x, y);
	}
	desktop_reveal.progress = progress;
	cursor_update_focus();
}

static void
desktop_reveal_clear(void)
{
	struct reveal_view *entry, *tmp;
	wl_list_for_each_safe(entry, tmp, &desktop_reveal.views, link) {
		wl_list_remove(&entry->destroy.link);
		wl_list_remove(&entry->link);
		free(entry);
	}
}

static bool
box_intersects(const struct wlr_box *a, const struct wlr_box *b)
{
	return a->x < b->x + b->width && a->x + a->width > b->x
		&& a->y < b->y + b->height && a->y + a->height > b->y;
}

static bool
desktop_reveal_capture(void)
{
	struct view *view;
	for_each_view(view, &server.views, LAB_VIEW_CRITERIA_CURRENT_WORKSPACE) {
		if (view->minimized || !view->scene_tree
				|| !output_is_usable(view->output)) {
			continue;
		}
		struct wlr_box output_box;
		wlr_output_layout_get_box(server.output_layout,
			view->output->wlr_output, &output_box);
		if (!box_intersects(&view->current, &output_box)) {
			continue;
		}

		view_animation_cancel(view);
		struct reveal_view *entry = calloc(1, sizeof(*entry));
		if (!entry) {
			continue;
		}
		entry->view = view;
		entry->from_x = view->scene_tree->node.x;
		entry->from_y = view->scene_tree->node.y;
		entry->to_x = entry->from_x;
		entry->to_y = entry->from_y;

		int center_x = view->current.x + view->current.width / 2;
		int center_y = view->current.y + view->current.height / 2;
		if (view->singularity_scrolling_tiled) {
			entry->to_x = center_x < output_box.x + output_box.width / 2
				? output_box.x - view->current.width + REVEAL_EDGE_SIZE
				: output_box.x + output_box.width - REVEAL_EDGE_SIZE;
		} else {
			int left = center_x - output_box.x;
			int right = output_box.x + output_box.width - center_x;
			int top = center_y - output_box.y;
			int bottom = output_box.y + output_box.height - center_y;
			int nearest = MIN(MIN(left, right), MIN(top, bottom));
			if (nearest == left) {
				entry->to_x = output_box.x - view->current.width
					+ REVEAL_EDGE_SIZE;
			} else if (nearest == right) {
				entry->to_x = output_box.x + output_box.width
					- REVEAL_EDGE_SIZE;
			} else if (nearest == top) {
				entry->to_y = output_box.y - view->current.height
					+ REVEAL_EDGE_SIZE;
			} else {
				entry->to_y = output_box.y + output_box.height
					- REVEAL_EDGE_SIZE;
			}
		}

		entry->destroy.notify = reveal_view_destroy;
		wl_signal_add(&view->events.destroy, &entry->destroy);
		wl_list_insert(desktop_reveal.views.prev, &entry->link);
	}
	return !wl_list_empty(&desktop_reveal.views);
}

static double
ease_out_cubic(double progress)
{
	double remaining = 1.0 - progress;
	return 1.0 - remaining * remaining * remaining;
}

static void
desktop_reveal_finish_settle(void)
{
	if (desktop_reveal.timer) {
		wl_event_source_remove(desktop_reveal.timer);
		desktop_reveal.timer = NULL;
	}
	desktop_reveal_apply(desktop_reveal.settle_to);
	if (desktop_reveal.settle_to == 0.0) {
		desktop_reveal_clear();
	}
}

static int
desktop_reveal_settle(void *data)
{
	desktop_reveal.frame++;
	double progress = MIN(1.0,
		(double)desktop_reveal.frame / REVEAL_ANIMATION_FRAMES);
	double eased = ease_out_cubic(progress);
	desktop_reveal_apply(desktop_reveal.settle_from
		+ (desktop_reveal.settle_to - desktop_reveal.settle_from) * eased);
	if (desktop_reveal.frame >= REVEAL_ANIMATION_FRAMES) {
		desktop_reveal_finish_settle();
		return 0;
	}
	if (wl_event_source_timer_update(desktop_reveal.timer,
			REVEAL_ANIMATION_INTERVAL_MS) < 0) {
		desktop_reveal_finish_settle();
	}
	return 0;
}

static void
desktop_reveal_animate_to(double progress)
{
	if (desktop_reveal.timer) {
		wl_event_source_remove(desktop_reveal.timer);
		desktop_reveal.timer = NULL;
	}
	desktop_reveal.settle_from = desktop_reveal.progress;
	desktop_reveal.settle_to = progress;
	desktop_reveal.frame = 0;
	desktop_reveal.timer = wl_event_loop_add_timer(server.wl_event_loop,
		desktop_reveal_settle, NULL);
	if (!desktop_reveal.timer
			|| wl_event_source_timer_update(desktop_reveal.timer,
				REVEAL_ANIMATION_INTERVAL_MS) < 0) {
		desktop_reveal_finish_settle();
	}
}

static void
desktop_reveal_toggle(void)
{
	if (wl_list_empty(&desktop_reveal.views) && !desktop_reveal_capture()) {
		return;
	}
	desktop_reveal_animate_to(desktop_reveal.progress < 0.5 ? 1.0 : 0.0);
}

static void
handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
handle_toggle_desktop_reveal(struct wl_client *client,
	struct wl_resource *resource)
{
	desktop_reveal_toggle();
}

static const struct zsingularity_gesture_manager_v1_interface manager_impl = {
	.toggle_desktop_reveal = handle_toggle_desktop_reveal,
	.destroy = handle_destroy,
};

static void
resource_destroy(struct wl_resource *resource)
{
	wl_list_remove(wl_resource_get_link(resource));
}

static void
bind_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct singularity_gesture_manager *manager = data;
	struct wl_resource *resource = wl_resource_create(client,
		&zsingularity_gesture_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, manager,
		resource_destroy);
	wl_list_insert(&manager->resources, wl_resource_get_link(resource));
}

void
singularity_gesture_init(void)
{
	wl_list_init(&desktop_reveal.views);
	desktop_reveal.initialized = true;
	gesture_manager = calloc(1, sizeof(*gesture_manager));
	if (!gesture_manager) {
		return;
	}
	wl_list_init(&gesture_manager->resources);
	gesture_manager->global = wl_global_create(server.wl_display,
		&zsingularity_gesture_manager_v1_interface, 2,
		gesture_manager, bind_manager);
}

bool
singularity_gesture_pinch_begin(uint32_t fingers, double scale)
{
	if (fingers != 4 || !desktop_reveal.initialized) {
		return false;
	}
	if (desktop_reveal.timer) {
		wl_event_source_remove(desktop_reveal.timer);
		desktop_reveal.timer = NULL;
	}
	if (wl_list_empty(&desktop_reveal.views) && !desktop_reveal_capture()) {
		return false;
	}
	desktop_reveal.gesture_start_progress = desktop_reveal.progress;
	desktop_reveal.gesture_start_scale = scale;
	return true;
}

void
singularity_gesture_pinch_update(double scale)
{
	double delta = (scale / desktop_reveal.gesture_start_scale - 1.0)
		/ REVEAL_GESTURE_DISTANCE;
	desktop_reveal_apply(MAX(0.0, MIN(1.0,
		desktop_reveal.gesture_start_progress + delta)));
}

void
singularity_gesture_pinch_end(bool cancelled)
{
	double target = cancelled ? desktop_reveal.gesture_start_progress
		: (desktop_reveal.progress >= 0.5 ? 1.0 : 0.0);
	desktop_reveal_animate_to(target);
}

void
singularity_gesture_finish(void)
{
	if (!desktop_reveal.initialized) {
		return;
	}
	if (desktop_reveal.timer) {
		wl_event_source_remove(desktop_reveal.timer);
		desktop_reveal.timer = NULL;
	}
	desktop_reveal_apply(0.0);
	desktop_reveal_clear();
	desktop_reveal.initialized = false;
}

bool
singularity_gesture_has_clients(void)
{
	return gesture_manager && !wl_list_empty(&gesture_manager->resources);
}

void
singularity_gesture_send_begin(uint32_t fingers, enum direction direction)
{
	if (!gesture_manager) {
		return;
	}
	struct wl_resource *resource;
	wl_resource_for_each(resource, &gesture_manager->resources) {
		zsingularity_gesture_manager_v1_send_begin(resource, fingers, direction);
	}
}

void
singularity_gesture_send_update(double dx, double dy)
{
	if (!gesture_manager) {
		return;
	}
	struct wl_resource *resource;
	wl_resource_for_each(resource, &gesture_manager->resources) {
		zsingularity_gesture_manager_v1_send_update(resource,
			wl_fixed_from_double(dx), wl_fixed_from_double(dy));
	}
}

void
singularity_gesture_send_end(bool cancelled, bool committed)
{
	if (!gesture_manager) {
		return;
	}
	struct wl_resource *resource;
	wl_resource_for_each(resource, &gesture_manager->resources) {
		zsingularity_gesture_manager_v1_send_end(resource,
			cancelled ? 1u : 0u, committed ? 1u : 0u);
	}
}
