// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <string.h>
#include <wlr/types/wlr_scene.h>
#include "common/macros.h"
#include "common/mem.h"
#include "common/scene-helpers.h"
#include "config/rcxml.h"
#include "group.h"
#include "labwc.h"
#include "node.h"
#include "protocols/singularity-tiling.h"
#include "scaled-buffer/scaled-font-buffer.h"
#include "theme.h"
#include "view.h"

#define DROP_ZONE_MIN 16
#define TAB_GAP 2
#define TAB_PADDING 8
#define TAB_MAX_WIDTH 200
#define TAB_LABEL_MIN_WIDTH 56

struct group_tab {
	struct view_group *group;
	struct view *view;
	struct wlr_scene_tree *tree;
	struct wlr_scene_rect *rect;
	struct scaled_font_buffer *text;
	char *label;
	int label_width;
	bool label_active;
};

static void group_layout(struct view_group *group);
static void bar_rebuild(struct view_group *group);
static void bar_arrange(struct view_group *group);
static void bar_create_tabs(struct view_group *group);
static bool tabs_match_members(struct view_group *group);

int
view_group_bar_height(void)
{
	return rc.theme ? MAX(rc.theme->titlebar_height, 16) : 24;
}

uint32_t
view_group_id(struct view_group *group)
{
	return group ? group->id : 0;
}

uint32_t
view_group_index(struct view *view)
{
	if (!view || !view->group) {
		return 0;
	}
	uint32_t index = 0;
	struct view *member;
	wl_list_for_each(member, &view->group->members, group_link) {
		if (member == view) {
			break;
		}
		index++;
	}
	return index;
}

static void
group_broadcast(struct view_group *group)
{
	struct view *view;
	wl_list_for_each(view, &group->members, group_link) {
		singularity_tiling_send_group_state(view);
	}
}

static int
member_count(struct view_group *group)
{
	return wl_list_length(&group->members);
}

static struct view *
first_member(struct view_group *group)
{
	if (wl_list_empty(&group->members)) {
		return NULL;
	}
	struct view *view = wl_container_of(group->members.next, view, group_link);
	return view;
}

struct view *
view_group_anchor(struct view_group *group)
{
	if (!group) {
		return NULL;
	}
	return group->spread ? first_member(group) : group->active;
}

bool
view_group_is_hidden(struct view *view)
{
	return view && view->group && !view->group->spread
		&& view->group->active != view;
}

static void
update_member_visibility(struct view_group *group)
{
	struct view *view;
	wl_list_for_each(view, &group->members, group_link) {
		view_update_visibility(view);
	}
}

static void
bar_clear_tabs(struct view_group *group)
{
	struct group_tab **tab;
	wl_array_for_each(tab, &group->tabs) {
		wlr_scene_node_destroy(&(*tab)->tree->node);
		free((*tab)->label);
		free(*tab);
	}
	group->tabs.size = 0;
}

static void
bar_destroy(struct view_group *group)
{
	bar_clear_tabs(group);
	if (group->tree) {
		wlr_scene_node_destroy(&group->tree->node);
		group->tree = NULL;
		group->background = NULL;
	}
	group->bar_parent = NULL;
}

static void
group_destroy(struct view_group *group)
{
	bar_destroy(group);
	wl_array_release(&group->tabs);
	free(group);
}

static void
seed_box(struct view_group *group, struct view *view)
{
	int bar = view_group_bar_height();
	group->box = (struct wlr_box){
		.x = view->current.x,
		.y = view->current.y - bar,
		.width = view->current.width,
		.height = view->current.height + bar,
	};
	group->box_valid = group->box.width > 0 && group->box.height > bar;
}

static struct wlr_box
member_box(struct view_group *group, struct view *view)
{
	int bar = view_group_bar_height();
	struct wlr_box inner = {
		.x = group->box.x,
		.y = group->box.y + bar,
		.width = group->box.width,
		.height = group->box.height - bar,
	};
	if (!group->spread) {
		return inner;
	}
	int count = member_count(group);
	int index = 0;
	struct view *member;
	wl_list_for_each(member, &group->members, group_link) {
		if (member == view) {
			break;
		}
		index++;
	}
	int each = inner.height / MAX(count, 1);
	int y = inner.y + index * each;
	int height = index == count - 1 ? inner.y + inner.height - y : each;
	return (struct wlr_box){ inner.x, y, inner.width, height };
}

static void
group_layout(struct view_group *group)
{
	if (group->box_valid) {
		int bar = view_group_bar_height();
		if (group->box.width > 0 && group->box.height > bar) {
			group->applying = true;
			struct view *view;
			wl_list_for_each(view, &group->members, group_link) {
				if (!group->spread && view != group->active) {
					continue;
				}
				view_move_resize(view, member_box(group, view));
			}
			group->applying = false;
		}
	}
	bar_rebuild(group);
}

static const float *
tab_background(struct group_tab *tab)
{
	struct theme *theme = rc.theme;
	bool active = tab->group->active == tab->view;
	return theme->window[active].title_bg.color;
}

static const float *
tab_text_color(struct group_tab *tab)
{
	struct theme *theme = rc.theme;
	bool active = tab->group->active == tab->view;
	return theme->window[active].label_text_color;
}

static void
bar_rebuild(struct view_group *group)
{
	struct view *anchor = view_group_anchor(group);
	if (!anchor || !anchor->scene_tree || member_count(group) < 2) {
		bar_destroy(group);
		return;
	}

	if (!group->tree) {
		group->tree = lab_wlr_scene_tree_create(anchor->scene_tree);
		node_descriptor_create(&group->tree->node, LAB_NODE_GROUP_BAR,
			anchor, group);
		group->background = lab_wlr_scene_rect_create(group->tree, 1, 1,
			rc.theme->window[0].title_bg.color);
		group->bar_parent = anchor;
	} else if (group->bar_parent != anchor) {
		wlr_scene_node_reparent(&group->tree->node, anchor->scene_tree);
		group->bar_parent = anchor;
	}
	wlr_scene_node_raise_to_top(&group->tree->node);

	if (!tabs_match_members(group)) {
		bar_clear_tabs(group);
		bar_create_tabs(group);
	}
	bar_arrange(group);
}

static bool
tabs_match_members(struct view_group *group)
{
	size_t index = 0;
	size_t count = group->tabs.size / sizeof(struct group_tab *);
	struct group_tab **slot = group->tabs.data;
	struct view *view;
	wl_list_for_each(view, &group->members, group_link) {
		if (index >= count || slot[index]->view != view) {
			return false;
		}
		index++;
	}
	return index == count;
}

static void
bar_create_tabs(struct view_group *group)
{
	struct view *view;
	wl_list_for_each(view, &group->members, group_link) {
		struct group_tab *tab = znew(*tab);
		tab->group = group;
		tab->view = view;
		tab->tree = lab_wlr_scene_tree_create(group->tree);
		node_descriptor_create(&tab->tree->node, LAB_NODE_GROUP_TAB,
			view, tab);
		tab->rect = lab_wlr_scene_rect_create(tab->tree, 1, 1,
			tab_background(tab));
		tab->text = scaled_font_buffer_create(tab->tree);
		struct group_tab **slot = wl_array_add(&group->tabs, sizeof(*slot));
		*slot = tab;
	}
}

static void
bar_arrange(struct view_group *group)
{
	if (!group->tree || !group->background) {
		return;
	}
	int bar = view_group_bar_height();
	int width = MAX(group->box.width, 1);
	int count = group->tabs.size / sizeof(struct group_tab *);
	if (count < 1) {
		return;
	}

	wlr_scene_node_set_position(&group->tree->node, 0, -bar);
	wlr_scene_rect_set_size(group->background, width, bar);

	int available = width - (count - 1) * TAB_GAP;
	int tab_width = MAX(available / count, 1);
	tab_width = MIN(tab_width, TAB_MAX_WIDTH);
	int x = 0;
	struct group_tab **slot;
	wl_array_for_each(slot, &group->tabs) {
		struct group_tab *tab = *slot;
		wlr_scene_node_set_position(&tab->tree->node, x, 0);
		wlr_scene_rect_set_size(tab->rect, tab_width, bar);
		wlr_scene_rect_set_color(tab->rect, tab_background(tab));

		int label_width = tab_width - 2 * TAB_PADDING;
		const char *title = tab_width >= TAB_LABEL_MIN_WIDTH
			&& tab->view->title ? tab->view->title : "";
		bool active = tab->group->active == tab->view;
		if (tab->label_width != label_width || tab->label_active != active
				|| !tab->label || strcmp(tab->label, title)) {
			scaled_font_buffer_update(tab->text, title,
				MAX(label_width, 1), &rc.font_activewindow,
				tab_text_color(tab), tab_background(tab));
			free(tab->label);
			tab->label = xstrdup(title);
			tab->label_width = label_width;
			tab->label_active = active;
		}
		wlr_scene_node_set_position(&tab->text->scene_buffer->node,
			x + TAB_PADDING, MAX((bar - tab->text->height) / 2, 0));
		wlr_scene_node_set_enabled(&tab->text->scene_buffer->node,
			label_width > 0);
		x += tab_width + TAB_GAP;
	}
}

static struct view_group *
group_create(struct view *view)
{
	static uint32_t next_id;
	struct view_group *group = znew(*group);
	group->id = ++next_id;
	wl_list_init(&group->members);
	wl_array_init(&group->tabs);
	view->group = group;
	wl_list_insert(group->members.prev, &view->group_link);
	group->active = view;
	seed_box(group, view);
	return group;
}

void
view_group_join(struct view *view, struct view *target)
{
	if (!view || !target || view == target || !view->mapped) {
		return;
	}
	if (view->group && view->group == target->group) {
		return;
	}
	view_group_leave(view);

	struct view_group *group = target->group;
	if (!group) {
		group = group_create(target);
	}
	view->group = group;
	wl_list_insert(group->members.prev, &view->group_link);
	group->active = view;

	update_member_visibility(group);
	group_layout(group);
	group_broadcast(group);
	desktop_focus_view(view, /* raise */ true);
}

void
view_group_leave(struct view *view)
{
	if (!view || !view->group) {
		return;
	}
	struct view_group *group = view->group;
	wl_list_remove(&view->group_link);
	view->group = NULL;
	view_update_visibility(view);
	singularity_tiling_send_group_state(view);

	if (group->active == view) {
		group->active = first_member(group);
	}

	if (member_count(group) < 2) {
		struct view *last = first_member(group);
		if (last) {
			wl_list_remove(&last->group_link);
			last->group = NULL;
			view_update_visibility(last);
			if (group->box_valid) {
				view_move_resize(last, group->box);
			}
			singularity_tiling_send_group_state(last);
		}
		group_destroy(group);
		return;
	}

	update_member_visibility(group);
	group_layout(group);
	group_broadcast(group);
}

void
view_group_activate(struct view *view)
{
	if (!view || !view->group) {
		return;
	}
	struct view_group *group = view->group;
	if (group->active == view) {
		desktop_focus_view(view, /* raise */ true);
		return;
	}
	group->active = view;
	update_member_visibility(group);
	group_layout(group);
	group_broadcast(group);
	desktop_focus_view(view, /* raise */ true);
}

void
view_group_cycle(struct view *view, int direction)
{
	if (!view || !view->group || member_count(view->group) < 2) {
		return;
	}
	struct view_group *group = view->group;
	struct view *current = group->active ? group->active : first_member(group);
	struct wl_list *link = direction < 0
		? current->group_link.prev : current->group_link.next;
	if (link == &group->members) {
		link = direction < 0 ? group->members.prev : group->members.next;
	}
	struct view *next = wl_container_of(link, next, group_link);
	view_group_activate(next);
}

void
view_group_toggle_spread(struct view *view)
{
	if (!view || !view->group) {
		return;
	}
	struct view_group *group = view->group;
	group->spread = !group->spread;
	update_member_visibility(group);
	group_layout(group);
	group_broadcast(group);
}

bool
view_group_apply_box(struct view *view, struct wlr_box box)
{
	if (!view || !view->group) {
		return false;
	}
	struct view_group *group = view->group;
	group->box = box;
	group->box_valid = true;
	group_layout(group);
	return true;
}

void
view_group_notify_geometry(struct view *view)
{
	if (!view || !view->group) {
		return;
	}
	struct view_group *group = view->group;
	if (group->applying || view->fullscreen
			|| view->maximized != VIEW_AXIS_NONE) {
		return;
	}
	if (!group->box_valid) {
		seed_box(group, view);
		group_layout(group);
		return;
	}
	if (view != view_group_anchor(group)) {
		return;
	}
	struct wlr_box want = member_box(group, view);
	group->box.x += view->current.x - want.x;
	group->box.y += view->current.y - want.y;
	group->box.width = view->current.width;
	if (!group->spread) {
		group->box.height = view->current.height + view_group_bar_height();
	}
	group_layout(group);
}

void
view_group_notify_minimized(struct view *view)
{
	if (!view || !view->group || !view->minimized) {
		return;
	}
	struct view_group *group = view->group;
	if (group->active != view) {
		return;
	}
	struct view *member;
	wl_list_for_each(member, &group->members, group_link) {
		if (member != view && !member->minimized) {
			view_group_activate(member);
			return;
		}
	}
}

void
view_group_notify_title(struct view *view)
{
	if (view && view->group) {
		bar_arrange(view->group);
	}
}

void
view_group_notify_destroy(struct view *view)
{
	view_group_leave(view);
}

struct view *
view_group_bar_view(struct wlr_scene_node *node)
{
	struct node_descriptor *desc = node ? node->data : NULL;
	if (!desc || desc->type != LAB_NODE_GROUP_BAR) {
		return NULL;
	}
	return view_group_anchor(desc->data);
}

struct view *
view_group_drop_target(struct view *dragged, double x, double y)
{
	if (!dragged) {
		return NULL;
	}
	struct view *view;
	for_each_view(view, &server.views,
			LAB_VIEW_CRITERIA_CURRENT_WORKSPACE) {
		if (view == dragged || view->minimized
				|| view_group_is_hidden(view)) {
			continue;
		}
		struct wlr_box box = view->current;
		if (box.width < 4 * DROP_ZONE_MIN || box.height < 4 * DROP_ZONE_MIN) {
			continue;
		}
		if (!wlr_box_contains_point(&box, x, y)) {
			continue;
		}
		struct wlr_box zone = {
			.x = box.x + box.width / 4,
			.y = box.y + box.height / 4,
			.width = box.width / 2,
			.height = box.height / 2,
		};
		if (!wlr_box_contains_point(&zone, x, y)) {
			return NULL;
		}
		if (dragged->group && dragged->group == view->group) {
			return NULL;
		}
		return view;
	}
	return NULL;
}

struct view *
view_group_pick_partner(struct view *view)
{
	if (!view) {
		return NULL;
	}
	struct view *candidate;
	for_each_view(candidate, &server.views,
			LAB_VIEW_CRITERIA_CURRENT_WORKSPACE) {
		if (candidate == view || candidate->minimized
				|| view_group_is_hidden(candidate)) {
			continue;
		}
		return candidate;
	}
	return NULL;
}

void
view_group_activate_from_node(struct wlr_scene_node *node)
{
	struct node_descriptor *desc = node ? node->data : NULL;
	if (!desc || desc->type != LAB_NODE_GROUP_TAB) {
		return;
	}
	struct group_tab *tab = desc->data;
	if (tab) {
		view_group_activate(tab->view);
	}
}
