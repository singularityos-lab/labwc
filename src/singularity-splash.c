/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>
#include "labwc.h"
#include "output.h"
#include "buffer.h"
#include "img/img.h"
#include "common/scene-helpers.h"
#include "scaled-buffer/scaled-img-buffer.h"
#include "singularity-splash.h"

#define SINGULARITY_SPLASH_LOGO "/usr/share/singularity/splash-logo.png"
#define SINGULARITY_SPLASH_MARKER "/run/singularity/compositor-first-frame"
#define SINGULARITY_SPLASH_LOGO_SIZE 256
#define SINGULARITY_SPLASH_MIN_SHOW_MS 400

static struct wlr_scene_tree *splash_tree;
static bool splash_shown;
static bool splash_dismissed;
static bool marker_written;
static struct timespec splash_shown_at;
static struct wl_event_source *dismiss_timer;

static void
write_first_frame_marker(void)
{
	if (marker_written) {
		return;
	}
	int fd = open(SINGULARITY_SPLASH_MARKER,
		O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		close(fd);
		marker_written = true;
	}
}

void
singularity_splash_maybe_show(struct output *output)
{
	if (splash_dismissed) {
		return;
	}
	if (splash_shown) {
		write_first_frame_marker();
		return;
	}
	if (!output || !output->wlr_output) {
		return;
	}
	int width = output->wlr_output->width;
	int height = output->wlr_output->height;
	if (width <= 0 || height <= 0) {
		return;
	}

	splash_tree = lab_wlr_scene_tree_create(&server.scene->tree);
	wlr_scene_node_set_position(&splash_tree->node, 0, 0);

	static const float bg[4] = { 0.05f, 0.05f, 0.07f, 1.0f };
	lab_wlr_scene_rect_create(splash_tree, width, height, bg);

	/*
	 * Render the logo once, directly, at the output's scale: the splash is
	 * a fixed one-shot overlay, so the scaled-buffer LRU machinery (built
	 * for themed icons that re-render on scale changes) buys nothing here
	 * and its deferred outputs_update render left the on-screen buffer
	 * unrendered (a noise square) when created from the frame handler.
	 */
	struct lab_img *logo =
		lab_img_load(LAB_IMG_PNG, SINGULARITY_SPLASH_LOGO, NULL);
	if (logo) {
		float output_scale = output->wlr_output->scale;
		struct lab_data_buffer *pixels = lab_img_render(logo,
			SINGULARITY_SPLASH_LOGO_SIZE,
			SINGULARITY_SPLASH_LOGO_SIZE, output_scale);
		lab_img_destroy(logo);
		if (pixels) {
			struct wlr_scene_buffer *sb = wlr_scene_buffer_create(
				splash_tree, &pixels->base);
			wlr_buffer_drop(&pixels->base);
			if (sb) {
				wlr_scene_buffer_set_dest_size(sb,
					SINGULARITY_SPLASH_LOGO_SIZE,
					SINGULARITY_SPLASH_LOGO_SIZE);
				int x = (width - SINGULARITY_SPLASH_LOGO_SIZE) / 2;
				int y = (height - SINGULARITY_SPLASH_LOGO_SIZE) / 2;
				wlr_scene_node_set_position(&sb->node, x, y);
			}
		}
	} else {
		wlr_log(WLR_INFO, "singularity splash: no logo at %s, brand fill only",
			SINGULARITY_SPLASH_LOGO);
	}

	wlr_scene_node_raise_to_top(&splash_tree->node);
	splash_shown = true;
	clock_gettime(CLOCK_MONOTONIC, &splash_shown_at);
	write_first_frame_marker();
}

static void
destroy_splash(void)
{
	splash_dismissed = true;
	if (dismiss_timer) {
		wl_event_source_remove(dismiss_timer);
		dismiss_timer = NULL;
	}
	if (splash_tree) {
		wlr_scene_node_destroy(&splash_tree->node);
		splash_tree = NULL;
	}
}

static int
handle_dismiss_timer(void *data)
{
	destroy_splash();
	return 0;
}

static int
splash_elapsed_ms(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - splash_shown_at.tv_sec) * 1000
		+ (int)((now.tv_nsec - splash_shown_at.tv_nsec) / 1000000);
}

void
singularity_splash_dismiss(void)
{
	if (splash_dismissed) {
		return;
	}
	if (!splash_shown) {
		/* A client mapped before a single splash frame was presented;
		 * mark dismissed so the splash is never raised after the fact. */
		splash_dismissed = true;
		return;
	}

	/* Hold the logo for a minimum so it is an actual splash rather than a
	 * one-frame flash, even when the first client maps immediately after the
	 * compositor's first frame. A timer drives the late dismiss so it fires
	 * even if the scene goes idle and stops producing frames. */
	int elapsed = splash_elapsed_ms();
	if (elapsed >= SINGULARITY_SPLASH_MIN_SHOW_MS) {
		destroy_splash();
		return;
	}
	if (!dismiss_timer) {
		dismiss_timer = wl_event_loop_add_timer(server.wl_event_loop,
			handle_dismiss_timer, NULL);
		if (!dismiss_timer) {
			destroy_splash();
			return;
		}
		wl_event_source_timer_update(dismiss_timer,
			SINGULARITY_SPLASH_MIN_SHOW_MS - elapsed);
	}
}
