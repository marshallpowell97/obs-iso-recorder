# ISO Recorder for OBS

Records each camera source in OBS as its own **growing MOV with time-of-day timecode**, so DaVinci Resolve (including Resolve Replay) can edit the ISOs while they are still recording, with every angle already in sync.

Built for a Windows laptop running OBS with a DeckLink, recording to a Blackmagic Cloud Store, with Resolve on a Mac reading the files over the network.

## What it does

- **ISO Record filter.** Add it to any source (e.g. a DeckLink input). While recording, that source is rendered at its native resolution into its own OBS view, encoded with its own encoder (NVENC HEVC by default) and written to its own file.
- **Growing files.** Files are written as fragmented QuickTime (OBS's Hybrid MOV writer), so Resolve picks up new footage every fragment (one per keyframe interval, 1 s by default). When recording stops, the file is finalised as a normal MOV.
- **Time-of-day timecode.** A QuickTime timecode (`tmcd`) track is written into the file header and first fragment, so the timecode is readable **while the file grows**. At 59.94/29.97 it uses drop-frame.
- **Shared clock.** All ISOs recording at the same time share one anchor, so the same frame gets the same timecode in every file, even if cameras start at different moments.
- **One control.** Tools → *ISO Recorder: Start/Stop all ISOs*, a hotkey pair for the same, and (per filter, on by default) start/stop together with OBS's own Record button.

## Using it

1. Install the plugin (see *Building*), restart OBS.
2. On each camera source: **Filters → + → ISO Record**.
3. Set the recording folder (blank = OBS's recording path; use the Cloud Store share), encoder, bitrate and audio mix track.
4. Press **Record** in OBS (or Tools → *Start all ISOs*).
5. In Resolve, import the growing files from the share. They grow in the viewer/timeline and carry synced time-of-day timecode for Replay and multicam.

Files are named `<Source name> <date> <time>.mov` and never overwrite an existing file (Resolve caches media by name).

### Recommended settings

| Setting | Value | Why |
|---|---|---|
| Encoder | NVIDIA NVENC HEVC | hardware encode, low CPU |
| Bitrate | 50–80 Mbps | replay/slow-mo quality |
| Keyframe interval | 1 s | = fragment length = how often Resolve sees new footage |
| Min fragment length | 0 (or 1000 ms for ProRes) | ProRes is all-keyframe; 1000 ms keeps fragments ~1 s |
| Drop-frame | on | standard for 59.94 |

**Watch OBS's "frames missed due to rendering lag".** If OBS drops frames, the ISO's timeline gets shorter than real time and later frames' timecode drifts from the other angles. It must stay at (or very near) zero on the recording laptop.

## How the timecode is computed

- The first video packet an output receives is a keyframe, and OBS sets its `sys_dts_usec` to the capture time of that frame (`os_gettime_ns` clock). That marks media time zero.
- The first ISO of a session converts that time to local time of day and stores it as the start frame.
- Every later ISO counts whole OBS frame intervals from that anchor (exact, since all OBS mixes render on the same frame clock).
- The anchor resets when no ISO is recording, so each session re-syncs to the wall clock.

The `tmcd` layout (60-frame counting for 59.94 DF, `tref` from the video track, track flagged "in movie") matches what FFmpeg writes, which was verified to be read correctly by DaVinci Resolve 21.1 while the file is growing.

## Code layout

| Path | What |
|---|---|
| `src/plugin-main.c` | module load, Tools menu, hotkeys, follow-Record events, global procs |
| `src/iso-filter.c` | the ISO Record filter: view, encoders, output lifecycle |
| `src/iso-output.c` | `iso_mov_output`: trimmed copy of OBS's Hybrid MOV output + timecode |
| `src/timecode.c` | drop-frame maths, wall-clock mapping, shared session clock |
| `src/mux/` | OBS 32.2.2's MP4/MOV muxer (GPL-2.0+) with the timecode track and fragment-interval additions (search for "ISO Recorder") |
| `tests/timecode-test.c` | unit tests (label maths over a full day, shared clock) |
| `tests/headless-record.c` | Linux end-to-end test: headless libobs, two test cameras |
| `tests/inspect_mov.py` | prints layout, tracks and start timecode of a growing or finished file |

## Building

Uses the official OBS plugin template (targets OBS 31.1.1; also loads in OBS 32).

- **Windows / macOS:** push to GitHub; the included GitHub Actions workflows build and package for Windows x64 and macOS. Or locally: `cmake --preset windows-x64` then `cmake --build --preset windows-x64` (Visual Studio 2022), `cmake --preset macos` (Xcode).
- **Linux (tests):** `cmake --preset ubuntu-x86_64 -DENABLE_TESTS=ON && cmake --build build_x86_64 && ./build_x86_64/timecode-test`

The headless test needs `libobs-dev`, `obs-studio`, Xvfb and FFmpeg:

```sh
Xvfb :99 &
gcc -o build_x86_64/headless-record tests/headless-record.c -I/usr/include/obs -lobs -lX11
DISPLAY=:99 ./build_x86_64/headless-record build_x86_64/obs-iso-recorder.so data /tmp/iso 20 640 360
python3 tests/inspect_mov.py /tmp/iso/*.mov
```

## Status (v0.1.0)

Tested on Linux with headless OBS 30.2 (x264, two test-pattern cameras, 59.94 DF):

- growing files carry the timecode track from the first fragment; ffprobe reads it mid-recording
- one fragment per second with a 1 s keyframe interval; clean finalisation; no leaks
- ISOs started one frame apart get timecodes exactly one frame apart
- DaVinci Resolve 21.1 (macOS): the plugin's growing bytes, replayed in real time, grew live with the correct start timecode; the finished file imported with correct timecode, drop-frame flag, frame count and audio

Not yet tested: Windows build, NVENC, DeckLink sources, Cloud Store over SMB, multi-hour soak.

## License

GPL-2.0-or-later. `src/mux/` is derived from OBS Studio (Copyright (C) 2024 Dennis Sädtler).
