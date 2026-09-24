/*
 * Headless end-to-end test (macOS), run against the installed OBS.app.
 * Built and run by tests/mac/e2e.sh, checked by tests/mac/check_e2e.py.
 *
 * CAM 2 is 1366 wide: OBS garbles frames whose width is not a multiple of 4,
 * so the plugin pads its view.
 *
 * Phase 1: CAM 1 / "ISO A" and CAM 2 / "ISO C" start together, then
 *          CAM 1 / "ISO B" starts ~2.5 s later on the same source. The
 *          growing files are copied to <out>/snap mid-recording.
 * Phase 2: start all, then stop all and immediately start all again (like
 *          pressing Stop then Record in OBS).
 * Phase 3: "ISO C" is removed from CAM 2 while recording.
 *
 * usage: headless-record-mac <plugin binary> <plugin data dir> <out dir> <encoder id>
 */

#include <obs.h>
#include <util/platform.h>
#include <util/dstr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP "/Applications/OBS.app/Contents/"

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

static bool load_app_plugin(const char *name)
{
	char bin[512], data[512];
	snprintf(bin, sizeof(bin), APP "PlugIns/%s.plugin/Contents/MacOS/%s", name, name);
	snprintf(data, sizeof(data), APP "PlugIns/%s.plugin/Contents/Resources", name);
	return load(bin, data);
}

static const char *out_dir;
static const char *encoder;

static obs_source_t *add_iso(obs_source_t *src, const char *filter_name)
{
	obs_data_t *fs = obs_data_create();
	obs_data_set_string(fs, "folder", out_dir);
	obs_data_set_string(fs, "encoder", encoder);
	obs_data_set_int(fs, "bitrate", 8000);
	obs_data_set_int(fs, "keyint_sec", 1);
	obs_data_set_int(fs, "audio_track", 1);
	obs_source_t *filter = obs_source_create("iso_record_filter", filter_name, fs, NULL);
	obs_data_release(fs);
	obs_source_filter_add(src, filter);
	return filter;
}

static obs_source_t *make_lavfi(obs_scene_t *scene, const char *name, const char *lavfi)
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
	return src;
}

static void click(obs_source_t *filter, const char *button)
{
	obs_properties_t *props = obs_source_properties(filter);
	obs_property_button_clicked(obs_properties_get(props, button), filter);
	obs_properties_destroy(props);
}

static long long active_count(void)
{
	calldata_t cd = {0};
	proc_handler_call(obs_get_proc_handler(), "iso_recorder_active_count", &cd);
	long long n = calldata_int(&cd, "count");
	calldata_free(&cd);
	return n;
}

static void call(const char *proc)
{
	calldata_t cd = {0};
	proc_handler_call(obs_get_proc_handler(), proc, &cd);
	calldata_free(&cd);
}

/* Waits until every file has been finalised and its session released. */
static void wait_finalised(void)
{
	for (int i = 0; i < 30; i++)
		os_sleep_ms(100);
	obs_wait_for_destroy_queue();
}

static void snapshot_growing_files(void)
{
	struct dstr dir = {0}, src = {0}, dst = {0};
	dstr_printf(&dir, "%s/snap", out_dir);
	os_mkdir(dir.array);

	os_dir_t *d = os_opendir(out_dir);
	struct os_dirent *ent;
	while (d && (ent = os_readdir(d)) != NULL) {
		if (ent->directory || !strstr(ent->d_name, ".mov"))
			continue;
		dstr_printf(&src, "%s/%s", out_dir, ent->d_name);
		dstr_printf(&dst, "%s/%s", dir.array, ent->d_name);
		os_copyfile(src.array, dst.array);
	}
	os_closedir(d);
	dstr_free(&dir);
	dstr_free(&src);
	dstr_free(&dst);
}

int main(int argc, char **argv)
{
	if (argc < 5) {
		fprintf(stderr, "usage: %s plugin datadir outdir encoder\n", argv[0]);
		return 2;
	}
	const char *plugin = argv[1], *plugin_data = argv[2];
	out_dir = argv[3];
	encoder = argv[4];

	base_set_log_handler(do_log, NULL);
	if (!obs_startup("en-US", NULL, NULL))
		return 1;

	struct obs_video_info ovi = {0};
	ovi.graphics_module = APP "Frameworks/libobs-opengl.dylib";
	ovi.fps_num = 60000;
	ovi.fps_den = 1001;
	ovi.base_width = ovi.output_width = 1280;
	ovi.base_height = ovi.output_height = 720;
	ovi.output_format = VIDEO_FORMAT_NV12;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
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

	if (!load_app_plugin("obs-ffmpeg") || !load_app_plugin("mac-videotoolbox") ||
	    !load_app_plugin("coreaudio-encoder") || !load_app_plugin("obs-x264") || !load(plugin, plugin_data))
		return 1;
	obs_post_load_modules();

	obs_scene_t *scene = obs_scene_create("Scene");
	obs_source_t *cam1 = make_lavfi(scene, "CAM 1", "testsrc2=size=1280x720:rate=60000/1001");
	obs_source_t *cam2 = make_lavfi(scene, "CAM 2", "testsrc=size=1366x768:rate=60000/1001");
	obs_source_t *tone = make_lavfi(scene, "Tone", "sine=frequency=1000:sample_rate=48000");
	obs_set_output_source(0, obs_scene_get_source(scene));

	for (int i = 0; i < 50 && (!obs_source_get_width(cam1) || !obs_source_get_width(cam2)); i++)
		os_sleep_ms(100);
	fprintf(stderr, "sources ready: %ux%u and %ux%u\n", obs_source_get_width(cam1), obs_source_get_height(cam1),
		obs_source_get_width(cam2), obs_source_get_height(cam2));

	obs_source_t *iso_a = add_iso(cam1, "ISO A");
	obs_source_t *iso_b = add_iso(cam1, "ISO B");
	obs_source_t *iso_c = add_iso(cam2, "ISO C");

	/* Phase 1 */
	fprintf(stderr, "PHASE 1\n");
	click(iso_a, "start");
	click(iso_c, "start");
	os_sleep_ms(2500);
	click(iso_b, "start");
	os_sleep_ms(3500);
	snapshot_growing_files();
	fprintf(stderr, "SNAPSHOT taken\n");
	os_sleep_ms(3000);
	call("iso_recorder_stop_all");
	wait_finalised();
	fprintf(stderr, "PHASE 1 done, active=%lld\n", active_count());

	/* Phase 2 */
	fprintf(stderr, "PHASE 2\n");
	call("iso_recorder_start_all");
	fprintf(stderr, "PHASE 2 first start, active=%lld\n", active_count());
	os_sleep_ms(3000);
	call("iso_recorder_stop_all");
	call("iso_recorder_start_all");
	fprintf(stderr, "PHASE 2 restart, active=%lld\n", active_count());
	os_sleep_ms(3000);
	call("iso_recorder_stop_all");
	wait_finalised();
	fprintf(stderr, "PHASE 2 done, active=%lld\n", active_count());

	/* Phase 3 */
	fprintf(stderr, "PHASE 3\n");
	click(iso_c, "start");
	os_sleep_ms(2000);
	obs_source_filter_remove(cam2, iso_c);
	obs_source_release(iso_c);
	iso_c = NULL;
	wait_finalised();
	fprintf(stderr, "PHASE 3 done, active=%lld\n", active_count());

	obs_source_release(iso_a);
	obs_source_release(iso_b);
	obs_set_output_source(0, NULL);
	/* Scenes belong to the main canvas in OBS 32, which keeps a reference
	 * until the scene is removed. */
	obs_source_remove(obs_scene_get_source(scene));
	obs_source_release(cam1);
	obs_source_release(cam2);
	obs_source_release(tone);
	obs_scene_release(scene);
	obs_wait_for_destroy_queue();
	fprintf(stderr, "TEARDOWN\n");
	fflush(stderr);
	obs_shutdown();
	fprintf(stderr, "DONE leaks=%ld\n", bnum_allocs());
	return 0;
}
