/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_TABLET_CONFIG_H
#define LABWC_TABLET_CONFIG_H

#include <stdint.h>
#include <wayland-util.h>
#include "config/types.h"

struct tablet_padbind {
	uint32_t button;
	uint32_t mouse_button;
	struct wl_list actions;
	struct wl_list link; /* rcxml.tablet.padbinds */
};

double tablet_get_dbl_if_positive(const char *content, const char *name);
enum lab_rotation tablet_parse_rotation(int value);
uint32_t tablet_button_from_str(const char *button);
void tablet_button_mapping_add(uint32_t from, uint32_t to);
void tablet_load_default_button_mappings(void);
uint32_t tablet_get_mapped_button(uint32_t src_button);
struct tablet_padbind *tablet_padbind_create(uint32_t button);
struct tablet_padbind *tablet_padbind_find(uint32_t button);

#endif /* LABWC_TABLET_CONFIG_H */
