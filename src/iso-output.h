/*
 * ISO Recorder for OBS
 * Copyright (C) 2026 Top Set Media, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <obs-module.h>

#define ISO_OUTPUT_ID "iso_mov_output"
#define ISO_FILTER_ID "iso_record_filter"

extern struct obs_output_info iso_mov_output_info;
extern struct obs_source_info iso_record_filter_info;

/* Starts or stops every ISO Record filter in the current scene collection.
 * `only_follow_main` limits the action to filters set to follow OBS's main
 * recording. */
void iso_filters_start_all(bool only_follow_main);
void iso_filters_stop_all(bool only_follow_main);
size_t iso_filters_count_active(void);

void iso_filters_init(void);
void iso_filters_free(void);
