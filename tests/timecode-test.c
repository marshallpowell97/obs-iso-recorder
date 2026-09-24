/* Unit tests for src/timecode.c (build with -DENABLE_TESTS=ON). */

#include "timecode.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, ...)                                  \
	do {                                              \
		if (!(cond)) {                            \
			failures++;                       \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__);     \
			fprintf(stderr, "\n");            \
		}                                         \
	} while (0)

static void expect_label(uint32_t num, uint32_t den, bool df, uint32_t frames, const char *expected)
{
	struct iso_tc_format fmt;
	iso_tc_format_init(&fmt, num, den, df);
	char buf[16];
	iso_tc_to_string(&fmt, frames, buf, sizeof(buf));
	CHECK(strcmp(buf, expected) == 0, "%u/%u frame %u -> %s, expected %s", num, den, frames, buf, expected);

	uint32_t back = 0;
	CHECK(iso_tc_from_string(&fmt, expected, &back) && back == frames, "parse %s -> %u, expected %u", expected,
	      back, frames);
}

static void round_trip_day(uint32_t num, uint32_t den, bool df)
{
	struct iso_tc_format fmt;
	iso_tc_format_init(&fmt, num, den, df);
	uint32_t per_day = iso_tc_frames_per_day(&fmt);
	char buf[16];
	uint32_t prev_label_ff = 0;
	int bad = 0;

	for (uint32_t n = 0; n < per_day && bad < 5; n++) {
		iso_tc_to_string(&fmt, n, buf, sizeof(buf));
		uint32_t back;
		if (!iso_tc_from_string(&fmt, buf, &back) || back != n) {
			CHECK(0, "%u/%u round trip frame %u -> %s -> %u", num, den, n, buf, back);
			bad++;
		}
		(void)prev_label_ff;
	}
	printf("round trip %u/%u %s: %u frames/day OK\n", num, den, fmt.drop_frame ? "DF" : "NDF", per_day);
}

int main(void)
{
	/* 59.94 drop-frame: 4 frame numbers dropped each minute except every 10th */
	expect_label(60000, 1001, true, 0, "00:00:00;00");
	expect_label(60000, 1001, true, 3599, "00:00:59;59");
	expect_label(60000, 1001, true, 3600, "00:01:00;04");
	expect_label(60000, 1001, true, 35963, "00:09:59;59");
	expect_label(60000, 1001, true, 35964, "00:10:00;00");
	expect_label(60000, 1001, true, 215784, "01:00:00;00");

	/* 29.97 drop-frame */
	expect_label(30000, 1001, true, 1799, "00:00:59;29");
	expect_label(30000, 1001, true, 1800, "00:01:00;02");
	expect_label(30000, 1001, true, 17982, "00:10:00;00");
	expect_label(30000, 1001, true, 107892, "01:00:00;00");

	/* Non-drop */
	expect_label(60, 1, false, 216000, "01:00:00:00");
	expect_label(25, 1, false, 90000 + 24, "01:00:00:24");
	expect_label(60000, 1001, false, 216000, "01:00:00:00");

	/* Invalid drop-frame labels are rejected */
	{
		struct iso_tc_format fmt;
		iso_tc_format_init(&fmt, 60000, 1001, true);
		uint32_t n;
		CHECK(!iso_tc_from_string(&fmt, "00:01:00;02", &n), "00:01:00;02 should be invalid at 59.94 DF");
		CHECK(iso_tc_from_string(&fmt, "00:10:00;02", &n), "00:10:00;02 should be valid");
	}

	round_trip_day(60000, 1001, true);
	round_trip_day(30000, 1001, true);
	round_trip_day(60, 1, false);

	/* Time of day -> frames. Drop-frame labels inherently wander from the
	 * wall clock: up to ~3.6 frames within each minute (59.94) plus
	 * ~5 frames of accumulated error by the end of the day, so ~9 frames
	 * (0.15 s) is the expected worst case for any DF generator. */
	{
		struct iso_tc_format fmt;
		iso_tc_format_init(&fmt, 60000, 1001, true);
		int worst = 0;
		for (uint64_t sec = 0; sec < 86400; sec += 37) {
			uint32_t n = iso_tc_frames_from_ns_of_day(&fmt, sec * 1000000000ULL);
			char buf[16];
			iso_tc_to_string(&fmt, n, buf, sizeof(buf));
			unsigned hh, mm, ss, ff;
			sscanf(buf, "%u:%u:%u;%u", &hh, &mm, &ss, &ff);
			long label_frames = (long)((hh * 3600 + mm * 60 + ss) * 60 + ff);
			long wall_frames = (long)(sec * 60);
			int diff = (int)(label_frames - wall_frames);
			if (diff < 0)
				diff = -diff;
			if (diff > worst)
				worst = diff;
		}
		printf("59.94 DF label vs wall clock: worst %d frame(s) over 24h\n", worst);
		CHECK(worst <= 10, "DF label drifted %d frames from wall clock", worst);
	}

	/* Shared clock: two outputs starting on different frames of the same
	 * OBS frame grid must agree exactly. */
	{
		iso_clock_init();
		struct iso_tc_format fmt;
		iso_tc_format_init(&fmt, 60000, 1001, true);
		uint64_t interval = 16683333; /* OBS's integer frame interval for 59.94 */
		uint64_t t0 = 5000000000000ULL + 1234567;

		iso_clock_acquire();
		uint32_t a = iso_clock_start_frame(&fmt, t0, interval);
		iso_clock_acquire();
		/* 3 hours later, with microsecond truncation noise */
		uint64_t k = 3ULL * 3600 * 60000 / 1001;
		uint32_t b = iso_clock_start_frame(&fmt, t0 + k * interval - 999, interval);
		CHECK(b == (a + (uint32_t)k) % iso_tc_frames_per_day(&fmt), "clock mismatch: a=%u b=%u k=%llu", a, b,
		      (unsigned long long)k);

		iso_clock_release();
		iso_clock_release();
		iso_clock_free();
		printf("shared clock: second output 3h later offset %llu frames OK\n", (unsigned long long)k);
	}

	if (failures) {
		printf("%d failure(s)\n", failures);
		return 1;
	}
	printf("all timecode tests passed\n");
	return 0;
}
