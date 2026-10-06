// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L
#include "input/accessx.h"
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard.h>
#include "a11y.h"
#include "common/macros.h"
#include "common/spawn.h"
#include "common/string-helpers.h"
#include "config/rcxml.h"
#include "input/cursor.h"
#include "input/keyboard.h"
#include "labwc.h"

#define KEYSET_MAX 32
#define SHORTCUT_TAPS 5
#define SHORTCUT_TAP_TIMEOUT_MS 2000
#define SHORTCUT_HOLD_MS 8000
#define MOUSEKEYS_INTERVAL_MS 20
#define MOUSEKEYS_CURVE 1.5

struct keyset {
	uint32_t keycodes[KEYSET_MAX];
	int len;
};

struct held_modifier {
	uint32_t keycode;
	uint32_t mask;
};

static const struct {
	uint32_t keycode;
	int dx, dy;
} mousekeys_dirs[] = {
	{ KEY_KP7, -1, -1 },
	{ KEY_KP8,  0, -1 },
	{ KEY_KP9,  1, -1 },
	{ KEY_KP4, -1,  0 },
	{ KEY_KP6,  1,  0 },
	{ KEY_KP1, -1,  1 },
	{ KEY_KP2,  0,  1 },
	{ KEY_KP3,  1,  1 },
};

static struct {
	struct keyboard *keyboard;
	bool rc_sticky;
	bool rc_slow;

	bool sticky_on;
	struct held_modifier held[KEYSET_MAX];
	int nr_held;
	bool mod_used;
	uint32_t latched;
	uint32_t locked;

	bool slow_on;
	bool slow_pending;
	uint32_t slow_keycode;
	struct wl_event_source *slow_timer;

	bool bounce_valid;
	uint32_t bounce_keycode;
	uint32_t bounce_time;

	struct keyset dropped;

	int shift_taps;
	uint32_t last_shift_tap;
	bool shift_hold_fired;
	struct wl_event_source *shift_hold_timer;

	bool ctrl_tap;

	struct keyset mousekeys_pressed;
	uint32_t mousekeys_dirs;
	uint32_t mousekeys_button;
	bool mousekeys_button_held;
	uint32_t mousekeys_start;
	struct wl_event_source *mousekeys_timer;
} ax;

static uint32_t
now_msec(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

static bool
keyset_contains(struct keyset *set, uint32_t keycode)
{
	for (int i = 0; i < set->len; i++) {
		if (set->keycodes[i] == keycode) {
			return true;
		}
	}
	return false;
}

static void
keyset_add(struct keyset *set, uint32_t keycode)
{
	if (set->len < KEYSET_MAX && !keyset_contains(set, keycode)) {
		set->keycodes[set->len++] = keycode;
	}
}

static bool
keyset_remove(struct keyset *set, uint32_t keycode)
{
	for (int i = 0; i < set->len; i++) {
		if (set->keycodes[i] == keycode) {
			set->keycodes[i] = set->keycodes[--set->len];
			return true;
		}
	}
	return false;
}

static void
send_feedback(const char *event, bool on)
{
	if (!rc.a11y_feedback_command) {
		return;
	}
	char *command = strdup_printf("%s a11y:%s:%s", rc.a11y_feedback_command,
		event, on ? "on" : "off");
	spawn_async_no_shell(command);
	free(command);
}

static void
apply_sticky_modifiers(uint32_t changed)
{
	if (!ax.keyboard) {
		return;
	}
	struct wlr_keyboard *kb = ax.keyboard->wlr_keyboard;
	struct wlr_keyboard_modifiers mods = kb->modifiers;
	wlr_keyboard_notify_modifiers(kb, mods.depressed,
		(mods.latched & ~changed) | (ax.latched & changed),
		(mods.locked & ~changed) | (ax.locked & changed),
		mods.group);
}

static void
set_sticky_keys(bool on)
{
	uint32_t changed = ax.latched | ax.locked;
	ax.latched = 0;
	ax.locked = 0;
	ax.nr_held = 0;
	ax.mod_used = false;
	ax.sticky_on = on;
	if (changed) {
		apply_sticky_modifiers(changed);
	}
	wlr_log(WLR_INFO, "sticky keys %s", on ? "on" : "off");
}

static void
set_slow_keys(bool on)
{
	if (ax.slow_pending) {
		ax.slow_pending = false;
		keyset_add(&ax.dropped, ax.slow_keycode);
		wl_event_source_timer_update(ax.slow_timer, 0);
	}
	ax.slow_on = on;
	wlr_log(WLR_INFO, "slow keys %s", on ? "on" : "off");
}

static void
sticky_press(uint32_t keycode, uint32_t time_msec)
{
	struct keyboard *keyboard = ax.keyboard;
	if (!ax.sticky_on) {
		keyboard_inject_key(keyboard, keycode,
			WL_KEYBOARD_KEY_STATE_PRESSED, time_msec);
		return;
	}

	if (keyboard_key_is_modifier(keyboard, keycode)) {
		if (ax.nr_held > 0 && rc.sticky_keys_two_key_off) {
			keyboard_inject_key(keyboard, keycode,
				WL_KEYBOARD_KEY_STATE_PRESSED, time_msec);
			set_sticky_keys(false);
			send_feedback("sticky-keys", false);
			return;
		}
		uint32_t before = keyboard->wlr_keyboard->modifiers.depressed;
		keyboard_inject_key(keyboard, keycode,
			WL_KEYBOARD_KEY_STATE_PRESSED, time_msec);
		uint32_t mask = keyboard->wlr_keyboard->modifiers.depressed & ~before;
		if (ax.nr_held < KEYSET_MAX) {
			ax.held[ax.nr_held++] = (struct held_modifier){
				.keycode = keycode,
				.mask = mask,
			};
		}
		ax.mod_used = false;
		return;
	}

	keyboard_inject_key(keyboard, keycode,
		WL_KEYBOARD_KEY_STATE_PRESSED, time_msec);
	ax.mod_used = true;
	if (ax.latched) {
		uint32_t changed = ax.latched;
		ax.latched = 0;
		apply_sticky_modifiers(changed);
	}
}

static void
sticky_release(uint32_t keycode, uint32_t time_msec)
{
	keyboard_inject_key(ax.keyboard, keycode,
		WL_KEYBOARD_KEY_STATE_RELEASED, time_msec);

	int index = -1;
	for (int i = 0; i < ax.nr_held; i++) {
		if (ax.held[i].keycode == keycode) {
			index = i;
			break;
		}
	}
	if (index < 0) {
		return;
	}
	uint32_t mask = ax.held[index].mask;
	ax.held[index] = ax.held[--ax.nr_held];
	if (!ax.sticky_on || ax.mod_used || !mask) {
		return;
	}

	if (ax.locked & mask) {
		ax.locked &= ~mask;
	} else if (ax.latched & mask) {
		ax.latched &= ~mask;
		ax.locked |= mask;
	} else {
		ax.latched |= mask;
	}
	apply_sticky_modifiers(mask);
	if (rc.sticky_keys_modifier_beep) {
		send_feedback("sticky-modifier", (ax.latched | ax.locked) & mask);
	}
}

static int
handle_slow_timer(void *data)
{
	if (ax.slow_pending && ax.keyboard) {
		ax.slow_pending = false;
		sticky_press(ax.slow_keycode, now_msec());
	}
	return 0;
}

static int
handle_shift_hold_timer(void *data)
{
	ax.shift_taps = 0;
	ax.shift_hold_fired = true;
	set_slow_keys(!ax.slow_on);
	send_feedback("slow-keys", ax.slow_on);
	return 0;
}

static void
track_shortcuts(uint32_t keycode, bool pressed, uint32_t time_msec)
{
	if (!rc.a11y_keyboard_shortcuts) {
		return;
	}
	if (keycode != KEY_LEFTSHIFT && keycode != KEY_RIGHTSHIFT) {
		if (pressed) {
			ax.shift_taps = 0;
			wl_event_source_timer_update(ax.shift_hold_timer, 0);
		}
		return;
	}
	if (pressed) {
		if (ax.shift_taps && time_msec - ax.last_shift_tap
				> SHORTCUT_TAP_TIMEOUT_MS) {
			ax.shift_taps = 0;
		}
		ax.shift_hold_fired = false;
		wl_event_source_timer_update(ax.shift_hold_timer, SHORTCUT_HOLD_MS);
		return;
	}
	wl_event_source_timer_update(ax.shift_hold_timer, 0);
	if (ax.shift_hold_fired) {
		ax.shift_hold_fired = false;
		return;
	}
	ax.last_shift_tap = time_msec;
	if (++ax.shift_taps >= SHORTCUT_TAPS) {
		ax.shift_taps = 0;
		set_sticky_keys(!ax.sticky_on);
		send_feedback("sticky-keys", ax.sticky_on);
	}
}

static void
track_toggle_keys(uint32_t keycode)
{
	bool caps = keycode == KEY_CAPSLOCK;
	struct xkb_state *state = ax.keyboard->wlr_keyboard->xkb_state;
	bool was_locked = state && xkb_state_mod_name_is_active(state,
		caps ? XKB_MOD_NAME_CAPS : XKB_MOD_NAME_NUM,
		XKB_STATE_MODS_LOCKED) > 0;
	a11y_flash(NULL);
	send_feedback(caps ? "caps-lock" : "num-lock", !was_locked);
}

static void
track_locate_pointer(uint32_t keycode, bool pressed)
{
	if (!rc.locate_pointer) {
		return;
	}
	if (keycode != KEY_LEFTCTRL) {
		if (pressed) {
			ax.ctrl_tap = false;
		}
		return;
	}
	if (pressed) {
		ax.ctrl_tap = true;
	} else if (ax.ctrl_tap) {
		ax.ctrl_tap = false;
		a11y_locate_pointer();
	}
}

static void
mousekeys_move(double speed, uint32_t time_msec)
{
	int dx = 0;
	int dy = 0;
	for (size_t i = 0; i < ARRAY_SIZE(mousekeys_dirs); i++) {
		if (ax.mousekeys_dirs & (1u << i)) {
			dx += mousekeys_dirs[i].dx;
			dy += mousekeys_dirs[i].dy;
		}
	}
	dx = MAX(-1, MIN(1, dx));
	dy = MAX(-1, MIN(1, dy));
	if (dx || dy) {
		cursor_emulate_move(&server.seat, NULL, dx * speed, dy * speed,
			time_msec);
	}
}

static int
handle_mousekeys_timer(void *data)
{
	if (!ax.mousekeys_dirs) {
		return 0;
	}
	uint32_t now = now_msec();
	double elapsed = (double)(int32_t)(now - ax.mousekeys_start);
	double speed = rc.mouse_keys_max_speed;
	if (rc.mouse_keys_accel_time > 0 && elapsed < rc.mouse_keys_accel_time) {
		speed *= pow(MAX(0.0, elapsed) / rc.mouse_keys_accel_time,
			MOUSEKEYS_CURVE);
		speed = MAX(1.0, speed);
	}
	mousekeys_move(speed, now);
	wl_event_source_timer_update(ax.mousekeys_timer, MOUSEKEYS_INTERVAL_MS);
	return 0;
}

static void
mousekeys_button(enum wl_pointer_button_state state, uint32_t time_msec)
{
	cursor_emulate_button(&server.seat, ax.mousekeys_button, state, time_msec);
}

static void
mousekeys_reset(void)
{
	if (ax.mousekeys_button_held) {
		ax.mousekeys_button_held = false;
		mousekeys_button(WL_POINTER_BUTTON_STATE_RELEASED, now_msec());
	}
	ax.mousekeys_dirs = 0;
	ax.mousekeys_pressed.len = 0;
	ax.mousekeys_button = BTN_LEFT;
	if (ax.mousekeys_timer) {
		wl_event_source_timer_update(ax.mousekeys_timer, 0);
	}
}

static bool
mousekeys_handle(uint32_t keycode, bool pressed, uint32_t time_msec)
{
	if (!pressed) {
		if (!keyset_remove(&ax.mousekeys_pressed, keycode)) {
			return false;
		}
		for (size_t i = 0; i < ARRAY_SIZE(mousekeys_dirs); i++) {
			if (mousekeys_dirs[i].keycode == keycode) {
				ax.mousekeys_dirs &= ~(1u << i);
			}
		}
		if (!ax.mousekeys_dirs) {
			wl_event_source_timer_update(ax.mousekeys_timer, 0);
		}
		return true;
	}

	if (!rc.mouse_keys || !ax.keyboard) {
		return false;
	}
	struct xkb_state *state = ax.keyboard->wlr_keyboard->xkb_state;
	if (state && xkb_state_mod_name_is_active(state, XKB_MOD_NAME_NUM,
			XKB_STATE_MODS_EFFECTIVE) > 0) {
		return false;
	}

	for (size_t i = 0; i < ARRAY_SIZE(mousekeys_dirs); i++) {
		if (mousekeys_dirs[i].keycode != keycode) {
			continue;
		}
		keyset_add(&ax.mousekeys_pressed, keycode);
		bool first = !ax.mousekeys_dirs;
		ax.mousekeys_dirs |= 1u << i;
		if (first) {
			mousekeys_move(1.0, time_msec);
			ax.mousekeys_start = now_msec() + rc.mouse_keys_init_delay;
			wl_event_source_timer_update(ax.mousekeys_timer,
				MAX(1, rc.mouse_keys_init_delay));
		}
		return true;
	}

	switch (keycode) {
	case KEY_KP5:
		mousekeys_button(WL_POINTER_BUTTON_STATE_PRESSED, time_msec);
		mousekeys_button(WL_POINTER_BUTTON_STATE_RELEASED, time_msec);
		break;
	case KEY_KPPLUS:
		for (int i = 0; i < 2; i++) {
			mousekeys_button(WL_POINTER_BUTTON_STATE_PRESSED, time_msec);
			mousekeys_button(WL_POINTER_BUTTON_STATE_RELEASED, time_msec);
		}
		break;
	case KEY_KP0:
		if (!ax.mousekeys_button_held) {
			ax.mousekeys_button_held = true;
			mousekeys_button(WL_POINTER_BUTTON_STATE_PRESSED, time_msec);
		}
		break;
	case KEY_KPDOT:
		if (ax.mousekeys_button_held) {
			ax.mousekeys_button_held = false;
			mousekeys_button(WL_POINTER_BUTTON_STATE_RELEASED, time_msec);
		}
		break;
	case KEY_KPSLASH:
		ax.mousekeys_button = BTN_LEFT;
		break;
	case KEY_KPASTERISK:
		ax.mousekeys_button = BTN_MIDDLE;
		break;
	case KEY_KPMINUS:
		ax.mousekeys_button = BTN_RIGHT;
		break;
	default:
		return false;
	}
	keyset_add(&ax.mousekeys_pressed, keycode);
	return true;
}

static bool
keys_filtered(void)
{
	return ax.sticky_on || ax.slow_on || rc.bounce_keys;
}

bool
accessx_filter_key(struct keyboard *keyboard,
		struct wlr_keyboard_key_event *event)
{
	uint32_t keycode = event->keycode;
	bool pressed = event->state == WL_KEYBOARD_KEY_STATE_PRESSED;
	ax.keyboard = keyboard;

	track_shortcuts(keycode, pressed, event->time_msec);
	track_locate_pointer(keycode, pressed);
	if (rc.toggle_keys && pressed
			&& (keycode == KEY_CAPSLOCK || keycode == KEY_NUMLOCK)) {
		track_toggle_keys(keycode);
	}

	if (mousekeys_handle(keycode, pressed, event->time_msec)) {
		return true;
	}
	if (!pressed && keyset_remove(&ax.dropped, keycode)) {
		return true;
	}
	if (!keys_filtered()) {
		return false;
	}

	if (pressed) {
		if (rc.bounce_keys && ax.bounce_valid
				&& keycode == ax.bounce_keycode
				&& event->time_msec - ax.bounce_time
					< (uint32_t)rc.bounce_keys_delay) {
			keyset_add(&ax.dropped, keycode);
			return true;
		}
		if (ax.slow_on && rc.slow_keys_delay > 0) {
			if (ax.slow_pending && ax.slow_keycode != keycode) {
				keyset_add(&ax.dropped, ax.slow_keycode);
			}
			ax.slow_pending = true;
			ax.slow_keycode = keycode;
			wl_event_source_timer_update(ax.slow_timer,
				rc.slow_keys_delay);
			return true;
		}
		sticky_press(keycode, event->time_msec);
		return true;
	}

	if (ax.slow_pending && ax.slow_keycode == keycode) {
		ax.slow_pending = false;
		wl_event_source_timer_update(ax.slow_timer, 0);
		return true;
	}
	ax.bounce_valid = true;
	ax.bounce_keycode = keycode;
	ax.bounce_time = event->time_msec;
	sticky_release(keycode, event->time_msec);
	return true;
}

void
accessx_cancel_key_taps(void)
{
	ax.ctrl_tap = false;
	ax.shift_taps = 0;
}

void
accessx_keyboard_destroy(struct keyboard *keyboard)
{
	if (ax.keyboard != keyboard) {
		return;
	}
	ax.keyboard = NULL;
	ax.slow_pending = false;
	if (ax.slow_timer) {
		wl_event_source_timer_update(ax.slow_timer, 0);
	}
	ax.nr_held = 0;
	ax.latched = 0;
	ax.locked = 0;
	ax.dropped.len = 0;
	mousekeys_reset();
}

void
accessx_reconfigure(void)
{
	if (ax.rc_sticky != rc.sticky_keys) {
		ax.rc_sticky = rc.sticky_keys;
		set_sticky_keys(rc.sticky_keys);
	}
	if (ax.rc_slow != rc.slow_keys) {
		ax.rc_slow = rc.slow_keys;
		set_slow_keys(rc.slow_keys);
	}
	if (!rc.bounce_keys) {
		ax.bounce_valid = false;
	}
	if (!rc.mouse_keys) {
		mousekeys_reset();
	}
	if (!rc.a11y_keyboard_shortcuts) {
		ax.shift_taps = 0;
		wl_event_source_timer_update(ax.shift_hold_timer, 0);
	}
}

void
accessx_init(void)
{
	ax.slow_timer = wl_event_loop_add_timer(server.wl_event_loop,
		handle_slow_timer, NULL);
	ax.shift_hold_timer = wl_event_loop_add_timer(server.wl_event_loop,
		handle_shift_hold_timer, NULL);
	ax.mousekeys_timer = wl_event_loop_add_timer(server.wl_event_loop,
		handle_mousekeys_timer, NULL);
	ax.mousekeys_button = BTN_LEFT;
	accessx_reconfigure();
}

void
accessx_finish(void)
{
	wl_event_source_remove(ax.slow_timer);
	wl_event_source_remove(ax.shift_hold_timer);
	wl_event_source_remove(ax.mousekeys_timer);
	ax.slow_timer = NULL;
	ax.shift_hold_timer = NULL;
	ax.mousekeys_timer = NULL;
}
