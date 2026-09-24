/*
 * ISO Recorder for OBS
 * Copyright (C) 2026 Top Set Media, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include <obs-module.h>
#include <obs-frontend-api.h>

#include "iso-output.h"
#include "timecode.h"
#include "plugin-support.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Records each camera source as a growing MOV with time-of-day timecode (for DaVinci Resolve Replay).";
}

static obs_hotkey_pair_id hotkey_pair = OBS_INVALID_HOTKEY_PAIR_ID;

/* ------------------------------------------------------------------------ */
/* Global procedures (usable from scripts / tests via obs_get_proc_handler) */

static void proc_start_all(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(cd);
	iso_filters_start_all(false);
}

static void proc_stop_all(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(cd);
	iso_filters_stop_all(false);
}

static void proc_active_count(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	calldata_set_int(cd, "count", (long long)iso_filters_count_active());
}

/* ------------------------------------------------------------------------ */
/* Frontend integration                                                     */

static void menu_start_all(void *data)
{
	UNUSED_PARAMETER(data);
	iso_filters_start_all(false);
}

static void menu_stop_all(void *data)
{
	UNUSED_PARAMETER(data);
	iso_filters_stop_all(false);
}

static bool hotkey_start(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed || iso_filters_count_active())
		return false;
	iso_filters_start_all(false);
	return true;
}

static bool hotkey_stop(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed || !iso_filters_count_active())
		return false;
	iso_filters_stop_all(false);
	return true;
}

static void save_callback(obs_data_t *save_data, bool saving, void *data)
{
	UNUSED_PARAMETER(data);

	if (saving) {
		obs_data_array_t *start = NULL;
		obs_data_array_t *stop = NULL;
		obs_hotkey_pair_save(hotkey_pair, &start, &stop);
		obs_data_set_array(save_data, "iso_recorder_start_all", start);
		obs_data_set_array(save_data, "iso_recorder_stop_all", stop);
		obs_data_array_release(start);
		obs_data_array_release(stop);
	} else {
		obs_data_array_t *start = obs_data_get_array(save_data, "iso_recorder_start_all");
		obs_data_array_t *stop = obs_data_get_array(save_data, "iso_recorder_stop_all");
		obs_hotkey_pair_load(hotkey_pair, start, stop);
		obs_data_array_release(start);
		obs_data_array_release(stop);
	}
}

static void frontend_event(enum obs_frontend_event event, void *data)
{
	UNUSED_PARAMETER(data);

	switch (event) {
	case OBS_FRONTEND_EVENT_RECORDING_STARTING:
		iso_filters_start_all(true);
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPING:
		iso_filters_stop_all(true);
		break;
	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
	case OBS_FRONTEND_EVENT_EXIT:
		iso_filters_stop_all(false);
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------------ */

bool obs_module_load(void)
{
	iso_clock_init();
	iso_filters_init();

	obs_register_output(&iso_mov_output_info);
	obs_register_source(&iso_record_filter_info);

	proc_handler_t *ph = obs_get_proc_handler();
	proc_handler_add(ph, "void iso_recorder_start_all()", proc_start_all, NULL);
	proc_handler_add(ph, "void iso_recorder_stop_all()", proc_stop_all, NULL);
	proc_handler_add(ph, "void iso_recorder_active_count(out int count)", proc_active_count, NULL);

	obs_frontend_add_tools_menu_item(obs_module_text("Menu.StartAll"), menu_start_all, NULL);
	obs_frontend_add_tools_menu_item(obs_module_text("Menu.StopAll"), menu_stop_all, NULL);
	obs_frontend_add_event_callback(frontend_event, NULL);
	obs_frontend_add_save_callback(save_callback, NULL);

	hotkey_pair = obs_hotkey_pair_register_frontend("iso_recorder.start_all", obs_module_text("Hotkey.StartAll"),
							"iso_recorder.stop_all", obs_module_text("Hotkey.StopAll"),
							hotkey_start, hotkey_stop, NULL, NULL);

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(frontend_event, NULL);
	obs_frontend_remove_save_callback(save_callback, NULL);
	if (hotkey_pair != OBS_INVALID_HOTKEY_PAIR_ID)
		obs_hotkey_pair_unregister(hotkey_pair);

	iso_filters_free();
	iso_clock_free();
	obs_log(LOG_INFO, "plugin unloaded");
}
