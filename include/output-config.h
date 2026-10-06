/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_OUTPUT_CONFIG_H
#define LABWC_OUTPUT_CONFIG_H

#include <stdbool.h>
#include <wayland-server-protocol.h>
#include <wayland-util.h>

struct output_config {
	char *name;
	int width, height, refresh;
	bool has_mode;
	int x, y;
	bool has_position;
	double scale;
	bool has_scale;
	enum wl_output_transform transform;
	bool has_transform;
	bool enabled;
	bool has_enabled;
	bool adaptive_sync;
	bool has_adaptive_sync;
	bool hdr;
	bool has_hdr;
	int sdr_brightness;
	char *icc_profile;
	struct wl_list link;
};

void output_config_load(void);
void output_config_finish(void);
const struct output_config *output_config_find(const char *name);

#endif /* LABWC_OUTPUT_CONFIG_H */
