// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/render/color.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>
#include "common/mem.h"
#include "labwc.h"
#include "output.h"
#include "output-config.h"

static bool
read_file(const char *path, void **data, size_t *size)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	bool ok = false;
	if (fseek(f, 0, SEEK_END) == 0) {
		long len = ftell(f);
		if (len > 128 && len < 16 * 1024 * 1024 && fseek(f, 0, SEEK_SET) == 0) {
			*data = xmalloc(len);
			if (fread(*data, 1, len, f) == (size_t)len) {
				*size = len;
				ok = true;
			} else {
				free(*data);
				*data = NULL;
			}
		}
	}
	fclose(f);
	return ok;
}

void
output_color_release(struct output *output)
{
	if (output->icc_transform) {
		wlr_color_transform_unref(output->icc_transform);
		output->icc_transform = NULL;
	}
	zfree(output->icc_path);
	output->icc_error = NULL;
}

void
output_color_load_profile(struct output *output)
{
	const struct output_config *config =
		output_config_find(output->wlr_output->name);
	const char *path = config ? config->icc_profile : NULL;
	if (path && output->icc_path && !strcmp(path, output->icc_path)
			&& (output->icc_transform || output->icc_error)) {
		return;
	}
	output_color_release(output);
	if (!path) {
		return;
	}
	output->icc_path = xstrdup(path);
	if (!server.renderer->features.output_color_transform) {
		output->icc_error = "renderer does not support output color transforms";
		wlr_log(WLR_INFO, "Cannot apply color profile on output %s: %s",
			output->wlr_output->name, output->icc_error);
		return;
	}
	void *data = NULL;
	size_t size = 0;
	if (!read_file(path, &data, &size)) {
		output->icc_error = "profile file cannot be read";
		wlr_log(WLR_ERROR, "Cannot read color profile %s", path);
		return;
	}
	output->icc_transform = wlr_color_transform_init_linear_to_icc(data, size);
	free(data);
	if (!output->icc_transform) {
		output->icc_error = "profile is not a valid display profile";
		wlr_log(WLR_ERROR, "Invalid color profile %s", path);
		return;
	}
	wlr_log(WLR_INFO, "Applied color profile %s on output %s",
		path, output->wlr_output->name);
}

struct wlr_color_transform *
output_color_transform(struct output *output)
{
	if (!output || !output->icc_transform) {
		return NULL;
	}
	struct wlr_output *wlr_output = output->wlr_output;
	if (output->pending.committed & WLR_OUTPUT_STATE_IMAGE_DESCRIPTION) {
		if (output->pending.image_description) {
			return NULL;
		}
	} else if (wlr_output->image_description) {
		return NULL;
	}
	return output->icc_transform;
}

void
output_color_reconfigure(void)
{
	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (!output->wlr_output->enabled) {
			continue;
		}
		output_color_load_profile(output);
		output_state_setup_hdr(output, false);
		wlr_output_schedule_frame(output->wlr_output);
	}
	output_color_write_state();
}

static const char *
yes_no(bool value)
{
	return value ? "true" : "false";
}

void
output_color_write_state(void)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	const char *socket = getenv("WAYLAND_DISPLAY");
	if (!runtime || !socket || strchr(socket, '/')) {
		return;
	}
	char path[4096], tmp[4200];
	snprintf(path, sizeof(path), "%s/labwc-color-%s.ini", runtime, socket);
	snprintf(tmp, sizeof(tmp), "%s.%d", path, (int)getpid());
	FILE *f = fopen(tmp, "w");
	if (!f) {
		return;
	}
	bool icc_capable = server.renderer->features.output_color_transform;
	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		const struct output_config *config =
			output_config_find(output->wlr_output->name);
		fprintf(f, "[%s]\n", output->wlr_output->name);
		fprintf(f, "HdrSupported=%s\n", yes_no(output->hdr_supported));
		fprintf(f, "HdrReason=%s\n", output->hdr_unsupported_reason
			? output->hdr_unsupported_reason : "");
		fprintf(f, "HdrActive=%s\n", yes_no(output->hdr_active));
		fprintf(f, "SdrBrightnessSupported=false\n");
		fprintf(f, "SdrBrightness=%d\n",
			config && config->sdr_brightness ? config->sdr_brightness : 203);
		fprintf(f, "IccSupported=%s\n", yes_no(icc_capable));
		fprintf(f, "IccProfile=%s\n", output->icc_path ? output->icc_path : "");
		fprintf(f, "IccActive=%s\n", yes_no(output->icc_transform != NULL));
		fprintf(f, "IccError=%s\n\n", output->icc_error ? output->icc_error : "");
	}
	fclose(f);
	if (rename(tmp, path) != 0) {
		unlink(tmp);
	}
}
