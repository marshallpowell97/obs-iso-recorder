/*
 * ISO Recorder for OBS - time-of-day timecode
 * Copyright (C) 2026 Top Set Media, LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "timecode.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <util/platform.h>
#include <util/threading.h>
#include <util/util_uint64.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define NS_PER_SEC 1000000000ULL

/* ------------------------------------------------------------------------ */
/* Frame-count maths                                                        */

void iso_tc_format_init(struct iso_tc_format *fmt, uint32_t fps_num, uint32_t fps_den, bool allow_drop_frame)
{
	memset(fmt, 0, sizeof(*fmt));
	if (!fps_num || !fps_den) {
		fps_num = 30;
		fps_den = 1;
	}

	fmt->fps_num = fps_num;
	fmt->fps_den = fps_den;
	fmt->nominal = (fps_num + fps_den / 2) / fps_den;
	if (!fmt->nominal)
		fmt->nominal = 1;

	/* SMPTE drop-frame only exists for 29.97 and 59.94. */
	bool ntsc = fps_den == 1001 && (fmt->nominal == 30 || fmt->nominal == 60);
	fmt->drop_frame = allow_drop_frame && ntsc;
	fmt->drop = fmt->drop_frame ? fmt->nominal / 15 : 0; /* 2 for 29.97, 4 for 59.94 */
}

static inline uint32_t frames_per_minute(const struct iso_tc_format *fmt)
{
	return fmt->nominal * 60 - fmt->drop;
}

static inline uint32_t frames_per_10_minutes(const struct iso_tc_format *fmt)
{
	return fmt->nominal * 600 - fmt->drop * 9;
}

uint32_t iso_tc_frames_per_day(const struct iso_tc_format *fmt)
{
	if (fmt->drop_frame)
		return frames_per_10_minutes(fmt) * 6 * 24;
	return fmt->nominal * 86400;
}

uint32_t iso_tc_frames_from_ns_of_day(const struct iso_tc_format *fmt, uint64_t ns_of_day)
{
	uint64_t frames;

	if (fmt->drop_frame) {
		/* Real frames since midnight; the drop-frame label stays within
		 * a frame or two of the wall clock. */
		uint64_t div = (uint64_t)fmt->fps_den * NS_PER_SEC;
		frames = util_mul_div64(ns_of_day, fmt->fps_num, div / 2);
		frames = (frames + 1) / 2; /* round to nearest */
	} else {
		frames = (ns_of_day * 2 * fmt->nominal / NS_PER_SEC + 1) / 2;
	}

	return (uint32_t)(frames % iso_tc_frames_per_day(fmt));
}

void iso_tc_to_string(const struct iso_tc_format *fmt, uint32_t frames, char *out, size_t out_size)
{
	uint64_t n = frames % iso_tc_frames_per_day(fmt);

	if (fmt->drop_frame) {
		/* Convert real frame count into a label count by re-inserting the
		 * dropped frame numbers (standard SMPTE 12M algorithm). */
		uint64_t fp10 = frames_per_10_minutes(fmt);
		uint64_t fpm = frames_per_minute(fmt);
		uint64_t d = n / fp10;
		uint64_t m = n % fp10;

		n += (uint64_t)fmt->drop * 9 * d;
		if (m > fmt->drop)
			n += (uint64_t)fmt->drop * ((m - fmt->drop) / fpm);
	}

	uint32_t fps = fmt->nominal;
	unsigned ff = (unsigned)(n % fps);
	unsigned ss = (unsigned)((n / fps) % 60);
	unsigned mm = (unsigned)((n / (fps * 60ULL)) % 60);
	unsigned hh = (unsigned)((n / (fps * 3600ULL)) % 24);

	snprintf(out, out_size, "%02u:%02u:%02u%c%02u", hh, mm, ss, fmt->drop_frame ? ';' : ':', ff);
}

bool iso_tc_from_string(const struct iso_tc_format *fmt, const char *str, uint32_t *frames)
{
	unsigned hh, mm, ss, ff;
	char sep;

	if (!str || sscanf(str, "%u:%u:%u%c%u", &hh, &mm, &ss, &sep, &ff) != 5)
		return false;
	if (hh > 23 || mm > 59 || ss > 59 || ff >= fmt->nominal)
		return false;

	uint64_t label = (((uint64_t)hh * 60 + mm) * 60 + ss) * fmt->nominal + ff;

	if (fmt->drop_frame) {
		uint64_t total_minutes = (uint64_t)hh * 60 + mm;
		/* Labels :00 and :01 (or :00-:03) are skipped at the start of
		 * every minute except every tenth. */
		if (mm % 10 != 0 && ss == 0 && ff < fmt->drop)
			return false;
		label -= (uint64_t)fmt->drop * (total_minutes - total_minutes / 10);
	}

	*frames = (uint32_t)label;
	return true;
}

/* ------------------------------------------------------------------------ */
/* Wall clock                                                               */

static uint64_t realtime_ns(void)
{
#ifdef _WIN32
	FILETIME ft;
	GetSystemTimePreciseAsFileTime(&ft);
	uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
	/* 100 ns intervals since 1601 -> ns since 1970 */
	return (t - 116444736000000000ULL) * 100ULL;
#else
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec * NS_PER_SEC + (uint64_t)ts.tv_nsec;
#endif
}

static uint64_t unix_ns_to_ns_of_day(uint64_t unix_ns)
{
	time_t secs = (time_t)(unix_ns / NS_PER_SEC);
	uint64_t frac = unix_ns % NS_PER_SEC;
	struct tm local;

#ifdef _WIN32
	localtime_s(&local, &secs);
#else
	localtime_r(&secs, &local);
#endif

	uint64_t sec_of_day = (uint64_t)local.tm_hour * 3600 + (uint64_t)local.tm_min * 60 + (uint64_t)local.tm_sec;
	return sec_of_day * NS_PER_SEC + frac;
}

uint64_t iso_clock_mono_to_ns_of_day(uint64_t mono_ns)
{
	/* Sample both clocks back-to-back, bracketing the realtime read with
	 * two monotonic reads to minimise scheduling error. */
	uint64_t m0 = os_gettime_ns();
	uint64_t wall = realtime_ns();
	uint64_t m1 = os_gettime_ns();
	uint64_t mono_now = m0 + (m1 - m0) / 2;

	uint64_t unix_ns = wall - (mono_now - mono_ns);
	if (mono_ns > mono_now)
		unix_ns = wall + (mono_ns - mono_now);

	return unix_ns_to_ns_of_day(unix_ns);
}

/* ------------------------------------------------------------------------ */
/* Shared session anchor                                                    */

static struct {
	bool valid;
	struct iso_tc_format fmt;
	uint64_t mono_ns;
	uint32_t frame;
} anchor;

static void anchor_reset(void)
{
	memset(&anchor, 0, sizeof(anchor));
}

static pthread_mutex_t clock_mutex;
static long clock_users = 0;

void iso_clock_init(void)
{
	pthread_mutex_init(&clock_mutex, NULL);
	clock_users = 0;
	anchor_reset();
}

void iso_clock_free(void)
{
	pthread_mutex_destroy(&clock_mutex);
}

void iso_clock_acquire(void)
{
	pthread_mutex_lock(&clock_mutex);
	clock_users++;
	pthread_mutex_unlock(&clock_mutex);
}

void iso_clock_release(void)
{
	pthread_mutex_lock(&clock_mutex);
	if (clock_users > 0)
		clock_users--;
	if (clock_users == 0)
		anchor.valid = false;
	pthread_mutex_unlock(&clock_mutex);
}

static inline bool same_format(const struct iso_tc_format *a, const struct iso_tc_format *b)
{
	return a->fps_num == b->fps_num && a->fps_den == b->fps_den && a->drop_frame == b->drop_frame;
}

uint32_t iso_clock_start_frame(const struct iso_tc_format *fmt, uint64_t frame_mono_ns, uint64_t frame_interval_ns)
{
	uint32_t result;

	pthread_mutex_lock(&clock_mutex);

	if (!anchor.valid || !same_format(&anchor.fmt, fmt) || !frame_interval_ns) {
		anchor.valid = true;
		anchor.fmt = *fmt;
		anchor.mono_ns = frame_mono_ns;
		anchor.frame = iso_tc_frames_from_ns_of_day(fmt, iso_clock_mono_to_ns_of_day(frame_mono_ns));
		result = anchor.frame;
	} else {
		/* Count whole OBS frame intervals from the anchor. */
		int64_t diff = (int64_t)(frame_mono_ns - anchor.mono_ns);
		int64_t interval = (int64_t)frame_interval_ns;
		int64_t elapsed = diff >= 0 ? (diff + interval / 2) / interval : -((-diff + interval / 2) / interval);

		int64_t per_day = (int64_t)iso_tc_frames_per_day(fmt);
		int64_t frame = ((int64_t)anchor.frame + elapsed) % per_day;
		if (frame < 0)
			frame += per_day;
		result = (uint32_t)frame;
	}

	pthread_mutex_unlock(&clock_mutex);
	return result;
}
