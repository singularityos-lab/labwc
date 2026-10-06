/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_MOTION_H
#define LABWC_MOTION_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-util.h>

struct output;

#define MOTION_DURATION_INSTANT 0
#define MOTION_DURATION_MICRO 90
#define MOTION_DURATION_SMALL 140
#define MOTION_DURATION_MEDIUM 220
#define MOTION_DURATION_LARGE 320
#define MOTION_DURATION_PAGE 380
#define MOTION_DURATION_SCENE 600
#define MOTION_EXIT_FACTOR 0.7

#define MOTION_ENTER_SCALE 0.96
#define MOTION_EXIT_SCALE 0.98
#define MOTION_RUBBER_BAND_COEFFICIENT 0.55

enum motion_curve {
	MOTION_CURVE_LINEAR = 0,
	MOTION_CURVE_STANDARD,
	MOTION_CURVE_ENTER,
	MOTION_CURVE_EXIT,
	MOTION_CURVE_EMPHASIZED,
};

enum motion_spring_kind {
	MOTION_SPRING_GENTLE = 0,
	MOTION_SPRING_SNAPPY,
	MOTION_SPRING_BOUNCY,
	MOTION_SPRING_COUNT,
};

enum motion_kind {
	MOTION_WINDOW_OPEN = 0,
	MOTION_WINDOW_OPEN_FADE,
	MOTION_WINDOW_CLOSE,
	MOTION_WINDOW_MINIMIZE,
	MOTION_WINDOW_RESTORE,
	MOTION_WINDOW_GEOMETRY,
	MOTION_WORKSPACE_SWITCH,
	MOTION_WORKSPACE_SETTLE,
	MOTION_SNAP_PREVIEW,
	MOTION_SPLASH_FADE,
	MOTION_TILING_SCROLL,
	MOTION_KIND_COUNT,
};

struct motion_timing {
	int duration_ms;
	enum motion_curve curve;
};

struct motion_spring {
	double damping;
	double stiffness;
	double mass;
};

struct motion_config {
	double duration_scale;
	struct motion_timing timing[MOTION_KIND_COUNT];
	struct motion_spring springs[MOTION_SPRING_COUNT];
	enum motion_spring_kind workspace_release_spring;
};

struct motion_ticker {
	struct wl_list link;
	bool active;
	void (*tick)(struct motion_ticker *ticker, int64_t now_ns);
};

struct motion_timed {
	struct motion_ticker ticker;
	enum motion_kind kind;
	enum motion_curve curve;
	int64_t start_ns;
	int64_t duration_ns;
	int64_t last_ns;
	int frames;
	void (*update)(struct motion_timed *timed, double eased, double linear);
	void (*done)(struct motion_timed *timed);
};

struct motion_spring_anim {
	struct motion_ticker ticker;
	struct motion_spring spring;
	double from;
	double to;
	double velocity;
	double value;
	bool clamp;
	int64_t start_ns;
	int64_t last_ns;
	int frames;
	void (*update)(struct motion_spring_anim *anim, double value);
	void (*done)(struct motion_spring_anim *anim);
};

void motion_config_defaults(struct motion_config *config);
bool motion_config_set_curve(enum motion_curve *curve, const char *name);
int motion_kind_from_name(const char *name);
int motion_spring_from_name(const char *name);

bool motion_enabled(void);
int motion_duration_ms(enum motion_kind kind);
double motion_ease(enum motion_curve curve, double t);
int64_t motion_now_ns(void);
double motion_rubber_band(double distance, double extent);
double motion_rubber_band_slope(double resisted, double extent);

void motion_ticker_add(struct motion_ticker *ticker);
void motion_ticker_remove(struct motion_ticker *ticker);
void motion_output_frame(struct output *output);

void motion_timed_start(struct motion_timed *timed, enum motion_kind kind);
void motion_timed_start_custom(struct motion_timed *timed, enum motion_kind kind,
	int duration_ms, enum motion_curve curve);
void motion_timed_finish(struct motion_timed *timed);
void motion_timed_cancel(struct motion_timed *timed);
bool motion_timed_running(struct motion_timed *timed);

void motion_spring_start(struct motion_spring_anim *anim,
	enum motion_spring_kind kind, double from, double to, double velocity);
void motion_spring_finish(struct motion_spring_anim *anim);
void motion_spring_cancel(struct motion_spring_anim *anim);
bool motion_spring_running(struct motion_spring_anim *anim);

void motion_finish(void);

#endif /* LABWC_MOTION_H */
