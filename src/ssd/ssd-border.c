// SPDX-License-Identifier: GPL-2.0-only

#include <assert.h>
#include <wlr/types/wlr_scene.h>
#include "buffer.h"
#include "common/graphic-helpers.h"
#include "common/macros.h"
#include "common/scene-helpers.h"
#include "config/rcxml.h"
#include "ssd.h"
#include "ssd-internal.h"
#include "theme.h"
#include "view.h"

enum border_corner {
	BORDER_CORNER_TOP_LEFT,
	BORDER_CORNER_TOP_RIGHT,
	BORDER_CORNER_BOTTOM_LEFT,
	BORDER_CORNER_BOTTOM_RIGHT,
};

static struct lab_data_buffer *
create_border_corner(const float color[static 4], enum border_corner corner)
{
	int size = rc.corner_radius + rc.theme->border_width;
	struct lab_data_buffer *buffer = buffer_create_cairo(size, size, 1);
	cairo_t *cairo = cairo_create(buffer->surface);
	cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cairo);
	cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
	set_cairo_color(cairo, color);
	double half = rc.theme->border_width / 2.0;
	double radius = size - half;
	double cx = 0;
	double cy = 0;
	double start = 0;
	double end = 0;
	switch (corner) {
	case BORDER_CORNER_TOP_LEFT:
		cx = size;
		cy = size;
		start = G_PI;
		end = 1.5 * G_PI;
		break;
	case BORDER_CORNER_TOP_RIGHT:
		cy = size;
		start = 1.5 * G_PI;
		end = 2 * G_PI;
		break;
	case BORDER_CORNER_BOTTOM_LEFT:
		cx = size;
		start = 0.5 * G_PI;
		end = G_PI;
		break;
	case BORDER_CORNER_BOTTOM_RIGHT:
		start = 0;
		end = 0.5 * G_PI;
		break;
	}
	cairo_set_line_width(cairo, rc.theme->border_width);
	cairo_arc(cairo, cx, cy, radius, start, end);
	cairo_stroke(cairo);
	cairo_surface_flush(buffer->surface);
	cairo_destroy(cairo);
	return buffer;
}

static struct wlr_scene_buffer *
add_border_corner(struct wlr_scene_tree *parent,
	const float color[static 4], enum border_corner corner)
{
	struct lab_data_buffer *buffer = create_border_corner(color, corner);
	struct wlr_scene_buffer *scene_buffer =
		lab_wlr_scene_buffer_create(parent, &buffer->base);
	wlr_buffer_drop(&buffer->base);
	return scene_buffer;
}

void
ssd_border_create(struct ssd *ssd)
{
	assert(ssd);
	assert(!ssd->border.tree);

	struct view *view = ssd->view;
	struct theme *theme = rc.theme;
	int width = view->current.width;
	int height = view_effective_height(view, /* use_pending */ false);
	int full_width = width + 2 * theme->border_width;
	int corner_width = ssd_get_corner_width();

	ssd->border.tree = lab_wlr_scene_tree_create(ssd->tree);
	wlr_scene_node_set_position(&ssd->border.tree->node, -theme->border_width, 0);

	enum ssd_active_state active;
	FOR_EACH_ACTIVE_STATE(active) {
		struct ssd_border_subtree *subtree = &ssd->border.subtrees[active];
		subtree->tree = lab_wlr_scene_tree_create(ssd->border.tree);
		struct wlr_scene_tree *parent = subtree->tree;
		wlr_scene_node_set_enabled(&parent->node, active);
		float *color = theme->window[active].border_color;

		subtree->left = lab_wlr_scene_rect_create(parent,
			theme->border_width, height, color);
		wlr_scene_node_set_position(&subtree->left->node, 0, 0);

		subtree->right = lab_wlr_scene_rect_create(parent,
			theme->border_width, height, color);
		wlr_scene_node_set_position(&subtree->right->node,
			theme->border_width + width, 0);

		subtree->bottom = lab_wlr_scene_rect_create(parent,
			full_width, theme->border_width, color);
		wlr_scene_node_set_position(&subtree->bottom->node,
			0, height);

		subtree->top = lab_wlr_scene_rect_create(parent,
			MAX(width - 2 * corner_width, 0), theme->border_width, color);
		wlr_scene_node_set_position(&subtree->top->node,
			theme->border_width + corner_width,
			-(ssd->titlebar.height + theme->border_width));

		subtree->top_left = add_border_corner(parent, color,
			BORDER_CORNER_TOP_LEFT);
		subtree->top_right = add_border_corner(parent, color,
			BORDER_CORNER_TOP_RIGHT);
		subtree->bottom_left = add_border_corner(parent, color,
			BORDER_CORNER_BOTTOM_LEFT);
		subtree->bottom_right = add_border_corner(parent, color,
			BORDER_CORNER_BOTTOM_RIGHT);
	}

	if (view->maximized == VIEW_AXIS_BOTH) {
		wlr_scene_node_set_enabled(&ssd->border.tree->node, false);
	}

	if (view->current.width > 0 && view->current.height > 0) {
		/*
		 * The SSD is recreated by a Reconfigure request
		 * thus we may need to handle squared corners.
		 */
		ssd_border_update(ssd);
	}
}

void
ssd_border_update(struct ssd *ssd)
{
	assert(ssd);
	assert(ssd->border.tree);

	struct view *view = ssd->view;
	if (view->maximized == VIEW_AXIS_BOTH
			&& ssd->border.tree->node.enabled) {
		/* Disable borders on maximize */
		wlr_scene_node_set_enabled(&ssd->border.tree->node, false);
		ssd->margin = ssd_thickness(ssd->view);
	}

	if (view->maximized == VIEW_AXIS_BOTH) {
		return;
	} else if (!ssd->border.tree->node.enabled) {
		/* And re-enabled them when unmaximized */
		wlr_scene_node_set_enabled(&ssd->border.tree->node, true);
		ssd->margin = ssd_thickness(ssd->view);
	}

	struct theme *theme = rc.theme;

	int width = view->current.width;
	int height = view_effective_height(view, /* use_pending */ false);
	int full_width = width + 2 * theme->border_width;
	int corner_width = ssd_get_corner_width();
	bool scrolling_rounded = view->singularity_scrolling_tiled
		&& rc.corner_radius > 0 && theme->border_width > 0;
	int scrolling_corner_size = rc.corner_radius + theme->border_width;

	/*
	 * From here on we have to cover the following border scenarios:
	 * Non-tiled (partial border, rounded corners):
	 *    _____________
	 *   o           oox
	 *  |---------------|
	 *  |_______________|
	 *
	 * Tiled (full border, squared corners):
	 *   _______________
	 *  |o           oox|
	 *  |---------------|
	 *  |_______________|
	 *
	 * Tiled or non-tiled with zero title height (full boarder, no title):
	 *   _______________
	 *  |_______________|
	 */

	int side_height = scrolling_rounded
		? MAX(height - 2 * rc.corner_radius, 0)
		: ssd->state.was_squared
		? height + ssd->titlebar.height
		: height;
	int side_y = scrolling_rounded
		? rc.corner_radius
		: ssd->state.was_squared
		? -ssd->titlebar.height
		: 0;
	int top_width = scrolling_rounded
		? MAX(full_width - 2 * scrolling_corner_size, 0)
		: ssd->titlebar.height <= 0 || ssd->state.was_squared
		? full_width
		: MAX(width - 2 * corner_width, 0);
	int top_x = scrolling_rounded
		? scrolling_corner_size
		: ssd->titlebar.height <= 0 || ssd->state.was_squared
		? 0
		: theme->border_width + corner_width;

	enum ssd_active_state active;
	FOR_EACH_ACTIVE_STATE(active) {
		struct ssd_border_subtree *subtree = &ssd->border.subtrees[active];

		wlr_scene_rect_set_size(subtree->left,
			theme->border_width, side_height);
		wlr_scene_node_set_position(&subtree->left->node,
			0, side_y);

		wlr_scene_rect_set_size(subtree->right,
			theme->border_width, side_height);
		wlr_scene_node_set_position(&subtree->right->node,
			theme->border_width + width, side_y);

		wlr_scene_rect_set_size(subtree->bottom,
			scrolling_rounded ? top_width : full_width,
			theme->border_width);
		wlr_scene_node_set_position(&subtree->bottom->node,
			scrolling_rounded ? top_x : 0, height);

		wlr_scene_rect_set_size(subtree->top,
			top_width, theme->border_width);
		wlr_scene_node_set_position(&subtree->top->node,
			top_x, -(ssd->titlebar.height + theme->border_width));

		wlr_scene_node_set_enabled(&subtree->top_left->node,
			scrolling_rounded);
		wlr_scene_node_set_enabled(&subtree->top_right->node,
			scrolling_rounded);
		wlr_scene_node_set_enabled(&subtree->bottom_left->node,
			scrolling_rounded);
		wlr_scene_node_set_enabled(&subtree->bottom_right->node,
			scrolling_rounded);
		if (scrolling_rounded) {
			int right_x = full_width - scrolling_corner_size;
			int bottom_y = height + theme->border_width
				- scrolling_corner_size;
			wlr_scene_node_set_position(&subtree->top_left->node,
				0, -theme->border_width);
			wlr_scene_node_set_position(&subtree->top_right->node,
				right_x, -theme->border_width);
			wlr_scene_node_set_position(&subtree->bottom_left->node,
				0, bottom_y);
			wlr_scene_node_set_position(&subtree->bottom_right->node,
				right_x, bottom_y);
		}
	}
}

void
ssd_border_destroy(struct ssd *ssd)
{
	assert(ssd);
	assert(ssd->border.tree);

	wlr_scene_node_destroy(&ssd->border.tree->node);
	ssd->border = (struct ssd_border_scene){0};
}
