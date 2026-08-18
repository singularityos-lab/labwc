/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_GROUP_H
#define LABWC_GROUP_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>

struct view;
struct wlr_scene_node;

struct view_group {
	uint32_t id;
	struct wl_list members;
	struct view *active;
	struct view *bar_parent;
	bool spread;
	bool applying;
	bool box_valid;
	struct wlr_box box;
	struct wlr_scene_tree *tree;
	struct wlr_scene_rect *background;
	struct wl_array tabs;
};

int view_group_bar_height(void);
uint32_t view_group_id(struct view_group *group);
uint32_t view_group_index(struct view *view);
bool view_group_is_hidden(struct view *view);
struct view *view_group_anchor(struct view_group *group);

void view_group_join(struct view *view, struct view *target);
void view_group_leave(struct view *view);
void view_group_activate(struct view *view);
void view_group_cycle(struct view *view, int direction);
void view_group_toggle_spread(struct view *view);

bool view_group_apply_box(struct view *view, struct wlr_box box);
void view_group_notify_geometry(struct view *view);
void view_group_notify_title(struct view *view);
void view_group_notify_minimized(struct view *view);
void view_group_notify_destroy(struct view *view);

struct view *view_group_bar_view(struct wlr_scene_node *node);
struct view *view_group_pick_partner(struct view *view);
void view_group_activate_from_node(struct wlr_scene_node *node);

#endif /* LABWC_GROUP_H */
