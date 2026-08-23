// SPDX-License-Identifier: GPL-2.0-only

#include <assert.h>
#include <drm_fourcc.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/render/allocator.h>
#include <wlr/render/color.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>
#include <wlr/util/transform.h>
#include "labwc.h"
#include "output.h"
#include "protocols/singularity-blur.h"
#include "singularity-blur-unstable-v1-protocol.h"

struct singularity_blur_entry {
	struct wl_resource *resource;
	struct wlr_surface *surface;
	uint32_t pending_mode;
	uint32_t mode;
	uint32_t pending_radius;
	uint32_t radius;
	uint32_t pending_noise;
	uint32_t noise;
	uint32_t pending_strength;
	uint32_t strength;
	bool pending_region_set;
	bool region_set;
	pixman_region32_t pending_region;
	pixman_region32_t region;
	bool position_valid;
	bool motion_active;
	int last_x;
	int last_y;
	int last_width;
	int last_height;
	double motion_time;
	float velocity_x;
	float velocity_y;
	float motion_x;
	float motion_y;
	float spring_x;
	float spring_y;
	struct wl_list link;
	struct wl_listener surface_destroy;
};

struct singularity_blur_manager {
	struct wl_global *global;
	struct wl_list entries;
};

struct material_renderer {
	struct wlr_renderer *renderer;
	struct wlr_swapchain *mask_swapchain;
	int width;
	int height;
	int texture_width;
	int texture_height;
	GLuint framebuffer;
	GLuint scene_texture;
	GLuint mask_texture;
	GLuint reconstruct_texture;
	GLuint blur_texture[2];
	GLuint distance_texture[2];
	GLuint reconstruct_program;
	GLuint blur_program;
	GLuint distance_seed_program;
	GLuint distance_program;
	GLuint compose_program;
};

struct surface_find_data {
	struct wlr_surface *target;
	struct wlr_scene_buffer *scene_buffer;
	int x;
	int y;
	int z_index;
	int next_z_index;
};

struct material_surface {
	struct singularity_blur_entry *entry;
	struct surface_find_data find;
};

static struct singularity_blur_manager blur_manager;
static struct material_renderer material_renderer;

static const char vertex_shader_source[] =
	"attribute vec2 position;\n"
	"varying vec2 uv;\n"
	"void main() {\n"
	"  uv = position * 0.5 + 0.5;\n"
	"  gl_Position = vec4(position, 0.0, 1.0);\n"
	"}\n";

static const char reconstruct_shader_source[] =
	"precision mediump float;\n"
	"uniform sampler2D scene_texture;\n"
	"uniform sampler2D surface_texture;\n"
	"varying vec2 uv;\n"
	"void main() {\n"
	"  vec4 scene = texture2D(scene_texture, uv);\n"
	"  vec4 surface = texture2D(surface_texture, uv);\n"
	"  float alpha = clamp(surface.a, 0.0, 1.0);\n"
	"  float valid = 1.0 - step(0.998, alpha);\n"
	"  vec3 backdrop = clamp((scene.rgb - surface.rgb) / max(1.0 - alpha, 0.002), 0.0, 1.0);\n"
	"  vec3 linear_backdrop = pow(backdrop, vec3(2.2));\n"
	"  gl_FragColor = vec4(linear_backdrop * valid, valid);\n"
	"}\n";

static const char blur_shader_source[] =
	"precision mediump float;\n"
	"uniform sampler2D source_texture;\n"
	"uniform vec2 direction;\n"
	"varying vec2 uv;\n"
	"void main() {\n"
	"  vec4 color = texture2D(source_texture, uv) * 0.227027;\n"
	"  color += texture2D(source_texture, uv + direction * 1.384615) * 0.316216;\n"
	"  color += texture2D(source_texture, uv - direction * 1.384615) * 0.316216;\n"
	"  color += texture2D(source_texture, uv + direction * 3.230769) * 0.070270;\n"
	"  color += texture2D(source_texture, uv - direction * 3.230769) * 0.070270;\n"
	"  gl_FragColor = color;\n"
	"}\n";

static const char distance_seed_shader_source[] =
	"precision mediump float;\n"
	"uniform sampler2D surface_texture;\n"
	"varying vec2 uv;\n"
	"void main() {\n"
	"  float inside = step(0.002, texture2D(surface_texture, uv).a);\n"
	"  gl_FragColor = vec4(inside, inside, inside, 1.0);\n"
	"}\n";

static const char distance_shader_source[] =
	"precision mediump float;\n"
	"uniform sampler2D source_texture;\n"
	"uniform vec2 pixel_size;\n"
	"uniform float distance_step;\n"
	"varying vec2 uv;\n"
	"float sample_distance(vec2 offset, float weight) {\n"
	"  return texture2D(source_texture, uv + offset * pixel_size).r\n"
	"    + distance_step * weight;\n"
	"}\n"
	"void main() {\n"
	"  float distance_value = texture2D(source_texture, uv).r;\n"
	"  distance_value = min(distance_value, sample_distance(vec2(-1.0, 0.0), 1.0));\n"
	"  distance_value = min(distance_value, sample_distance(vec2(1.0, 0.0), 1.0));\n"
	"  distance_value = min(distance_value, sample_distance(vec2(0.0, -1.0), 1.0));\n"
	"  distance_value = min(distance_value, sample_distance(vec2(0.0, 1.0), 1.0));\n"
	"  distance_value = min(distance_value, sample_distance(vec2(-1.0, -1.0), 1.414214));\n"
	"  distance_value = min(distance_value, sample_distance(vec2(1.0, -1.0), 1.414214));\n"
	"  distance_value = min(distance_value, sample_distance(vec2(-1.0, 1.0), 1.414214));\n"
	"  distance_value = min(distance_value, sample_distance(vec2(1.0, 1.0), 1.414214));\n"
	"  distance_value = min(distance_value, 1.0);\n"
	"  gl_FragColor = vec4(distance_value, distance_value, distance_value, 1.0);\n"
	"}\n";

static const char compose_shader_source[] =
	"precision mediump float;\n"
	"uniform sampler2D scene_texture;\n"
	"uniform sampler2D surface_texture;\n"
	"uniform sampler2D reconstruct_texture;\n"
	"uniform sampler2D blurred_texture;\n"
	"uniform sampler2D distance_texture;\n"
	"uniform vec2 pixel_size;\n"
	"uniform vec2 distance_pixel;\n"
	"uniform vec2 motion_pixels;\n"
	"uniform float mode;\n"
	"uniform float effect_strength;\n"
	"varying vec2 uv;\n"
	"vec3 normalized_sample(sampler2D image, vec2 point) {\n"
	"  vec4 sample_value = texture2D(image, clamp(point, vec2(0.0), vec2(1.0)));\n"
	"  return sample_value.rgb / max(sample_value.a, 0.025);\n"
	"}\n"
	"vec3 backdrop_sample(vec2 point, vec3 fallback) {\n"
	"  vec4 sample_value = texture2D(reconstruct_texture, clamp(point, vec2(0.0), vec2(1.0)));\n"
	"  vec3 color = sample_value.rgb / max(sample_value.a, 0.025);\n"
	"  return mix(fallback, color, smoothstep(0.04, 0.45, sample_value.a));\n"
	"}\n"
	"float squircle_height(float distance_value) {\n"
	"  float inner = max(1.0 - pow(1.0 - distance_value, 4.0), 0.00001);\n"
	"  return pow(inner, 0.25);\n"
	"}\n"
	"float squircle_slope(float distance_value) {\n"
	"  float remaining = 1.0 - distance_value;\n"
	"  float inner = max(1.0 - pow(remaining, 4.0), 0.00001);\n"
	"  return min(pow(remaining, 3.0) / pow(inner, 0.75), 16.0);\n"
	"}\n"
	"void main() {\n"
	"  vec4 scene = texture2D(scene_texture, uv);\n"
	"  vec4 surface = texture2D(surface_texture, uv);\n"
	"  float alpha = clamp(surface.a, 0.0, 1.0);\n"
	"  if (alpha <= 0.002 || alpha >= 0.998) {\n"
	"    gl_FragColor = scene;\n"
	"    return;\n"
	"  }\n"
	"  vec3 sharp = normalized_sample(reconstruct_texture, uv);\n"
	"  vec3 blurred = normalized_sample(blurred_texture, uv);\n"
	"  vec3 material = mix(sharp, blurred, min(effect_strength * 1.2, 0.96));\n"
	"  if (mode < 1.5) {\n"
	"    float distance_value = texture2D(distance_texture, uv).r;\n"
	"    vec2 gradient = vec2(\n"
	"      texture2D(distance_texture, uv + vec2(distance_pixel.x, 0.0)).r\n"
	"        - texture2D(distance_texture, uv - vec2(distance_pixel.x, 0.0)).r,\n"
	"      texture2D(distance_texture, uv + vec2(0.0, distance_pixel.y)).r\n"
	"        - texture2D(distance_texture, uv - vec2(0.0, distance_pixel.y)).r);\n"
	"    float gradient_length = length(gradient);\n"
	"    vec2 outward = -gradient / max(gradient_length, 0.0001);\n"
	"    float profile_x = clamp(distance_value, 0.012, 1.0);\n"
	"    float profile_height = squircle_height(profile_x);\n"
	"    float slope = squircle_slope(profile_x);\n"
	"    vec3 surface_normal = normalize(vec3(outward * slope, -1.0));\n"
	"    vec3 ray = refract(vec3(0.0, 0.0, 1.0), surface_normal, 0.666667);\n"
	"    vec2 lens_pixels = ray.xy / max(ray.z, 0.08)\n"
	"      * (1.0 - profile_height) * 38.0;\n"
	"    lens_pixels *= smoothstep(0.0005, 0.003, gradient_length);\n"
	"    float motion_amount = length(motion_pixels);\n"
	"    vec2 motion_direction = motion_pixels / max(motion_amount, 0.001);\n"
	"    float direction = dot(outward, motion_direction);\n"
	"    float motion_energy = clamp(motion_amount / 24.0, 0.0, 1.0);\n"
	"    lens_pixels *= clamp(1.0 + direction * motion_energy * 0.65, 0.45, 1.65);\n"
	"    float interior_weight = mix(1.0, 0.62, smoothstep(0.0, 1.0, distance_value));\n"
	"    vec2 flex_pixels = motion_pixels * interior_weight;\n"
	"    flex_pixels += outward * direction * motion_amount\n"
	"      * (1.0 - smoothstep(0.0, 0.8, distance_value)) * 0.14;\n"
	"    vec2 sample_point = uv + (lens_pixels + flex_pixels) * pixel_size;\n"
	"    vec3 refracted = backdrop_sample(sample_point, sharp);\n"
	"    float blur_weight = min(effect_strength\n"
	"      * mix(0.70, 1.10, smoothstep(0.0, 0.8, distance_value)), 0.92);\n"
	"    material = mix(refracted, blurred, blur_weight);\n"
	"    float luminance = dot(material, vec3(0.2126, 0.7152, 0.0722));\n"
	"    material *= mix(1.015, 0.99, smoothstep(0.12, 0.82, luminance));\n"
	"    float rim = (1.0 - smoothstep(0.0, 0.18, distance_value))\n"
	"      * smoothstep(0.0005, 0.003, gradient_length);\n"
	"    float light = dot(outward, normalize(vec2(-0.55, 0.84)));\n"
	"    material += rim * (0.014 + max(light, 0.0) * 0.018\n"
	"      + motion_energy * max(direction, 0.0) * 0.012);\n"
	"  }\n"
	"  vec3 material_srgb = pow(clamp(material, 0.0, 1.0), vec3(0.454545));\n"
	"  vec3 result = surface.rgb + material_srgb * (1.0 - alpha);\n"
	"  gl_FragColor = vec4(result, 1.0);\n"
	"}\n";

static void
schedule_effect_frame(void)
{
	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		wlr_output_schedule_frame(output->wlr_output);
	}
}

static bool
mode_is_active(uint32_t mode)
{
	return mode == ZSINGULARITY_BLUR_V1_MODE_GLASS
		|| mode == ZSINGULARITY_BLUR_V1_MODE_BLUR;
}

bool
singularity_blur_has_effects(void)
{
	if (!wlr_renderer_is_gles2(server.renderer)) {
		return false;
	}

	struct singularity_blur_entry *entry;
	wl_list_for_each(entry, &blur_manager.entries, link) {
		if (entry->surface && mode_is_active(entry->mode)) {
			return true;
		}
	}
	return false;
}

bool
singularity_blur_output_has_animations(struct output *output)
{
	int width, height;
	wlr_output_effective_resolution(output->wlr_output, &width, &height);
	struct wlr_box output_box = {
		.x = output->scene_output->x,
		.y = output->scene_output->y,
		.width = width,
		.height = height,
	};
	struct singularity_blur_entry *entry;
	wl_list_for_each(entry, &blur_manager.entries, link) {
		if (entry->surface
				&& entry->mode == ZSINGULARITY_BLUR_V1_MODE_GLASS
				&& entry->motion_active) {
			struct wlr_box surface_box = {
				.x = entry->last_x,
				.y = entry->last_y,
				.width = entry->last_width,
				.height = entry->last_height,
			};
			struct wlr_box intersection;
			if (wlr_box_intersection(&intersection,
					&surface_box, &output_box)) {
				return true;
			}
		}
	}
	return false;
}

static double
monotonic_time(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec + now.tv_nsec / 1000000000.0;
}

static void
reset_motion(struct singularity_blur_entry *entry)
{
	entry->position_valid = false;
	entry->motion_active = false;
	entry->velocity_x = 0.0f;
	entry->velocity_y = 0.0f;
	entry->motion_x = 0.0f;
	entry->motion_y = 0.0f;
	entry->spring_x = 0.0f;
	entry->spring_y = 0.0f;
}

static void
update_motion(struct singularity_blur_entry *entry,
		const struct surface_find_data *find)
{
	double now = monotonic_time();
	if (!entry->position_valid) {
		entry->position_valid = true;
		entry->last_x = find->x;
		entry->last_y = find->y;
		entry->motion_time = now;
		return;
	}

	float elapsed = now - entry->motion_time;
	if (elapsed < 0.004f) {
		return;
	}
	float dt = fminf(elapsed, 0.05f);
	float sample_x = (find->x - entry->last_x) / dt;
	float sample_y = (find->y - entry->last_y) / dt;
	float filter = 1.0f - expf(-18.0f * dt);
	entry->velocity_x += (sample_x - entry->velocity_x) * filter;
	entry->velocity_y += (sample_y - entry->velocity_y) * filter;

	float target_x = -entry->velocity_x * 0.038f;
	float target_y = -entry->velocity_y * 0.038f;
	float target_length = hypotf(target_x, target_y);
	if (target_length > 30.0f) {
		target_x *= 30.0f / target_length;
		target_y *= 30.0f / target_length;
	}

	int steps = ceilf(dt / 0.012f);
	float step = dt / steps;
	for (int i = 0; i < steps; ++i) {
		float acceleration_x = (target_x - entry->motion_x) * 145.0f
			- entry->spring_x * 21.0f;
		float acceleration_y = (target_y - entry->motion_y) * 145.0f
			- entry->spring_y * 21.0f;
		entry->spring_x += acceleration_x * step;
		entry->spring_y += acceleration_y * step;
		entry->motion_x += entry->spring_x * step;
		entry->motion_y += entry->spring_y * step;
	}

	entry->last_x = find->x;
	entry->last_y = find->y;
	entry->motion_time = now;
	entry->motion_active = hypotf(entry->motion_x, entry->motion_y) > 0.04f
		|| hypotf(entry->spring_x, entry->spring_y) > 0.3f
		|| hypotf(entry->velocity_x, entry->velocity_y) > 1.0f;
	if (!entry->motion_active) {
		entry->velocity_x = 0.0f;
		entry->velocity_y = 0.0f;
		entry->motion_x = 0.0f;
		entry->motion_y = 0.0f;
		entry->spring_x = 0.0f;
		entry->spring_y = 0.0f;
	}
}

static GLuint
compile_shader(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);

	GLint status = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (status == GL_TRUE) {
		return shader;
	}

	char log[1024] = {0};
	glGetShaderInfoLog(shader, sizeof(log), NULL, log);
	wlr_log(WLR_ERROR, "Failed to compile background material shader: %s", log);
	glDeleteShader(shader);
	return 0;
}

static GLuint
create_program(const char *fragment_source)
{
	GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_shader_source);
	GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
	if (!vertex || !fragment) {
		glDeleteShader(vertex);
		glDeleteShader(fragment);
		return 0;
	}

	GLuint program = glCreateProgram();
	glAttachShader(program, vertex);
	glAttachShader(program, fragment);
	glBindAttribLocation(program, 0, "position");
	glLinkProgram(program);
	glDeleteShader(vertex);
	glDeleteShader(fragment);

	GLint status = GL_FALSE;
	glGetProgramiv(program, GL_LINK_STATUS, &status);
	if (status == GL_TRUE) {
		return program;
	}

	char log[1024] = {0};
	glGetProgramInfoLog(program, sizeof(log), NULL, log);
	wlr_log(WLR_ERROR, "Failed to link background material shader: %s", log);
	glDeleteProgram(program);
	return 0;
}

static void
configure_texture(GLuint texture, int width, int height)
{
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
		GL_RGBA, GL_UNSIGNED_BYTE, NULL);
}

static bool
prepare_gl_resources(int width, int height)
{
	struct material_renderer *renderer = &material_renderer;
	if (!renderer->reconstruct_program || !renderer->blur_program
			|| !renderer->distance_seed_program
			|| !renderer->distance_program || !renderer->compose_program) {
		glDeleteProgram(renderer->reconstruct_program);
		glDeleteProgram(renderer->blur_program);
		glDeleteProgram(renderer->distance_seed_program);
		glDeleteProgram(renderer->distance_program);
		glDeleteProgram(renderer->compose_program);
		renderer->reconstruct_program = create_program(reconstruct_shader_source);
		renderer->blur_program = create_program(blur_shader_source);
		renderer->distance_seed_program =
			create_program(distance_seed_shader_source);
		renderer->distance_program = create_program(distance_shader_source);
		renderer->compose_program = create_program(compose_shader_source);
		if (!renderer->reconstruct_program || !renderer->blur_program
				|| !renderer->distance_seed_program
				|| !renderer->distance_program
				|| !renderer->compose_program) {
			glDeleteProgram(renderer->reconstruct_program);
			glDeleteProgram(renderer->blur_program);
			glDeleteProgram(renderer->distance_seed_program);
			glDeleteProgram(renderer->distance_program);
			glDeleteProgram(renderer->compose_program);
			renderer->reconstruct_program = 0;
			renderer->blur_program = 0;
			renderer->distance_seed_program = 0;
			renderer->distance_program = 0;
			renderer->compose_program = 0;
			return false;
		}
	}

	if (!renderer->framebuffer) {
		glGenFramebuffers(1, &renderer->framebuffer);
		glGenTextures(1, &renderer->scene_texture);
		glGenTextures(1, &renderer->mask_texture);
		glGenTextures(1, &renderer->reconstruct_texture);
		glGenTextures(2, renderer->blur_texture);
		glGenTextures(2, renderer->distance_texture);
	}

	if (renderer->texture_width != width || renderer->texture_height != height) {
		renderer->texture_width = width;
		renderer->texture_height = height;
		configure_texture(renderer->scene_texture, width, height);
		configure_texture(renderer->mask_texture, width, height);
		configure_texture(renderer->reconstruct_texture,
			width, height);
		configure_texture(renderer->blur_texture[0],
			(width + 1) / 2, (height + 1) / 2);
		configure_texture(renderer->blur_texture[1],
			(width + 1) / 2, (height + 1) / 2);
		configure_texture(renderer->distance_texture[0],
			(width + 3) / 4, (height + 3) / 4);
		configure_texture(renderer->distance_texture[1],
			(width + 3) / 4, (height + 3) / 4);
	}
	glBindTexture(GL_TEXTURE_2D, 0);
	return true;
}

static const struct wlr_drm_format *
pick_mask_format(void)
{
	static uint64_t modifiers[] = {
		DRM_FORMAT_MOD_INVALID,
		DRM_FORMAT_MOD_LINEAR,
	};
	static struct wlr_drm_format format = {
		.format = DRM_FORMAT_ARGB8888,
		.len = sizeof(modifiers) / sizeof(modifiers[0]),
		.capacity = sizeof(modifiers) / sizeof(modifiers[0]),
		.modifiers = modifiers,
	};
	return &format;
}

static bool
prepare_mask_swapchain(int width, int height)
{
	struct material_renderer *renderer = &material_renderer;
	if (renderer->renderer != server.renderer) {
		if (renderer->mask_swapchain) {
			wlr_swapchain_destroy(renderer->mask_swapchain);
		}
		memset(renderer, 0, sizeof(*renderer));
		renderer->renderer = server.renderer;
	}

	if (renderer->mask_swapchain && renderer->width == width
			&& renderer->height == height) {
		return true;
	}

	if (renderer->mask_swapchain) {
		wlr_swapchain_destroy(renderer->mask_swapchain);
		renderer->mask_swapchain = NULL;
	}
	const struct wlr_drm_format *format = pick_mask_format();
	if (!format) {
		wlr_log(WLR_ERROR, "No alpha render format for background materials");
		return false;
	}

	renderer->mask_swapchain = wlr_swapchain_create(server.allocator,
		width, height, format);
	if (!renderer->mask_swapchain) {
		wlr_log(WLR_ERROR, "Failed to create background material mask buffer");
		return false;
	}
	renderer->width = width;
	renderer->height = height;
	return true;
}

static void
find_surface_buffer(struct wlr_scene_buffer *scene_buffer, int x, int y,
		void *data)
{
	struct surface_find_data *find = data;
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(scene_buffer);
	if (!find->scene_buffer && scene_surface
			&& scene_surface->surface == find->target) {
		find->scene_buffer = scene_buffer;
		find->x = x;
		find->y = y;
		find->z_index = find->next_z_index;
	}
	++find->next_z_index;
}

static int
compare_material_surfaces(const void *left, const void *right)
{
	const struct material_surface *a = left;
	const struct material_surface *b = right;
	return b->find.z_index - a->find.z_index;
}

static void
scene_buffer_size(struct wlr_scene_buffer *scene_buffer, int *width, int *height)
{
	if (scene_buffer->dst_width > 0 && scene_buffer->dst_height > 0) {
		*width = scene_buffer->dst_width;
		*height = scene_buffer->dst_height;
		return;
	}

	*width = scene_buffer->WLR_PRIVATE.buffer_width;
	*height = scene_buffer->WLR_PRIVATE.buffer_height;
	wlr_output_transform_coords(scene_buffer->transform, width, height);
}

static int
scale_length(int length, int offset, float scale)
{
	return round((offset + length) * scale) - round(offset * scale);
}

static void
scale_box(struct wlr_box *box, float scale)
{
	box->width = scale_length(box->width, box->x, scale);
	box->height = scale_length(box->height, box->y, scale);
	box->x = round(box->x * scale);
	box->y = round(box->y * scale);
}

static void
transform_output_box(struct wlr_box *box, struct wlr_output *output,
		int width, int height)
{
	int transformed_width = width;
	int transformed_height = height;
	wlr_output_transform_coords(output->transform,
		&transformed_width, &transformed_height);
	scale_box(box, output->scale);
	wlr_box_transform(box, box, wlr_output_transform_invert(output->transform),
		transformed_width, transformed_height);
}

static void
transform_output_vector(float *x, float *y, struct wlr_output *output)
{
	float source_x = *x * output->scale;
	float source_y = *y * output->scale;
	switch (wlr_output_transform_invert(output->transform)) {
	case WL_OUTPUT_TRANSFORM_NORMAL:
		*x = source_x;
		*y = source_y;
		break;
	case WL_OUTPUT_TRANSFORM_90:
		*x = -source_y;
		*y = source_x;
		break;
	case WL_OUTPUT_TRANSFORM_180:
		*x = -source_x;
		*y = -source_y;
		break;
	case WL_OUTPUT_TRANSFORM_270:
		*x = source_y;
		*y = -source_x;
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED:
		*x = -source_x;
		*y = source_y;
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
		*x = source_y;
		*y = source_x;
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
		*x = source_x;
		*y = -source_y;
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		*x = -source_y;
		*y = -source_x;
		break;
	}
}

static void
logical_region_to_buffer(pixman_region32_t *region, struct wlr_output *output,
		int width, int height)
{
	int transformed_width = width;
	int transformed_height = height;
	wlr_output_transform_coords(output->transform,
		&transformed_width, &transformed_height);
	wlr_region_scale(region, region, output->scale);
	wlr_region_transform(region, region,
		wlr_output_transform_invert(output->transform),
		transformed_width, transformed_height);
}

static bool
build_effect_region_logical(struct singularity_blur_entry *entry,
		struct surface_find_data *find, struct output *output,
		pixman_region32_t *region)
{
	int surface_width, surface_height;
	scene_buffer_size(find->scene_buffer, &surface_width, &surface_height);
	if (surface_width <= 0 || surface_height <= 0) {
		return false;
	}

	if (entry->region_set) {
		pixman_region32_copy(region, &entry->region);
		struct wlr_xdg_surface *xdg =
			wlr_xdg_surface_try_from_wlr_surface(entry->surface);
		if (xdg && xdg->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
			pixman_region32_translate(region,
				xdg->geometry.x, xdg->geometry.y);
		}
		pixman_region32_intersect_rect(region, region, 0, 0,
			surface_width, surface_height);
	} else {
		pixman_region32_union_rect(region, region, 0, 0,
			surface_width, surface_height);
	}
	pixman_region32_translate(region, find->x, find->y);
	pixman_region32_intersect(region, region,
		&find->scene_buffer->node.WLR_PRIVATE.visible);
	pixman_region32_translate(region,
		-output->scene_output->x, -output->scene_output->y);
	return pixman_region32_not_empty(region);
}

static bool
build_effect_region(struct singularity_blur_entry *entry,
		struct surface_find_data *find, struct output *output,
		struct wlr_buffer *output_buffer, pixman_region32_t *region)
{
	if (!build_effect_region_logical(entry, find, output, region)) {
		return false;
	}
	logical_region_to_buffer(region, output->wlr_output,
		output_buffer->width, output_buffer->height);
	pixman_region32_intersect_rect(region, region, 0, 0,
		output_buffer->width, output_buffer->height);
	return pixman_region32_not_empty(region);
}

void
singularity_blur_output_damage(struct output *output,
		pixman_region32_t *damage)
{
	struct singularity_blur_entry *entry;
	wl_list_for_each(entry, &blur_manager.entries, link) {
		if (!entry->surface || !mode_is_active(entry->mode)) {
			continue;
		}
		struct surface_find_data find = { .target = entry->surface };
		wlr_scene_node_for_each_buffer(&server.scene->tree.node,
			find_surface_buffer, &find);
		if (!find.scene_buffer) {
			continue;
		}
		pixman_region32_t region;
		pixman_region32_init(&region);
		if (build_effect_region_logical(entry, &find, output, &region)) {
			logical_region_to_buffer(&region, output->wlr_output,
				output->wlr_output->width, output->wlr_output->height);
			wlr_region_expand(&region, &region, 64);
			pixman_region32_intersect_rect(&region, &region, 0, 0,
				output->wlr_output->width, output->wlr_output->height);
			pixman_region32_union(damage, damage, &region);
		}
		pixman_region32_fini(&region);
	}
}

static struct wlr_texture *
get_surface_texture(struct wlr_scene_buffer *scene_buffer, bool *owned)
{
	*owned = false;
	if (scene_buffer->WLR_PRIVATE.texture) {
		return scene_buffer->WLR_PRIVATE.texture;
	}

	struct wlr_client_buffer *client_buffer =
		wlr_client_buffer_get(scene_buffer->buffer);
	if (client_buffer) {
		return client_buffer->texture;
	}

	struct wlr_texture *texture =
		wlr_texture_from_buffer(server.renderer, scene_buffer->buffer);
	*owned = texture != NULL;
	return texture;
}

static struct wlr_buffer *
render_surface_mask(struct surface_find_data *find, struct output *output,
		const pixman_region32_t *clip)
{
	struct wlr_buffer *mask =
		wlr_swapchain_acquire(material_renderer.mask_swapchain);
	if (!mask) {
		return NULL;
	}

	struct wlr_render_pass *pass =
		wlr_renderer_begin_buffer_pass(server.renderer, mask, NULL);
	if (!pass) {
		wlr_buffer_unlock(mask);
		return NULL;
	}
	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
		.box = { .width = mask->width, .height = mask->height },
		.color = {0},
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
	});

	bool owned = false;
	struct wlr_texture *texture = get_surface_texture(find->scene_buffer, &owned);
	if (!texture) {
		wlr_render_pass_submit(pass);
		wlr_buffer_unlock(mask);
		return NULL;
	}

	int surface_width, surface_height;
	scene_buffer_size(find->scene_buffer, &surface_width, &surface_height);
	struct wlr_box destination = {
		.x = find->x - output->scene_output->x,
		.y = find->y - output->scene_output->y,
		.width = surface_width,
		.height = surface_height,
	};
	transform_output_box(&destination, output->wlr_output,
		mask->width, mask->height);

	enum wl_output_transform transform =
		wlr_output_transform_invert(find->scene_buffer->transform);
	transform = wlr_output_transform_compose(transform,
		output->wlr_output->transform);
	wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
		.texture = texture,
		.src_box = find->scene_buffer->src_box,
		.dst_box = destination,
		.alpha = &find->scene_buffer->opacity,
		.clip = clip,
		.transform = transform,
		.filter_mode = find->scene_buffer->filter_mode,
		.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
		.transfer_function = find->scene_buffer->transfer_function,
		.color_encoding = find->scene_buffer->color_encoding,
		.color_range = find->scene_buffer->color_range,
		.wait_timeline = find->scene_buffer->WLR_PRIVATE.wait_timeline,
		.wait_point = find->scene_buffer->WLR_PRIVATE.wait_point,
	});
	bool submitted = wlr_render_pass_submit(pass);
	if (owned) {
		wlr_texture_destroy(texture);
	}
	if (!submitted) {
		wlr_buffer_unlock(mask);
		return NULL;
	}
	return mask;
}

static void
bind_texture(GLuint program, const char *name, GLuint texture, int unit)
{
	glActiveTexture(GL_TEXTURE0 + unit);
	glBindTexture(GL_TEXTURE_2D, texture);
	glUniform1i(glGetUniformLocation(program, name), unit);
}

static bool
bind_target(GLuint texture, int width, int height)
{
	glBindFramebuffer(GL_FRAMEBUFFER, material_renderer.framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		GL_TEXTURE_2D, texture, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		wlr_log(WLR_ERROR, "Incomplete background material framebuffer");
		return false;
	}
	glViewport(0, 0, width, height);
	return true;
}

static void
draw_quad(void)
{
	static const GLfloat vertices[] = {
		-1.0f, -1.0f,
		 1.0f, -1.0f,
		-1.0f,  1.0f,
		 1.0f,  1.0f,
	};
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray(0);
}

static bool
set_target_clip(const pixman_region32_t *clip, int width, int height,
		int padding)
{
	const pixman_box32_t *extents = pixman_region32_extents(clip);
	float scale_x = (float)width / material_renderer.width;
	float scale_y = (float)height / material_renderer.height;
	int x1 = floorf(extents->x1 * scale_x) - ceilf(padding * scale_x);
	int y1 = floorf(extents->y1 * scale_y) - ceilf(padding * scale_y);
	int x2 = ceilf(extents->x2 * scale_x) + ceilf(padding * scale_x);
	int y2 = ceilf(extents->y2 * scale_y) + ceilf(padding * scale_y);
	x1 = x1 < 0 ? 0 : x1;
	y1 = y1 < 0 ? 0 : y1;
	x2 = x2 > width ? width : x2;
	y2 = y2 > height ? height : y2;
	if (x1 >= x2 || y1 >= y2) {
		return false;
	}
	glEnable(GL_SCISSOR_TEST);
	glScissor(x1, height - y2, x2 - x1, y2 - y1);
	return true;
}

static bool
render_reconstructed_backdrop(const pixman_region32_t *clip)
{
	int width = material_renderer.width;
	int height = material_renderer.height;
	if (!bind_target(material_renderer.reconstruct_texture, width, height)) {
		return false;
	}
	if (!set_target_clip(clip, width, height, 64)) {
		return false;
	}
	glUseProgram(material_renderer.reconstruct_program);
	bind_texture(material_renderer.reconstruct_program, "scene_texture",
		material_renderer.scene_texture, 0);
	bind_texture(material_renderer.reconstruct_program, "surface_texture",
		material_renderer.mask_texture, 1);
	draw_quad();
	glDisable(GL_SCISSOR_TEST);
	return true;
}

static bool
render_blur(uint32_t mode, uint32_t strength,
		const pixman_region32_t *clip)
{
	int width = (material_renderer.width + 1) / 2;
	int height = (material_renderer.height + 1) / 2;
	int iterations = 1;
	float radius = strength / 100.0f * 2.083333f;
	GLuint source = material_renderer.reconstruct_texture;

	glUseProgram(material_renderer.blur_program);
	for (int i = 0; i < iterations; ++i) {
		if (!bind_target(material_renderer.blur_texture[0], width, height)) {
			return false;
		}
		if (!set_target_clip(clip, width, height, 64)) {
			return false;
		}
		bind_texture(material_renderer.blur_program, "source_texture", source, 0);
		glUniform2f(glGetUniformLocation(material_renderer.blur_program, "direction"),
			radius / width, 0.0f);
		draw_quad();

		if (!bind_target(material_renderer.blur_texture[1], width, height)) {
			return false;
		}
		if (!set_target_clip(clip, width, height, 64)) {
			return false;
		}
		bind_texture(material_renderer.blur_program, "source_texture",
			material_renderer.blur_texture[0], 0);
		glUniform2f(glGetUniformLocation(material_renderer.blur_program, "direction"),
			0.0f, radius / height);
		draw_quad();
		source = material_renderer.blur_texture[1];
	}
	glDisable(GL_SCISSOR_TEST);
	return true;
}

static bool
render_distance_field(const pixman_region32_t *clip)
{
	const int passes = 12;
	int width = (material_renderer.width + 3) / 4;
	int height = (material_renderer.height + 3) / 4;
	if (!bind_target(material_renderer.distance_texture[0], width, height)) {
		return false;
	}
	if (!set_target_clip(clip, width, height, 64)) {
		return false;
	}
	glUseProgram(material_renderer.distance_seed_program);
	bind_texture(material_renderer.distance_seed_program, "surface_texture",
		material_renderer.mask_texture, 0);
	draw_quad();

	glUseProgram(material_renderer.distance_program);
	for (int i = 0; i < passes; ++i) {
		GLuint source = material_renderer.distance_texture[i & 1];
		GLuint target = material_renderer.distance_texture[(i + 1) & 1];
		if (!bind_target(target, width, height)) {
			return false;
		}
		if (!set_target_clip(clip, width, height, 64)) {
			return false;
		}
		bind_texture(material_renderer.distance_program,
			"source_texture", source, 0);
		glUniform2f(glGetUniformLocation(material_renderer.distance_program,
			"pixel_size"), 1.0f / width, 1.0f / height);
		glUniform1f(glGetUniformLocation(material_renderer.distance_program,
			"distance_step"), 1.0f / passes);
		draw_quad();
	}
	glDisable(GL_SCISSOR_TEST);
	return true;
}

static void
render_composite(GLuint output_fbo, uint32_t mode,
		uint32_t strength, float motion_x, float motion_y,
		const pixman_region32_t *clip)
{
	int distance_width = (material_renderer.width + 3) / 4;
	int distance_height = (material_renderer.height + 3) / 4;
	glBindFramebuffer(GL_FRAMEBUFFER, output_fbo);
	glViewport(0, 0, material_renderer.width, material_renderer.height);
	glUseProgram(material_renderer.compose_program);
	bind_texture(material_renderer.compose_program, "scene_texture",
		material_renderer.scene_texture, 0);
	bind_texture(material_renderer.compose_program, "surface_texture",
		material_renderer.mask_texture, 1);
	bind_texture(material_renderer.compose_program, "reconstruct_texture",
		material_renderer.reconstruct_texture, 2);
	bind_texture(material_renderer.compose_program, "blurred_texture",
		material_renderer.blur_texture[1], 3);
	bind_texture(material_renderer.compose_program, "distance_texture",
		material_renderer.distance_texture[0], 4);
	glUniform2f(glGetUniformLocation(material_renderer.compose_program, "pixel_size"),
		1.0f / material_renderer.width, 1.0f / material_renderer.height);
	glUniform2f(glGetUniformLocation(material_renderer.compose_program,
		"distance_pixel"), 1.0f / distance_width, 1.0f / distance_height);
	glUniform2f(glGetUniformLocation(material_renderer.compose_program,
		"motion_pixels"), motion_x, motion_y);
	glUniform1f(glGetUniformLocation(material_renderer.compose_program, "mode"),
		(float)mode);
	glUniform1f(glGetUniformLocation(material_renderer.compose_program,
		"effect_strength"), strength / 100.0f);

	glEnable(GL_SCISSOR_TEST);
	int count = 0;
	pixman_box32_t *rectangles = pixman_region32_rectangles(clip, &count);
	for (int i = 0; i < count; ++i) {
		int width = rectangles[i].x2 - rectangles[i].x1;
		int height = rectangles[i].y2 - rectangles[i].y1;
		glScissor(rectangles[i].x1,
			material_renderer.height - rectangles[i].y2,
			width, height);
		draw_quad();
	}
	glDisable(GL_SCISSOR_TEST);
}

static bool
apply_material(struct singularity_blur_entry *entry,
		struct surface_find_data *find, struct output *output,
		struct wlr_buffer *output_buffer, pixman_region32_t *damage,
		pixman_region32_t *covered)
{
	pixman_region32_t clip;
	pixman_region32_init(&clip);
	if (!build_effect_region(entry, find, output, output_buffer, &clip)) {
		pixman_region32_fini(&clip);
		return false;
	}
	pixman_region32_t full_clip;
	pixman_region32_init(&full_clip);
	pixman_region32_copy(&full_clip, &clip);
	pixman_region32_subtract(&clip, &clip, covered);
	pixman_region32_union(covered, covered, &full_clip);
	pixman_region32_fini(&full_clip);
	if (!pixman_region32_not_empty(&clip)) {
		pixman_region32_fini(&clip);
		return false;
	}

	struct wlr_buffer *mask = render_surface_mask(find, output, &clip);
	if (!mask) {
		pixman_region32_fini(&clip);
		return false;
	}
	GLuint output_fbo = wlr_gles2_renderer_get_buffer_fbo(
		server.renderer, output_buffer);
	GLuint mask_fbo = wlr_gles2_renderer_get_buffer_fbo(server.renderer, mask);
	if (!output_fbo || !mask_fbo) {
		wlr_buffer_unlock(mask);
		pixman_region32_fini(&clip);
		return false;
	}

	struct wlr_egl *egl = wlr_gles2_renderer_get_egl(server.renderer);
	EGLDisplay egl_display = wlr_egl_get_display(egl);
	EGLContext previous_context = eglGetCurrentContext();
	EGLDisplay previous_display = eglGetCurrentDisplay();
	EGLSurface previous_draw = eglGetCurrentSurface(EGL_DRAW);
	EGLSurface previous_read = eglGetCurrentSurface(EGL_READ);
	if (!eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
			wlr_egl_get_context(egl))) {
		wlr_log(WLR_ERROR, "Failed to activate GLES context for background material");
		wlr_buffer_unlock(mask);
		pixman_region32_fini(&clip);
		return false;
	}

	bool rendered = prepare_gl_resources(output_buffer->width, output_buffer->height);
	if (rendered) {
		glDisable(GL_BLEND);
		glDisable(GL_SCISSOR_TEST);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, material_renderer.scene_texture);
		glBindFramebuffer(GL_FRAMEBUFFER, output_fbo);
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0,
			output_buffer->width, output_buffer->height);
		glBindTexture(GL_TEXTURE_2D, material_renderer.mask_texture);
		glBindFramebuffer(GL_FRAMEBUFFER, mask_fbo);
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0,
			mask->width, mask->height);
		rendered = render_reconstructed_backdrop(&clip)
			&& render_blur(entry->mode, entry->strength, &clip)
			&& (entry->mode != ZSINGULARITY_BLUR_V1_MODE_GLASS
				|| render_distance_field(&clip));
		if (rendered) {
			float motion_x = entry->motion_x;
			float motion_y = entry->motion_y;
			transform_output_vector(&motion_x, &motion_y,
				output->wlr_output);
			render_composite(output_fbo, entry->mode, entry->strength,
				motion_x, motion_y, &clip);
			pixman_region32_union(damage, damage, &clip);
		}
	}

	for (int i = 0; i < 5; ++i) {
		glActiveTexture(GL_TEXTURE0 + i);
		glBindTexture(GL_TEXTURE_2D, 0);
	}
	glActiveTexture(GL_TEXTURE0);
	glUseProgram(0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (previous_display != EGL_NO_DISPLAY) {
		eglMakeCurrent(previous_display, previous_draw, previous_read,
			previous_context);
	} else {
		eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
			EGL_NO_CONTEXT);
	}

	wlr_buffer_unlock(mask);
	pixman_region32_fini(&clip);
	return rendered;
}

void
singularity_blur_render(struct output *output, struct wlr_output_state *state)
{
	assert(state->buffer);
	if (!singularity_blur_has_effects()
			|| !prepare_mask_swapchain(state->buffer->width, state->buffer->height)) {
		return;
	}

	pixman_region32_t damage;
	pixman_region32_init(&damage);
	pixman_region32_t covered;
	pixman_region32_init(&covered);
	bool animate_output = false;
	if (state->committed & WLR_OUTPUT_STATE_DAMAGE) {
		pixman_region32_copy(&damage, &state->damage);
	}

	size_t capacity = wl_list_length(&blur_manager.entries);
	struct material_surface *surfaces = calloc(capacity, sizeof(*surfaces));
	if (!surfaces) {
		pixman_region32_fini(&covered);
		pixman_region32_fini(&damage);
		return;
	}
	size_t count = 0;
	struct singularity_blur_entry *entry;
	wl_list_for_each(entry, &blur_manager.entries, link) {
		if (!entry->surface || !mode_is_active(entry->mode)) {
			continue;
		}
		struct surface_find_data find = {
			.target = entry->surface,
			.z_index = -1,
		};
		wlr_scene_node_for_each_buffer(&server.scene->tree.node,
			find_surface_buffer, &find);
		if (find.scene_buffer) {
			surfaces[count++] = (struct material_surface) {
				.entry = entry,
				.find = find,
			};
		}
	}
	qsort(surfaces, count, sizeof(*surfaces), compare_material_surfaces);
	for (size_t i = 0; i < count; ++i) {
		entry = surfaces[i].entry;
		struct surface_find_data *find = &surfaces[i].find;
		scene_buffer_size(find->scene_buffer,
			&entry->last_width, &entry->last_height);
		if (entry->mode == ZSINGULARITY_BLUR_V1_MODE_GLASS) {
			update_motion(entry, find);
		} else if (entry->position_valid || entry->motion_active) {
			reset_motion(entry);
		}
		if (apply_material(entry, find, output, state->buffer,
				&damage, &covered) && entry->motion_active) {
			animate_output = true;
		}
	}
	free(surfaces);

	wlr_output_state_set_damage(state, &damage);
	pixman_region32_fini(&covered);
	pixman_region32_fini(&damage);
	if (animate_output) {
		wlr_output_schedule_frame(output->wlr_output);
	}
}

static void
handle_surface_destroy(struct wl_listener *listener, void *data)
{
	(void)data;
	struct singularity_blur_entry *entry =
		wl_container_of(listener, entry, surface_destroy);
	entry->surface = NULL;
	wl_list_remove(&entry->surface_destroy.link);
	wl_list_init(&entry->surface_destroy.link);
	schedule_effect_frame();
}

static void
blur_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
blur_handle_set_radius(struct wl_client *client, struct wl_resource *resource,
		uint32_t radius)
{
	(void)client;
	struct singularity_blur_entry *entry = wl_resource_get_user_data(resource);
	if (entry) {
		entry->pending_radius = radius;
	}
}

static void
blur_handle_set_noise(struct wl_client *client, struct wl_resource *resource,
		uint32_t noise)
{
	(void)client;
	struct singularity_blur_entry *entry = wl_resource_get_user_data(resource);
	if (entry) {
		entry->pending_noise = noise;
	}
}

static void
blur_handle_set_mode(struct wl_client *client, struct wl_resource *resource,
		uint32_t mode)
{
	(void)client;
	struct singularity_blur_entry *entry = wl_resource_get_user_data(resource);
	if (entry) {
		entry->pending_mode = mode <= ZSINGULARITY_BLUR_V1_MODE_BLUR
			? mode : ZSINGULARITY_BLUR_V1_MODE_DISABLED;
	}
}

static void
blur_handle_set_region(struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *region_resource)
{
	(void)client;
	struct singularity_blur_entry *entry = wl_resource_get_user_data(resource);
	if (!entry) {
		return;
	}
	entry->pending_region_set = region_resource != NULL;
	if (region_resource) {
		pixman_region32_copy(&entry->pending_region,
			wlr_region_from_resource(region_resource));
	} else {
		pixman_region32_clear(&entry->pending_region);
	}
}

static void
blur_handle_set_strength(struct wl_client *client, struct wl_resource *resource,
		uint32_t strength)
{
	(void)client;
	struct singularity_blur_entry *entry = wl_resource_get_user_data(resource);
	if (entry) {
		entry->pending_strength = strength > 100 ? 100 : strength;
	}
}

static void
blur_handle_commit(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	struct singularity_blur_entry *entry = wl_resource_get_user_data(resource);
	if (!entry) {
		return;
	}
	uint32_t previous_mode = entry->mode;
	entry->radius = entry->pending_radius;
	entry->noise = entry->pending_noise;
	entry->strength = entry->pending_strength;
	entry->mode = wl_resource_get_version(resource) >= 2
		? entry->pending_mode
		: (entry->radius > 0 ? ZSINGULARITY_BLUR_V1_MODE_BLUR
			: ZSINGULARITY_BLUR_V1_MODE_DISABLED);
	entry->region_set = entry->pending_region_set;
	pixman_region32_copy(&entry->region, &entry->pending_region);
	if (entry->mode != previous_mode) {
		reset_motion(entry);
	}
	schedule_effect_frame();
}

static const struct zsingularity_blur_v1_interface blur_impl = {
	.destroy = blur_handle_destroy,
	.set_radius = blur_handle_set_radius,
	.set_noise = blur_handle_set_noise,
	.commit = blur_handle_commit,
	.set_mode = blur_handle_set_mode,
	.set_region = blur_handle_set_region,
	.set_strength = blur_handle_set_strength,
};

static void
blur_resource_destroy(struct wl_resource *resource)
{
	struct singularity_blur_entry *entry = wl_resource_get_user_data(resource);
	if (!entry) {
		return;
	}
	bool was_active = mode_is_active(entry->mode);
	wl_list_remove(&entry->link);
	if (entry->surface) {
		wl_list_remove(&entry->surface_destroy.link);
	}
	pixman_region32_fini(&entry->pending_region);
	pixman_region32_fini(&entry->region);
	free(entry);
	if (was_active) {
		schedule_effect_frame();
	}
}

static void
manager_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
manager_handle_get_blur(struct wl_client *client, struct wl_resource *resource,
		uint32_t id, struct wl_resource *surface_resource)
{
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	if (!surface) {
		wl_resource_post_error(resource,
			ZSINGULARITY_BLUR_MANAGER_V1_ERROR_INVALID_SURFACE,
			"invalid surface");
		return;
	}

	struct singularity_blur_entry *existing;
	wl_list_for_each(existing, &blur_manager.entries, link) {
		if (existing->surface == surface) {
			wl_resource_post_error(resource,
				ZSINGULARITY_BLUR_MANAGER_V1_ERROR_EFFECT_EXISTS,
				"surface already has a background effect");
			return;
		}
	}

	struct singularity_blur_entry *entry = calloc(1, sizeof(*entry));
	if (!entry) {
		wl_client_post_no_memory(client);
		return;
	}
	entry->resource = wl_resource_create(client,
		&zsingularity_blur_v1_interface,
		wl_resource_get_version(resource), id);
	if (!entry->resource) {
		free(entry);
		wl_client_post_no_memory(client);
		return;
	}
	pixman_region32_init(&entry->pending_region);
	pixman_region32_init(&entry->region);
	entry->pending_strength = 60;
	entry->strength = 60;
	wl_resource_set_implementation(entry->resource, &blur_impl, entry,
		blur_resource_destroy);

	entry->surface = surface;
	entry->surface_destroy.notify = handle_surface_destroy;
	wl_signal_add(&surface->events.destroy, &entry->surface_destroy);
	wl_list_insert(&blur_manager.entries, &entry->link);
}

static const struct zsingularity_blur_manager_v1_interface manager_impl = {
	.destroy = manager_handle_destroy,
	.get_blur = manager_handle_get_blur,
};

static void
bind_manager(struct wl_client *client, void *data, uint32_t version,
		uint32_t id)
{
	uint32_t supported_version = version < 3 ? version : 3;
	struct wl_resource *resource = wl_resource_create(client,
		&zsingularity_blur_manager_v1_interface, supported_version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}

void
singularity_blur_init(void)
{
	wl_list_init(&blur_manager.entries);
	blur_manager.global = wl_global_create(server.wl_display,
		&zsingularity_blur_manager_v1_interface, 3,
		&blur_manager, bind_manager);
	if (!blur_manager.global) {
		wlr_log(WLR_ERROR, "Failed to create background material manager");
	}
}

void
singularity_blur_finish(void)
{
	struct material_renderer *renderer = &material_renderer;
	if (renderer->mask_swapchain) {
		wlr_swapchain_destroy(renderer->mask_swapchain);
		renderer->mask_swapchain = NULL;
	}
	if (!renderer->renderer || !wlr_renderer_is_gles2(renderer->renderer)) {
		return;
	}

	struct wlr_egl *egl = wlr_gles2_renderer_get_egl(renderer->renderer);
	EGLDisplay egl_display = wlr_egl_get_display(egl);
	EGLContext previous_context = eglGetCurrentContext();
	EGLDisplay previous_display = eglGetCurrentDisplay();
	EGLSurface previous_draw = eglGetCurrentSurface(EGL_DRAW);
	EGLSurface previous_read = eglGetCurrentSurface(EGL_READ);
	if (!eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
			wlr_egl_get_context(egl))) {
		return;
	}
	glDeleteFramebuffers(1, &renderer->framebuffer);
	glDeleteTextures(1, &renderer->scene_texture);
	glDeleteTextures(1, &renderer->mask_texture);
	glDeleteTextures(1, &renderer->reconstruct_texture);
	glDeleteTextures(2, renderer->blur_texture);
	glDeleteTextures(2, renderer->distance_texture);
	glDeleteProgram(renderer->reconstruct_program);
	glDeleteProgram(renderer->blur_program);
	glDeleteProgram(renderer->distance_seed_program);
	glDeleteProgram(renderer->distance_program);
	glDeleteProgram(renderer->compose_program);
	if (previous_display != EGL_NO_DISPLAY) {
		eglMakeCurrent(previous_display, previous_draw, previous_read,
			previous_context);
	} else {
		eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
			EGL_NO_CONTEXT);
	}
	memset(renderer, 0, sizeof(*renderer));
}
