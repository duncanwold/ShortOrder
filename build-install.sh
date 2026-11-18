#!/bin/bash
# Build ShortOrder (AU, VST3, Standalone, AAX when the AAX SDK is at ~/AAX_SDK, and the offline tests), install the AU and the VST3
# for this user, run auval. (The AAX is built, not installed: Pro Tools loads only a PACE-signed one; ./release.sh signs it.)
# (Up to v0.6 it was "GTT Lab", aufx Gtlb Ypkt; the old component is removed on install, so Logic doesn't list both.)
#   ./build-install.sh           build, sign (ad-hoc), install the AU and the VST3, auval
#   ./build-install.sh --devid   sign with the Developer ID certificate instead (DEVELOPER_ID_APP from signing.env; --timestamp
#                                --options runtime). The release (notarized, packaged, AAX wrapped) is ./release.sh.
set -e
cd "$(dirname "$0")"

TARGET="ShortOrder"
AU_NAME="ShortOrder.component"
APP_NAME="ShortOrder.app"
AU_INSTALL_DIR="$HOME/Library/Audio/Plug-Ins/Components"
VST3_NAME="ShortOrder.vst3"
VST3_INSTALL_DIR="$HOME/Library/Audio/Plug-Ins/VST3"
AU_TYPE="aufx"
AU_SUBTYPE="ShOr"
AU_MANUFACTURER="Ypkt"
OLD_AU_NAME="GTT Lab.component" # v0.1 ... v0.6
SIGN_ID="-"
for arg in "$@"; do
    case "$arg" in
        --devid)
            [ -f signing.env ] || { echo "--devid: signing.env is missing (copy signing.env.example)"; exit 1; }
            SIGN_ID="$(source signing.env && echo "$DEVELOPER_ID_APP")"
            [ -n "$SIGN_ID" ] || { echo "--devid: DEVELOPER_ID_APP is empty in signing.env"; exit 1; }
            ;;
        *) echo "Unknown option: $arg"; exit 1 ;;
    esac
done

# --- Configure (every time: quick, and an older build directory learns new targets, e.g. v0.6.1's rename) ---
if [ ! -f build/CMakeCache.txt ]; then
    cmake -B build -DCMAKE_BUILD_TYPE=Release
else
    cmake -B build > /dev/null
fi

# --- Build ---
echo "Building..."
TARGETS=("${TARGET}_AU" "${TARGET}_VST3" "${TARGET}_Standalone" GTTTests)
if cmake --build build --target help 2>/dev/null | grep -q "${TARGET}_AAX"; then # configured with the AAX SDK
    TARGETS+=("${TARGET}_AAX")
fi
cmake --build build --config Release --target "${TARGETS[@]}" -j 8

# --- Locate built AU (Makefile vs Xcode generator layouts) ---
if [ -d "build/${TARGET}_artefacts/Release/AU/$AU_NAME" ]; then
    BASE="build/${TARGET}_artefacts/Release"
elif [ -d "build/${TARGET}_artefacts/AU/$AU_NAME" ]; then
    BASE="build/${TARGET}_artefacts"
else
    echo "Error: could not find built AU."
    exit 1
fi

# --- Stage for clean signing (strip Finder/iCloud extended attributes) ---
STAGING="build/stage"
rm -rf "$STAGING"
mkdir -p "$STAGING"
cp -R "$BASE/AU/$AU_NAME" "$STAGING/"
cp -R "$BASE/VST3/$VST3_NAME" "$STAGING/"
xattr -cr "$STAGING/$AU_NAME" "$STAGING/$VST3_NAME"
find "$STAGING" -name '._*' -delete 2>/dev/null || true
find "$STAGING" -name '.DS_Store' -delete 2>/dev/null || true

# --- AU tags (Apple's standard ones; JUCE writes no tags for an AU v2), before signing ---
PLIST="$STAGING/$AU_NAME/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Delete :AudioComponents:0:tags" "$PLIST" 2>/dev/null || true
/usr/libexec/PlistBuddy -c "Add :AudioComponents:0:tags array" \
                        -c "Add :AudioComponents:0:tags:0 string Effects" \
                        -c "Add :AudioComponents:0:tags:1 string Guitar" \
                        -c "Add :AudioComponents:0:tags:2 string Synthesizer" \
                        -c "Add :AudioComponents:0:tags:3 string Pitch" "$PLIST"

for BUNDLE in "$AU_NAME" "$VST3_NAME"; do
    echo "Signing $BUNDLE ($SIGN_ID)..."
    if [[ "$SIGN_ID" == "-" ]]; then
        codesign --force --deep --sign - "$STAGING/$BUNDLE"
    else
        codesign --force --deep --timestamp --options runtime --sign "$SIGN_ID" "$STAGING/$BUNDLE"
    fi
    codesign --verify --deep --strict "$STAGING/$BUNDLE"
done

# --- Install ---
mkdir -p "$AU_INSTALL_DIR"
if [ -d "$AU_INSTALL_DIR/$OLD_AU_NAME" ]; then
    rm -rf "$AU_INSTALL_DIR/$OLD_AU_NAME"
    echo "✓ Removed the old $AU_INSTALL_DIR/$OLD_AU_NAME (GTT Lab is now ShortOrder)"
fi
rm -rf "$AU_INSTALL_DIR/$AU_NAME"
cp -R "$STAGING/$AU_NAME" "$AU_INSTALL_DIR/"
echo "✓ Installed AU to $AU_INSTALL_DIR/$AU_NAME"
mkdir -p "$VST3_INSTALL_DIR"
rm -rf "$VST3_INSTALL_DIR/$VST3_NAME"
cp -R "$STAGING/$VST3_NAME" "$VST3_INSTALL_DIR/"
echo "✓ Installed VST3 to $VST3_INSTALL_DIR/$VST3_NAME"

# Make the AU registrar notice the new build.
killall -9 AudioComponentRegistrar 2>/dev/null || true

# --- Validate ---
echo "Running auval..."
if auval -v "$AU_TYPE" "$AU_SUBTYPE" "$AU_MANUFACTURER" > build/auval.log 2>&1; then
    tail -n 3 build/auval.log
    echo "✓ auval passed (full log: build/auval.log)"
else
    tail -n 40 build/auval.log
    echo "✗ auval FAILED (full log: build/auval.log)"
    exit 1
fi

echo "✓ Done. ShortOrder ($AU_TYPE $AU_SUBTYPE $AU_MANUFACTURER). Standalone: $BASE/Standalone/$APP_NAME$([ -d "$BASE/AAX/ShortOrder.aaxplugin" ] && echo "   AAX (unsigned): $BASE/AAX/ShortOrder.aaxplugin")   Tests: ./run-tests.sh"
