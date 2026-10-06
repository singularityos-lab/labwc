// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L
#include "input/tablet-state.h"
#include <errno.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wlr/config.h>
#include <wlr/types/wlr_tablet_pad.h>
#include <wlr/types/wlr_tablet_tool.h>
#include <wlr/types/wlr_tablet_v2.h>
#include <wlr/util/log.h>
#include "common/mem.h"
#include "common/string-helpers.h"
#include "input/tablet.h"
#include "input/tablet-pad.h"
#include "labwc.h"

#if WLR_HAS_LIBINPUT_BACKEND
	#include <wlr/backend/libinput.h>
#endif

static char *socket_name;

void
tablet_state_set_socket(const char *socket)
{
	free(socket_name);
	socket_name = socket ? strdup(socket) : NULL;
}

char *
tablet_state_path(void)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (string_null_or_empty(runtime) || string_null_or_empty(socket_name)
			|| strchr(socket_name, '/')) {
		return NULL;
	}
	return strdup_printf("%s/labwc/tablets-%s.ini", runtime, socket_name);
}

void
tablet_state_remove(void)
{
	char *path = tablet_state_path();
	if (path) {
		unlink(path);
		free(path);
	}
	tablet_state_set_socket(NULL);
}

static void
write_mm(FILE *f, const char *key, double mm)
{
	long hundredths = lround(mm * 100.0);
	if (hundredths < 0) {
		hundredths = 0;
	}
	fprintf(f, "%s=%ld.%02ld\n", key, hundredths / 100, hundredths % 100);
}

static void
write_name(FILE *f, const char *name)
{
	fputs("Name=", f);
	for (const char *p = name ? name : ""; *p; p++) {
		fputc(*p == '\n' || *p == '\r' ? ' ' : *p, f);
	}
	fputc('\n', f);
}

static const char *
tool_type_name(enum wlr_tablet_tool_type type)
{
	switch (type) {
	case WLR_TABLET_TOOL_TYPE_PEN:
		return "pen";
	case WLR_TABLET_TOOL_TYPE_ERASER:
		return "eraser";
	case WLR_TABLET_TOOL_TYPE_BRUSH:
		return "brush";
	case WLR_TABLET_TOOL_TYPE_PENCIL:
		return "pencil";
	case WLR_TABLET_TOOL_TYPE_AIRBRUSH:
		return "airbrush";
	case WLR_TABLET_TOOL_TYPE_MOUSE:
		return "mouse";
	case WLR_TABLET_TOOL_TYPE_LENS:
		return "lens";
	default:
		return "unknown";
	}
}

static void
write_tool_buttons(FILE *f, struct wlr_tablet_tool *wlr_tool)
{
	fputs("Buttons=", f);
#if WLR_HAS_LIBINPUT_BACKEND
	struct libinput_tablet_tool *handle =
		wlr_libinput_get_tablet_tool_handle(wlr_tool);
	if (handle) {
		if (libinput_tablet_tool_has_button(handle, BTN_STYLUS)) {
			fputs("Stylus;", f);
		}
		if (libinput_tablet_tool_has_button(handle, BTN_STYLUS2)) {
			fputs("Stylus2;", f);
		}
		if (libinput_tablet_tool_has_button(handle, BTN_STYLUS3)) {
			fputs("Stylus3;", f);
		}
		fputc('\n', f);
		return;
	}
#endif
	fputs("Stylus;Stylus2;\n", f);
}

void
tablet_state_write(struct seat *seat)
{
	char *path = tablet_state_path();
	if (!path) {
		return;
	}
	char *dir = strdup(path);
	char *slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		if (mkdir(dir, 0700) < 0 && errno != EEXIST) {
			wlr_log_errno(WLR_ERROR, "cannot create %s", dir);
		}
	}
	free(dir);

	char *tmp = strdup_printf("%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		wlr_log_errno(WLR_ERROR, "cannot write %s", tmp);
		free(tmp);
		free(path);
		return;
	}

	int index = 0;
	struct drawing_tablet *tablet;
	wl_list_for_each_reverse(tablet, &seat->tablets, link) {
		fprintf(f, "[Tablet %d]\n", ++index);
		write_name(f, tablet->wlr_input_device->name);
		fprintf(f, "VendorId=%u\nProductId=%u\n",
			tablet->tablet->usb_vendor_id, tablet->tablet->usb_product_id);
		write_mm(f, "WidthMm", tablet->tablet->width_mm);
		write_mm(f, "HeightMm", tablet->tablet->height_mm);
		size_t buttons = 0, rings = 0, strips = 0;
		struct drawing_tablet_pad *pad;
		wl_list_for_each(pad, &seat->tablet_pads, link) {
			if (pad->tablet == tablet) {
				buttons += pad->pad->button_count;
				rings += pad->pad->ring_count;
				strips += pad->pad->strip_count;
			}
		}
		fprintf(f, "PadButtons=%zu\nPadRings=%zu\nPadStrips=%zu\n\n",
			buttons, rings, strips);
	}

	index = 0;
	struct drawing_tablet_pad *pad;
	wl_list_for_each_reverse(pad, &seat->tablet_pads, link) {
		if (pad->tablet) {
			continue;
		}
		fprintf(f, "[Pad %d]\n", ++index);
		write_name(f, pad->wlr_input_device->name);
		fprintf(f, "Buttons=%zu\nRings=%zu\nStrips=%zu\n\n",
			pad->pad->button_count, pad->pad->ring_count,
			pad->pad->strip_count);
	}

	index = 0;
	struct drawing_tablet_tool *tool;
	wl_list_for_each_reverse(tool, &seat->tablet_tools, link) {
		struct wlr_tablet_tool *wlr_tool = tool->tool_v2->wlr_tool;
		fprintf(f, "[Tool %d]\n", ++index);
		fprintf(f, "Type=%s\n", tool_type_name(wlr_tool->type));
		fprintf(f, "Serial=%llu\n", (unsigned long long)wlr_tool->hardware_serial);
		fprintf(f, "Pressure=%s\nTilt=%s\n",
			wlr_tool->pressure ? "true" : "false",
			wlr_tool->tilt ? "true" : "false");
		write_tool_buttons(f, wlr_tool);
		fputc('\n', f);
	}

	if (fclose(f) != 0 || rename(tmp, path) < 0) {
		wlr_log_errno(WLR_ERROR, "cannot update %s", path);
		unlink(tmp);
	}
	free(tmp);
	free(path);
}
