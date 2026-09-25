// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L
#include "workspaces.h"
#include <assert.h>
#include <cairo.h>
#include <pango/pangocairo.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wlr/types/wlr_ext_workspace_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
#include "buffer.h"
#include "common/font.h"
#include "common/graphic-helpers.h"
#include "common/list.h"
#include "common/mem.h"
#include "common/scene-helpers.h"
#include "config/rcxml.h"
#include "input/keyboard.h"
#include "foreign-toplevel/foreign.h"
#include "labwc.h"
#include "output.h"
#include "show-desktop.h"
#include "ssd.h"
#include "theme.h"
#include "view.h"

#define EXT_WORKSPACES_VERSION 1
#define WORKSPACE_SWIPE_INTERVAL_MS 16
#define WORKSPACE_SWIPE_FRAMES 12

struct workspace_output_handle {
	struct wl_list link;
	struct workspace *workspace;
	struct output *output;
	struct wlr_ext_workspace_handle_v1 *ext_workspace;
};

static struct {
	bool active;
	struct workspace *from;
	struct workspace *to;
	struct output *output;
	struct wlr_box output_box;
	enum direction direction;
	int width;
	double position;
	double settle_from;
	double settle_to;
	int frame;
	bool commit;
	struct wl_event_source *timer;
} workspace_swipe;

static bool per_output;

/* Internal helpers */
static size_t
parse_workspace_index(const char *name)
{
	/*
	 * We only want to get positive numbers which span the whole string.
	 *
	 * More detailed requirement:
	 *  .---------------.--------------.
	 *  |     Input     | Return value |
	 *  |---------------+--------------|
	 *  | "2nd desktop" |      0       |
	 *  |    "-50"      |      0       |
	 *  |     "0"       |      0       |
	 *  |    "124"      |     124      |
	 *  |    "1.24"     |      0       |
	 *  `------------------------------´
	 *
	 * As atoi() happily parses any numbers until it hits a non-number we
	 * can't really use it for this case. Instead, we use strtol() combined
	 * with further checks for the endptr (remaining non-number characters)
	 * and returned negative numbers.
	 */
	long index;
	char *endptr;
	errno = 0;
	index = strtol(name, &endptr, 10);
	if (errno || *endptr != '\0' || index < 0) {
		return 0;
	}
	return index;
}

static void
_osd_update(void)
{
	struct theme *theme = rc.theme;

	/* Settings */
	uint16_t margin = 10;
	uint16_t padding = 2;
	uint16_t rect_height = theme->osd_workspace_switcher_boxes_height;
	uint16_t rect_width = theme->osd_workspace_switcher_boxes_width;
	bool hide_boxes = theme->osd_workspace_switcher_boxes_width == 0 ||
		theme->osd_workspace_switcher_boxes_height == 0;

	/* Dimensions */
	size_t workspace_count = wl_list_length(&server.workspaces.all);
	uint16_t marker_width = workspace_count * (rect_width + padding) - padding;
	uint16_t width = margin * 2 + (marker_width < 200 ? 200 : marker_width);
	uint16_t height = margin * (hide_boxes ? 2 : 3) + rect_height + font_height(&rc.font_osd);

	cairo_t *cairo;
	cairo_surface_t *surface;
	struct workspace *workspace;

	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (!output_is_usable(output)) {
			continue;
		}
		struct lab_data_buffer *buffer = buffer_create_cairo(width, height,
			output->wlr_output->scale);
		if (!buffer) {
			wlr_log(WLR_ERROR, "Failed to allocate buffer for workspace OSD");
			continue;
		}

		cairo = cairo_create(buffer->surface);

		/* Background */
		set_cairo_color(cairo, theme->osd_bg_color);
		cairo_rectangle(cairo, 0, 0, width, height);
		cairo_fill(cairo);

		/* Border */
		set_cairo_color(cairo, theme->osd_border_color);
		struct wlr_fbox border_fbox = {
			.width = width,
			.height = height,
		};
		draw_cairo_border(cairo, border_fbox, theme->osd_border_width);

		/* Boxes */
		uint16_t x;
		if (!hide_boxes) {
			x = (width - marker_width) / 2;
			wl_list_for_each(workspace, &server.workspaces.all, link) {
				bool active = workspace == workspaces_current_on(output);
				set_cairo_color(cairo, rc.theme->osd_label_text_color);
				struct wlr_fbox fbox = {
					.x = x,
					.y = margin,
					.width = rect_width,
					.height = rect_height,
				};
				draw_cairo_border(cairo, fbox,
					theme->osd_workspace_switcher_boxes_border_width);
				if (active) {
					cairo_rectangle(cairo, x, margin,
						rect_width, rect_height);
					cairo_fill(cairo);
				}
				x += rect_width + padding;
			}
		}

		/* Text */
		set_cairo_color(cairo, rc.theme->osd_label_text_color);
		PangoLayout *layout = pango_cairo_create_layout(cairo);
		pango_context_set_round_glyph_positions(pango_layout_get_context(layout), false);
		pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

		/* Center workspace indicator on the x axis */
		const char *current_name = workspaces_current_on(output)->name;
		int req_width = font_width(&rc.font_osd, current_name);
		req_width = MIN(req_width, width - 2 * margin);
		x = (width - req_width) / 2;
		if (!hide_boxes) {
			cairo_move_to(cairo, x, margin * 2 + rect_height);
		} else {
			cairo_move_to(cairo, x, (height - font_height(&rc.font_osd)) / 2.0);
		}
		PangoFontDescription *desc = font_to_pango_desc(&rc.font_osd);
		//pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
		pango_layout_set_font_description(layout, desc);
		pango_layout_set_width(layout, req_width * PANGO_SCALE);
		pango_font_description_free(desc);
		pango_layout_set_text(layout, current_name, -1);
		pango_cairo_show_layout(cairo, layout);

		g_object_unref(layout);
		surface = cairo_get_target(cairo);
		cairo_surface_flush(surface);
		cairo_destroy(cairo);

		if (!output->workspace_osd) {
			output->workspace_osd = lab_wlr_scene_buffer_create(
				&server.scene->tree, NULL);
		}
		/* Position the whole thing */
		struct wlr_box output_box;
		wlr_output_layout_get_box(server.output_layout,
			output->wlr_output, &output_box);
		int lx = output_box.x + (output_box.width - width) / 2;
		int ly = output_box.y + (output_box.height - height) / 2;
		wlr_scene_node_set_position(&output->workspace_osd->node, lx, ly);
		wlr_scene_buffer_set_buffer(output->workspace_osd, &buffer->base);
		wlr_scene_buffer_set_dest_size(output->workspace_osd,
			buffer->logical_width, buffer->logical_height);

		/* And finally drop the buffer so it will get destroyed on OSD hide */
		wlr_buffer_drop(&buffer->base);
	}
}

static struct workspace *
workspace_find_by_name(const char *name)
{
	struct workspace *workspace;

	/* by index */
	size_t parsed_index = parse_workspace_index(name);
	if (parsed_index) {
		size_t index = 0;
		wl_list_for_each(workspace, &server.workspaces.all, link) {
			if (parsed_index == ++index) {
				return workspace;
			}
		}
	}

	/* by name */
	wl_list_for_each(workspace, &server.workspaces.all, link) {
		if (!strcmp(workspace->name, name)) {
			return workspace;
		}
	}

	wlr_log(WLR_ERROR, "Workspace '%s' not found", name);
	return NULL;
}

static void
handle_ext_workspace_commit(struct wl_listener *listener, void *data)
{
	struct wlr_ext_workspace_v1_commit_event *event = data;

	struct wlr_ext_workspace_v1_request *req;
	wl_list_for_each(req, event->requests, link) {
		if (req->type != WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE) {
			continue;
		}
		if (per_output) {
			struct workspace_output_handle *handle =
				req->activate.workspace->data;
			workspaces_switch_output(handle->output,
				handle->workspace, /* update_focus */ true);
			wlr_log(WLR_INFO, "activating workspace %s on %s",
				handle->workspace->name,
				handle->output->wlr_output->name);
			continue;
		}
		struct workspace *workspace = req->activate.workspace->data;
		workspaces_switch_to(workspace, /* update_focus */ true);
		wlr_log(WLR_INFO, "activating workspace %s", workspace->name);
	}
}

static void
ext_global_add_workspace(struct workspace *workspace)
{
	workspace->ext_workspace = wlr_ext_workspace_handle_v1_create(
		server.workspaces.ext_manager, /*id*/ NULL,
		EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE);
	workspace->ext_workspace->data = workspace;
	wlr_ext_workspace_handle_v1_set_group(
		workspace->ext_workspace, server.workspaces.ext_group);
	wlr_ext_workspace_handle_v1_set_name(workspace->ext_workspace,
		workspace->name);
	wlr_ext_workspace_handle_v1_set_active(workspace->ext_workspace,
		workspace == server.workspaces.current);
}

static void
ext_output_add_workspace(struct output *output, struct workspace *workspace)
{
	struct workspace_output_handle *handle = znew(*handle);
	handle->workspace = workspace;
	handle->output = output;
	handle->ext_workspace = wlr_ext_workspace_handle_v1_create(
		server.workspaces.ext_manager, /*id*/ NULL,
		EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE);
	handle->ext_workspace->data = handle;
	wlr_ext_workspace_handle_v1_set_group(handle->ext_workspace,
		output->workspace_group);
	wlr_ext_workspace_handle_v1_set_name(handle->ext_workspace,
		workspace->name);
	wlr_ext_workspace_handle_v1_set_active(handle->ext_workspace,
		workspace == output->workspace_current);
	wl_list_append(&workspace->output_handles, &handle->link);
}

static void
ext_output_handle_destroy(struct workspace_output_handle *handle)
{
	wlr_ext_workspace_handle_v1_destroy(handle->ext_workspace);
	wl_list_remove(&handle->link);
	free(handle);
}

static void
ext_output_create(struct output *output)
{
	if (output->workspace_group) {
		return;
	}
	output->workspace_group = wlr_ext_workspace_group_handle_v1_create(
		server.workspaces.ext_manager, /*caps*/ 0);
	wlr_ext_workspace_group_handle_v1_output_enter(
		output->workspace_group, output->wlr_output);
	struct workspace *workspace;
	wl_list_for_each(workspace, &server.workspaces.all, link) {
		ext_output_add_workspace(output, workspace);
	}
}

static void
ext_output_destroy(struct output *output)
{
	if (!output->workspace_group) {
		return;
	}
	struct workspace *workspace;
	wl_list_for_each(workspace, &server.workspaces.all, link) {
		struct workspace_output_handle *handle, *tmp;
		wl_list_for_each_safe(handle, tmp, &workspace->output_handles, link) {
			if (handle->output == output) {
				ext_output_handle_destroy(handle);
			}
		}
	}
	wlr_ext_workspace_group_handle_v1_destroy(output->workspace_group);
	output->workspace_group = NULL;
}

static void
ext_global_create(void)
{
	server.workspaces.ext_group = wlr_ext_workspace_group_handle_v1_create(
		server.workspaces.ext_manager, /*caps*/ 0);
	struct workspace *workspace;
	wl_list_for_each(workspace, &server.workspaces.all, link) {
		ext_global_add_workspace(workspace);
	}
	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (output_is_usable(output)) {
			wlr_ext_workspace_group_handle_v1_output_enter(
				server.workspaces.ext_group, output->wlr_output);
		}
	}
}

static void
ext_global_destroy(void)
{
	struct workspace *workspace;
	wl_list_for_each(workspace, &server.workspaces.all, link) {
		if (workspace->ext_workspace) {
			wlr_ext_workspace_handle_v1_destroy(workspace->ext_workspace);
			workspace->ext_workspace = NULL;
		}
	}
	if (server.workspaces.ext_group) {
		wlr_ext_workspace_group_handle_v1_destroy(server.workspaces.ext_group);
		server.workspaces.ext_group = NULL;
	}
}

static void
ext_output_set_active(struct output *output, struct workspace *workspace,
		bool active)
{
	struct workspace_output_handle *handle;
	wl_list_for_each(handle, &workspace->output_handles, link) {
		if (handle->output == output) {
			wlr_ext_workspace_handle_v1_set_active(
				handle->ext_workspace, active);
		}
	}
}

static struct output *
active_output(void)
{
	struct output *output = output_nearest_to_cursor();
	if (output_is_usable(output) && output->workspace_current) {
		return output;
	}
	wl_list_for_each(output, &server.outputs, link) {
		if (output_is_usable(output) && output->workspace_current) {
			return output;
		}
	}
	return NULL;
}

/* Internal API */
static void
add_workspace(const char *name)
{
	struct workspace *workspace = znew(*workspace);
	workspace->name = xstrdup(name);
	workspace->tree = lab_wlr_scene_tree_create(server.workspace_tree);
	workspace->view_trees[VIEW_LAYER_ALWAYS_ON_BOTTOM] =
		lab_wlr_scene_tree_create(workspace->tree);
	workspace->view_trees[VIEW_LAYER_NORMAL] =
		lab_wlr_scene_tree_create(workspace->tree);
	workspace->view_trees[VIEW_LAYER_ALWAYS_ON_TOP] =
		lab_wlr_scene_tree_create(workspace->tree);
	wl_list_append(&server.workspaces.all, &workspace->link);
	wl_list_init(&workspace->output_handles);
	wlr_scene_node_set_enabled(&workspace->tree->node, per_output);

	if (!per_output) {
		ext_global_add_workspace(workspace);
		return;
	}
	if (!server.outputs.next) {
		return;
	}
	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (output->workspace_group) {
			ext_output_add_workspace(output, workspace);
		}
	}
}

static struct workspace *
get_prev(struct workspace *current, struct wl_list *workspaces, bool wrap)
{
	struct wl_list *target_link = current->link.prev;
	if (target_link == workspaces) {
		/* Current workspace is the first one */
		if (!wrap) {
			return NULL;
		}
		/* Roll over */
		target_link = target_link->prev;
	}
	return wl_container_of(target_link, current, link);
}

static struct workspace *
get_next(struct workspace *current, struct wl_list *workspaces, bool wrap)
{
	struct wl_list *target_link = current->link.next;
	if (target_link == workspaces) {
		/* Current workspace is the last one */
		if (!wrap) {
			return NULL;
		}
		/* Roll over */
		target_link = target_link->next;
	}
	return wl_container_of(target_link, current, link);
}

static bool
workspace_has_views(struct workspace *workspace)
{
	struct view *view;

	for_each_view(view, &server.views, LAB_VIEW_CRITERIA_NO_OMNIPRESENT) {
		if (view->workspace == workspace) {
			return true;
		}
	}
	return false;
}

static struct workspace *
get_adjacent_occupied(struct workspace *current, struct wl_list *workspaces,
		bool wrap, bool reverse)
{
	struct wl_list *start = &current->link;
	struct wl_list *link = reverse ? start->prev : start->next;
	bool has_wrapped = false;

	while (true) {
		/* Handle list boundaries */
		if (link == workspaces) {
			if (!wrap) {
				break;  /* No wrapping allowed - stop searching */
			}
			if (has_wrapped) {
				break;  /* Already wrapped once - stop to prevent infinite loop */
			}
			/* Wrap around */
			link = reverse ? workspaces->prev : workspaces->next;
			has_wrapped = true;
			continue;
		}

		/* Get the workspace */
		struct workspace *target = wl_container_of(link, target, link);

		/* Check if we've come full circle */
		if (link == start) {
			break;
		}

		/* Check if it's occupied (and not current) */
		if (target != current && workspace_has_views(target)) {
			return target;
		}

		/* Move to next/prev */
		link = reverse ? link->prev : link->next;
	}

	return NULL;  /* No occupied workspace found */
}

static struct workspace *
get_prev_occupied(struct workspace *current, struct wl_list *workspaces, bool wrap)
{
	return get_adjacent_occupied(current, workspaces, wrap, true);
}

static struct workspace *
get_next_occupied(struct workspace *current, struct wl_list *workspaces, bool wrap)
{
	return get_adjacent_occupied(current, workspaces, wrap, false);
}

static int
_osd_handle_timeout(void *data)
{
	struct seat *seat = data;
	workspaces_osd_hide(seat);
	/* Don't re-check */
	return 0;
}

static void
_osd_show(struct output *only)
{
	if (!rc.workspace_config.popuptime) {
		return;
	}

	_osd_update();
	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (only && output != only) {
			continue;
		}
		if (output_is_usable(output) && output->workspace_osd) {
			wlr_scene_node_set_enabled(&output->workspace_osd->node, true);
		}
	}
	if (keyboard_get_all_modifiers(&server.seat)) {
		/* Hidden by release of all modifiers */
		server.seat.workspace_osd_shown_by_modifier = true;
	} else {
		/* Hidden by timer */
		if (!server.seat.workspace_osd_timer) {
			server.seat.workspace_osd_timer = wl_event_loop_add_timer(
				server.wl_event_loop, _osd_handle_timeout, &server.seat);
		}
		wl_event_source_timer_update(server.seat.workspace_osd_timer,
			rc.workspace_config.popuptime);
	}
}

/* Public API */
void
workspaces_init(void)
{
	server.workspaces.ext_manager = wlr_ext_workspace_manager_v1_create(
		server.wl_display, EXT_WORKSPACES_VERSION);

	per_output = rc.workspace_config.per_output;
	if (!per_output) {
		server.workspaces.ext_group = wlr_ext_workspace_group_handle_v1_create(
			server.workspaces.ext_manager, /*caps*/ 0);
	}

	server.workspaces.on_ext_manager.commit.notify = handle_ext_workspace_commit;
	wl_signal_add(&server.workspaces.ext_manager->events.commit,
		&server.workspaces.on_ext_manager.commit);

	wl_list_init(&server.workspaces.all);

	struct workspace_config *conf;
	wl_list_for_each(conf, &rc.workspace_config.workspaces, link) {
		add_workspace(conf->name);
	}

	/*
	 * After adding workspaces, check if there is an initial workspace
	 * selected and set that as the initial workspace.
	 */
	char *initial_name = rc.workspace_config.initial_workspace_name;
	struct workspace *initial = NULL;
	struct workspace *first = wl_container_of(
		server.workspaces.all.next, first, link);

	if (initial_name) {
		initial = workspace_find_by_name(initial_name);
	}
	if (!initial) {
		initial = first;
	}

	server.workspaces.current = initial;
	wlr_scene_node_set_enabled(&initial->tree->node, true);
	if (initial->ext_workspace) {
		wlr_ext_workspace_handle_v1_set_active(initial->ext_workspace, true);
	}
}

/*
 * update_focus should normally be set to true. It is set to false only
 * when this function is called from desktop_focus_view(), in order to
 * avoid unnecessary extra focus changes and possible recursion.
 */
void
workspaces_switch_to(struct workspace *target, bool update_focus)
{
	assert(target);
	if (per_output) {
		struct output *output = active_output();
		if (output) {
			workspaces_switch_output(output, target, update_focus);
		} else {
			server.workspaces.current = target;
		}
		return;
	}
	if (target == server.workspaces.current) {
		return;
	}

	/* Disable the old workspace */
	wlr_scene_node_set_enabled(
		&server.workspaces.current->tree->node, false);

	wlr_ext_workspace_handle_v1_set_active(
		server.workspaces.current->ext_workspace, false);

	/*
	 * Move Omnipresent views to new workspace.
	 * Not using for_each_view() since it skips views that
	 * view_is_focusable() returns false (e.g. Conky).
	 */
	struct view *view;
	wl_list_for_each_reverse(view, &server.views, link) {
		if (view->visible_on_all_workspaces) {
			view_move_to_workspace(view, target);
		}
	}

	/* Enable the new workspace */
	wlr_scene_node_set_enabled(&target->tree->node, true);

	/* Save the last visited workspace */
	server.workspaces.last = server.workspaces.current;

	/* Make sure new views will spawn on the new workspace */
	server.workspaces.current = target;

	struct view *grabbed_view = server.grabbed_view;
	if (grabbed_view) {
		view_move_to_workspace(grabbed_view, target);
	}

	/*
	 * Make sure we are focusing what the user sees. Only refocus if
	 * the focus is not already on an omnipresent view.
	 */
	if (update_focus) {
		struct view *active_view = server.active_view;
		if (!(active_view && active_view->visible_on_all_workspaces)) {
			desktop_focus_topmost_view();
		}
	}

	/* And finally show the OSD */
	_osd_show(NULL);

	/*
	 * Make sure we are not carrying around a
	 * cursor image from the previous desktop
	 */
	cursor_update_focus();

	/* Ensure that only currently visible fullscreen windows hide the top layer */
	desktop_update_top_layer_visibility();

	wlr_ext_workspace_handle_v1_set_active(target->ext_workspace, true);

	show_desktop_reset();
}

void
workspaces_switch_output(struct output *output, struct workspace *target,
		bool update_focus)
{
	assert(output);
	assert(target);
	if (!per_output) {
		workspaces_switch_to(target, update_focus);
		return;
	}
	struct workspace *from = output->workspace_current;
	if (target == from) {
		return;
	}
	output->workspace_last = from;
	output->workspace_current = target;
	if (from) {
		ext_output_set_active(output, from, false);
	}
	ext_output_set_active(output, target, true);

	struct view *view;
	wl_list_for_each_reverse(view, &server.views, link) {
		if (view->output != output) {
			continue;
		}
		if (view->visible_on_all_workspaces || view == server.grabbed_view) {
			view_move_to_workspace(view, target);
		}
		view_update_visibility(view);
	}

	if (output == active_output()) {
		server.workspaces.last = from;
		server.workspaces.current = target;
	}

	if (update_focus) {
		struct view *active_view = server.active_view;
		bool keep = active_view && (active_view->visible_on_all_workspaces
			|| (active_view->output != output
				&& workspaces_view_on_current(active_view)));
		if (!keep) {
			struct view *topmost = NULL;
			for_each_view(view, &server.views,
					LAB_VIEW_CRITERIA_CURRENT_WORKSPACE) {
				if (view->output == output && !view->minimized) {
					topmost = view;
					break;
				}
			}
			if (topmost) {
				desktop_focus_view(topmost, /*raise*/ true);
			} else {
				desktop_focus_topmost_view();
			}
		}
	}

	_osd_show(output);
	cursor_update_focus();
	desktop_update_top_layer_visibility();
	show_desktop_reset();
}

bool
workspaces_per_output(void)
{
	return per_output;
}

struct workspace *
workspaces_current_on(struct output *output)
{
	if (per_output && output && output->workspace_current) {
		return output->workspace_current;
	}
	return server.workspaces.current;
}

bool
workspaces_view_on_current(struct view *view)
{
	return view->workspace == workspaces_current_on(view->output);
}

void
workspaces_output_enter(struct output *output)
{
	if (!per_output) {
		if (server.workspaces.ext_group) {
			wlr_ext_workspace_group_handle_v1_output_enter(
				server.workspaces.ext_group, output->wlr_output);
		}
		return;
	}
	if (!output->workspace_current) {
		output->workspace_current = server.workspaces.current;
	}
	ext_output_create(output);
}

void
workspaces_output_leave(struct output *output)
{
	if (!per_output) {
		if (server.workspaces.ext_group) {
			wlr_ext_workspace_group_handle_v1_output_leave(
				server.workspaces.ext_group, output->wlr_output);
		}
		return;
	}
	ext_output_destroy(output);
	if (workspace_swipe.output == output) {
		workspace_swipe.output = NULL;
	}
}

void
workspaces_track_cursor(void)
{
	if (!per_output) {
		return;
	}
	struct output *output = output_nearest_to_cursor();
	if (!output_is_usable(output) || !output->workspace_current
			|| output->workspace_current == server.workspaces.current) {
		return;
	}
	server.workspaces.current = output->workspace_current;
	server.workspaces.last = output->workspace_last
		? output->workspace_last : output->workspace_current;
}

void
workspaces_view_output_changed(struct view *view)
{
	if (!per_output || !view->output || !view->output->workspace_current
			|| !view->workspace || !view->scene_tree
			|| workspace_swipe.active) {
		return;
	}
	if (view->workspace != view->output->workspace_current) {
		view_move_to_workspace(view, view->output->workspace_current);
	}
}

static bool
swipe_view_slides(struct view *view)
{
	return view->output == workspace_swipe.output && view->mapped
		&& !view->minimized && !view->visible_on_all_workspaces
		&& (view->workspace == workspace_swipe.from
			|| view->workspace == workspace_swipe.to);
}

static void
swipe_view_clip(struct view *view, int offset)
{
	wlr_scene_node_set_position(&view->scene_tree->node,
		view->current.x + offset, view->current.y);
	if (!view->content_tree) {
		return;
	}
	struct wlr_box shifted = view->current;
	shifted.x += offset;
	struct wlr_box visible;
	if (!wlr_box_intersection(&visible, &shifted,
			&workspace_swipe.output_box)) {
		wlr_scene_node_set_enabled(&view->scene_tree->node, false);
		return;
	}
	wlr_scene_node_set_enabled(&view->scene_tree->node, true);
	struct wlr_box clip = {
		.x = visible.x - shifted.x,
		.y = visible.y - shifted.y,
		.width = visible.width,
		.height = visible.height,
	};
	wlr_scene_subsurface_tree_set_clip(&view->content_tree->node, &clip);
	bool whole = visible.width == shifted.width
		&& visible.height == shifted.height;
	if (view->ssd) {
		ssd_set_visible(view->ssd, whole);
	}
}

static void
swipe_view_restore(struct view *view)
{
	wlr_scene_node_set_position(&view->scene_tree->node,
		view->current.x, view->current.y);
	if (view->content_tree) {
		wlr_scene_subsurface_tree_set_clip(&view->content_tree->node, NULL);
	}
	if (view->ssd) {
		ssd_set_visible(view->ssd, true);
	}
	view_update_visibility(view);
}

static void
workspace_swipe_position(double position)
{
	workspace_swipe.position = position;
	int target_x = workspace_swipe.direction == LAB_DIRECTION_LEFT
		? workspace_swipe.width + (int)position
		: -workspace_swipe.width + (int)position;
	if (!workspace_swipe.output) {
		wlr_scene_node_set_position(&workspace_swipe.from->tree->node,
			(int)position, 0);
		wlr_scene_node_set_position(&workspace_swipe.to->tree->node,
			target_x, 0);
		return;
	}
	struct view *view;
	wl_list_for_each(view, &server.views, link) {
		if (swipe_view_slides(view)) {
			swipe_view_clip(view, view->workspace == workspace_swipe.from
				? (int)position : target_x);
		}
	}
}

static void
workspace_swipe_finish(void)
{
	if (workspace_swipe.timer) {
		wl_event_source_remove(workspace_swipe.timer);
		workspace_swipe.timer = NULL;
	}
	struct output *output = workspace_swipe.output;
	if (per_output) {
		workspace_swipe.active = false;
		struct view *view;
		wl_list_for_each(view, &server.views, link) {
			if (swipe_view_slides(view)) {
				swipe_view_restore(view);
			}
		}
		if (workspace_swipe.commit && output) {
			workspaces_switch_output(output, workspace_swipe.to, true);
		}
		workspace_swipe.from = NULL;
		workspace_swipe.to = NULL;
		workspace_swipe.output = NULL;
		return;
	}
	wlr_scene_node_set_position(&workspace_swipe.from->tree->node, 0, 0);
	wlr_scene_node_set_position(&workspace_swipe.to->tree->node, 0, 0);
	if (workspace_swipe.commit) {
		workspaces_switch_to(workspace_swipe.to, true);
	} else {
		wlr_scene_node_set_enabled(&workspace_swipe.to->tree->node, false);
	}
	workspace_swipe.active = false;
	workspace_swipe.from = NULL;
	workspace_swipe.to = NULL;
}

static double
ease_out_cubic(double progress)
{
	double remaining = 1.0 - progress;
	return 1.0 - remaining * remaining * remaining;
}

static int
workspace_swipe_settle(void *data)
{
	workspace_swipe.frame++;
	double progress = MIN(1.0,
		(double)workspace_swipe.frame / WORKSPACE_SWIPE_FRAMES);
	double eased = ease_out_cubic(progress);
	workspace_swipe_position(workspace_swipe.settle_from
		+ (workspace_swipe.settle_to - workspace_swipe.settle_from) * eased);
	if (workspace_swipe.frame >= WORKSPACE_SWIPE_FRAMES) {
		workspace_swipe_finish();
		return 0;
	}
	if (wl_event_source_timer_update(workspace_swipe.timer,
			WORKSPACE_SWIPE_INTERVAL_MS) < 0) {
		workspace_swipe_finish();
	}
	return 0;
}

bool
workspaces_swipe_begin(enum direction direction)
{
	if (direction != LAB_DIRECTION_LEFT && direction != LAB_DIRECTION_RIGHT) {
		return false;
	}
	if (workspace_swipe.active) {
		workspace_swipe_finish();
	}
	struct output *output = per_output ? active_output() : NULL;
	if (per_output && !output) {
		return false;
	}
	struct workspace *from = workspaces_current_on(output);
	struct workspace *target = direction == LAB_DIRECTION_LEFT
		? get_next(from, &server.workspaces.all, true)
		: get_prev(from, &server.workspaces.all, true);
	if (!target || target == from) {
		return false;
	}
	struct wlr_box layout_box;
	wlr_output_layout_get_box(server.output_layout,
		output ? output->wlr_output : NULL, &layout_box);
	if (layout_box.width < 1) {
		return false;
	}
	workspace_swipe.active = true;
	workspace_swipe.from = from;
	workspace_swipe.to = target;
	workspace_swipe.output = output;
	workspace_swipe.output_box = layout_box;
	workspace_swipe.direction = direction;
	workspace_swipe.width = layout_box.width;
	workspace_swipe.position = 0;
	workspace_swipe.timer = NULL;
	if (!output) {
		wlr_scene_node_set_enabled(&target->tree->node, true);
	}
	workspace_swipe_position(0);
	return true;
}

void
workspaces_swipe_update(double dx)
{
	if (!workspace_swipe.active || workspace_swipe.timer) {
		return;
	}
	double position = workspace_swipe.direction == LAB_DIRECTION_LEFT
		? MAX(-workspace_swipe.width, MIN(0, dx))
		: MIN(workspace_swipe.width, MAX(0, dx));
	workspace_swipe_position(position);
}

void
workspaces_swipe_end(bool commit)
{
	if (!workspace_swipe.active || workspace_swipe.timer) {
		return;
	}
	workspace_swipe.commit = commit;
	workspace_swipe.settle_from = workspace_swipe.position;
	workspace_swipe.settle_to = commit
		? (workspace_swipe.direction == LAB_DIRECTION_LEFT
			? -workspace_swipe.width : workspace_swipe.width)
		: 0;
	workspace_swipe.frame = 0;
	if (!rc.window_animations
			|| workspace_swipe.settle_from == workspace_swipe.settle_to) {
		workspace_swipe_position(workspace_swipe.settle_to);
		workspace_swipe_finish();
		return;
	}
	workspace_swipe.timer = wl_event_loop_add_timer(server.wl_event_loop,
		workspace_swipe_settle, NULL);
	if (!workspace_swipe.timer
			|| wl_event_source_timer_update(workspace_swipe.timer,
				WORKSPACE_SWIPE_INTERVAL_MS) < 0) {
		workspace_swipe_finish();
	}
}

void
workspaces_osd_hide(struct seat *seat)
{
	assert(seat);
	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (!output->workspace_osd) {
			continue;
		}
		wlr_scene_node_set_enabled(&output->workspace_osd->node, false);
		wlr_scene_buffer_set_buffer(output->workspace_osd, NULL);
	}
	seat->workspace_osd_shown_by_modifier = false;

	/* Update the cursor focus in case it was on top of the OSD before */
	cursor_update_focus();
}

struct workspace *
workspaces_find(struct workspace *anchor, const char *name, bool wrap)
{
	assert(anchor);
	if (!name) {
		return NULL;
	}
	struct wl_list *workspaces = &server.workspaces.all;

	if (!strcasecmp(name, "current")) {
		return anchor;
	} else if (!strcasecmp(name, "last")) {
		return server.workspaces.last;
	} else if (!strcasecmp(name, "left")) {
		return get_prev(anchor, workspaces, wrap);
	} else if (!strcasecmp(name, "right")) {
		return get_next(anchor, workspaces, wrap);
	} else if (!strcasecmp(name, "left-occupied")) {
		return get_prev_occupied(anchor, workspaces, wrap);
	} else if (!strcasecmp(name, "right-occupied")) {
		return get_next_occupied(anchor, workspaces, wrap);
	}
	return workspace_find_by_name(name);
}

static void
set_per_output(bool enable)
{
	if (enable == per_output) {
		return;
	}
	if (workspace_swipe.active) {
		workspace_swipe.commit = false;
		workspace_swipe_finish();
	}

	struct workspace *workspace;
	struct output *output;
	if (enable) {
		ext_global_destroy();
		per_output = true;
		wl_list_for_each(workspace, &server.workspaces.all, link) {
			wlr_scene_node_set_enabled(&workspace->tree->node, true);
		}
		wl_list_for_each(output, &server.outputs, link) {
			if (!output_is_usable(output)) {
				continue;
			}
			output->workspace_current = server.workspaces.current;
			output->workspace_last = server.workspaces.last;
			ext_output_create(output);
		}
	} else {
		wl_list_for_each(output, &server.outputs, link) {
			ext_output_destroy(output);
			output->workspace_current = NULL;
			output->workspace_last = NULL;
		}
		per_output = false;
		wl_list_for_each(workspace, &server.workspaces.all, link) {
			wlr_scene_node_set_enabled(&workspace->tree->node,
				workspace == server.workspaces.current);
		}
		ext_global_create();
	}

	struct view *view;
	wl_list_for_each(view, &server.views, link) {
		view_update_visibility(view);
	}
	desktop_focus_topmost_view();
	cursor_update_focus();
	desktop_update_top_layer_visibility();
	wlr_log(WLR_INFO, "per-output workspaces %s", enable ? "enabled" : "disabled");
}

static void
destroy_workspace(struct workspace *workspace)
{
	wlr_scene_node_destroy(&workspace->tree->node);
	zfree(workspace->name);
	wl_list_remove(&workspace->link);

	if (workspace->ext_workspace) {
		wlr_ext_workspace_handle_v1_destroy(workspace->ext_workspace);
	}
	struct workspace_output_handle *handle, *tmp;
	wl_list_for_each_safe(handle, tmp, &workspace->output_handles, link) {
		ext_output_handle_destroy(handle);
	}
	free(workspace);
}

void
workspaces_reconfigure(void)
{
	/*
	 * Compare actual workspace list with the new desired configuration to:
	 *   - Update names
	 *   - Add workspaces if more workspaces are desired
	 *   - Destroy workspaces if fewer workspace are desired
	 */

	struct wl_list *workspace_link = server.workspaces.all.next;

	struct workspace_config *conf;
	wl_list_for_each(conf, &rc.workspace_config.workspaces, link) {
		struct workspace *workspace = wl_container_of(
			workspace_link, workspace, link);

		if (workspace_link == &server.workspaces.all) {
			/* # of configured workspaces increased */
			wlr_log(WLR_DEBUG, "Adding workspace \"%s\"",
				conf->name);
			add_workspace(conf->name);
			continue;
		}
		if (strcmp(workspace->name, conf->name)) {
			/* Workspace is renamed */
			wlr_log(WLR_DEBUG, "Renaming workspace \"%s\" to \"%s\"",
				workspace->name, conf->name);
			xstrdup_replace(workspace->name, conf->name);
			if (workspace->ext_workspace) {
				wlr_ext_workspace_handle_v1_set_name(
					workspace->ext_workspace, workspace->name);
			}
			struct workspace_output_handle *handle;
			wl_list_for_each(handle, &workspace->output_handles, link) {
				wlr_ext_workspace_handle_v1_set_name(
					handle->ext_workspace, workspace->name);
			}
		}
		workspace_link = workspace_link->next;
	}

	if (workspace_link == &server.workspaces.all) {
		set_per_output(rc.workspace_config.per_output);
		return;
	}

	/* # of configured workspaces decreased */
	overlay_finish(&server.seat);
	struct workspace *first_workspace =
		wl_container_of(server.workspaces.all.next, first_workspace, link);

	while (workspace_link != &server.workspaces.all) {
		struct workspace *workspace = wl_container_of(
			workspace_link, workspace, link);

		wlr_log(WLR_DEBUG, "Destroying workspace \"%s\"",
			workspace->name);

		struct view *view;
		wl_list_for_each(view, &server.views, link) {
			if (view->workspace == workspace) {
				view_move_to_workspace(view, first_workspace);
			}
		}

		struct output *output;
		wl_list_for_each(output, &server.outputs, link) {
			if (output->workspace_current == workspace) {
				workspaces_switch_output(output, first_workspace,
					/* update_focus */ true);
			}
			if (output->workspace_last == workspace) {
				output->workspace_last = first_workspace;
			}
		}
		if (server.workspaces.current == workspace) {
			workspaces_switch_to(first_workspace,
				/* update_focus */ true);
		}
		if (server.workspaces.last == workspace) {
			server.workspaces.last = first_workspace;
		}

		workspace_link = workspace_link->next;
		destroy_workspace(workspace);
	}
	set_per_output(rc.workspace_config.per_output);
}

void
workspaces_destroy(void)
{
	struct workspace *workspace, *tmp;
	wl_list_for_each_safe(workspace, tmp, &server.workspaces.all, link) {
		destroy_workspace(workspace);
	}
	assert(wl_list_empty(&server.workspaces.all));
	wl_list_remove(&server.workspaces.on_ext_manager.commit.link);
}
