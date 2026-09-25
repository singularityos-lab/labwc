/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_WORKSPACES_H
#define LABWC_WORKSPACES_H

#include <stdbool.h>
#include <wayland-util.h>
#include <wayland-server-core.h>
#include "config/mousebind.h"

struct output;
struct seat;
struct server;
struct view;
struct wlr_scene_tree;

struct workspace {
	struct wl_list link; /* struct server.workspaces */

	char *name;
	struct wlr_scene_tree *tree;
	struct wlr_scene_tree *view_trees[3];

	struct wlr_ext_workspace_handle_v1 *ext_workspace;
	struct wl_list output_handles;
};

void workspaces_init(void);
void workspaces_switch_to(struct workspace *target, bool update_focus);
void workspaces_destroy(void);
void workspaces_osd_hide(struct seat *seat);
struct workspace *workspaces_find(struct workspace *anchor, const char *name,
	bool wrap);
void workspaces_reconfigure(void);
bool workspaces_swipe_begin(enum direction direction);
void workspaces_swipe_update(double dx);
void workspaces_swipe_end(bool commit);

bool workspaces_per_output(void);
struct workspace *workspaces_current_on(struct output *output);
bool workspaces_view_on_current(struct view *view);
void workspaces_switch_output(struct output *output, struct workspace *target,
	bool update_focus);
void workspaces_output_enter(struct output *output);
void workspaces_output_leave(struct output *output);
void workspaces_track_cursor(void);
void workspaces_view_output_changed(struct view *view);

#endif /* LABWC_WORKSPACES_H */
