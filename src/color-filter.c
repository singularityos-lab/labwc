// SPDX-License-Identifier: GPL-2.0-only
#include "config.h"
#include "color-filter.h"
#include <drm_fourcc.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <scenefx/types/wlr_scene.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#include "common/scene-helpers.h"
#include "config/rcxml.h"
#include "labwc.h"
#include "magnifier.h"
#include "output.h"

#if HAVE_GLES2
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#endif

const float color_filter_invert[12] = {
	-1, 0, 0, 1,
	0, -1, 0, 1,
	0, 0, -1, 1,
};

const float color_filter_invert_lightness[12] = {
	0.574f, -1.430f, -0.144f, 1,
	-0.426f, -0.430f, -0.144f, 1,
	-0.426f, -1.430f, 0.856f, 1,
};

static const float grayscale[12] = {
	0.2126f, 0.7152f, 0.0722f, 0,
	0.2126f, 0.7152f, 0.0722f, 0,
	0.2126f, 0.7152f, 0.0722f, 0,
};

static const float rgb_to_lms[9] = {
	17.8824f, 43.5161f, 4.11935f,
	3.45565f, 27.1554f, 3.86714f,
	0.0299566f, 0.184309f, 1.46709f,
};

static const float lms_to_rgb[9] = {
	0.0809444479f, -0.130504409f, 0.116721066f,
	-0.0102485335f, 0.0540193266f, -0.113614708f,
	-0.000365296938f, -0.00412161469f, 0.693511405f,
};

static const float protanopia[9] = {
	0, 2.02344f, -2.52581f,
	0, 1, 0,
	0, 0, 1,
};

static const float deuteranopia[9] = {
	1, 0, 0,
	0.494207f, 0, 1.24827f,
	0, 0, 1,
};

static const float tritanopia[9] = {
	1, 0, 0,
	0, 1, 0,
	-0.395913f, 0.801109f, 0,
};

static const float error_shift[9] = {
	0, 0, 0,
	0.7f, 1, 0,
	0.7f, 0, 1,
};

static float active_matrix[12];
static bool active;

static void
multiply(float out[static 9], const float a[static 9], const float b[static 9])
{
	float result[9];
	for (int row = 0; row < 3; row++) {
		for (int col = 0; col < 3; col++) {
			result[row * 3 + col] = a[row * 3] * b[col]
				+ a[row * 3 + 1] * b[3 + col]
				+ a[row * 3 + 2] * b[6 + col];
		}
	}
	memcpy(out, result, sizeof(result));
}

static void
build_correction(float out[static 12], const float simulation[static 9])
{
	float sim[9];
	multiply(sim, simulation, rgb_to_lms);
	multiply(sim, lms_to_rgb, sim);

	float error[9];
	for (int i = 0; i < 9; i++) {
		error[i] = (i % 4 == 0 ? 1.0f : 0.0f) - sim[i];
	}
	float shift[9];
	multiply(shift, error_shift, error);

	for (int row = 0; row < 3; row++) {
		for (int col = 0; col < 3; col++) {
			out[row * 4 + col] = (row == col ? 1.0f : 0.0f)
				+ shift[row * 3 + col];
		}
		out[row * 4 + 3] = 0;
	}
}

bool
color_filter_active(void)
{
	return active;
}

void
color_filter_update_direct_scanout(void)
{
	server.scene->WLR_PRIVATE.direct_scanout = server.direct_scanout_enabled
		&& !active && !magnifier_is_enabled();
}

static void
damage_all_outputs(void)
{
	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (output->scene_output) {
			lab_wlr_scene_output_damage_whole(output->scene_output);
			wlr_output_schedule_frame(output->wlr_output);
		}
	}
}

void
color_filter_reconfigure(void)
{
	static enum a11y_color_filter current = LAB_A11Y_COLOR_FILTER_NONE;
	if (current == rc.color_filter) {
		return;
	}
	current = rc.color_filter;
	active = true;
	switch (rc.color_filter) {
	case LAB_A11Y_COLOR_FILTER_INVERT:
		memcpy(active_matrix, color_filter_invert, sizeof(active_matrix));
		break;
	case LAB_A11Y_COLOR_FILTER_GRAYSCALE:
		memcpy(active_matrix, grayscale, sizeof(active_matrix));
		break;
	case LAB_A11Y_COLOR_FILTER_PROTANOPIA:
		build_correction(active_matrix, protanopia);
		break;
	case LAB_A11Y_COLOR_FILTER_DEUTERANOPIA:
		build_correction(active_matrix, deuteranopia);
		break;
	case LAB_A11Y_COLOR_FILTER_TRITANOPIA:
		build_correction(active_matrix, tritanopia);
		break;
	default:
		active = false;
		break;
	}
	color_filter_update_direct_scanout();
	damage_all_outputs();
}

static bool
format_is_8888(uint32_t format, bool *swap_rb)
{
	switch (format) {
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XRGB8888:
		*swap_rb = true;
		return true;
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_XBGR8888:
		*swap_rb = false;
		return true;
	default:
		return false;
	}
}

static uint8_t
clamp_channel(float value)
{
	if (value <= 0.0f) {
		return 0;
	}
	if (value >= 1.0f) {
		return 255;
	}
	return (uint8_t)(value * 255.0f + 0.5f);
}

static void
apply_cpu(struct wlr_buffer *buffer, const struct wlr_box *box,
		const float m[static 12])
{
	struct wlr_texture *source = wlr_texture_from_buffer(server.renderer, buffer);
	if (!source) {
		return;
	}
	uint32_t format = wlr_texture_preferred_read_format(source);
	bool swap_rb;
	if (!format_is_8888(format, &swap_rb)) {
		wlr_log(WLR_ERROR, "color filter: unsupported read format 0x%08x",
			format);
		wlr_texture_destroy(source);
		return;
	}

	uint32_t stride = box->width * 4;
	uint8_t *data = malloc((size_t)stride * box->height);
	if (!data) {
		wlr_texture_destroy(source);
		return;
	}
	bool ok = wlr_texture_read_pixels(source,
		&(struct wlr_texture_read_pixels_options){
			.data = data,
			.format = format,
			.stride = stride,
			.src_box = *box,
		});
	wlr_texture_destroy(source);
	if (!ok) {
		free(data);
		return;
	}

	int r_index = swap_rb ? 2 : 0;
	int b_index = swap_rb ? 0 : 2;
	size_t pixels = (size_t)box->width * box->height;
	for (size_t i = 0; i < pixels; i++) {
		uint8_t *px = data + i * 4;
		float r = px[r_index] / 255.0f;
		float g = px[1] / 255.0f;
		float b = px[b_index] / 255.0f;
		px[r_index] = clamp_channel(m[0] * r + m[1] * g + m[2] * b + m[3]);
		px[1] = clamp_channel(m[4] * r + m[5] * g + m[6] * b + m[7]);
		px[b_index] = clamp_channel(m[8] * r + m[9] * g + m[10] * b + m[11]);
	}

	struct wlr_texture *filtered = wlr_texture_from_pixels(server.renderer,
		format, stride, box->width, box->height, data);
	free(data);
	if (!filtered) {
		return;
	}
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server.renderer, buffer, NULL);
	if (pass) {
		wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
			.texture = filtered,
			.dst_box = *box,
			.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
		});
		wlr_render_pass_submit(pass);
	}
	wlr_texture_destroy(filtered);
}

#if HAVE_GLES2
static const char vertex_source[] =
	"attribute vec2 pos;\n"
	"varying vec2 uv;\n"
	"void main() {\n"
	"	uv = pos;\n"
	"	gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

static const char fragment_source[] =
	"precision mediump float;\n"
	"uniform sampler2D tex;\n"
	"uniform mat3 matrix;\n"
	"uniform vec3 offset;\n"
	"varying vec2 uv;\n"
	"void main() {\n"
	"	vec3 color = texture2D(tex, uv).rgb;\n"
	"	gl_FragColor = vec4(clamp(matrix * color + offset, 0.0, 1.0), 1.0);\n"
	"}\n";

static struct {
	bool failed;
	GLuint program;
	GLuint texture;
	GLint pos;
	GLint tex;
	GLint matrix;
	GLint offset;
	int width;
	int height;
} gl;

static GLuint
compile_shader(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	GLint ok = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static bool
gl_setup(void)
{
	if (gl.program) {
		return true;
	}
	GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_source);
	GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
	if (!vs || !fs) {
		glDeleteShader(vs);
		glDeleteShader(fs);
		return false;
	}
	GLuint program = glCreateProgram();
	glAttachShader(program, vs);
	glAttachShader(program, fs);
	glLinkProgram(program);
	glDeleteShader(vs);
	glDeleteShader(fs);
	GLint ok = GL_FALSE;
	glGetProgramiv(program, GL_LINK_STATUS, &ok);
	if (!ok) {
		glDeleteProgram(program);
		return false;
	}
	gl.program = program;
	gl.pos = glGetAttribLocation(program, "pos");
	gl.tex = glGetUniformLocation(program, "tex");
	gl.matrix = glGetUniformLocation(program, "matrix");
	gl.offset = glGetUniformLocation(program, "offset");
	glGenTextures(1, &gl.texture);
	return true;
}

static bool
apply_gl(struct wlr_buffer *buffer, const struct wlr_box *box,
		const float m[static 12])
{
	if (gl.failed) {
		return false;
	}
	GLuint fbo = wlr_gles2_renderer_get_buffer_fbo(server.renderer, buffer);
	if (!fbo) {
		return false;
	}

	struct wlr_egl *egl = wlr_gles2_renderer_get_egl(server.renderer);
	EGLDisplay display = wlr_egl_get_display(egl);
	EGLDisplay prev_display = eglGetCurrentDisplay();
	EGLContext prev_context = eglGetCurrentContext();
	EGLSurface prev_draw = eglGetCurrentSurface(EGL_DRAW);
	EGLSurface prev_read = eglGetCurrentSurface(EGL_READ);
	if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE,
			wlr_egl_get_context(egl))) {
		return false;
	}

	bool done = false;
	while (glGetError() != GL_NO_ERROR) {
	}
	if (!gl_setup()) {
		wlr_log(WLR_ERROR, "color filter: shader setup failed");
		gl.failed = true;
		goto restore;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, gl.texture);
	if (gl.width != box->width || gl.height != box->height) {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, box->width, box->height,
			0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		gl.width = box->width;
		gl.height = box->height;
	}
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, box->x, box->y,
		box->width, box->height);

	const GLfloat matrix[9] = {
		m[0], m[4], m[8],
		m[1], m[5], m[9],
		m[2], m[6], m[10],
	};
	static const GLfloat quad[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };

	GLboolean blend = glIsEnabled(GL_BLEND);
	GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glViewport(box->x, box->y, box->width, box->height);
	glUseProgram(gl.program);
	glUniform1i(gl.tex, 0);
	glUniformMatrix3fv(gl.matrix, 1, GL_FALSE, matrix);
	glUniform3f(gl.offset, m[3], m[7], m[11]);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glVertexAttribPointer(gl.pos, 2, GL_FLOAT, GL_FALSE, 0, quad);
	glEnableVertexAttribArray(gl.pos);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray(gl.pos);
	glUseProgram(0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (blend) {
		glEnable(GL_BLEND);
	}
	if (scissor) {
		glEnable(GL_SCISSOR_TEST);
	}
	glFinish();
	if (glGetError() != GL_NO_ERROR) {
		wlr_log(WLR_ERROR, "color filter: GL error, using the CPU path");
		gl.failed = true;
	}
	done = true;

restore:
	if (prev_context != EGL_NO_CONTEXT) {
		eglMakeCurrent(prev_display, prev_draw, prev_read, prev_context);
	} else {
		eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE,
			EGL_NO_CONTEXT);
	}
	return done;
}
#endif

void
color_filter_apply(struct wlr_buffer *buffer, const struct wlr_box *box,
		const float matrix[static 12])
{
	struct wlr_box clipped;
	struct wlr_box bounds = {
		.width = buffer->width,
		.height = buffer->height,
	};
	if (!wlr_box_intersection(&clipped, box, &bounds)) {
		return;
	}
#if HAVE_GLES2
	if (wlr_renderer_is_gles2(server.renderer)
			&& apply_gl(buffer, &clipped, matrix)) {
		return;
	}
#endif
	apply_cpu(buffer, &clipped, matrix);
}

void
color_filter_draw(struct wlr_buffer *buffer)
{
	if (!active) {
		return;
	}
	struct wlr_box box = {
		.width = buffer->width,
		.height = buffer->height,
	};
	color_filter_apply(buffer, &box, active_matrix);
}
