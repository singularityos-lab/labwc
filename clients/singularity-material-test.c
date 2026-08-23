// SPDX-License-Identifier: GPL-2.0-only

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "singularity-blur-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

struct test_client;

struct surface_state {
	struct test_client *client;
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	struct wl_buffer *buffer;
	struct wl_callback *frame_callback;
	bool material;
	int width;
	int height;
};

struct test_client {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct zsingularity_blur_manager_v1 *blur_manager;
	struct zsingularity_blur_v1 *blur;
	uint32_t blur_version;
	uint32_t mode;
	uint32_t strength;
	bool animate;
	double animation_start;
	struct surface_state background;
	struct surface_state material;
};

static double
monotonic_time(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec + now.tv_nsec / 1000000000.0;
}

static void
request_animation_frame(struct surface_state *state);

static void
handle_frame_done(void *data, struct wl_callback *callback, uint32_t time)
{
	(void)time;
	struct surface_state *state = data;
	wl_callback_destroy(callback);
	state->frame_callback = NULL;
	if (!state->client->animate) {
		return;
	}

	double now = monotonic_time();
	if (state->client->animation_start == 0.0) {
		state->client->animation_start = now;
	}
	double elapsed = now - state->client->animation_start;
	double cycle = fmod(elapsed, 6.0);
	double position;
	if (cycle < 1.0) {
		position = 0.0;
	} else if (cycle < 2.2) {
		position = (cycle - 1.0) / 1.2;
	} else if (cycle < 3.6) {
		position = 1.0;
	} else if (cycle < 4.8) {
		position = 1.0 - (cycle - 3.6) / 1.2;
	} else {
		position = 0.0;
	}
	int left = 100 + lround(position * 440.0);
	int top = 115 + lround(position * 70.0);
	zwlr_layer_surface_v1_set_margin(state->layer_surface,
		top, 0, 0, left);
	request_animation_frame(state);
	wl_surface_commit(state->surface);
}

static const struct wl_callback_listener frame_listener = {
	.done = handle_frame_done,
};

static void
request_animation_frame(struct surface_state *state)
{
	if (state->frame_callback) {
		return;
	}
	state->frame_callback = wl_surface_frame(state->surface);
	wl_callback_add_listener(state->frame_callback, &frame_listener, state);
}

static uint32_t
premultiply(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha)
{
	uint32_t r = red * alpha / 255;
	uint32_t g = green * alpha / 255;
	uint32_t b = blue * alpha / 255;
	return ((uint32_t)alpha << 24) | (r << 16) | (g << 8) | b;
}

static int
create_shm_file(size_t size)
{
	char name[64];
	snprintf(name, sizeof(name), "/singularity-material-%ld", (long)getpid());
	int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0) {
		return -1;
	}
	shm_unlink(name);
	if (ftruncate(fd, size) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static bool
inside_round_rect(int x, int y, int width, int height, int inset, int radius)
{
	if (x < inset || y < inset || x >= width - inset || y >= height - inset) {
		return false;
	}
	int left = inset + radius;
	int right = width - inset - radius - 1;
	int top = inset + radius;
	int bottom = height - inset - radius - 1;
	int nearest_x = x < left ? left : (x > right ? right : x);
	int nearest_y = y < top ? top : (y > bottom ? bottom : y);
	int dx = x - nearest_x;
	int dy = y - nearest_y;
	return dx * dx + dy * dy <= radius * radius;
}

static void
paint_checkerboard(uint32_t *pixels, int width, int height)
{
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			bool light = ((x / 12) + (y / 12)) & 1;
			int red = 38 + 118 * x / width + 30 * y / height;
			int green = 66 + 100 * y / height;
			int blue = 214 - 84 * x / width;
			red += light ? 22 : -10;
			green += light ? 14 : -8;
			blue += light ? 18 : -6;
			pixels[y * width + x] = 0xff000000
				| ((uint32_t)red << 16)
				| ((uint32_t)green << 8)
				| (uint32_t)blue;
		}
	}

	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			if ((x > 25 && x < 38) || (y > 25 && y < 38)) {
				pixels[y * width + x] = 0xffffffff;
			}
		}
	}
}

static void
paint_material(uint32_t *pixels, int width, int height)
{
	memset(pixels, 0, (size_t)width * height * sizeof(*pixels));
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			int coverage = 0;
			for (int sample_y = 0; sample_y < 4; ++sample_y) {
				for (int sample_x = 0; sample_x < 4; ++sample_x) {
					coverage += inside_round_rect(x * 4 + sample_x,
						y * 4 + sample_y, width * 4, height * 4,
						28 * 4, 48 * 4);
				}
			}
			if (coverage == 0) {
				continue;
			}
			pixels[y * width + x] = premultiply(232, 238, 248,
				(uint8_t)(36 * coverage / 16));
		}
	}

	int hole_x = width / 3;
	int hole_y = height / 2;
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			int dx = x - hole_x;
			int dy = y - hole_y;
			if (dx * dx + dy * dy < 45 * 45) {
				pixels[y * width + x] = 0;
			}
		}
	}

	for (int y = height / 2 - 56; y < height / 2 + 56; ++y) {
		for (int x = width * 2 / 3 - 72; x < width * 2 / 3 + 72; ++x) {
			bool light = ((x / 8) + (y / 8)) & 1;
			pixels[y * width + x] = light ? 0xffffffff : 0xff111827;
		}
	}

	for (int y = height - 88; y < height - 52; ++y) {
		for (int x = 76; x < width - 76; ++x) {
			uint8_t alpha = (uint8_t)((x - 76) * 250 / (width - 152));
			pixels[y * width + x] = premultiply(255, 255, 255, alpha);
		}
	}
}

static struct wl_buffer *
create_buffer(struct test_client *client, int width, int height, bool material)
{
	int stride = width * 4;
	size_t size = (size_t)stride * height;
	int fd = create_shm_file(size);
	if (fd < 0) {
		fprintf(stderr, "shm allocation failed: %s\n", strerror(errno));
		return NULL;
	}
	uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE,
		MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) {
		close(fd);
		return NULL;
	}
	if (material) {
		paint_material(pixels, width, height);
	} else {
		paint_checkerboard(pixels, width, height);
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(client->shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0,
		width, height, stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	munmap(pixels, size);
	close(fd);
	return buffer;
}

static void
handle_layer_configure(void *data, struct zwlr_layer_surface_v1 *layer_surface,
		uint32_t serial, uint32_t width, uint32_t height)
{
	struct surface_state *state = data;
	zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
	int next_width;
	int next_height;
	if (state->material) {
		next_width = 640;
		next_height = 420;
	} else {
		next_width = width;
		next_height = height;
	}
	if (next_width <= 0 || next_height <= 0) {
		return;
	}
	if (!state->buffer || state->width != next_width
			|| state->height != next_height) {
		if (state->buffer) {
			wl_buffer_destroy(state->buffer);
		}
		state->width = next_width;
		state->height = next_height;
		state->buffer = create_buffer(state->client,
			state->width, state->height, state->material);
		if (!state->buffer) {
			exit(EXIT_FAILURE);
		}
		wl_surface_attach(state->surface, state->buffer, 0, 0);
		wl_surface_damage_buffer(state->surface, 0, 0,
			state->width, state->height);
	}
	if (state->material && state->client->animate) {
		request_animation_frame(state);
	}
	wl_surface_commit(state->surface);
}

static void
handle_layer_closed(void *data, struct zwlr_layer_surface_v1 *layer_surface)
{
	(void)data;
	(void)layer_surface;
	exit(EXIT_SUCCESS);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = handle_layer_configure,
	.closed = handle_layer_closed,
};

static void
handle_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version)
{
	struct test_client *client = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		client->compositor = wl_registry_bind(registry, name,
			&wl_compositor_interface, version < 6 ? version : 6);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		client->shm = wl_registry_bind(registry, name,
			&wl_shm_interface, 1);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		client->layer_shell = wl_registry_bind(registry, name,
			&zwlr_layer_shell_v1_interface, version < 4 ? version : 4);
	} else if (strcmp(interface,
			zsingularity_blur_manager_v1_interface.name) == 0) {
		client->blur_version = version < 3 ? version : 3;
		client->blur_manager = wl_registry_bind(registry, name,
			&zsingularity_blur_manager_v1_interface, client->blur_version);
	}
}

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

static void
create_layer_surface(struct test_client *client, struct surface_state *state,
		bool material)
{
	state->client = client;
	state->material = material;
	state->surface = wl_compositor_create_surface(client->compositor);
	state->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		client->layer_shell, state->surface, NULL,
		material ? ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY
			: ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
		material ? "material-test" : "checkerboard-test");
	zwlr_layer_surface_v1_add_listener(state->layer_surface,
		&layer_surface_listener, state);
	zwlr_layer_surface_v1_set_keyboard_interactivity(state->layer_surface,
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
	if (material) {
		zwlr_layer_surface_v1_set_size(state->layer_surface, 640, 420);
		zwlr_layer_surface_v1_set_anchor(state->layer_surface,
			ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
			| ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
		zwlr_layer_surface_v1_set_margin(state->layer_surface,
			115, 0, 0, 100);
	} else {
		zwlr_layer_surface_v1_set_anchor(state->layer_surface,
			ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
			| ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT
			| ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
			| ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
		zwlr_layer_surface_v1_set_exclusive_zone(state->layer_surface, -1);
	}
	wl_surface_commit(state->surface);
}

static uint32_t
parse_mode(const char *value)
{
	if (strcmp(value, "glass") == 0) {
		return ZSINGULARITY_BLUR_V1_MODE_GLASS;
	}
	if (strcmp(value, "blur") == 0) {
		return ZSINGULARITY_BLUR_V1_MODE_BLUR;
	}
	return ZSINGULARITY_BLUR_V1_MODE_DISABLED;
}

int
main(int argc, char **argv)
{
	bool background_only = argc > 1 && strcmp(argv[1], "background") == 0;
	int strength = argc > 3 ? atoi(argv[3]) : 60;
	struct test_client client = {
		.mode = parse_mode(argc > 1 ? argv[1] : "glass"),
		.strength = (uint32_t)(strength < 0 ? 0 : strength > 100 ? 100 : strength),
		.animate = argc > 2 && strcmp(argv[2], "motion") == 0,
	};
	client.display = wl_display_connect(NULL);
	if (!client.display) {
		fprintf(stderr, "cannot connect to Wayland display\n");
		return EXIT_FAILURE;
	}
	struct wl_registry *registry = wl_display_get_registry(client.display);
	wl_registry_add_listener(registry, &registry_listener, &client);
	wl_display_roundtrip(client.display);
	if (!client.compositor || !client.shm || !client.layer_shell
			|| !client.blur_manager) {
		fprintf(stderr, "required Wayland globals are missing\n");
		return EXIT_FAILURE;
	}

	create_layer_surface(&client, &client.background, false);
	if (background_only) {
		while (wl_display_dispatch(client.display) >= 0) {
		}
		return EXIT_SUCCESS;
	}
	create_layer_surface(&client, &client.material, true);
	client.blur = zsingularity_blur_manager_v1_get_blur(
		client.blur_manager, client.material.surface);
	zsingularity_blur_v1_set_mode(client.blur, client.mode);
	if (client.blur_version >= 3) {
		zsingularity_blur_v1_set_strength(client.blur, client.strength);
	}
	struct wl_region *region = wl_compositor_create_region(client.compositor);
	wl_region_add(region, 20, 20, 600, 380);
	zsingularity_blur_v1_set_region(client.blur, region);
	wl_region_destroy(region);
	zsingularity_blur_v1_commit(client.blur);
	wl_surface_commit(client.material.surface);

	while (wl_display_dispatch(client.display) >= 0) {
	}
	return EXIT_SUCCESS;
}
