/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_KEYBOARD_H
#define LABWC_KEYBOARD_H

#include <stdbool.h>
#include <wayland-server-protocol.h>
#include <xkbcommon/xkbcommon.h>
#include "input/input.h"

/*
 * Virtual keyboards should not belong to seat->keyboard_group. As a result we
 * need to be able to ascertain which wlr_keyboard key/modifier events come from
 * and we achieve that by using `struct keyboard` which inherits `struct input`
 * and adds keyboard specific listeners and a wlr_keyboard pointer.
 */
struct keyboard {
	struct input base;
	struct wlr_keyboard *wlr_keyboard;
	bool is_virtual;
	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener accessx_key;
	bool accessx_swallow;
	/* key repeat for compositor keybinds */
	uint32_t keybind_repeat_keycode;
	int32_t keybind_repeat_rate;
	struct wl_event_source *keybind_repeat;
};

void keyboard_reset_current_keybind(void);
void keyboard_configure(struct seat *seat, struct wlr_keyboard *kb,
	bool is_virtual);

void keyboard_group_init(struct seat *seat);
void keyboard_group_finish(struct seat *seat);

void keyboard_setup_handlers(struct keyboard *keyboard);
void keyboard_setup_accessx(struct keyboard *keyboard);
void keyboard_inject_key(struct keyboard *keyboard, uint32_t keycode,
	enum wl_keyboard_key_state state, uint32_t time_msec);
bool keyboard_key_is_modifier(struct keyboard *keyboard, uint32_t keycode);
void keyboard_set_numlock(struct wlr_keyboard *keyboard);
void keyboard_update_layout(struct seat *seat, xkb_layout_index_t layout);
void keyboard_cancel_keybind_repeat(struct keyboard *keyboard);
void keyboard_cancel_all_keybind_repeats(struct seat *seat);

uint32_t keyboard_get_all_modifiers(struct seat *seat);

#endif /* LABWC_KEYBOARD_H */
