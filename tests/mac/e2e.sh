#!/bin/zsh
# Headless end-to-end test on macOS. Needs OBS.app in /Applications and FFmpeg
# (ffprobe/ffmpeg) on PATH. Runs tests/mac/build-local.sh first.
#
# usage: tests/mac/e2e.sh [encoder id]   (default: VideoToolbox HEVC)

set -uo pipefail

ROOT=${0:A:h:h:h}
OUT=$ROOT/build_local/e2e
ENC=${1:-com.apple.videotoolbox.videoencoder.ave.hevc}

$ROOT/tests/mac/build-local.sh > /dev/null || exit 1
$ROOT/build_local/timecode-test | tail -1 || exit 1

rm -rf $OUT && mkdir -p $OUT
echo "Recording (about 25 s)..."
$ROOT/build_local/headless-record-mac $ROOT/build_local/obs-iso-recorder.plugin/Contents/MacOS/obs-iso-recorder \
  $ROOT/data $OUT $ENC > $OUT/log.txt 2>&1
echo "harness exit code $?"
python3 $ROOT/tests/mac/check_e2e.py $OUT
