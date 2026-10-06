// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L
#include "motion.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>
#include "common/mem.h"
#include "config/rcxml.h"
#include "labwc.h"
#include "output.h"

#define SPRING_REST_EPSILON 0.5
#define SPRING_TIME_LIMIT_NS 3000000000LL

struct motion_output {
	struct wl_list link;
	struct wlr_output *wlr_output;
	int64_t last_present_ns;
	int64_t refresh_ns;
	struct wl_listener present;
	struct wl_listener destroy;
};

static struct wl_list tickers = { &tickers, &tickers };
static struct wl_list motion_outputs = { &motion_outputs, &motion_outputs };
static int64_t last_frame_ns;
static int trace = -1;

static const char *const kind_names[MOTION_KIND_COUNT] = {
	[MOTION_WINDOW_OPEN] = "windowOpen",
	[MOTION_WINDOW_OPEN_FADE] = "windowOpenFade",
	[MOTION_WINDOW_CLOSE] = "windowClose",
	[MOTION_WINDOW_MINIMIZE] = "minimize",
	[MOTION_WINDOW_RESTORE] = "restore",
	[MOTION_WINDOW_GEOMETRY] = "geometry",
	[MOTION_WORKSPACE_SWITCH] = "workspaceSwitch",
	[MOTION_WORKSPACE_SETTLE] = "workspaceSettle",
	[MOTION_SNAP_PREVIEW] = "snapPreview",
	[MOTION_SPLASH_FADE] = "splashFade",
	[MOTION_TILING_SCROLL] = "tilingScroll",
};

static const char *const spring_names[MOTION_SPRING_COUNT] = {
	[MOTION_SPRING_GENTLE] = "gentle",
	[MOTION_SPRING_SNAPPY] = "snappy",
	[MOTION_SPRING_BOUNCY] = "bouncy",
};

void
motion_config_defaults(struct motion_config *config)
{
	config->duration_scale = 1.0;
	config->timing[MOTION_WINDOW_OPEN] =
		(struct motion_timing){ MOTION_DURATION_LARGE, MOTION_CURVE_ENTER };
	config->timing[MOTION_WINDOW_OPEN_FADE] =
		(struct motion_timing){ MOTION_DURATION_SMALL, MOTION_CURVE_LINEAR };
	config->timing[MOTION_WINDOW_CLOSE] =
		(struct motion_timing){ MOTION_DURATION_MEDIUM, MOTION_CURVE_EXIT };
	config->timing[MOTION_WINDOW_MINIMIZE] =
		(struct motion_timing){ MOTION_DURATION_PAGE, MOTION_CURVE_STANDARD };
	config->timing[MOTION_WINDOW_RESTORE] =
		(struct motion_timing){ MOTION_DURATION_LARGE, MOTION_CURVE_ENTER };
	config->timing[MOTION_WINDOW_GEOMETRY] =
		(struct motion_timing){ MOTION_DURATION_LARGE, MOTION_CURVE_STANDARD };
	config->timing[MOTION_WORKSPACE_SWITCH] =
		(struct motion_timing){ MOTION_DURATION_PAGE, MOTION_CURVE_STANDARD };
	config->timing[MOTION_WORKSPACE_SETTLE] =
		(struct motion_timing){ MOTION_DURATION_SMALL, MOTION_CURVE_LINEAR };
	config->timing[MOTION_SNAP_PREVIEW] =
		(struct motion_timing){ MOTION_DURATION_MEDIUM, MOTION_CURVE_STANDARD };
	config->timing[MOTION_SPLASH_FADE] =
		(struct motion_timing){ MOTION_DURATION_SCENE, MOTION_CURVE_LINEAR };
	config->timing[MOTION_TILING_SCROLL] =
		(struct motion_timing){ MOTION_DURATION_MEDIUM, MOTION_CURVE_STANDARD };
	config->springs[MOTION_SPRING_GENTLE] =
		(struct motion_spring){ 26.0, 170.0, 1.0 };
	config->springs[MOTION_SPRING_SNAPPY] =
		(struct motion_spring){ 30.0, 400.0, 1.0 };
	config->springs[MOTION_SPRING_BOUNCY] =
		(struct motion_spring){ 14.0, 300.0, 1.0 };
	config->workspace_release_spring = MOTION_SPRING_SNAPPY;
}

bool
motion_config_set_curve(enum motion_curve *curve, const char *name)
{
	static const struct {
		const char *name;
		enum motion_curve curve;
	} curves[] = {
		{ "linear", MOTION_CURVE_LINEAR },
		{ "standard", MOTION_CURVE_STANDARD },
		{ "enter", MOTION_CURVE_ENTER },
		{ "exit", MOTION_CURVE_EXIT },
		{ "emphasized", MOTION_CURVE_EMPHASIZED },
	};
	for (size_t i = 0; i < sizeof(curves) / sizeof(curves[0]); i++) {
		if (!strcasecmp(name, curves[i].name)) {
			*curve = curves[i].curve;
			return true;
		}
	}
	return false;
}

int
motion_kind_from_name(const char *name)
{
	for (int i = 0; i < MOTION_KIND_COUNT; i++) {
		if (!strcasecmp(name, kind_names[i])) {
			return i;
		}
	}
	return -1;
}

int
motion_spring_from_name(const char *name)
{
	for (int i = 0; i < MOTION_SPRING_COUNT; i++) {
		if (!strcasecmp(name, spring_names[i])) {
			return i;
		}
	}
	return -1;
}

double
motion_rubber_band(double distance, double extent)
{
	if (!(extent > 0.0)) {
		return 0.0;
	}
	double resisted = extent * (1.0 - 1.0
		/ (fabs(distance) * MOTION_RUBBER_BAND_COEFFICIENT / extent + 1.0));
	return distance < 0.0 ? -resisted : resisted;
}

double
motion_rubber_band_slope(double resisted, double extent)
{
	if (!(extent > 0.0)) {
		return 0.0;
	}
	double remaining = 1.0 - fmin(0.999, fabs(resisted) / extent);
	return MOTION_RUBBER_BAND_COEFFICIENT * remaining * remaining;
}

static bool
tracing(void)
{
	if (trace < 0) {
		const char *value = getenv("LABWC_MOTION_TRACE");
		trace = value && *value && strcmp(value, "0") ? 1 : 0;
	}
	return trace == 1;
}

bool
motion_enabled(void)
{
	return rc.window_animations;
}

int
motion_duration_ms(enum motion_kind kind)
{
	double scale = rc.motion.duration_scale;
	if (!(scale > 0.0)) {
		scale = 1.0;
	}
	return (int)lround(rc.motion.timing[kind].duration_ms * scale);
}

static double
bezier_coordinate(double p1, double p2, double s)
{
	double inverse = 1.0 - s;
	return 3.0 * inverse * inverse * s * p1 + 3.0 * inverse * s * s * p2
		+ s * s * s;
}

static double
bezier_slope(double p1, double p2, double s)
{
	double inverse = 1.0 - s;
	return 3.0 * inverse * inverse * p1 + 6.0 * inverse * s * (p2 - p1)
		+ 3.0 * s * s * (1.0 - p2);
}

static double
bezier_solve(double x1, double y1, double x2, double y2, double t)
{
	if (t <= 0.0) {
		return 0.0;
	}
	if (t >= 1.0) {
		return 1.0;
	}
	double s = t;
	for (int i = 0; i < 8; i++) {
		double error = bezier_coordinate(x1, x2, s) - t;
		if (fabs(error) < 1e-7) {
			return bezier_coordinate(y1, y2, s);
		}
		double derivative = bezier_slope(x1, x2, s);
		if (fabs(derivative) < 1e-6) {
			break;
		}
		s -= error / derivative;
	}
	double low = 0.0;
	double high = 1.0;
	s = t;
	while (high - low > 1e-7) {
		double value = bezier_coordinate(x1, x2, s);
		if (fabs(value - t) < 1e-7) {
			break;
		}
		if (value < t) {
			low = s;
		} else {
			high = s;
		}
		s = (low + high) * 0.5;
	}
	return bezier_coordinate(y1, y2, s);
}

double
motion_ease(enum motion_curve curve, double t)
{
	if (t <= 0.0) {
		return 0.0;
	}
	if (t >= 1.0) {
		return 1.0;
	}
	switch (curve) {
	case MOTION_CURVE_STANDARD:
		return bezier_solve(0.2, 0.0, 0.0, 1.0, t);
	case MOTION_CURVE_ENTER:
		return bezier_solve(0.05, 0.7, 0.1, 1.0, t);
	case MOTION_CURVE_EXIT:
		return bezier_solve(0.3, 0.0, 0.8, 0.15, t);
	case MOTION_CURVE_EMPHASIZED:
		return bezier_solve(0.22, 1.0, 0.36, 1.0, t);
	case MOTION_CURVE_LINEAR:
	default:
		return t;
	}
}

int64_t
motion_now_ns(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
}

static void
schedule_frames(void)
{
	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (output_is_usable(output)) {
			wlr_output_schedule_frame(output->wlr_output);
		}
	}
}

void
motion_ticker_add(struct motion_ticker *ticker)
{
	if (ticker->active) {
		return;
	}
	ticker->active = true;
	wl_list_insert(tickers.prev, &ticker->link);
	schedule_frames();
}

void
motion_ticker_remove(struct motion_ticker *ticker)
{
	if (!ticker->active) {
		return;
	}
	ticker->active = false;
	wl_list_remove(&ticker->link);
	wl_list_init(&ticker->link);
}

static void
handle_output_present(struct wl_listener *listener, void *data)
{
	struct motion_output *record = wl_container_of(listener, record, present);
	struct wlr_output_event_present *event = data;
	if (!event->presented) {
		return;
	}
	int64_t when = (int64_t)event->when.tv_sec * 1000000000LL
		+ event->when.tv_nsec;
	if (when > 0) {
		record->last_present_ns = when;
	}
	if (event->refresh > 0) {
		record->refresh_ns = event->refresh;
	}
}

static void
handle_output_destroy(struct wl_listener *listener, void *data)
{
	struct motion_output *record = wl_container_of(listener, record, destroy);
	wl_list_remove(&record->present.link);
	wl_list_remove(&record->destroy.link);
	wl_list_remove(&record->link);
	free(record);
}

static struct motion_output *
motion_output_get(struct wlr_output *wlr_output)
{
	struct motion_output *record;
	wl_list_for_each(record, &motion_outputs, link) {
		if (record->wlr_output == wlr_output) {
			return record;
		}
	}
	record = znew(*record);
	record->wlr_output = wlr_output;
	record->present.notify = handle_output_present;
	wl_signal_add(&wlr_output->events.present, &record->present);
	record->destroy.notify = handle_output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &record->destroy);
	wl_list_insert(&motion_outputs, &record->link);
	return record;
}

static int64_t
predicted_frame_time(struct output *output)
{
	int64_t now = motion_now_ns();
	if (!output || !output->wlr_output) {
		return now;
	}
	struct motion_output *record = motion_output_get(output->wlr_output);
	int64_t refresh = record->refresh_ns;
	if (refresh <= 0 && output->wlr_output->refresh > 0) {
		refresh = 1000000000000LL / output->wlr_output->refresh;
	}
	if (refresh <= 0 || record->last_present_ns <= 0
			|| record->last_present_ns > now) {
		return now;
	}
	int64_t intervals = (now - record->last_present_ns) / refresh + 1;
	int64_t predicted = record->last_present_ns + intervals * refresh;
	if (predicted > now + refresh) {
		predicted = now + refresh;
	}
	return predicted;
}

void
motion_output_frame(struct output *output)
{
	if (wl_list_empty(&tickers)) {
		if (output && output->wlr_output) {
			motion_output_get(output->wlr_output);
		}
		return;
	}
	int64_t now = predicted_frame_time(output);
	if (now < last_frame_ns) {
		now = last_frame_ns;
	}
	last_frame_ns = now;

	struct wl_list pending;
	wl_list_init(&pending);
	wl_list_insert_list(&pending, &tickers);
	wl_list_init(&tickers);
	while (!wl_list_empty(&pending)) {
		struct motion_ticker *ticker =
			wl_container_of(pending.next, ticker, link);
		wl_list_remove(&ticker->link);
		wl_list_insert(tickers.prev, &ticker->link);
		ticker->tick(ticker, now);
	}
	if (!wl_list_empty(&tickers)) {
		schedule_frames();
	}
}

static void
timed_trace(struct motion_timed *timed, const char *how)
{
	if (!tracing()) {
		return;
	}
	double elapsed = timed->last_ns > timed->start_ns
		? (timed->last_ns - timed->start_ns) / 1000000.0 : 0.0;
	wlr_log(WLR_INFO, "motion: %s %s duration=%.1fms elapsed=%.1fms frames=%d",
		kind_names[timed->kind], how, timed->duration_ns / 1000000.0,
		elapsed, timed->frames);
}

static void
timed_tick(struct motion_ticker *ticker, int64_t now_ns)
{
	struct motion_timed *timed = wl_container_of(ticker, timed, ticker);
	timed->frames++;
	timed->last_ns = now_ns;
	double linear = timed->duration_ns > 0
		? (double)(now_ns - timed->start_ns) / timed->duration_ns : 1.0;
	if (linear < 0.0) {
		linear = 0.0;
	}
	if (linear >= 1.0) {
		motion_ticker_remove(&timed->ticker);
		timed_trace(timed, "done");
		if (timed->update) {
			timed->update(timed, 1.0, 1.0);
		}
		if (timed->done) {
			timed->done(timed);
		}
		return;
	}
	if (timed->update) {
		timed->update(timed, motion_ease(timed->curve, linear), linear);
	}
}

void
motion_timed_start_custom(struct motion_timed *timed, enum motion_kind kind,
		int duration_ms, enum motion_curve curve)
{
	motion_ticker_remove(&timed->ticker);
	timed->ticker.tick = timed_tick;
	timed->kind = kind;
	timed->curve = curve;
	timed->start_ns = motion_now_ns();
	timed->last_ns = timed->start_ns;
	timed->duration_ns = (int64_t)(duration_ms > 0 ? duration_ms : 0) * 1000000LL;
	timed->frames = 0;
	if (timed->duration_ns == 0) {
		if (timed->update) {
			timed->update(timed, 1.0, 1.0);
		}
		if (timed->done) {
			timed->done(timed);
		}
		return;
	}
	motion_ticker_add(&timed->ticker);
}

void
motion_timed_start(struct motion_timed *timed, enum motion_kind kind)
{
	motion_timed_start_custom(timed, kind, motion_duration_ms(kind),
		rc.motion.timing[kind].curve);
}

void
motion_timed_finish(struct motion_timed *timed)
{
	if (!timed->ticker.active) {
		return;
	}
	motion_ticker_remove(&timed->ticker);
	timed_trace(timed, "skipped");
	if (timed->update) {
		timed->update(timed, 1.0, 1.0);
	}
	if (timed->done) {
		timed->done(timed);
	}
}

void
motion_timed_cancel(struct motion_timed *timed)
{
	motion_ticker_remove(&timed->ticker);
}

bool
motion_timed_running(struct motion_timed *timed)
{
	return timed->ticker.active;
}

static void
spring_evaluate(const struct motion_spring *spring, double displacement,
		double velocity, double seconds, double *position, double *speed)
{
	double omega = sqrt(spring->stiffness / spring->mass);
	double zeta = spring->damping
		/ (2.0 * sqrt(spring->stiffness * spring->mass));
	if (fabs(zeta - 1.0) < 1e-6) {
		double b = velocity + omega * displacement;
		double decay = exp(-omega * seconds);
		*position = decay * (displacement + b * seconds);
		*speed = decay * (b - omega * (displacement + b * seconds));
		return;
	}
	if (zeta < 1.0) {
		double omega_d = omega * sqrt(1.0 - zeta * zeta);
		double a = displacement;
		double b = (velocity + zeta * omega * displacement) / omega_d;
		double decay = exp(-zeta * omega * seconds);
		double cosine = cos(omega_d * seconds);
		double sine = sin(omega_d * seconds);
		*position = decay * (a * cosine + b * sine);
		*speed = decay * (-zeta * omega * (a * cosine + b * sine)
			+ omega_d * (b * cosine - a * sine));
		return;
	}
	double root = sqrt(zeta * zeta - 1.0);
	double r1 = -omega * (zeta - root);
	double r2 = -omega * (zeta + root);
	double c2 = (velocity - r1 * displacement) / (r2 - r1);
	double c1 = displacement - c2;
	*position = c1 * exp(r1 * seconds) + c2 * exp(r2 * seconds);
	*speed = r1 * c1 * exp(r1 * seconds) + r2 * c2 * exp(r2 * seconds);
}

static void
spring_done(struct motion_spring_anim *anim, const char *how)
{
	motion_ticker_remove(&anim->ticker);
	anim->value = anim->to;
	if (tracing()) {
		wlr_log(WLR_INFO, "motion: spring %s from=%.1f to=%.1f velocity=%.1f elapsed=%.1fms frames=%d",
			how, anim->from, anim->to, anim->velocity,
			(anim->last_ns - anim->start_ns) / 1000000.0, anim->frames);
	}
	if (anim->update) {
		anim->update(anim, anim->to);
	}
	if (anim->done) {
		anim->done(anim);
	}
}

static void
spring_tick(struct motion_ticker *ticker, int64_t now_ns)
{
	struct motion_spring_anim *anim = wl_container_of(ticker, anim, ticker);
	anim->frames++;
	anim->last_ns = now_ns;
	double seconds = (now_ns - anim->start_ns) / 1000000000.0;
	if (seconds < 0.0) {
		seconds = 0.0;
	}
	double position, speed;
	spring_evaluate(&anim->spring, anim->from - anim->to, anim->velocity,
		seconds, &position, &speed);
	double initial = anim->from - anim->to;
	bool crossed = anim->clamp && initial != 0.0
		&& ((initial > 0.0 && position < 0.0)
			|| (initial < 0.0 && position > 0.0));
	bool rest = fabs(position) < SPRING_REST_EPSILON
		&& fabs(speed) / 60.0 < SPRING_REST_EPSILON;
	if (crossed || rest
			|| now_ns - anim->start_ns > SPRING_TIME_LIMIT_NS) {
		spring_done(anim, "done");
		return;
	}
	anim->value = anim->to + position;
	if (anim->update) {
		anim->update(anim, anim->value);
	}
}

void
motion_spring_start(struct motion_spring_anim *anim,
		enum motion_spring_kind kind, double from, double to, double velocity)
{
	motion_ticker_remove(&anim->ticker);
	anim->ticker.tick = spring_tick;
	anim->spring = rc.motion.springs[kind];
	double scale = rc.motion.duration_scale;
	if (scale > 0.0 && scale != 1.0) {
		anim->spring.stiffness /= scale * scale;
		anim->spring.damping /= scale;
		velocity /= scale;
	}
	anim->from = from;
	anim->to = to;
	anim->velocity = velocity;
	anim->value = from;
	anim->start_ns = motion_now_ns();
	anim->last_ns = anim->start_ns;
	anim->frames = 0;
	if (from == to && fabs(velocity) < SPRING_REST_EPSILON) {
		spring_done(anim, "done");
		return;
	}
	motion_ticker_add(&anim->ticker);
}

void
motion_spring_finish(struct motion_spring_anim *anim)
{
	if (anim->ticker.active) {
		spring_done(anim, "skipped");
	}
}

void
motion_spring_cancel(struct motion_spring_anim *anim)
{
	motion_ticker_remove(&anim->ticker);
}

bool
motion_spring_running(struct motion_spring_anim *anim)
{
	return anim->ticker.active;
}

void
motion_finish(void)
{
	while (!wl_list_empty(&motion_outputs)) {
		struct motion_output *record =
			wl_container_of(motion_outputs.next, record, link);
		handle_output_destroy(&record->destroy, NULL);
	}
}
