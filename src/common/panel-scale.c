// SPDX-License-Identifier: GPL-2.0-only
#include <math.h>
#include "common/panel-scale.h"

#define HANDHELD_MAX_INCHES 7.5
#define HANDHELD_DPI_PER_SCALE 160.0
#define HANDHELD_MAX_SCALE 3.0
#define DESKTOP_HIDPI 192.0

double
panel_default_scale(int width_mm, int height_mm, int width_px, int height_px)
{
	if (width_mm <= 0 || height_mm <= 0 || width_px <= 0 || height_px <= 0) {
		return 1.0;
	}
	double diag_in = hypot(width_mm, height_mm) / 25.4;
	if (diag_in < 1.0) {
		return 1.0;
	}
	double dpi = hypot(width_px, height_px) / diag_in;
	if (diag_in < HANDHELD_MAX_INCHES) {
		double steps = floor(dpi / HANDHELD_DPI_PER_SCALE * 4.0) / 4.0;
		return fmax(1.0, fmin(HANDHELD_MAX_SCALE, steps));
	}
	return dpi >= DESKTOP_HIDPI ? 2.0 : 1.0;
}
