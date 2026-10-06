// SPDX-License-Identifier: GPL-2.0-only
#define _DEFAULT_SOURCE
#include "a11y.h"
#include <cairo.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <time.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output_layout.h>
#include <scenefx/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_system_bell_v1.h>
#include "buffer.h"
#include "color-filter.h"
#include "common/scene-helpers.h"
#include "config/rcxml.h"
#include "input/accessx.h"
#include "input/cursor.h"
#include "labwc.h"
#include "magnifier.h"
#include "output.h"
#include "ssd.h"
#include "view.h"

#define FRAME_MS 16
#define LOCATE_DURATION_MS 700
#define LOCATE_RADIUS_START 72.0
#define LOCATE_RADIUS_END 12.0
#define INDICATOR_SIZE 26
#define INDICATOR_OFFSET 14
#define FLASH_DURATION_MS 300
#define FLASH_OPACITY 0.55f

enum dwell_state {
	DWELL_IDLE = 0,
	DWELL_COUNTING,
	DWELL_DONE,
};

static struct {
	struct wlr_scene_tree *tree;

	struct wlr_scene_buffer *locate;
	double locate_x, locate_y;
	uint32_t locate_start;
	struct wl_event_source *locate_timer;

	struct wlr_scene_buffer *indicator;
	struct wl_event_source *progress_timer;
	int buttons_down;

	enum dwell_state dwell;
	double dwell_x, dwell_y;
	uint32_t dwell_start;

	bool secondary_pending;
	bool secondary_swallow_release;
	double secondary_x, secondary_y;
	uint32_t secondary_start;
	uint32_t secondary_press_time;

	struct wlr_scene_buffer *flash;
	uint32_t flash_start;
	struct wl_event_source *flash_timer;

	struct wlr_xdg_system_bell_v1 *system_bell;
	struct wl_listener ring;
} a11y;

static uint32_t
now_msec(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

static float
cursor_scale(void)
{
	struct output *output = output_nearest_to_cursor();
	return output ? output->wlr_output->scale : 1.0f;
}

static bool
reject_input(struct wlr_scene_buffer *buffer, double *sx, double *sy)
{
	return false;
}

static struct wlr_scene_buffer *
show_buffer(struct wlr_scene_buffer *node, struct lab_data_buffer *buffer,
		int x, int y, int width, int height)
{
	if (node) {
		wlr_scene_buffer_set_buffer(node, &buffer->base);
	} else {
		node = lab_wlr_scene_buffer_create(a11y.tree, &buffer->base);
		node->point_accepts_input = reject_input;
	}
	wlr_buffer_drop(&buffer->base);
	wlr_scene_buffer_set_dest_size(node, width, height);
	wlr_scene_node_set_position(&node->node, x, y);
	wlr_scene_node_set_enabled(&node->node, true);
	wlr_scene_node_raise_to_top(&a11y.tree->node);
	return node;
}

static void
hide_buffer(struct wlr_scene_buffer *node)
{
	if (node) {
		wlr_scene_node_set_enabled(&node->node, false);
	}
}

static void
indicator_show(double progress)
{
	struct lab_data_buffer *buffer = buffer_create_cairo(INDICATOR_SIZE,
		INDICATOR_SIZE, cursor_scale());
	cairo_t *cairo = cairo_create(buffer->surface);
	double center = INDICATOR_SIZE / 2.0;

	cairo_arc(cairo, center, center, center - 1, 0, 2 * M_PI);
	cairo_set_source_rgba(cairo, 0, 0, 0, 0.65);
	cairo_fill(cairo);

	cairo_set_line_width(cairo, 3);
	cairo_set_line_cap(cairo, CAIRO_LINE_CAP_ROUND);
	cairo_arc(cairo, center, center, center - 6, 0, 2 * M_PI);
	cairo_set_source_rgba(cairo, 1, 1, 1, 0.3);
	cairo_stroke(cairo);
	cairo_arc(cairo, center, center, center - 6, -M_PI / 2,
		-M_PI / 2 + 2 * M_PI * MIN(1.0, progress));
	cairo_set_source_rgba(cairo, 1, 1, 1, 1);
	cairo_stroke(cairo);

	cairo_destroy(cairo);
	cairo_surface_flush(buffer->surface);

	a11y.indicator = show_buffer(a11y.indicator, buffer,
		server.seat.cursor->x + INDICATOR_OFFSET,
		server.seat.cursor->y + INDICATOR_OFFSET,
		INDICATOR_SIZE, INDICATOR_SIZE);
}

static void
emulate_click(uint32_t button, uint32_t time_msec)
{
	cursor_emulate_button(&server.seat, button,
		WL_POINTER_BUTTON_STATE_PRESSED, time_msec);
	cursor_emulate_button(&server.seat, button,
		WL_POINTER_BUTTON_STATE_RELEASED, time_msec);
}

static void
release_secondary_press(void)
{
	a11y.secondary_pending = false;
	hide_buffer(a11y.indicator);
	cursor_emulate_button(&server.seat, BTN_LEFT,
		WL_POINTER_BUTTON_STATE_PRESSED, a11y.secondary_press_time);
}

static int
handle_progress_timer(void *data)
{
	uint32_t now = now_msec();

	if (a11y.secondary_pending) {
		double progress = (double)(now - a11y.secondary_start)
			/ rc.secondary_click_time;
		if (progress < 1.0) {
			indicator_show(progress);
			wl_event_source_timer_update(a11y.progress_timer, FRAME_MS);
			return 0;
		}
		a11y.secondary_pending = false;
		a11y.secondary_swallow_release = true;
		hide_buffer(a11y.indicator);
		emulate_click(BTN_RIGHT, now);
		return 0;
	}

	if (a11y.dwell == DWELL_COUNTING && rc.dwell_click) {
		double progress = (double)(now - a11y.dwell_start) / rc.dwell_time;
		if (progress < 1.0) {
			indicator_show(progress);
			wl_event_source_timer_update(a11y.progress_timer, FRAME_MS);
			return 0;
		}
		a11y.dwell = DWELL_DONE;
		a11y.dwell_x = server.seat.cursor->x;
		a11y.dwell_y = server.seat.cursor->y;
		hide_buffer(a11y.indicator);
		emulate_click(BTN_LEFT, now);
		return 0;
	}

	hide_buffer(a11y.indicator);
	return 0;
}

bool
a11y_pointer_button(uint32_t button, enum wl_pointer_button_state state,
		uint32_t time_msec)
{
	bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;

	accessx_cancel_key_taps();
	if (pressed) {
		a11y.buttons_down++;
		if (a11y.dwell != DWELL_IDLE) {
			a11y.dwell = DWELL_DONE;
			a11y.dwell_x = server.seat.cursor->x;
			a11y.dwell_y = server.seat.cursor->y;
			hide_buffer(a11y.indicator);
		}
	} else if (a11y.buttons_down > 0) {
		a11y.buttons_down--;
	}

	if (button != BTN_LEFT) {
		return false;
	}
	if (pressed) {
		if (!rc.secondary_click || a11y.buttons_down > 1) {
			return false;
		}
		a11y.secondary_pending = true;
		a11y.secondary_x = server.seat.cursor->x;
		a11y.secondary_y = server.seat.cursor->y;
		a11y.secondary_start = now_msec();
		a11y.secondary_press_time = time_msec;
		wl_event_source_timer_update(a11y.progress_timer, FRAME_MS);
		return true;
	}
	if (a11y.secondary_swallow_release) {
		a11y.secondary_swallow_release = false;
		return true;
	}
	if (a11y.secondary_pending) {
		release_secondary_press();
	}
	return false;
}

void
a11y_pointer_motion(double dx, double dy)
{
	double x = server.seat.cursor->x + dx;
	double y = server.seat.cursor->y + dy;
	double threshold = rc.dwell_threshold;

	if (a11y.secondary_pending && hypot(x - a11y.secondary_x,
			y - a11y.secondary_y) > threshold) {
		release_secondary_press();
	}

	if (!rc.dwell_click) {
		return;
	}
	if (a11y.buttons_down > 0) {
		a11y.dwell = DWELL_DONE;
		a11y.dwell_x = x;
		a11y.dwell_y = y;
		return;
	}
	if (a11y.dwell == DWELL_IDLE
			|| hypot(x - a11y.dwell_x, y - a11y.dwell_y) > threshold) {
		a11y.dwell = DWELL_COUNTING;
		a11y.dwell_x = x;
		a11y.dwell_y = y;
		a11y.dwell_start = now_msec();
		if (!a11y.secondary_pending) {
			wl_event_source_timer_update(a11y.progress_timer, FRAME_MS);
		}
	}
}

static void
locate_draw(double t)
{
	double eased = 1.0 - (1.0 - t) * (1.0 - t);
	double radius = LOCATE_RADIUS_START
		+ (LOCATE_RADIUS_END - LOCATE_RADIUS_START) * eased;
	int size = (int)ceil(2 * (LOCATE_RADIUS_START + 6));
	double center = size / 2.0;

	struct lab_data_buffer *buffer =
		buffer_create_cairo(size, size, cursor_scale());
	cairo_t *cairo = cairo_create(buffer->surface);
	cairo_arc(cairo, center, center, radius, 0, 2 * M_PI);
	cairo_set_line_width(cairo, 8);
	cairo_set_source_rgba(cairo, 0, 0, 0, 0.6);
	cairo_stroke_preserve(cairo);
	cairo_set_line_width(cairo, 4);
	cairo_set_source_rgba(cairo, 1, 1, 1, 1);
	cairo_stroke(cairo);
	cairo_destroy(cairo);
	cairo_surface_flush(buffer->surface);

	a11y.locate = show_buffer(a11y.locate, buffer,
		lround(a11y.locate_x - center), lround(a11y.locate_y - center),
		size, size);
	wlr_scene_buffer_set_opacity(a11y.locate, 1.0f - 0.6f * (float)t);
}

static int
handle_locate_timer(void *data)
{
	double t = (double)(now_msec() - a11y.locate_start) / LOCATE_DURATION_MS;
	if (t >= 1.0) {
		hide_buffer(a11y.locate);
		return 0;
	}
	locate_draw(t);
	wl_event_source_timer_update(a11y.locate_timer, FRAME_MS);
	return 0;
}

void
a11y_locate_pointer(void)
{
	a11y.locate_x = server.seat.cursor->x;
	a11y.locate_y = server.seat.cursor->y;
	a11y.locate_start = now_msec();
	locate_draw(0.0);
	wl_event_source_timer_update(a11y.locate_timer, FRAME_MS);
}

static int
handle_flash_timer(void *data)
{
	double t = (double)(now_msec() - a11y.flash_start) / FLASH_DURATION_MS;
	if (t >= 1.0) {
		hide_buffer(a11y.flash);
		return 0;
	}
	wlr_scene_buffer_set_opacity(a11y.flash, FLASH_OPACITY * (1.0f - (float)t));
	wl_event_source_timer_update(a11y.flash_timer, FRAME_MS);
	return 0;
}

void
a11y_flash(struct wlr_surface *surface)
{
	struct wlr_box box = {0};
	struct view *view = NULL;
	if (!rc.visual_bell_fullscreen) {
		view = surface ? view_from_wlr_surface(surface) : NULL;
		if (!view) {
			view = server.active_view;
		}
	}
	if (view) {
		struct border margin = ssd_thickness(view);
		box = (struct wlr_box){
			.x = view->current.x - margin.left,
			.y = view->current.y - margin.top,
			.width = view->current.width + margin.left + margin.right,
			.height = view->current.height + margin.top + margin.bottom,
		};
	} else {
		wlr_output_layout_get_box(server.output_layout, NULL, &box);
	}
	if (wlr_box_empty(&box)) {
		return;
	}

	struct lab_data_buffer *buffer = buffer_create_cairo(1, 1, 1);
	cairo_t *cairo = cairo_create(buffer->surface);
	cairo_set_source_rgba(cairo, 1, 1, 1, 1);
	cairo_paint(cairo);
	cairo_destroy(cairo);
	cairo_surface_flush(buffer->surface);

	a11y.flash = show_buffer(a11y.flash, buffer, box.x, box.y,
		box.width, box.height);
	wlr_scene_buffer_set_opacity(a11y.flash, FLASH_OPACITY);
	a11y.flash_start = now_msec();
	wl_event_source_timer_update(a11y.flash_timer, FRAME_MS);
}

static void
handle_ring(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_system_bell_v1_ring_event *event = data;
	if (rc.visual_bell) {
		a11y_flash(event->surface);
	}
}

void
a11y_reconfigure(void)
{
	accessx_reconfigure();
	magnifier_reconfigure();
	color_filter_reconfigure();

	if (!rc.dwell_click && a11y.dwell != DWELL_IDLE) {
		a11y.dwell = DWELL_IDLE;
		hide_buffer(a11y.indicator);
	}
	if (!rc.secondary_click && a11y.secondary_pending) {
		release_secondary_press();
	}
}

void
a11y_init(void)
{
	a11y.tree = lab_wlr_scene_tree_create(&server.scene->tree);
	struct wl_event_loop *loop = server.wl_event_loop;
	a11y.locate_timer = wl_event_loop_add_timer(loop, handle_locate_timer, NULL);
	a11y.progress_timer = wl_event_loop_add_timer(loop,
		handle_progress_timer, NULL);
	a11y.flash_timer = wl_event_loop_add_timer(loop, handle_flash_timer, NULL);

	a11y.system_bell = wlr_xdg_system_bell_v1_create(server.wl_display, 1);
	if (a11y.system_bell) {
		a11y.ring.notify = handle_ring;
		wl_signal_add(&a11y.system_bell->events.ring, &a11y.ring);
	} else {
		wlr_log(WLR_ERROR, "unable to create xdg-system-bell");
	}

	accessx_init();
	magnifier_reconfigure();
	color_filter_reconfigure();
}

void
a11y_finish(void)
{
	accessx_finish();
	if (a11y.system_bell) {
		wl_list_remove(&a11y.ring.link);
		a11y.system_bell = NULL;
	}
	wl_event_source_remove(a11y.locate_timer);
	wl_event_source_remove(a11y.progress_timer);
	wl_event_source_remove(a11y.flash_timer);
}
