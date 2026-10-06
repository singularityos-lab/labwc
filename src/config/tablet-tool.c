// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L
#include "config/tablet-tool.h"
#include <stdio.h>
#include <strings.h>
#include <wlr/util/log.h>

enum lab_motion
tablet_parse_motion(const char *name)
{
	if (!strcasecmp(name, "Absolute")) {
		return LAB_MOTION_ABSOLUTE;
	} else if (!strcasecmp(name, "Relative")) {
		return LAB_MOTION_RELATIVE;
	}
	wlr_log(WLR_ERROR, "Invalid value for tablet motion: %s", name);
	return LAB_MOTION_ABSOLUTE;
}

bool
tablet_parse_pressure_curve(const char *content, double curve[4])
{
	double v[4];
	char tail;
	if (!content || sscanf(content, " %lf , %lf , %lf , %lf %c",
			&v[0], &v[1], &v[2], &v[3], &tail) != 4) {
		wlr_log(WLR_ERROR, "Invalid pressure curve: %s", content ? content : "");
		return false;
	}
	for (int i = 0; i < 4; i++) {
		if (!(v[i] >= 0.0 && v[i] <= 1.0)) {
			wlr_log(WLR_ERROR, "Pressure curve values must be between 0 and 1");
			return false;
		}
		curve[i] = v[i];
	}
	wlr_log(WLR_INFO, "Tablet pressure curve %s", content);
	return true;
}

static double
bezier(double p1, double p2, double t)
{
	double u = 1.0 - t;
	return 3.0 * u * u * t * p1 + 3.0 * u * t * t * p2 + t * t * t;
}

double
tablet_pressure_curve_apply(const double curve[4], double pressure)
{
	if (pressure <= 0.0) {
		return 0.0;
	}
	if (pressure >= 1.0) {
		return 1.0;
	}
	double lo = 0.0;
	double hi = 1.0;
	double t = pressure;
	for (int i = 0; i < 32; i++) {
		t = (lo + hi) / 2.0;
		double x = bezier(curve[0], curve[2], t);
		if (x < pressure) {
			lo = t;
		} else {
			hi = t;
		}
	}
	double y = bezier(curve[1], curve[3], t);
	if (y < 0.0) {
		return 0.0;
	}
	return y > 1.0 ? 1.0 : y;
}
