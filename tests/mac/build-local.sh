#!/bin/zsh
# Local macOS build of the plugin against the installed OBS.app, without Xcode
# or the template's dependency download. Apple Silicon only, and only for the
# OBS version installed on this machine. Release builds come from CI.
#
# usage: tests/mac/build-local.sh [--install]
#   Builds build_local/obs-iso-recorder.plugin and the headless test harness.
#   --install copies the plugin into ~/Library/Application Support/obs-studio/plugins.
#
# First run fetches OBS headers (sparse, shallow clone of obs-studio at the
# installed version's tag) and simde into build_local/deps.

set -euo pipefail

ROOT=${0:A:h:h:h}
OUT=$ROOT/build_local
DEPS=$OUT/deps
APP=/Applications/OBS.app/Contents
FW=$APP/Frameworks
NAME=obs-iso-recorder
VERSION=$(sed -n 's/.*"version": "\(.*\)",/\1/p' $ROOT/buildspec.json | tail -1)

[[ -d $APP ]] || { echo "OBS.app not found in /Applications"; exit 1 }
OBS_VERSION=$(defaults read $APP/Info.plist CFBundleShortVersionString)
mkdir -p $DEPS

# Headers ------------------------------------------------------------------
if [[ ! -f $DEPS/obs-studio-$OBS_VERSION/libobs/obs.h ]]; then
  echo "Fetching OBS $OBS_VERSION headers..."
  rm -rf $DEPS/obs-studio-$OBS_VERSION
  git clone -q --depth 1 --branch $OBS_VERSION --filter=blob:none --sparse \
    https://github.com/obsproject/obs-studio.git $DEPS/obs-studio-$OBS_VERSION
  git -C $DEPS/obs-studio-$OBS_VERSION sparse-checkout set libobs frontend/api
fi
if [[ ! -f $DEPS/simde/simde/x86/sse2.h ]]; then
  echo "Fetching simde headers..."
  rm -rf $DEPS/simde
  git clone -q --depth 1 https://github.com/simd-everywhere/simde.git $DEPS/simde
fi

OBS_SRC=$DEPS/obs-studio-$OBS_VERSION
GEN=$OUT/gen
mkdir -p $GEN
printf '#pragma once\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n' > $GEN/obsconfig.h
sed -e "s/@CMAKE_PROJECT_NAME@/$NAME/" -e "s/@CMAKE_PROJECT_VERSION@/$VERSION/" \
  $ROOT/src/plugin-support.c.in > $GEN/plugin-support.c

INCLUDES=(-I$GEN -I$OBS_SRC/libobs -I$OBS_SRC/frontend/api -I$DEPS/simde)
# Same warnings the template enables; -Werror on the ones CI treats as errors.
WARN=(-Wall -Wextra -Wno-missing-field-initializers -Wno-missing-braces -Wno-unused-function
  -Wno-sign-conversion -Wno-conversion -Wno-unknown-pragmas -Wno-unused-label -Wunreachable-code
  -Wcomma -Wvla -Wnewline-eof -Wformat-security -Wshorten-64-to-32
  -Werror -Wno-error=shorten-64-to-32 -Wdeprecated-declarations)

# Plugin bundle ------------------------------------------------------------
BUNDLE=$OUT/$NAME.plugin
rm -rf $BUNDLE
mkdir -p $BUNDLE/Contents/MacOS $BUNDLE/Contents/Resources
echo "Building $NAME $VERSION against OBS $OBS_VERSION..."
clang -std=gnu17 -arch arm64 -O2 -g -bundle $WARN $INCLUDES -I$ROOT/src -I$ROOT/src/mux \
  $ROOT/src/plugin-main.c $ROOT/src/iso-filter.c $ROOT/src/iso-output.c $ROOT/src/timecode.c \
  $ROOT/src/mux/mp4-mux.c $ROOT/src/mux/rtmp-hevc.c $ROOT/src/mux/rtmp-av1.c $GEN/plugin-support.c \
  -F$FW -framework libobs $FW/libobs-frontend-api.1.dylib \
  -o $BUNDLE/Contents/MacOS/$NAME
rm -rf $OUT/$NAME.plugin.dSYM
[[ -d $BUNDLE/Contents/MacOS/$NAME.dSYM ]] && mv $BUNDLE/Contents/MacOS/$NAME.dSYM $OUT/$NAME.plugin.dSYM
cp -R $ROOT/data/. $BUNDLE/Contents/Resources/
cat > $BUNDLE/Contents/Info.plist <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key><string>$NAME</string>
	<key>CFBundleIdentifier</key><string>com.topsetmedia.$NAME</string>
	<key>CFBundleName</key><string>$NAME</string>
	<key>CFBundlePackageType</key><string>BNDL</string>
	<key>CFBundleShortVersionString</key><string>$VERSION</string>
	<key>CFBundleVersion</key><string>$VERSION</string>
	<key>LSMinimumSystemVersion</key><string>12.0</string>
</dict>
</plist>
EOF
codesign --force --sign - $BUNDLE
echo "Built $BUNDLE"

# Test harness -------------------------------------------------------------
clang -std=gnu17 -arch arm64 -g $INCLUDES $ROOT/tests/mac/headless-record-mac.c \
  -F$FW -framework libobs -Wl,-rpath,$FW -o $OUT/headless-record-mac
clang -std=gnu17 -arch arm64 -g -Wall -Wextra $INCLUDES -I$ROOT/src \
  $ROOT/tests/timecode-test.c $ROOT/src/timecode.c \
  -F$FW -framework libobs -Wl,-rpath,$FW -o $OUT/timecode-test

if [[ ${1:-} == --install ]]; then
  DEST="$HOME/Library/Application Support/obs-studio/plugins"
  mkdir -p $DEST
  rm -rf "$DEST/$NAME.plugin"
  cp -R $BUNDLE "$DEST/"
  echo "Installed to $DEST/$NAME.plugin (restart OBS)"
fi
