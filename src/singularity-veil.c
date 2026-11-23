// SPDX-License-Identifier: GPL-2.0-only
#include "singularity-veil.h"
#include <stdlib.h>
#include <string.h>
#include <scenefx/types/wlr_scene.h>
#include "common/mem.h"
#include "labwc.h"
#include "motion.h"

struct veiled_node {
	struct wl_list link;
	struct wlr_scene_node *node;
	float opacity;
	float color[4];
	float applied_opacity;
	float applied[4];
	struct wl_listener destroy;
};

static struct {
	struct wl_list nodes;
	struct motion_timed timed;
	double from;
	double to;
	double value;
	bool ready;
} veil = { .value = 1.0 };

static void
handle_node_destroy(struct wl_listener *listener, void *data)
{
	struct veiled_node *entry = wl_container_of(listener, entry, destroy);
	wl_list_remove(&entry->destroy.link);
	wl_list_remove(&entry->link);
	free(entry);
}

static void
read_current(struct wlr_scene_node *node, float *opacity, float color[4])
{
	switch (node->type) {
	case WLR_SCENE_NODE_BUFFER:
		*opacity = wlr_scene_buffer_from_node(node)->opacity;
		break;
	case WLR_SCENE_NODE_RECT:
		memcpy(color, wlr_scene_rect_from_node(node)->color, 4 * sizeof(float));
		break;
	case WLR_SCENE_NODE_SHADOW:
		memcpy(color, wlr_scene_shadow_from_node(node)->color, 4 * sizeof(float));
		break;
	default:
		break;
	}
}

static struct veiled_node *
remember(struct wlr_scene_node *node)
{
	struct veiled_node *entry;
	float opacity = 0.0f;
	float color[4] = { 0 };
	read_current(node, &opacity, color);
	wl_list_for_each(entry, &veil.nodes, link) {
		if (entry->node == node) {
			if (opacity != entry->applied_opacity
					|| memcmp(color, entry->applied, sizeof(color)) != 0) {
				entry->opacity = opacity;
				memcpy(entry->color, color, sizeof(color));
			}
			return entry;
		}
	}
	entry = znew(*entry);
	entry->node = node;
	entry->opacity = opacity;
	memcpy(entry->color, color, sizeof(color));
	entry->destroy.notify = handle_node_destroy;
	wl_signal_add(&node->events.destroy, &entry->destroy);
	wl_list_insert(&veil.nodes, &entry->link);
	return entry;
}

static void
apply(struct veiled_node *entry, float amount)
{
	struct wlr_scene_node *node = entry->node;
	float color[4];
	entry->applied_opacity = entry->opacity * amount;
	for (int i = 0; i < 4; i++) {
		entry->applied[i] = node->type == WLR_SCENE_NODE_SHADOW && i < 3
			? entry->color[i] : entry->color[i] * amount;
	}
	switch (node->type) {
	case WLR_SCENE_NODE_BUFFER:
		wlr_scene_buffer_set_opacity(wlr_scene_buffer_from_node(node), entry->opacity * amount);
		break;
	case WLR_SCENE_NODE_RECT:
		for (int i = 0; i < 4; i++) {
			color[i] = entry->color[i] * amount;
		}
		wlr_scene_rect_set_color(wlr_scene_rect_from_node(node), color);
		break;
	case WLR_SCENE_NODE_SHADOW:
		memcpy(color, entry->color, sizeof(color));
		color[3] *= amount;
		wlr_scene_shadow_set_color(wlr_scene_shadow_from_node(node), color);
		break;
	default:
		break;
	}
}

static void
fade_tree(struct wlr_scene_tree *tree, float amount)
{
	struct wlr_scene_node *child;
	wl_list_for_each(child, &tree->children, link) {
		if (child->type == WLR_SCENE_NODE_TREE) {
			fade_tree(wlr_scene_tree_from_node(child), amount);
		} else if (child->type == WLR_SCENE_NODE_BUFFER || child->type == WLR_SCENE_NODE_RECT
				|| child->type == WLR_SCENE_NODE_SHADOW) {
			apply(remember(child), amount);
		}
	}
}

static struct wlr_scene_tree **
trees(size_t *count)
{
	static struct wlr_scene_tree *list[4];
	size_t n = 0;
	list[n++] = server.workspace_tree;
	list[n++] = server.view_animation_tree;
	list[n++] = server.xdg_popup_tree;
#if HAVE_XWAYLAND
	list[n++] = server.unmanaged_tree;
#endif
	*count = n;
	return list;
}

static void
show_trees(bool shown)
{
	size_t count;
	struct wlr_scene_tree **list = trees(&count);
	for (size_t i = 0; i < count; i++) {
		if (list[i]) {
			wlr_scene_node_set_enabled(&list[i]->node, shown);
		}
	}
}

static void
set_amount(double amount)
{
	veil.value = amount;
	size_t count;
	struct wlr_scene_tree **list = trees(&count);
	for (size_t i = 0; i < count; i++) {
		if (list[i]) {
			fade_tree(list[i], (float)amount);
		}
	}
}

static void
forget_all(void)
{
	struct veiled_node *entry, *tmp;
	wl_list_for_each_safe(entry, tmp, &veil.nodes, link) {
		apply(entry, 1.0f);
		wl_list_remove(&entry->destroy.link);
		wl_list_remove(&entry->link);
		free(entry);
	}
}

static void
handle_update(struct motion_timed *timed, double eased, double linear)
{
	(void)timed;
	(void)linear;
	set_amount(veil.from + (veil.to - veil.from) * eased);
}

static void
handle_done(struct motion_timed *timed)
{
	(void)timed;
	if (veil.to <= 0.0) {
		veil.value = 0.0;
		show_trees(false);
	} else {
		veil.value = 1.0;
		forget_all();
	}
}

void
singularity_veil_fade(bool faded, int duration_ms)
{
	if (!veil.ready) {
		wl_list_init(&veil.nodes);
		veil.timed.update = handle_update;
		veil.timed.done = handle_done;
		veil.ready = true;
	}
	double target = faded ? 0.0 : 1.0;
	if (motion_timed_running(&veil.timed)) {
		motion_timed_cancel(&veil.timed);
	} else if (veil.value == target) {
		return;
	}
	show_trees(true);
	veil.from = veil.value;
	veil.to = target;
	if (!motion_enabled() || duration_ms <= 0) {
		set_amount(target);
		handle_done(&veil.timed);
		return;
	}
	set_amount(veil.from);
	motion_timed_start_custom(&veil.timed, MOTION_WINDOW_OPEN_FADE, duration_ms,
		faded ? MOTION_CURVE_ENTER : MOTION_CURVE_EXIT);
}

bool
singularity_veil_active(void)
{
	return veil.ready && veil.value < 1.0;
}

void
singularity_veil_finish(void)
{
	if (veil.ready && singularity_veil_active()) {
		singularity_veil_fade(false, 0);
	}
}
