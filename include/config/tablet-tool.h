/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_TABLET_TOOL_CONFIG_H
#define LABWC_TABLET_TOOL_CONFIG_H

#include <stdbool.h>
#include "config/types.h"

enum lab_motion tablet_parse_motion(const char *name);
bool tablet_parse_pressure_curve(const char *content, double curve[4]);
double tablet_pressure_curve_apply(const double curve[4], double pressure);

#endif /* LABWC_TABLET_TOOL_CONFIG_H */
