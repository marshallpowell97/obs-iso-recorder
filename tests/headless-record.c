/*
 * Headless end-to-end test (Linux, run under Xvfb):
 * starts libobs, loads the ISO Recorder plugin plus stock OBS modules, adds
 * two test-pattern sources with the ISO Record filter, records for N seconds
 * via the plugin's "start all" procedure, then stops and shuts down.
 *
 * usage: headless-record <plugin.so> <plugin data dir> <out dir> <seconds>
 */

#include <obs.h>
#include <obs-nix-platform.h>
#include <util/platform.h>

#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SYS_PLUGINS "/usr/lib/x86_64-linux-gnu/obs-plugins/"
#define SYS_DATA "/usr/share/obs/obs-plugins/"

static void do_log(int level, const char *fmt, va_list args, void *param)
{
	(void)param;
	if (level > LOG_INFO)
		return;
	char buf[4096];
	vsnprintf(buf, sizeof(buf), fmt, args);
	fprintf(stderr, "[obs %d] %s\n", level, buf);
}

static bool load(const char *bin, const char *data)
{
	obs_module_t *mod = NULL;
	if (obs_open_module(&mod, bin, data) != MODULE_SUCCESS) {
		fprintf(stderr, "failed to open %s\n", bin);
		return false;
	}
	if (!obs_init_module(mod)) {
		fprintf(stderr, "failed to init %s\n", bin);
		return false;
	}
	return true;
}

static obs_source_t *make_camera(obs_scene_t *scene, const char *name, const char *lavfi, const char *out_dir)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_bool(s, "is_local_file", false);
	obs_data_set_string(s, "input", lavfi);
	obs_data_set_string(s, "input_format", "lavfi");
	obs_data_set_bool(s, "restart_on_activate", false);
	obs_data_set_bool(s, "close_when_inactive", false);
	obs_source_t *src = obs_source_create("ffmpeg_source", name, s, NULL);
	obs_data_release(s);
	obs_scene_add(scene, src);

	obs_data_t *fs = obs_data_create();
	obs_data_set_string(fs, "folder", out_dir);
	obs_data_set_string(fs, "encoder", "obs_x264");
	obs_data_set_int(fs, "bitrate", 6000);
	obs_data_set_int(fs, "keyint_sec", 1);
	obs_data_set_int(fs, "audio_track", 1);
	obs_source_t *filter = obs_source_create("iso_record_filter", "ISO Record", fs, NULL);
	obs_data_release(fs);
	obs_source_filter_add(src, filter);
	obs_source_release(filter);
	return src;
}

int main(int argc, char **argv)
{
	if (argc < 5) {
		fprintf(stderr, "usage: %s plugin.so datadir outdir seconds\n", argv[0]);
		return 2;
	}
	const char *plugin = argv[1], *plugin_data = argv[2], *out_dir = argv[3];
	int seconds = atoi(argv[4]);
	uint32_t cw = argc > 6 ? (uint32_t)atoi(argv[5]) : 1280, ch = argc > 6 ? (uint32_t)atoi(argv[6]) : 720;

	base_set_log_handler(do_log, NULL);

	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "no X display\n");
		return 1;
	}
	obs_set_nix_platform(OBS_NIX_PLATFORM_X11_EGL);
	obs_set_nix_platform_display(dpy);

	if (!obs_startup("en-US", NULL, NULL))
		return 1;

	struct obs_video_info ovi = {0};
	ovi.graphics_module = "libobs-opengl.so.30";
	ovi.fps_num = 60000;
	ovi.fps_den = 1001;
	ovi.base_width = ovi.output_width = cw;
	ovi.base_height = ovi.output_height = ch;
	ovi.output_format = VIDEO_FORMAT_NV12;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.adapter = 0;
	ovi.gpu_conversion = true;
	ovi.scale_type = OBS_SCALE_BICUBIC;
	int r = obs_reset_video(&ovi);
	if (r != OBS_VIDEO_SUCCESS) {
		fprintf(stderr, "obs_reset_video failed: %d\n", r);
		return 1;
	}

	struct obs_audio_info ai = {48000, SPEAKERS_STEREO};
	if (!obs_reset_audio(&ai))
		return 1;

	if (!load(SYS_PLUGINS "obs-ffmpeg.so", SYS_DATA "obs-ffmpeg") ||
	    !load(SYS_PLUGINS "obs-x264.so", SYS_DATA "obs-x264") || !load(plugin, plugin_data))
		return 1;
	obs_post_load_modules();

	obs_scene_t *scene = obs_scene_create("Scene");
	char lavfi1[128], lavfi2[128];
	snprintf(lavfi1, sizeof(lavfi1), "testsrc2=size=%ux%u:rate=60000/1001", cw, ch);
	snprintf(lavfi2, sizeof(lavfi2), "smptehdbars=size=%ux%u:rate=60000/1001", cw, ch);
	obs_source_t *cam1 = make_camera(scene, "CAM 1", lavfi1, out_dir);
	obs_source_t *cam2 = make_camera(scene, "CAM 2", lavfi2, out_dir);

	/* Audio for the mix track: a tone source */
	obs_data_t *as = obs_data_create();
	obs_data_set_bool(as, "is_local_file", false);
	obs_data_set_string(as, "input", "sine=frequency=1000:sample_rate=48000");
	obs_data_set_string(as, "input_format", "lavfi");
	obs_source_t *tone = obs_source_create("ffmpeg_source", "Tone", as, NULL);
	obs_data_release(as);
	obs_scene_add(scene, tone);

	obs_set_output_source(0, obs_scene_get_source(scene));

	/* Let the sources produce their first frames. */
	for (int i = 0; i < 50 && (!obs_source_get_width(cam1) || !obs_source_get_width(cam2)); i++)
		os_sleep_ms(100);
	fprintf(stderr, "sources ready: %ux%u and %ux%u\n", obs_source_get_width(cam1), obs_source_get_height(cam1),
		obs_source_get_width(cam2), obs_source_get_height(cam2));

	proc_handler_t *ph = obs_get_proc_handler();
	calldata_t cd = {0};
	proc_handler_call(ph, "iso_recorder_start_all", &cd);
	calldata_free(&cd);

	calldata_init(&cd);
	proc_handler_call(ph, "iso_recorder_active_count", &cd);
	fprintf(stderr, "RECORDING_STARTED active=%lld at unix %.3f\n", calldata_int(&cd, "count"),
		(double)os_gettime_ns() / 1e9);
	calldata_free(&cd);
	fflush(stderr);

	os_sleep_ms((uint32_t)seconds * 1000);

	calldata_init(&cd);
	proc_handler_call(ph, "iso_recorder_stop_all", &cd);
	calldata_free(&cd);

	long long active = 1;
	for (int i = 0; i < 100 && active; i++) {
		os_sleep_ms(100);
		calldata_init(&cd);
		proc_handler_call(ph, "iso_recorder_active_count", &cd);
		active = calldata_int(&cd, "count");
		calldata_free(&cd);
	}
	obs_wait_for_destroy_queue();
	fprintf(stderr, "RECORDING_STOPPED active=%lld\n", active);

	obs_set_output_source(0, NULL);
	obs_source_remove(cam1);
	obs_source_remove(cam2);
	obs_source_remove(tone);
	obs_source_release(cam1);
	obs_source_release(cam2);
	obs_source_release(tone);
	obs_scene_release(scene);
	obs_wait_for_destroy_queue();
	obs_shutdown();
	XCloseDisplay(dpy);
	fprintf(stderr, "DONE leaks=%ld\n", bnum_allocs());
	return active ? 1 : 0;
}
