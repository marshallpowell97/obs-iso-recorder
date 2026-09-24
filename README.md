# ISO Recorder for OBS

Records each camera in OBS to its own growing MOV with time-of-day timecode. Resolve (including Replay) can edit the ISOs while they record, with every angle already in sync.

Built for: Windows laptop, OBS, DeckLink, recording to a Blackmagic Cloud Store. Resolve on a Mac reads the files over the network.

## What it does

- **ISO Record filter.** Add to any source. Records that source at native resolution, with its own encoder, to its own file.
- **Growing files.** New footage shows up in Resolve every keyframe interval (1 s default). File becomes a normal MOV when recording stops.
- **Time-of-day timecode.** Readable while the file grows. Drop-frame at 29.97 and 59.94.
- **Shared clock.** Same frame, same timecode, in every file. Cameras can start at different times.
- **One control.** OBS Record button, Tools menu, or hotkeys. Stop returns right away. Files finish writing in the background, so the next take can start immediately.

## Use it

1. Install the plugin (see Build). Restart OBS.
2. On each camera source: Filters > + > ISO Record.
3. Set recording folder to the Cloud Store share. Blank uses OBS's recording path.
4. Set encoder, bitrate, audio mix track.
5. Press Record in OBS. Or Tools > ISO Recorder: Start all ISOs.
6. In Resolve, import the growing files from the share.

## Settings

| Setting | Value |
|---|---|
| Encoder | NVIDIA NVENC HEVC |
| Bitrate | 50 to 80 Mbps |
| Keyframe interval | 1 s (how often Resolve sees new footage) |
| Min fragment length | 0. ProRes: 1000 ms |
| Drop-frame | On |

## Rules

- **Keep "frames missed due to rendering lag" at 0.** Dropped frames shorten the ISO and push its timecode off the other angles.
- Files are named `<Source> <date> <time>.mov`. Never overwritten.
- ISOs record without B-frames. With B-frames, growing files sit 2 frames off their timecode.
- Widths not divisible by 4 (for example 1366) get up to 3 px of black on the right. OBS scrambles those widths otherwise. 720p, 1080p and 4K are unaffected.

## How timecode works

- First video frame of a file: OBS capture time, converted to time of day.
- First ISO of a session sets the anchor. Later ISOs count OBS frames from it, so every file agrees to the frame.
- Anchor resets when nothing is recording. Each session re-syncs to the clock.
- The timecode track layout matches what FFmpeg writes.

## Build

Based on the official OBS plugin template. Built against OBS 31.1.1. Loads in OBS 31 and newer.

**Windows and macOS release builds.** Push to GitHub. `.github/workflows/build.yaml` builds Windows x64 and a macOS `.pkg`. Download from the workflow run's artifacts.

**Mac quick build (no Xcode).** Quit OBS, then:

```sh
tests/mac/build-local.sh --install
```

Builds against the OBS in `/Applications` and installs to `~/Library/Application Support/obs-studio/plugins`. Apple Silicon only, that OBS version only. First run downloads OBS headers into `build_local/deps`.

**Mac end-to-end test.** Needs FFmpeg.

```sh
tests/mac/e2e.sh
```

Records test cameras headless and checks:

- timecode in growing and finished files
- frame-accurate sync between ISOs started at different times
- Stop then immediate Record
- removing a filter mid-recording
- odd widths
- clean shutdown with no leaks

**Linux.** `tests/headless-record.c` and `tests/timecode-test.c`. See the comments at the top of each file.

## Code

| Path | What |
|---|---|
| `src/plugin-main.c` | Plugin load, Tools menu, hotkeys, Record button events |
| `src/iso-filter.c` | ISO Record filter: view, encoders, start and stop |
| `src/iso-output.c` | MOV output with timecode (trimmed copy of OBS's Hybrid MOV output) |
| `src/timecode.c` | Drop-frame math, wall clock, shared session clock |
| `src/mux/` | OBS's MP4/MOV muxer plus the timecode track. Changes marked "ISO Recorder" |
| `tests/` | Unit tests, end-to-end tests, `inspect_mov.py` for checking files |

## Status (v0.1.0)

Tested:

- Mac, OBS 32.2.1 and 32.2.2, VideoToolbox HEVC. End-to-end test passes. Real OBS session recorded 1080p60 with correct timecode.
- Linux, headless OBS 30.2, x264.
- Resolve 21.1 on Mac read growing and finished files with correct timecode (earlier testing, before B-frames were turned off).

Not tested yet:

- Windows build
- NVENC
- DeckLink
- Cloud Store over the network
- Multi-hour recordings

## License

GPL-2.0-or-later. `src/mux/` is from OBS Studio (Copyright (C) 2024 Dennis Sädtler).
