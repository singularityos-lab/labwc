// SPDX-License-Identifier: GPL-2.0-only
#include "protocols/singularity-pip.h"

#include <assert.h>
#include <cairo.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <linux/input-event-codes.h>
#include <pixman.h>
#include <wayland-server-core.h>
#include <wlr/render/allocator.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <wlr/util/transform.h>
#include "buffer.h"
#include "common/graphic-helpers.h"
#include "common/macros.h"
#include "common/scene-helpers.h"
#include "config/rcxml.h"
#include "labwc.h"
#include "output.h"
#include "singularity-pip-unstable-v1-protocol.h"
#include "ssd.h"
#include "theme.h"
#include "view.h"

#define PIP_BORDER 1
#define PIP_CORNER_RADIUS 10
#define PIP_SHADOW_BLUR 24
#define PIP_SHADOW_OFFSET_Y 8
#define PIP_SHADOW_LEFT PIP_SHADOW_BLUR
#define PIP_SHADOW_TOP (PIP_SHADOW_BLUR - PIP_SHADOW_OFFSET_Y)
#define PIP_SHADOW_RIGHT PIP_SHADOW_BLUR
#define PIP_SHADOW_BOTTOM (PIP_SHADOW_BLUR + PIP_SHADOW_OFFSET_Y)
#define PIP_CLOSE_SIZE 28
#define PIP_RESIZE_SIZE 16
#define PIP_MARGIN 18
#define PIP_MIN_WIDTH 160
#define PIP_MIN_HEIGHT 90
#define PIP_MAX_WIDTH 720

enum pip_interaction {
	PIP_INTERACTION_NONE = 0,
	PIP_INTERACTION_MOVE,
	PIP_INTERACTION_RESIZE,
};

struct pip_state {
	struct wl_global *global;
	struct view *view;
	struct wl_listener view_destroy;
	struct wl_listener surface_unmap;
	struct output *output;
	struct wlr_swapchain *swapchain;

	struct wlr_scene_tree *root;
	struct wlr_scene_buffer *frame;
	struct wlr_scene_buffer *content;
	struct wlr_scene_tree *close_control;
	struct wlr_scene_buffer *close_visual;
	struct wlr_scene_tree *resize_control;
	struct wlr_scene_buffer *resize_visual;
	struct lab_data_buffer *frame_buffer;
	struct lab_data_buffer *close_buffers[2];
	struct lab_data_buffer *resize_buffers[2];
	float visual_scale;
	bool close_hovered;
	bool resize_hovered;

	struct wlr_box crop;
	struct wlr_box requested_crop;
	bool full_toplevel;
	double aspect;
	int x;
	int y;
	int width;
	int height;

	enum pip_interaction interaction;
	double grab_x;
	double grab_y;
	int grab_pip_x;
	int grab_pip_y;
	int grab_width;
	int grab_height;
	bool moved;
};

static struct pip_state pip;

static void pip_update_scene_geometry(void);

static bool
node_is_descendant(struct wlr_scene_node *node, struct wlr_scene_node *ancestor)
{
	while (node) {
		if (node == ancestor) {
			return true;
		}
		node = node->parent ? &node->parent->node : NULL;
	}
	return false;
}

static struct wlr_box
pip_workarea(struct output *output)
{
	if (!output_is_usable(output)) {
		return (struct wlr_box) {0};
	}
	return output_usable_area_in_layout_coords(output);
}

static int
pip_outer_width(void)
{
	return pip.width + PIP_BORDER * 2;
}

static int
pip_outer_height(void)
{
	return pip.height + PIP_BORDER * 2;
}

static float
pip_output_scale(void)
{
	if (!output_is_usable(pip.output) || pip.output->wlr_output->scale <= 0.0f) {
		return 1.0f;
	}
	return pip.output->wlr_output->scale;
}

static void
pip_drop_buffer(struct lab_data_buffer **buffer)
{
	if (*buffer) {
		wlr_buffer_drop(&(*buffer)->base);
		*buffer = NULL;
	}
}

static void
pip_rounded_rectangle(cairo_t *cairo, double x, double y,
		double width, double height, double radius)
{
	double right = x + width;
	double bottom = y + height;
	double corner = MIN(radius, MIN(width, height) / 2.0);

	cairo_new_sub_path(cairo);
	cairo_arc(cairo, right - corner, y + corner,
		corner, -G_PI / 2.0, 0);
	cairo_arc(cairo, right - corner, bottom - corner,
		corner, 0, G_PI / 2.0);
	cairo_arc(cairo, x + corner, bottom - corner,
		corner, G_PI / 2.0, G_PI);
	cairo_arc(cairo, x + corner, y + corner,
		corner, G_PI, G_PI * 3.0 / 2.0);
	cairo_close_path(cairo);
}

static void
pip_draw_shadow(cairo_t *cairo, double x, double y,
		double width, double height)
{
	double sigma = PIP_SHADOW_BLUR / 3.0;
	for (int distance = PIP_SHADOW_BLUR; distance >= 0; --distance) {
		double normalized = distance / (sqrt(2.0) * sigma);
		double alpha = 0.45 * 0.5 * erfc(normalized);
		pip_rounded_rectangle(cairo,
			x - distance, y + PIP_SHADOW_OFFSET_Y - distance,
			width + distance * 2, height + distance * 2,
			PIP_CORNER_RADIUS + distance);
		cairo_set_source_rgba(cairo, 0, 0, 0, alpha);
		cairo_set_line_width(cairo, 1.1);
		cairo_stroke(cairo);
	}
}

static struct lab_data_buffer *
pip_create_frame_buffer(void)
{
	int frame_width = pip_outer_width();
	int frame_height = pip_outer_height();
	int width = frame_width + PIP_SHADOW_LEFT + PIP_SHADOW_RIGHT;
	int height = frame_height + PIP_SHADOW_TOP + PIP_SHADOW_BOTTOM;
	struct lab_data_buffer *buffer = buffer_create_cairo(
		width, height, pip_output_scale());
	cairo_t *cairo = cairo_create(buffer->surface);
	cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cairo);
	cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
	cairo_set_antialias(cairo, CAIRO_ANTIALIAS_BEST);

	pip_draw_shadow(cairo, PIP_SHADOW_LEFT, PIP_SHADOW_TOP,
		frame_width, frame_height);
	float *text_color = rc.theme->window[SSD_ACTIVE].label_text_color;
	float border_color[4] = {
		text_color[0] * 0.14f,
		text_color[1] * 0.14f,
		text_color[2] * 0.14f,
		text_color[3] * 0.14f,
	};
	set_cairo_color(cairo, border_color);
	pip_rounded_rectangle(cairo, PIP_SHADOW_LEFT, PIP_SHADOW_TOP,
		frame_width, frame_height, PIP_CORNER_RADIUS);
	cairo_fill(cairo);
	cairo_surface_flush(buffer->surface);
	cairo_destroy(cairo);
	return buffer;
}

static struct lab_data_buffer *
pip_create_close_buffer(bool hovered)
{
	struct lab_data_buffer *buffer = buffer_create_cairo(
		PIP_CLOSE_SIZE, PIP_CLOSE_SIZE, pip_output_scale());
	cairo_t *cairo = cairo_create(buffer->surface);
	cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cairo);
	cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
	cairo_set_antialias(cairo, CAIRO_ANTIALIAS_BEST);

	cairo_arc(cairo, PIP_CLOSE_SIZE / 2.0, PIP_CLOSE_SIZE / 2.0,
		PIP_CLOSE_SIZE / 2.0 - 0.5, 0, G_PI * 2.0);
	cairo_set_source_rgba(cairo, 0, 0, 0, hovered ? 0.60 : 0.45);
	cairo_fill_preserve(cairo);
	cairo_set_source_rgba(cairo, 1, 1, 1, hovered ? 0.28 : 0.18);
	cairo_set_line_width(cairo, 1.0);
	cairo_stroke(cairo);

	cairo_set_source_rgba(cairo, 1, 1, 1, 1);
	cairo_set_line_width(cairo, 1.5);
	cairo_set_line_cap(cairo, CAIRO_LINE_CAP_SQUARE);
	cairo_move_to(cairo, 11, 11);
	cairo_line_to(cairo, 17, 17);
	cairo_move_to(cairo, 17, 11);
	cairo_line_to(cairo, 11, 17);
	cairo_stroke(cairo);
	cairo_surface_flush(buffer->surface);
	cairo_destroy(cairo);
	return buffer;
}

static struct lab_data_buffer *
pip_create_resize_buffer(bool hovered)
{
	struct lab_data_buffer *buffer = buffer_create_cairo(
		PIP_RESIZE_SIZE, PIP_RESIZE_SIZE, pip_output_scale());
	cairo_t *cairo = cairo_create(buffer->surface);
	cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cairo);
	cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
	cairo_set_antialias(cairo, CAIRO_ANTIALIAS_BEST);

	float *text_color = rc.theme->window[SSD_ACTIVE].label_text_color;
	double alpha = hovered ? 0.51 : 0.21;
	set_cairo_color(cairo, (float[4]) {
		text_color[0] * alpha,
		text_color[1] * alpha,
		text_color[2] * alpha,
		text_color[3] * alpha,
	});
	cairo_arc(cairo, PIP_RESIZE_SIZE - 1.0, PIP_RESIZE_SIZE - 1.0,
		1.0, 0, G_PI * 2.0);
	cairo_fill(cairo);
	cairo_surface_flush(buffer->surface);
	cairo_destroy(cairo);
	return buffer;
}

static void
pip_update_frame_buffer(void)
{
	struct lab_data_buffer *buffer = pip_create_frame_buffer();
	wlr_scene_buffer_set_buffer(pip.frame, &buffer->base);
	wlr_scene_buffer_set_dest_size(pip.frame,
		buffer->logical_width, buffer->logical_height);
	pip_drop_buffer(&pip.frame_buffer);
	pip.frame_buffer = buffer;
}

static void
pip_update_control_buffers(void)
{
	for (int hovered = 0; hovered < 2; ++hovered) {
		struct lab_data_buffer *close = pip_create_close_buffer(hovered);
		struct lab_data_buffer *resize = pip_create_resize_buffer(hovered);
		pip_drop_buffer(&pip.close_buffers[hovered]);
		pip_drop_buffer(&pip.resize_buffers[hovered]);
		pip.close_buffers[hovered] = close;
		pip.resize_buffers[hovered] = resize;
	}
	wlr_scene_buffer_set_buffer(pip.close_visual,
		&pip.close_buffers[pip.close_hovered]->base);
	wlr_scene_buffer_set_dest_size(pip.close_visual,
		PIP_CLOSE_SIZE, PIP_CLOSE_SIZE);
	wlr_scene_buffer_set_buffer(pip.resize_visual,
		&pip.resize_buffers[pip.resize_hovered]->base);
	wlr_scene_buffer_set_dest_size(pip.resize_visual,
		PIP_RESIZE_SIZE, PIP_RESIZE_SIZE);
	pip.visual_scale = pip_output_scale();
}

static void
pip_schedule_frame(void)
{
	if (output_is_usable(pip.output)) {
		wlr_output_schedule_frame(pip.output->wlr_output);
	}
}

static void
pip_set_output(struct output *output)
{
	if (pip.output == output) {
		return;
	}
	pip.output = output;
	if (pip.swapchain) {
		wlr_swapchain_destroy(pip.swapchain);
		pip.swapchain = NULL;
	}
	if (pip.root) {
		pip_update_scene_geometry();
	}
}

static void
pip_clamp_position(void)
{
	struct wlr_box workarea = pip_workarea(pip.output);
	if (wlr_box_empty(&workarea)) {
		return;
	}

	int max_x = workarea.x + workarea.width - pip_outer_width();
	int max_y = workarea.y + workarea.height - pip_outer_height();
	max_x = MAX(max_x, workarea.x);
	max_y = MAX(max_y, workarea.y);
	pip.x = MAX(workarea.x, MIN(pip.x, max_x));
	pip.y = MAX(workarea.y, MIN(pip.y, max_y));
	if (pip.root) {
		wlr_scene_node_set_position(&pip.root->node,
			pip.x - PIP_SHADOW_LEFT, pip.y - PIP_SHADOW_TOP);
	}
}

static bool
pip_fit_output_bounds(void)
{
	struct wlr_box workarea = pip_workarea(pip.output);
	if (wlr_box_empty(&workarea) || pip.width <= 0 || pip.height <= 0) {
		return false;
	}
	int max_width = MAX(1, workarea.width - PIP_BORDER * 2);
	int max_height = MAX(1, workarea.height - PIP_BORDER * 2);
	if (pip.width <= max_width && pip.height <= max_height) {
		return false;
	}
	double scale = MIN((double)max_width / pip.width,
		(double)max_height / pip.height);
	pip.width = MAX(1, (int)lround(pip.width * scale));
	pip.height = MAX(1, (int)lround(pip.height * scale));
	return true;
}

static void
pip_update_scene_geometry(void)
{
	if (!pip.root) {
		return;
	}

	float scale = pip_output_scale();
	pip_update_frame_buffer();
	if (fabsf(scale - pip.visual_scale) > 0.001f) {
		pip_update_control_buffers();
	}
	wlr_scene_buffer_set_dest_size(pip.content, pip.width, pip.height);
	wlr_scene_node_set_position(&pip.content->node,
		PIP_SHADOW_LEFT + PIP_BORDER, PIP_SHADOW_TOP + PIP_BORDER);
	wlr_scene_node_set_position(&pip.close_control->node,
		PIP_SHADOW_LEFT + PIP_BORDER + pip.width - PIP_CLOSE_SIZE - 6,
		PIP_SHADOW_TOP + PIP_BORDER + 6);
	wlr_scene_node_set_position(&pip.resize_control->node,
		PIP_SHADOW_LEFT + PIP_BORDER + pip.width - PIP_RESIZE_SIZE - 2,
		PIP_SHADOW_TOP + PIP_BORDER + pip.height - PIP_RESIZE_SIZE - 2);
	pip_clamp_position();
}

static bool
pip_never_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy)
{
	(void)buffer;
	(void)sx;
	(void)sy;
	return false;
}

static bool
pip_content_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy)
{
	(void)buffer;
	double radius = PIP_CORNER_RADIUS - PIP_BORDER;
	double x = *sx < radius ? radius - *sx
		: *sx > pip.width - radius ? *sx - (pip.width - radius) : 0;
	double y = *sy < radius ? radius - *sy
		: *sy > pip.height - radius ? *sy - (pip.height - radius) : 0;
	return x == 0 || y == 0 || x * x + y * y <= radius * radius;
}

static void
pip_create_scene(void)
{
	pip.root = lab_wlr_scene_tree_create(server.pip_tree);
	pip.frame = lab_wlr_scene_buffer_create(pip.root, NULL);
	pip.frame->point_accepts_input = pip_never_accepts_input;
	pip.content = lab_wlr_scene_buffer_create(pip.root, NULL);
	pip.content->point_accepts_input = pip_content_accepts_input;
	wlr_scene_buffer_set_filter_mode(pip.content, WLR_SCALE_FILTER_BILINEAR);

	pip.close_control = lab_wlr_scene_tree_create(pip.root);
	pip.close_visual = lab_wlr_scene_buffer_create(pip.close_control, NULL);

	pip.resize_control = lab_wlr_scene_tree_create(pip.root);
	pip.resize_visual = lab_wlr_scene_buffer_create(pip.resize_control, NULL);
	pip_update_control_buffers();
	pip_update_scene_geometry();
}

static bool
pip_source_geometry(struct view *view, int *width, int *height,
		int *offset_x, int *offset_y)
{
	if (!view || !view->surface) {
		return false;
	}

	*width = view->current.width;
	*height = view->current.height;
	*offset_x = 0;
	*offset_y = 0;
	struct wlr_xdg_surface *xdg =
		wlr_xdg_surface_try_from_wlr_surface(view->surface);
	if (xdg && xdg->geometry.width > 0 && xdg->geometry.height > 0) {
		*width = xdg->geometry.width;
		*height = xdg->geometry.height;
		*offset_x = xdg->geometry.x;
		*offset_y = xdg->geometry.y;
	}
	if (*width <= 0 || *height <= 0) {
		*width = view->surface->current.width;
		*height = view->surface->current.height;
	}
	return *width > 0 && *height > 0;
}

static bool
pip_update_crop(void)
{
	int source_width, source_height, offset_x, offset_y;
	if (!pip_source_geometry(pip.view, &source_width, &source_height,
			&offset_x, &offset_y)) {
		return false;
	}

	if (pip.full_toplevel) {
		pip.crop = (struct wlr_box) {
			.width = source_width,
			.height = source_height,
		};
	} else {
		struct wlr_box source = {
			.width = source_width,
			.height = source_height,
		};
		if (!wlr_box_intersection(&pip.crop, &pip.requested_crop, &source)) {
			return false;
		}
	}
	return pip.crop.width > 0 && pip.crop.height > 0;
}

static void
pip_fit_initial_size(void)
{
	struct wlr_box workarea = pip_workarea(pip.output);
	int max_width = MAX(PIP_MIN_WIDTH,
		MIN(PIP_MAX_WIDTH, workarea.width - PIP_MARGIN * 2));
	int max_height = MAX(PIP_MIN_HEIGHT,
		workarea.height * 48 / 100);
	int desired_width = MAX(280, workarea.width * 34 / 100);

	pip.width = MIN(desired_width, max_width);
	pip.height = (int)lround((double)pip.width / pip.aspect);
	if (pip.height > max_height) {
		pip.height = max_height;
		pip.width = (int)lround((double)pip.height * pip.aspect);
	}
	if (pip.width < PIP_MIN_WIDTH && PIP_MIN_WIDTH <= max_width) {
		pip.width = PIP_MIN_WIDTH;
		pip.height = (int)lround((double)pip.width / pip.aspect);
	}
	if (pip.height < PIP_MIN_HEIGHT && PIP_MIN_HEIGHT <= max_height) {
		pip.height = PIP_MIN_HEIGHT;
		pip.width = (int)lround((double)pip.height * pip.aspect);
	}
	if (pip.width > max_width) {
		pip.width = max_width;
		pip.height = (int)lround((double)pip.width / pip.aspect);
	}
	if (pip.height > max_height) {
		pip.height = max_height;
		pip.width = (int)lround((double)pip.height * pip.aspect);
	}
	pip.width = MAX(1, pip.width);
	pip.height = MAX(1, pip.height);
}

static bool
format_matches(const struct wlr_drm_format *left,
	const struct wlr_drm_format *right)
{
	return left->format == right->format && left->len == right->len
		&& (!left->len || !memcmp(left->modifiers, right->modifiers,
			left->len * sizeof(left->modifiers[0])));
}

struct pip_render_context {
	struct wlr_render_pass *pass;
	pixman_region32_t *clip;
	double scale_x;
	double scale_y;
	int offset_x;
	int offset_y;
};

static void
render_surface(struct wlr_surface *surface, int sx, int sy, void *data)
{
	struct pip_render_context *ctx = data;
	if (!wlr_surface_has_buffer(surface)) {
		return;
	}
	struct wlr_texture *texture = wlr_surface_get_texture(surface);
	if (!texture) {
		return;
	}

	struct wlr_fbox source_box;
	wlr_surface_get_buffer_source_box(surface, &source_box);
	struct wlr_box destination = {
		.x = (int)lround((sx - ctx->offset_x - pip.crop.x) * ctx->scale_x),
		.y = (int)lround((sy - ctx->offset_y - pip.crop.y) * ctx->scale_y),
		.width = MAX(1, (int)lround(surface->current.width * ctx->scale_x)),
		.height = MAX(1, (int)lround(surface->current.height * ctx->scale_y)),
	};
	wlr_render_pass_add_texture(ctx->pass,
		&(struct wlr_render_texture_options) {
			.texture = texture,
			.src_box = source_box,
			.dst_box = destination,
			.transform = wlr_output_transform_invert(surface->current.transform),
			.clip = ctx->clip,
			.filter_mode = WLR_SCALE_FILTER_BILINEAR,
			.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
		});
}

static void
pip_clear_rounded_corners(struct wlr_render_pass *pass,
		int width, int height, float scale)
{
	int radius = MAX(1, (int)lround(
		(PIP_CORNER_RADIUS - PIP_BORDER) * scale));
	for (int row = 0; row < radius; ++row) {
		double y = row + 0.5;
		double distance = radius - y;
		int inset = (int)ceil(radius
			- sqrt(MAX(0.0, radius * radius - distance * distance)));
		if (inset <= 0) {
			continue;
		}
		struct wlr_box corners[4] = {
			{.x = 0, .y = row, .width = inset, .height = 1},
			{.x = width - inset, .y = row, .width = inset, .height = 1},
			{.x = 0, .y = height - row - 1, .width = inset, .height = 1},
			{.x = width - inset, .y = height - row - 1,
				.width = inset, .height = 1},
		};
		for (size_t i = 0; i < ARRAY_SIZE(corners); ++i) {
			wlr_render_pass_add_rect(pass,
				&(struct wlr_render_rect_options) {
					.box = corners[i],
					.color = {0},
					.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
				});
		}
	}
}

static bool
pip_render(void)
{
	if (!pip.root || !output_is_usable(pip.output)
			|| !pip.output->wlr_output->swapchain || !pip.view->surface
			|| !pip_update_crop()) {
		return false;
	}

	float output_scale = pip.output->wlr_output->scale;
	if (output_scale <= 0.0f) {
		output_scale = 1.0f;
	}
	int buffer_width = MAX(1, (int)ceil(pip.width * output_scale));
	int buffer_height = MAX(1, (int)ceil(pip.height * output_scale));
	const struct wlr_drm_format *format =
		&pip.output->wlr_output->swapchain->format;
	if (pip.swapchain && (pip.swapchain->width != buffer_width
			|| pip.swapchain->height != buffer_height
			|| !format_matches(&pip.swapchain->format, format))) {
		wlr_swapchain_destroy(pip.swapchain);
		pip.swapchain = NULL;
	}
	if (!pip.swapchain) {
		pip.swapchain = wlr_swapchain_create(server.allocator,
			buffer_width, buffer_height, format);
		if (!pip.swapchain) {
			return false;
		}
	}

	struct wlr_buffer *buffer = wlr_swapchain_acquire(pip.swapchain);
	if (!buffer) {
		return false;
	}
	struct wlr_render_pass *pass =
		wlr_renderer_begin_buffer_pass(server.renderer, buffer, NULL);
	if (!pass) {
		wlr_buffer_unlock(buffer);
		return false;
	}
	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options) {
		.box = {.width = buffer_width, .height = buffer_height},
		.color = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f},
	});

	int source_width, source_height, offset_x, offset_y;
	if (!pip_source_geometry(pip.view, &source_width, &source_height,
			&offset_x, &offset_y)) {
		wlr_render_pass_submit(pass);
		wlr_buffer_unlock(buffer);
		return false;
	}
	pixman_region32_t clip;
	pixman_region32_init_rect(&clip, 0, 0, buffer_width, buffer_height);
	struct pip_render_context context = {
		.pass = pass,
		.clip = &clip,
		.scale_x = (double)buffer_width / pip.crop.width,
		.scale_y = (double)buffer_height / pip.crop.height,
		.offset_x = offset_x,
		.offset_y = offset_y,
	};
	wlr_surface_for_each_surface(pip.view->surface, render_surface, &context);
	pip_clear_rounded_corners(pass, buffer_width, buffer_height, output_scale);
	pixman_region32_fini(&clip);
	if (!wlr_render_pass_submit(pass)) {
		wlr_buffer_unlock(buffer);
		return false;
	}

	wlr_scene_buffer_set_buffer(pip.content, buffer);
	wlr_scene_buffer_set_dest_size(pip.content, pip.width, pip.height);
	wlr_buffer_unlock(buffer);
	return true;
}

void
singularity_pip_close(void)
{
	struct output *output = pip.output;
	if (pip.view) {
		wl_list_remove(&pip.view_destroy.link);
		wl_list_remove(&pip.surface_unmap.link);
		pip.view = NULL;
	}
	pip.interaction = PIP_INTERACTION_NONE;
	if (pip.root) {
		wlr_scene_node_destroy(&pip.root->node);
		pip.root = NULL;
		pip.frame = NULL;
		pip.content = NULL;
		pip.close_control = NULL;
		pip.close_visual = NULL;
		pip.resize_control = NULL;
		pip.resize_visual = NULL;
	}
	pip_drop_buffer(&pip.frame_buffer);
	for (size_t i = 0; i < ARRAY_SIZE(pip.close_buffers); ++i) {
		pip_drop_buffer(&pip.close_buffers[i]);
		pip_drop_buffer(&pip.resize_buffers[i]);
	}
	pip.visual_scale = 0;
	pip.close_hovered = false;
	pip.resize_hovered = false;
	if (pip.swapchain) {
		wlr_swapchain_destroy(pip.swapchain);
		pip.swapchain = NULL;
	}
	pip.output = NULL;
	if (output_is_usable(output)) {
		wlr_output_schedule_frame(output->wlr_output);
	}
}

static void
handle_view_destroy(struct wl_listener *listener, void *data)
{
	(void)listener;
	(void)data;
	singularity_pip_close();
}

static void
handle_surface_unmap(struct wl_listener *listener, void *data)
{
	(void)listener;
	(void)data;
	singularity_pip_close();
}

static bool
pip_show(struct view *view, const struct wlr_box *crop, bool full_toplevel)
{
	if (!view || !view->mapped || !view->surface) {
		return false;
	}

	singularity_pip_close();
	pip.view = view;
	pip.full_toplevel = full_toplevel;
	if (crop) {
		pip.requested_crop = *crop;
	}
	if (!pip_update_crop()) {
		pip.view = NULL;
		return false;
	}
	pip.aspect = (double)pip.crop.width / pip.crop.height;
	pip_set_output(output_is_usable(view->output)
		? view->output : output_nearest_to_cursor());
	if (!output_is_usable(pip.output)) {
		pip.view = NULL;
		pip.output = NULL;
		return false;
	}
	pip_fit_initial_size();
	struct wlr_box workarea = pip_workarea(pip.output);
	pip.x = workarea.x + workarea.width - pip_outer_width() - PIP_MARGIN;
	pip.y = workarea.y + workarea.height - pip_outer_height() - PIP_MARGIN;
	pip_create_scene();

	pip.view_destroy.notify = handle_view_destroy;
	wl_signal_add(&view->events.destroy, &pip.view_destroy);
	pip.surface_unmap.notify = handle_surface_unmap;
	wl_signal_add(&view->surface->events.unmap, &pip.surface_unmap);
	if (!pip_render()) {
		singularity_pip_close();
		return false;
	}
	pip_schedule_frame();
	return true;
}

static struct view *
view_for_region(const struct wlr_box *selection, struct wlr_box *crop)
{
	struct view *view;
	struct view *best = NULL;
	struct wlr_box best_intersection = {0};
	int64_t best_area = 0;
	int center_x = selection->x + selection->width / 2;
	int center_y = selection->y + selection->height / 2;

	for_each_view(view, &server.views, LAB_VIEW_CRITERIA_CURRENT_WORKSPACE) {
		if (view->minimized || !view->surface || !view->scene_tree->node.enabled) {
			continue;
		}
		struct wlr_box intersection;
		if (!wlr_box_intersection(&intersection, selection, &view->current)) {
			continue;
		}
		if (wlr_box_contains_point(&view->current, center_x, center_y)) {
			best = view;
			best_intersection = intersection;
			break;
		}
		int64_t area = (int64_t)intersection.width * intersection.height;
		if (area > best_area) {
			best = view;
			best_intersection = intersection;
			best_area = area;
		}
	}
	if (!best) {
		return NULL;
	}
	*crop = (struct wlr_box) {
		.x = best_intersection.x - best->current.x,
		.y = best_intersection.y - best->current.y,
		.width = best_intersection.width,
		.height = best_intersection.height,
	};
	return best;
}

static void
manager_handle_show_toplevel(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *toplevel_resource)
{
	(void)client;
	struct wlr_foreign_toplevel_handle_v1 *toplevel =
		wl_resource_get_user_data(toplevel_resource);
	struct view *view = toplevel ? toplevel->data : NULL;
	if (!pip_show(view, NULL, true)) {
		zsingularity_pip_manager_v1_send_failed(resource,
			ZSINGULARITY_PIP_MANAGER_V1_FAILURE_REASON_INVALID_TOPLEVEL);
		return;
	}
	zsingularity_pip_manager_v1_send_shown(resource);
}

static void
manager_handle_show_region(struct wl_client *client,
		struct wl_resource *resource, int32_t x, int32_t y,
		int32_t width, int32_t height)
{
	(void)client;
	if (width <= 0 || height <= 0
			|| x > INT32_MAX - width || y > INT32_MAX - height) {
		zsingularity_pip_manager_v1_send_failed(resource,
			ZSINGULARITY_PIP_MANAGER_V1_FAILURE_REASON_INVALID_REGION);
		return;
	}
	struct wlr_box selection = {
		.x = x,
		.y = y,
		.width = width,
		.height = height,
	};
	struct wlr_box crop;
	struct view *view = view_for_region(&selection, &crop);
	if (!view) {
		zsingularity_pip_manager_v1_send_failed(resource,
			ZSINGULARITY_PIP_MANAGER_V1_FAILURE_REASON_NO_WINDOW);
		return;
	}
	if (!pip_show(view, &crop, false)) {
		zsingularity_pip_manager_v1_send_failed(resource,
			ZSINGULARITY_PIP_MANAGER_V1_FAILURE_REASON_RENDER_FAILED);
		return;
	}
	zsingularity_pip_manager_v1_send_shown(resource);
}

static void
manager_handle_close(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	(void)resource;
	singularity_pip_close();
}

static void
manager_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct zsingularity_pip_manager_v1_interface manager_impl = {
	.show_toplevel = manager_handle_show_toplevel,
	.show_region = manager_handle_show_region,
	.close = manager_handle_close,
	.destroy = manager_handle_destroy,
};

static void
bind_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	(void)data;
	struct wl_resource *resource = wl_resource_create(client,
		&zsingularity_pip_manager_v1_interface, MIN(version, 1), id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

void
singularity_pip_init(void)
{
	pip.global = wl_global_create(server.wl_display,
		&zsingularity_pip_manager_v1_interface, 1, NULL, bind_manager);
	if (!pip.global) {
		wlr_log(WLR_ERROR, "unable to create Singularity PiP manager");
	}
}

void
singularity_pip_finish(void)
{
	singularity_pip_close();
}

void
singularity_pip_output_destroy(struct output *output)
{
	if (!pip.root || pip.output != output) {
		return;
	}
	struct output *replacement = output_nearest_to_cursor();
	if (!output_is_usable(replacement)) {
		singularity_pip_close();
		return;
	}
	pip_set_output(replacement);
	if (pip_fit_output_bounds()) {
		pip_update_scene_geometry();
	} else {
		pip_clamp_position();
	}
	pip_schedule_frame();
}

void
singularity_pip_render_output(struct output *output)
{
	if (!pip.root || pip.output != output) {
		return;
	}
	if (!pip_update_crop()) {
		singularity_pip_close();
		return;
	}
	double aspect = (double)pip.crop.width / pip.crop.height;
	if (fabs(aspect - pip.aspect) > 0.001) {
		pip.aspect = aspect;
		pip.height = MAX(1, (int)lround((double)pip.width / aspect));
		pip_fit_output_bounds();
		pip_update_scene_geometry();
	}
	pip_render();
}

static void
send_surface_frame_done(struct wlr_surface *surface, int sx, int sy, void *data)
{
	(void)sx;
	(void)sy;
	wlr_surface_send_frame_done(surface, data);
}

void
singularity_pip_send_frame_done(struct output *output,
		const struct timespec *when)
{
	if (!pip.root || pip.output != output) {
		return;
	}
	if (pip.view && pip.view->surface) {
		wlr_surface_for_each_surface(pip.view->surface,
			send_surface_frame_done, (void *)when);
	}
	pip_schedule_frame();
}

static void
pip_set_control_hover(enum singularity_pip_cursor_area area)
{
	bool close_hovered = area == SINGULARITY_PIP_CURSOR_CLOSE;
	bool resize_hovered = area == SINGULARITY_PIP_CURSOR_RESIZE;
	if (close_hovered != pip.close_hovered) {
		pip.close_hovered = close_hovered;
		wlr_scene_buffer_set_buffer(pip.close_visual,
			&pip.close_buffers[close_hovered]->base);
		pip_schedule_frame();
	}
	if (resize_hovered != pip.resize_hovered) {
		pip.resize_hovered = resize_hovered;
		wlr_scene_buffer_set_buffer(pip.resize_visual,
			&pip.resize_buffers[resize_hovered]->base);
		pip_schedule_frame();
	}
}

enum singularity_pip_cursor_area
singularity_pip_cursor_area(struct wlr_scene_node *node)
{
	if (pip.interaction == PIP_INTERACTION_RESIZE) {
		pip_set_control_hover(SINGULARITY_PIP_CURSOR_RESIZE);
		return SINGULARITY_PIP_CURSOR_RESIZE;
	}
	if (pip.interaction == PIP_INTERACTION_MOVE) {
		pip_set_control_hover(SINGULARITY_PIP_CURSOR_CONTENT);
		return SINGULARITY_PIP_CURSOR_CONTENT;
	}
	if (!pip.root || !node || !node_is_descendant(node, &pip.root->node)) {
		if (pip.root) {
			pip_set_control_hover(SINGULARITY_PIP_CURSOR_NONE);
		}
		return SINGULARITY_PIP_CURSOR_NONE;
	}
	if (node_is_descendant(node, &pip.close_control->node)) {
		pip_set_control_hover(SINGULARITY_PIP_CURSOR_CLOSE);
		return SINGULARITY_PIP_CURSOR_CLOSE;
	}
	if (node_is_descendant(node, &pip.resize_control->node)) {
		pip_set_control_hover(SINGULARITY_PIP_CURSOR_RESIZE);
		return SINGULARITY_PIP_CURSOR_RESIZE;
	}
	pip_set_control_hover(SINGULARITY_PIP_CURSOR_CONTENT);
	return SINGULARITY_PIP_CURSOR_CONTENT;
}

bool
singularity_pip_interactive(void)
{
	return pip.interaction != PIP_INTERACTION_NONE;
}

bool
singularity_pip_button_press(struct wlr_scene_node *node,
		uint32_t button, double x, double y)
{
	if (button != BTN_LEFT) {
		return false;
	}
	enum singularity_pip_cursor_area area = singularity_pip_cursor_area(node);
	if (area == SINGULARITY_PIP_CURSOR_NONE) {
		return false;
	}
	if (area == SINGULARITY_PIP_CURSOR_CLOSE) {
		singularity_pip_close();
		return true;
	}

	pip.interaction = area == SINGULARITY_PIP_CURSOR_RESIZE
		? PIP_INTERACTION_RESIZE : PIP_INTERACTION_MOVE;
	pip.grab_x = x;
	pip.grab_y = y;
	pip.grab_pip_x = pip.x;
	pip.grab_pip_y = pip.y;
	pip.grab_width = pip.width;
	pip.grab_height = pip.height;
	pip.moved = false;
	return true;
}

bool
singularity_pip_button_release(uint32_t button)
{
	if (button != BTN_LEFT || pip.interaction == PIP_INTERACTION_NONE) {
		return false;
	}
	bool activate = pip.interaction == PIP_INTERACTION_MOVE && !pip.moved;
	pip.interaction = PIP_INTERACTION_NONE;
	if (activate && pip.view) {
		desktop_focus_view(pip.view, true);
	}
	return true;
}

void
singularity_pip_cursor_motion(double x, double y)
{
	if (!pip.root || pip.interaction == PIP_INTERACTION_NONE) {
		return;
	}
	double dx = x - pip.grab_x;
	double dy = y - pip.grab_y;
	if (fabs(dx) >= 3.0 || fabs(dy) >= 3.0) {
		pip.moved = true;
	}

	if (pip.interaction == PIP_INTERACTION_MOVE) {
		int next_x = pip.grab_pip_x + (int)lround(dx);
		int next_y = pip.grab_pip_y + (int)lround(dy);
		struct output *next_output = output_nearest_to(
			next_x + pip_outer_width() / 2,
			next_y + pip_outer_height() / 2);
		if (output_is_usable(next_output)) {
			pip_set_output(next_output);
		}
		pip.x = next_x;
		pip.y = next_y;
		if (pip_fit_output_bounds()) {
			pip_update_scene_geometry();
		} else {
			pip_clamp_position();
		}
		pip_schedule_frame();
		return;
	}

	struct wlr_box workarea = pip_workarea(pip.output);
	int available_width = workarea.x + workarea.width
		- pip.grab_pip_x - PIP_BORDER * 2;
	int available_height = workarea.y + workarea.height
		- pip.grab_pip_y - PIP_BORDER * 2;
	double scale_x = (pip.grab_width + dx) / pip.grab_width;
	double scale_y = (pip.grab_height + dy) / pip.grab_height;
	double scale = fabs(scale_x - 1.0) > fabs(scale_y - 1.0)
		? scale_x : scale_y;
	double min_scale = MAX((double)PIP_MIN_WIDTH / pip.grab_width,
		(double)PIP_MIN_HEIGHT / pip.grab_height);
	double max_scale = MIN((double)MAX(1, available_width) / pip.grab_width,
		(double)MAX(1, available_height) / pip.grab_height);
	min_scale = MIN(min_scale, max_scale);
	scale = MAX(min_scale, MIN(scale, max_scale));
	pip.width = MAX(1, (int)lround(pip.grab_width * scale));
	pip.height = MAX(1, (int)lround(pip.grab_height * scale));
	pip_update_scene_geometry();
	pip_schedule_frame();
}
