/*
 * ISO Recorder for OBS - time-of-day timecode
 * Copyright (C) 2026 Top Set Media, LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Frame rate description used for timecode. */
struct iso_tc_format {
	uint32_t fps_num;
	uint32_t fps_den;
	uint32_t nominal; /* rounded frames per second (60 for 59.94) */
	uint32_t drop;    /* frame numbers dropped per minute (4 for 59.94 DF, 0 = NDF) */
	bool drop_frame;
};

/* Fills `fmt` for the given frame rate. Drop-frame is used for 29.97 and
 * 59.94 when `allow_drop_frame` is set. */
void iso_tc_format_init(struct iso_tc_format *fmt, uint32_t fps_num, uint32_t fps_den, bool allow_drop_frame);

/* Number of frames in 24 hours of timecode (wraps at 24:00:00:00). */
uint32_t iso_tc_frames_per_day(const struct iso_tc_format *fmt);

/* Frame count for a time of day given in nanoseconds since local midnight.
 * Drop-frame rates count real frames (the label then tracks the wall clock);
 * other rates count at the nominal rate so the label matches the wall
 * clock at the start of the recording. */
uint32_t iso_tc_frames_from_ns_of_day(const struct iso_tc_format *fmt, uint64_t ns_of_day);

/* Formats a frame count as HH:MM:SS:FF (or HH:MM:SS;FF for drop-frame).
 * `out` must hold at least 12 characters. */
void iso_tc_to_string(const struct iso_tc_format *fmt, uint32_t frames, char *out, size_t out_size);

/* Parses HH:MM:SS:FF / HH:MM:SS;FF into a frame count. */
bool iso_tc_from_string(const struct iso_tc_format *fmt, const char *str, uint32_t *frames);

/* ------------------------------------------------------------------------ */
/* Shared session clock
 *
 * Every ISO output that records at the same time shares one anchor, so all
 * files agree on the timecode of any given frame. The anchor is created from
 * the wall clock by the first output of a session and released when the last
 * output stops. Later outputs derive their start timecode by counting OBS
 * frame intervals from the anchor, which is exact because all OBS video mixes
 * render on the same frame clock. */

void iso_clock_init(void);
void iso_clock_free(void);
void iso_clock_acquire(void);
void iso_clock_release(void);

/* Returns the timecode frame count for a video frame captured at
 * `frame_mono_ns` (OBS's os_gettime_ns clock). `frame_interval_ns` is the
 * OBS frame interval (video_output_get_frame_time). */
uint32_t iso_clock_start_frame(const struct iso_tc_format *fmt, uint64_t frame_mono_ns, uint64_t frame_interval_ns);

/* Converts a monotonic OBS timestamp to nanoseconds since local midnight. */
uint64_t iso_clock_mono_to_ns_of_day(uint64_t mono_ns);

#ifdef __cplusplus
}
#endif
