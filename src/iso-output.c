/*
 * ISO Recorder for OBS - growing MOV output with time-of-day timecode
 *
 * Based on OBS Studio's Hybrid MP4/MOV output (plugins/obs-outputs/mp4-output.c)
 * Copyright (C) 2024 by Dennis Sädtler <dennis@obsproject.com>
 * Modifications Copyright (C) 2026 Top Set Media, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * Differences from OBS's mov_output:
 *   - always writes a QuickTime timecode track (time of day, shared clock)
 *   - configurable minimum fragment duration
 *   - no file splitting, chapters or BPM (not needed for ISO recording)
 */

#include "iso-output.h"
#include "mux/mp4-mux.h"
#include "timecode.h"

#include <inttypes.h>

#include <obs-module.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <util/threading.h>
#include <util/buffered-file-serializer.h>

#define do_log(level, format, ...) \
	blog(level, "[iso-recorder output: '%s'] " format, obs_output_get_name(out->output), ##__VA_ARGS__)

#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)

struct iso_output {
	obs_output_t *output;
	struct dstr path;

	struct serializer serializer;

	volatile bool active;
	volatile bool stopping;
	uint64_t stop_ts;

	uint64_t total_bytes;

	pthread_mutex_t mutex;

	struct mp4_mux *muxer;
	int flags;

	/* Timecode */
	bool tc_enabled;
	bool tc_allow_drop_frame;
	bool tc_set;
	bool clock_acquired;
	char tc_string[16];

	int64_t min_frag_usec;
};

static inline bool stopping(struct iso_output *out)
{
	return os_atomic_load_bool(&out->stopping);
}

static inline bool active(struct iso_output *out)
{
	return os_atomic_load_bool(&out->active);
}

static const char *iso_output_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ISOOutput");
}

static void iso_output_destroy(void *data)
{
	struct iso_output *out = data;

	/* Output destroyed without a clean stop (e.g. force-stopped): still
	 * finalise the file so it ends up as a regular MOV. */
	if (out->muxer) {
		blog(LOG_WARNING, "[iso-recorder output] Finalising '%s' after forced stop", out->path.array);
		mp4_mux_finalise(out->muxer);
		buffered_file_serializer_free(&out->serializer);
		mp4_mux_destroy(out->muxer);
		out->muxer = NULL;
	}

	if (out->clock_acquired)
		iso_clock_release();

	pthread_mutex_destroy(&out->mutex);
	dstr_free(&out->path);
	bfree(out);
}

static void get_start_timecode_proc(void *data, calldata_t *cd)
{
	struct iso_output *out = data;
	pthread_mutex_lock(&out->mutex);
	calldata_set_string(cd, "timecode", out->tc_set ? out->tc_string : "");
	pthread_mutex_unlock(&out->mutex);
}

static void get_path_proc(void *data, calldata_t *cd)
{
	struct iso_output *out = data;
	calldata_set_string(cd, "path", out->path.array ? out->path.array : "");
}

static void *iso_output_create(obs_data_t *settings, obs_output_t *output)
{
	UNUSED_PARAMETER(settings);

	struct iso_output *out = bzalloc(sizeof(struct iso_output));
	out->output = output;
	pthread_mutex_init(&out->mutex, NULL);

	proc_handler_t *ph = obs_output_get_proc_handler(output);
	proc_handler_add(ph, "void get_start_timecode(out string timecode)", get_start_timecode_proc, out);
	proc_handler_add(ph, "void get_path(out string path)", get_path_proc, out);

	return out;
}

static void iso_output_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "timecode", true);
	obs_data_set_default_bool(settings, "drop_frame", true);
	obs_data_set_default_int(settings, "min_fragment_ms", 0);
}

static bool iso_output_start(void *data)
{
	struct iso_output *out = data;

	if (!obs_output_can_begin_data_capture(out->output, 0))
		return false;
	if (!obs_output_initialize_encoders(out->output, 0))
		return false;

	os_atomic_set_bool(&out->stopping, false);

	obs_data_t *settings = obs_output_get_settings(out->output);
	const char *path = obs_data_get_string(settings, "path");
	out->tc_enabled = obs_data_get_bool(settings, "timecode");
	out->tc_allow_drop_frame = obs_data_get_bool(settings, "drop_frame");
	out->min_frag_usec = obs_data_get_int(settings, "min_fragment_ms") * 1000;
	dstr_copy(&out->path, path);
	obs_data_release(settings);

	if (!out->path.len) {
		warn("No output path set");
		return false;
	}

	/* Make sure the destination folder exists. */
	char *slash = strrchr(out->path.array, '/');
	if (slash) {
		*slash = 0;
		os_mkdirs(out->path.array);
		*slash = '/';
	}

	if (!buffered_file_serializer_init(&out->serializer, out->path.array, 0, 0)) {
		warn("Unable to open file '%s'", out->path.array);
		return false;
	}

	out->flags = MP4_USE_NEGATIVE_CTS;
	out->muxer = mp4_mux_create(out->output, &out->serializer, out->flags, FLAVOR_MOV);
	mp4_mux_set_min_fragment_duration(out->muxer, out->min_frag_usec);

	out->tc_set = false;
	out->tc_string[0] = 0;
	out->total_bytes = 0;

	if (out->tc_enabled && !out->clock_acquired) {
		iso_clock_acquire();
		out->clock_acquired = true;
	}

	os_atomic_set_bool(&out->active, true);
	obs_output_begin_data_capture(out->output, 0);

	info("Writing growing MOV file '%s'...", out->path.array);
	return true;
}

static void iso_output_stop(void *data, uint64_t ts)
{
	struct iso_output *out = data;
	out->stop_ts = ts / 1000;
	os_atomic_set_bool(&out->stopping, true);
}

static void iso_mux_destroy_task(void *ptr)
{
	mp4_mux_destroy(ptr);
}

static void iso_output_actual_stop(struct iso_output *out, int code)
{
	os_atomic_set_bool(&out->active, false);

	uint64_t start_time = os_gettime_ns();

	mp4_mux_finalise(out->muxer);

	if (code) {
		obs_output_signal_stop(out->output, code);
	} else {
		obs_output_end_data_capture(out->output);
	}

	info("Waiting for file writer to finish...");

	buffered_file_serializer_free(&out->serializer);
	obs_queue_task(OBS_TASK_DESTROY, iso_mux_destroy_task, out->muxer, false);
	out->muxer = NULL;

	if (out->clock_acquired) {
		iso_clock_release();
		out->clock_acquired = false;
	}

	info("File output complete. Finalization took %" PRIu64 " ms.", (os_gettime_ns() - start_time) / 1000000);
}

/* Called for the first video packet of the file. The first packet an output
 * receives is always a keyframe, and OBS sets its sys_dts_usec to the capture
 * time of that frame (os_gettime_ns clock), so it marks media time zero. */
static void setup_timecode(struct iso_output *out, struct encoder_packet *pkt)
{
	video_t *video = obs_output_video(out->output);
	const struct video_output_info *voi = video ? video_output_get_info(video) : NULL;
	if (!voi) {
		warn("No video info, timecode disabled");
		return;
	}

	struct iso_tc_format fmt;
	iso_tc_format_init(&fmt, voi->fps_num, voi->fps_den, out->tc_allow_drop_frame);

	uint64_t frame_mono_ns = (uint64_t)pkt->sys_dts_usec * 1000ULL;
	uint64_t interval = video_output_get_frame_time(video);
	uint32_t start = iso_clock_start_frame(&fmt, frame_mono_ns, interval);

	if (!mp4_mux_set_timecode(out->muxer, start, fmt.fps_num, fmt.fps_den, fmt.drop_frame)) {
		warn("Could not add timecode track");
		return;
	}

	iso_tc_to_string(&fmt, start, out->tc_string, sizeof(out->tc_string));
	out->tc_set = true;
	info("Start timecode %s (%u/%u fps%s)", out->tc_string, fmt.fps_num, fmt.fps_den,
	     fmt.drop_frame ? ", drop-frame" : "");
}

static void iso_output_packet(void *data, struct encoder_packet *packet)
{
	struct iso_output *out = data;

	pthread_mutex_lock(&out->mutex);

	if (!active(out))
		goto unlock;

	if (!packet) {
		iso_output_actual_stop(out, OBS_OUTPUT_ENCODE_ERROR);
		goto unlock;
	}

	if (stopping(out)) {
		if (packet->sys_dts_usec >= (int64_t)out->stop_ts) {
			iso_output_actual_stop(out, 0);
			goto unlock;
		}
	}

	if (out->tc_enabled && !out->tc_set && packet->type == OBS_ENCODER_VIDEO && packet->track_idx == 0)
		setup_timecode(out, packet);

	out->total_bytes += packet->size;
	mp4_mux_submit_packet(out->muxer, packet);

	if (serializer_get_pos(&out->serializer) == -1)
		iso_output_actual_stop(out, OBS_OUTPUT_ERROR);

unlock:
	pthread_mutex_unlock(&out->mutex);
}

static obs_properties_t *iso_output_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "path", obs_module_text("FilePath"), OBS_TEXT_DEFAULT);
	obs_properties_add_bool(props, "timecode", obs_module_text("Timecode"));
	obs_properties_add_bool(props, "drop_frame", obs_module_text("DropFrame"));
	obs_properties_add_int(props, "min_fragment_ms", obs_module_text("MinFragment"), 0, 10000, 100);
	return props;
}

static uint64_t iso_output_total_bytes(void *data)
{
	struct iso_output *out = data;
	return out->total_bytes;
}

struct obs_output_info iso_mov_output_info = {
	.id = ISO_OUTPUT_ID,
	.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_MULTI_TRACK_AV,
	.encoded_video_codecs = "h264;hevc;prores",
	.encoded_audio_codecs = "aac;alac",
	.get_name = iso_output_name,
	.create = iso_output_create,
	.destroy = iso_output_destroy,
	.start = iso_output_start,
	.stop = iso_output_stop,
	.encoded_packet = iso_output_packet,
	.get_defaults = iso_output_defaults,
	.get_properties = iso_output_properties,
	.get_total_bytes = iso_output_total_bytes,
};
