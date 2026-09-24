/*
 * ISO Recorder for OBS - "ISO Record" filter
 * Copyright (C) 2026 Top Set Media, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * Add the filter to a camera source (e.g. a DeckLink input). While
 * recording, the source is rendered into its own OBS view at the source's
 * native resolution, encoded with its own video encoder and written by the
 * iso_mov_output (growing MOV + time-of-day timecode).
 */

#include "iso-output.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#define do_log(level, format, ...) \
	blog(level, "[iso-recorder: '%s'] " format, obs_source_get_name(f->source), ##__VA_ARGS__)
#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)

#define S_FOLDER "folder"
#define S_FILENAME_FORMAT "filename_format"
#define S_ENCODER "encoder"
#define S_BITRATE "bitrate"
#define S_KEYINT "keyint_sec"
#define S_AUDIO_TRACK "audio_track"
#define S_FOLLOW "follow_recording"
#define S_DROP_FRAME "drop_frame"
#define S_MIN_FRAGMENT "min_fragment_ms"

#define DEFAULT_FILENAME_FORMAT "%CCYY-%MM-%DD %hh-%mm-%ss"

/* Resources owned by one recording. A session is detached from its filter as
 * soon as a stop is requested, so the filter can start a new recording while
 * the old file is still being finalised. The session then releases itself on
 * OBS's destroy thread once the output reports "stop". */
struct iso_session {
	obs_output_t *output;
	obs_encoder_t *venc;
	obs_encoder_t *aenc;
	obs_view_t *view;
	obs_weak_source_t *parent;

	/* Owning filter while this is the filter's current session, NULL once
	 * detached. Guarded by session_mutex. */
	struct iso_filter *filter;
	struct dstr name; /* for logging after the filter may be gone */
};

struct iso_filter {
	obs_source_t *source;
	pthread_mutex_t mutex; /* serialises start/stop of this filter */
	struct iso_session *session; /* guarded by session_mutex */
	struct dstr last_path;
};

/* ------------------------------------------------------------------------ */
/* Registry of filter instances                                             */

static pthread_mutex_t filters_mutex;
static DARRAY(struct iso_filter *) filters;

/* Guards iso_filter.session and iso_session.filter. Only ever held briefly
 * and never while calling into OBS, so the output's stop signal can take it
 * from any thread. Lock order: iso_filter.mutex -> session_mutex. */
static pthread_mutex_t session_mutex;

void iso_filters_init(void)
{
	pthread_mutex_init(&filters_mutex, NULL);
	pthread_mutex_init(&session_mutex, NULL);
	da_init(filters);
}

void iso_filters_free(void)
{
	da_free(filters);
	pthread_mutex_destroy(&session_mutex);
	pthread_mutex_destroy(&filters_mutex);
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */

static const char *preferred_video_encoders[] = {
	"obs_nvenc_hevc_tex",
	"jim_hevc_nvenc",
	"ffmpeg_hevc_nvenc",
	"com.apple.videotoolbox.videoencoder.ave.hevc",
	"h265_texture_amf",
	"obs_qsv11_hevc",
	"obs_nvenc_h264_tex",
	"jim_nvenc",
	"com.apple.videotoolbox.videoencoder.ave.avc",
	"obs_x264",
	NULL,
};

static const char *preferred_audio_encoders[] = {"CoreAudio_AAC", "ffmpeg_aac", "libfdk_aac", NULL};

static bool encoder_available(const char *id)
{
	const char *val;
	for (size_t i = 0; obs_enum_encoder_types(i, &val); i++) {
		if (strcmp(val, id) == 0)
			return true;
	}
	return false;
}

static const char *first_available(const char **ids)
{
	for (size_t i = 0; ids[i]; i++) {
		if (encoder_available(ids[i]))
			return ids[i];
	}
	return NULL;
}

static bool is_usable_video_encoder(const char *id)
{
	if (obs_get_encoder_type(id) != OBS_ENCODER_VIDEO)
		return false;
	if (obs_get_encoder_caps(id) & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL))
		return false;

	const char *codec = obs_get_encoder_codec(id);
	return codec && (strcmp(codec, "hevc") == 0 || strcmp(codec, "h264") == 0 || strcmp(codec, "prores") == 0);
}

/* B-frames make fragmented files start their video a few frames after the
 * timecode track while the file is growing (the offset disappears only when
 * the file is finalised), so ISOs are always encoded without them. The
 * setting's name and type differ per encoder. */
static void disable_b_frames(const char *enc_id, obs_data_t *enc_settings)
{
	if (strstr(enc_id, "videotoolbox"))
		obs_data_set_bool(enc_settings, "bframes", false);
	else if (strstr(enc_id, "qsv"))
		obs_data_set_int(enc_settings, "bframes", 0);
	else if (strcmp(enc_id, "obs_x264") == 0)
		obs_data_set_string(enc_settings, "x264opts", "bframes=0");
	else
		obs_data_set_int(enc_settings, "bf", 0); /* NVENC, AMF */
}

static void sanitize_filename(struct dstr *str)
{
	for (size_t i = 0; i < str->len; i++) {
		char c = str->array[i];
		if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
		    c == '|')
			str->array[i] = '_';
	}
}

static void make_unique_path(struct dstr *path)
{
	if (!os_file_exists(path->array))
		return;

	const char *ext = strrchr(path->array, '.');
	size_t ext_start = ext ? (size_t)(ext - path->array) : path->len;
	struct dstr ext_str = {0};
	dstr_copy(&ext_str, ext ? ext : "");

	struct dstr test = {0};
	for (int num = 2;; num++) {
		dstr_ncopy(&test, path->array, ext_start);
		dstr_catf(&test, " (%d)", num);
		dstr_cat_dstr(&test, &ext_str);
		if (!os_file_exists(test.array))
			break;
	}

	dstr_move(path, &test);
	dstr_free(&ext_str);
}

static void build_path(struct iso_filter *f, obs_data_t *settings, struct dstr *path)
{
	const char *folder = obs_data_get_string(settings, S_FOLDER);
	const char *format = obs_data_get_string(settings, S_FILENAME_FORMAT);

	dstr_free(path);
	if (folder && *folder) {
		dstr_copy(path, folder);
	} else {
		/* Fall back to OBS's own recording folder. */
		char *rec = obs_frontend_get_current_record_output_path();
		if (rec) {
			dstr_copy(path, rec);
			bfree(rec);
		}
	}

	if (!path->len)
		dstr_copy(path, ".");

	dstr_replace(path, "\\", "/");
	if (dstr_end(path) != '/')
		dstr_cat_ch(path, '/');

	obs_source_t *parent = obs_filter_get_parent(f->source);
	struct dstr name = {0};
	dstr_copy(&name, parent ? obs_source_get_name(parent) : "ISO");
	sanitize_filename(&name);

	char *stamp = os_generate_formatted_filename("mov", true, format && *format ? format : DEFAULT_FILENAME_FORMAT);
	dstr_cat_dstr(path, &name);
	dstr_cat_ch(path, ' ');
	dstr_cat(path, stamp);
	bfree(stamp);
	dstr_free(&name);

	make_unique_path(path);
}

/* ------------------------------------------------------------------------ */
/* Session lifetime                                                         */

static void session_release(struct iso_session *s)
{
	if (!s)
		return;

	obs_output_release(s->output);
	obs_encoder_release(s->venc);
	obs_encoder_release(s->aenc);

	if (s->view) {
		obs_view_set_source(s->view, 0, NULL);
		obs_view_remove(s->view);
		obs_view_destroy(s->view);
	}

	if (s->parent) {
		obs_source_t *parent = obs_weak_source_get_source(s->parent);
		if (parent) {
			obs_source_dec_showing(parent);
			obs_source_release(parent);
		}
		obs_weak_source_release(s->parent);
	}

	dstr_free(&s->name);
	bfree(s);
}

static void session_release_task(void *param)
{
	session_release(param);
}

/* Detaches `f`'s current session, if any, and returns it. */
static struct iso_session *detach_session(struct iso_filter *f)
{
	pthread_mutex_lock(&session_mutex);
	struct iso_session *s = f->session;
	if (s) {
		f->session = NULL;
		s->filter = NULL;
	}
	pthread_mutex_unlock(&session_mutex);
	return s;
}

/* "stop" signal of a session's output. Runs once per started output, on
 * OBS's output thread. `data` is the session, never the filter, so this is
 * safe even after the filter has been destroyed. */
static void output_stopped(void *data, calldata_t *cd)
{
	struct iso_session *s = data;
	int code = (int)calldata_int(cd, "code");

	/* Unexpected stop (e.g. write error): detach from the filter so it
	 * shows as not recording. */
	pthread_mutex_lock(&session_mutex);
	if (s->filter) {
		s->filter->session = NULL;
		s->filter = NULL;
	}
	pthread_mutex_unlock(&session_mutex);

	if (code != OBS_OUTPUT_SUCCESS) {
		const char *err = obs_output_get_last_error(s->output);
		blog(LOG_WARNING, "[iso-recorder: '%s'] Recording stopped with error %d: %s", s->name.array, code,
		     err ? err : "unknown");
	} else {
		blog(LOG_INFO, "[iso-recorder: '%s'] Recording stopped", s->name.array);
	}

	obs_queue_task(OBS_TASK_DESTROY, session_release_task, s, false);
}

/* ------------------------------------------------------------------------ */
/* Start / stop                                                             */

static bool iso_filter_start(struct iso_filter *f)
{
	bool ok = false;

	pthread_mutex_lock(&f->mutex);

	pthread_mutex_lock(&session_mutex);
	bool running = f->session != NULL;
	pthread_mutex_unlock(&session_mutex);
	if (running) {
		ok = true;
		goto unlock;
	}

	if (!obs_source_enabled(f->source)) {
		info("Filter is disabled, not recording");
		goto unlock;
	}

	obs_source_t *parent = obs_filter_get_parent(f->source);
	if (!parent) {
		warn("Filter has no parent source");
		goto unlock;
	}

	uint32_t width = obs_source_get_width(parent);
	uint32_t height = obs_source_get_height(parent);
	/* OBS's raw frames come out sheared when the width is not a multiple of
	 * 4 (seen at 1366 and 1370 wide with every encoder), so pad the view:
	 * the source renders at native size with up to 3 px of black at the
	 * right. Height only needs to be even for 4:2:0. */
	width = (width + 3) & ~3u;
	height += height & 1;
	if (!width || !height) {
		warn("Source has no video yet (0x0), not recording");
		goto unlock;
	}

	obs_data_t *settings = obs_source_get_settings(f->source);

	struct iso_session *s = bzalloc(sizeof(struct iso_session));
	dstr_copy(&s->name, obs_source_get_name(f->source));

	/* View rendering just this source at its native size, on OBS's
	 * main frame clock. */
	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		warn("OBS video is not initialised");
		bfree(s);
		obs_data_release(settings);
		goto unlock;
	}
	ovi.base_width = width;
	ovi.base_height = height;
	ovi.output_width = width;
	ovi.output_height = height;

	s->view = obs_view_create();
	obs_view_set_source(s->view, 0, parent);
	video_t *video = obs_view_add2(s->view, &ovi);
	if (!video) {
		warn("Could not create video mix for source");
		goto fail;
	}

	s->parent = obs_source_get_weak_source(parent);
	obs_source_inc_showing(parent);

	/* Video encoder */
	const char *enc_id = obs_data_get_string(settings, S_ENCODER);
	if (!enc_id || !*enc_id || !encoder_available(enc_id))
		enc_id = first_available(preferred_video_encoders);
	if (!enc_id) {
		warn("No usable video encoder found");
		goto fail;
	}

	obs_data_t *enc_settings = obs_data_create();
	obs_data_set_string(enc_settings, "rate_control", "CBR");
	obs_data_set_int(enc_settings, "bitrate", obs_data_get_int(settings, S_BITRATE));
	obs_data_set_int(enc_settings, "keyint_sec", obs_data_get_int(settings, S_KEYINT));
	disable_b_frames(enc_id, enc_settings);

	struct dstr enc_name = {0};
	dstr_printf(&enc_name, "ISO video: %s", obs_source_get_name(parent));
	s->venc = obs_video_encoder_create(enc_id, enc_name.array, enc_settings, NULL);
	obs_data_release(enc_settings);
	if (!s->venc) {
		warn("Could not create video encoder '%s'", enc_id);
		dstr_free(&enc_name);
		goto fail;
	}
	obs_encoder_set_video(s->venc, video);

	/* Audio encoder (optional, from one of OBS's mix tracks) */
	long long track = obs_data_get_int(settings, S_AUDIO_TRACK);
	if (track >= 1 && track <= MAX_AUDIO_MIXES) {
		const char *aenc_id = first_available(preferred_audio_encoders);
		if (aenc_id) {
			obs_data_t *aenc_settings = obs_data_create();
			obs_data_set_int(aenc_settings, "bitrate", 192);
			dstr_printf(&enc_name, "ISO audio: %s", obs_source_get_name(parent));
			s->aenc = obs_audio_encoder_create(aenc_id, enc_name.array, aenc_settings, (size_t)track - 1,
							   NULL);
			obs_data_release(aenc_settings);
			if (s->aenc)
				obs_encoder_set_audio(s->aenc, obs_get_audio());
		} else {
			warn("No AAC encoder available, recording without audio");
		}
	}
	dstr_free(&enc_name);

	/* Output */
	build_path(f, settings, &f->last_path);

	obs_data_t *out_settings = obs_data_create();
	obs_data_set_string(out_settings, "path", f->last_path.array);
	obs_data_set_bool(out_settings, "timecode", true);
	obs_data_set_bool(out_settings, "drop_frame", obs_data_get_bool(settings, S_DROP_FRAME));
	obs_data_set_int(out_settings, "min_fragment_ms", obs_data_get_int(settings, S_MIN_FRAGMENT));

	struct dstr out_name = {0};
	dstr_printf(&out_name, "ISO: %s", obs_source_get_name(parent));
	s->output = obs_output_create(ISO_OUTPUT_ID, out_name.array, out_settings, NULL);
	dstr_free(&out_name);
	obs_data_release(out_settings);
	if (!s->output) {
		warn("Could not create output");
		goto fail;
	}

	obs_output_set_video_encoder(s->output, s->venc);
	if (s->aenc)
		obs_output_set_audio_encoder(s->output, s->aenc, 0);

	/* Publish before starting: an output that fails right after starting
	 * emits "stop", and output_stopped must find the session attached. */
	signal_handler_connect(obs_output_get_signal_handler(s->output), "stop", output_stopped, s);
	pthread_mutex_lock(&session_mutex);
	s->filter = f;
	f->session = s;
	pthread_mutex_unlock(&session_mutex);

	if (!obs_output_start(s->output)) {
		/* A failed start does not emit "stop", so the session is still
		 * ours to release. */
		const char *err = obs_output_get_last_error(s->output);
		warn("Could not start recording: %s", err ? err : "unknown error");
		detach_session(f);
		signal_handler_disconnect(obs_output_get_signal_handler(s->output), "stop", output_stopped, s);
		goto fail;
	}

	info("Recording to '%s' with %s", f->last_path.array, enc_id);
	obs_data_release(settings);
	ok = true;
	goto unlock;

fail:
	obs_data_release(settings);
	session_release(s);

unlock:
	pthread_mutex_unlock(&f->mutex);
	return ok;
}

static void iso_filter_stop(struct iso_filter *f)
{
	pthread_mutex_lock(&f->mutex);

	/* Detach first, so the filter can start again immediately while the
	 * old file is finalised. The session holds its own output reference
	 * until output_stopped queues its release. */
	pthread_mutex_lock(&session_mutex);
	struct iso_session *s = f->session;
	obs_output_t *output = s ? obs_output_get_ref(s->output) : NULL;
	if (s) {
		f->session = NULL;
		s->filter = NULL;
	}
	pthread_mutex_unlock(&session_mutex);

	pthread_mutex_unlock(&f->mutex);

	if (output) {
		info("Stopping, finalising '%s'", obs_output_get_name(output));
		obs_output_stop(output);
		obs_output_release(output);
	}
}

static bool iso_filter_active(struct iso_filter *f)
{
	pthread_mutex_lock(&session_mutex);
	bool active = f->session != NULL;
	pthread_mutex_unlock(&session_mutex);
	return active;
}

void iso_filters_start_all(bool only_follow_main)
{
	pthread_mutex_lock(&filters_mutex);
	for (size_t i = 0; i < filters.num; i++) {
		struct iso_filter *f = filters.array[i];
		obs_data_t *settings = obs_source_get_settings(f->source);
		bool follow = obs_data_get_bool(settings, S_FOLLOW);
		obs_data_release(settings);

		if (!only_follow_main || follow)
			iso_filter_start(f);
	}
	pthread_mutex_unlock(&filters_mutex);
}

void iso_filters_stop_all(bool only_follow_main)
{
	pthread_mutex_lock(&filters_mutex);
	for (size_t i = 0; i < filters.num; i++) {
		struct iso_filter *f = filters.array[i];
		obs_data_t *settings = obs_source_get_settings(f->source);
		bool follow = obs_data_get_bool(settings, S_FOLLOW);
		obs_data_release(settings);

		if (!only_follow_main || follow)
			iso_filter_stop(f);
	}
	pthread_mutex_unlock(&filters_mutex);
}

size_t iso_filters_count_active(void)
{
	size_t count = 0;
	pthread_mutex_lock(&filters_mutex);
	for (size_t i = 0; i < filters.num; i++) {
		if (iso_filter_active(filters.array[i]))
			count++;
	}
	pthread_mutex_unlock(&filters_mutex);
	return count;
}

/* ------------------------------------------------------------------------ */
/* obs_source_info callbacks                                                */

static const char *iso_filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ISORecord");
}

static void *iso_filter_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);

	struct iso_filter *f = bzalloc(sizeof(struct iso_filter));
	f->source = source;
	pthread_mutex_init(&f->mutex, NULL);

	pthread_mutex_lock(&filters_mutex);
	da_push_back(filters, &f);
	pthread_mutex_unlock(&filters_mutex);

	return f;
}

static void iso_filter_destroy(void *data)
{
	struct iso_filter *f = data;

	pthread_mutex_lock(&filters_mutex);
	da_erase_item(filters, &f);
	pthread_mutex_unlock(&filters_mutex);

	/* Filter removed while recording: stop like a normal stop. The file is
	 * finalised and the session released by output_stopped, which never
	 * touches the filter once detached. */
	iso_filter_stop(f);

	dstr_free(&f->last_path);
	pthread_mutex_destroy(&f->mutex);
	bfree(f);
}

static void iso_filter_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct iso_filter *f = data;
	obs_source_skip_video_filter(f->source);
}

static void iso_filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, S_FOLDER, "");
	obs_data_set_default_string(settings, S_FILENAME_FORMAT, DEFAULT_FILENAME_FORMAT);
	const char *enc = first_available(preferred_video_encoders);
	obs_data_set_default_string(settings, S_ENCODER, enc ? enc : "");
	obs_data_set_default_int(settings, S_BITRATE, 50000);
	obs_data_set_default_int(settings, S_KEYINT, 1);
	obs_data_set_default_int(settings, S_AUDIO_TRACK, 1);
	obs_data_set_default_bool(settings, S_FOLLOW, true);
	obs_data_set_default_bool(settings, S_DROP_FRAME, true);
	obs_data_set_default_int(settings, S_MIN_FRAGMENT, 0);
}

static bool start_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	iso_filter_start(data);
	return false;
}

static bool stop_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	iso_filter_stop(data);
	return false;
}

static obs_properties_t *iso_filter_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_path(props, S_FOLDER, obs_module_text("Folder"), OBS_PATH_DIRECTORY, NULL, NULL);
	obs_properties_add_text(props, S_FILENAME_FORMAT, obs_module_text("FilenameFormat"), OBS_TEXT_DEFAULT);

	obs_property_t *p = obs_properties_add_list(props, S_ENCODER, obs_module_text("Encoder"), OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_STRING);
	const char *id;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); i++) {
		if (is_usable_video_encoder(id))
			obs_property_list_add_string(p, obs_encoder_get_display_name(id), id);
	}

	p = obs_properties_add_int(props, S_BITRATE, obs_module_text("Bitrate"), 1000, 400000, 1000);
	obs_property_int_set_suffix(p, " Kbps");
	p = obs_properties_add_int(props, S_KEYINT, obs_module_text("KeyframeInterval"), 1, 10, 1);
	obs_property_int_set_suffix(p, " s");

	p = obs_properties_add_list(props, S_AUDIO_TRACK, obs_module_text("AudioTrack"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("AudioTrack.None"), 0);
	for (int i = 1; i <= MAX_AUDIO_MIXES; i++) {
		struct dstr name = {0};
		dstr_printf(&name, "%s %d", obs_module_text("AudioTrack.Track"), i);
		obs_property_list_add_int(p, name.array, i);
		dstr_free(&name);
	}

	obs_properties_add_bool(props, S_FOLLOW, obs_module_text("FollowRecording"));
	obs_properties_add_bool(props, S_DROP_FRAME, obs_module_text("DropFrame"));

	p = obs_properties_add_int(props, S_MIN_FRAGMENT, obs_module_text("MinFragment"), 0, 10000, 100);
	obs_property_int_set_suffix(p, " ms");
	obs_property_set_long_description(p, obs_module_text("MinFragment.Description"));

	obs_properties_add_button2(props, "start", obs_module_text("Start"), start_clicked, data);
	obs_properties_add_button2(props, "stop", obs_module_text("Stop"), stop_clicked, data);

	return props;
}

struct obs_source_info iso_record_filter_info = {
	.id = ISO_FILTER_ID,
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = iso_filter_get_name,
	.create = iso_filter_create,
	.destroy = iso_filter_destroy,
	.video_render = iso_filter_render,
	.get_defaults = iso_filter_defaults,
	.get_properties = iso_filter_properties,
};
