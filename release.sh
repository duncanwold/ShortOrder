#!/bin/bash
# ShortOrder's release: a clean universal build of every format, signed, notarized, stapled and packaged into dist/.
# (Adapted from GuitarVocoder's build-install.sh. ./build-install.sh is the everyday development build.)
#
#   ./release.sh             the signed release. Needs signing.env (copy signing.env.example), the Developer ID Application and
#                            Installer certificates in the keychain, a notarytool keychain profile, and, for AAX, PACE's wraptool
#                            with an iLok (its password is asked for, never stored). Output: dist/
#   ./release.sh --dry-run   the whole flow with no credentials: builds and packages UNSIGNED (ad hoc signatures, no notarization),
#                            into dist/dry-run/. Not for distribution.
#   ./release.sh --no-aax    leave AAX out (e.g. no iLok at hand). Combines with --dry-run.
#   ./release.sh --no-pkg    zips only: no .pkg installer, so no Developer ID Installer certificate is needed. Combines with the others.
#
# dist/ (gitignored): ShortOrder-<version>-macOS.pkg (choosable components: AU, VST3, AAX, Standalone), one zip per format,
# ShortOrder-Install-Guide.md, SHA256SUMS.txt. The pkg shows LICENSE, NOTICE, THIRD-PARTY-NOTICES.md and OFL.txt; every zip has them.
set -euo pipefail
set +x # never trace: the iLok password passes through a variable
cd "$(dirname "$0")"
ROOT="$(pwd)"

DRY=false
AAX_WANTED=true
PKG_WANTED=true
for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY=true ;;
        --no-aax) AAX_WANTED=false ;;
        --no-pkg) PKG_WANTED=false ;;
        *) echo "Unknown option: $arg"; sed -n '4,14p' "$0"; exit 1 ;;
    esac
done

NAME="ShortOrder"
VERSION="$(sed -n 's/^project(ShortOrder VERSION \([0-9.]*\)).*/\1/p' CMakeLists.txt)"
BUNDLE_ID="com.yachtpockets.shortorder"
BUILD="build-release"
STAGE="/tmp/ShortOrder-release"
if $DRY; then OUT="dist/dry-run"; else OUT="dist"; fi
AU="$NAME.component"; VST3="$NAME.vst3"; AAX="$NAME.aaxplugin"; APP="$NAME.app"
ENTITLEMENTS="Resources/Installer/Standalone.entitlements"
DOCS=(LICENSE NOTICE THIRD-PARTY-NOTICES.md Resources/Fonts/OFL.txt)
[ -n "$VERSION" ] || { echo "✗ couldn't read the version from CMakeLists.txt"; exit 1; }

step() { echo; echo "== $*"; }
fail() { echo "✗ $*"; exit 1; }

# --- Settings (the signed run only) ---
if ! $DRY; then
    [ -f signing.env ] || fail "signing.env is missing: copy signing.env.example to signing.env and fill it in."
    # shellcheck disable=SC1091
    source signing.env
    for v in DEVELOPER_ID_APP NOTARY_PROFILE $($PKG_WANTED && echo DEVELOPER_ID_INSTALLER); do
        [ -n "${!v:-}" ] || fail "$v is empty in signing.env"
    done
    IDENTITIES="$(security find-identity -v -p codesigning)"
    grep -F -- "$DEVELOPER_ID_APP" <<< "$IDENTITIES" > /dev/null || fail "DEVELOPER_ID_APP isn't a valid code-signing identity in the keychain"
    IDENTITIES="$(security find-identity -v)"
    if $PKG_WANTED; then
        grep -F -- "$DEVELOPER_ID_INSTALLER" <<< "$IDENTITIES" > /dev/null || fail "DEVELOPER_ID_INSTALLER isn't in the keychain (or run with --no-pkg)"
    fi
    if $AAX_WANTED; then
        for v in PACE_ACCOUNT PACE_WCGUID; do
            [ -n "${!v:-}" ] || fail "$v is empty in signing.env (or run with --no-aax)"
        done
        command -v wraptool > /dev/null || fail "wraptool (PACE Eden tools) isn't installed (or run with --no-aax)"
    fi
fi

# --- 1. Build: Release, universal, from a clean build directory ---
step "Build $NAME $VERSION (Release, universal)"
rm -rf "$BUILD"
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release > "$BUILD.log" 2>&1 || { cat "$BUILD.log"; fail "configure failed"; }
grep -E "JUCE|AAX" "$BUILD.log" || true
TARGETS=("${NAME}_AU" "${NAME}_VST3" "${NAME}_Standalone")
if $AAX_WANTED; then
    if grep -F "${NAME}_AAX" <<< "$(cmake --build "$BUILD" --target help 2> /dev/null)" > /dev/null; then
        TARGETS+=("${NAME}_AAX")
    elif $DRY; then
        echo "(no AAX SDK at ~/AAX_SDK: the dry run goes on without AAX)"
        AAX_WANTED=false
    else
        fail "the AAX SDK wasn't found (~/AAX_SDK); install it or run with --no-aax"
    fi
fi
cmake --build "$BUILD" --config Release --target "${TARGETS[@]}" -j 8 >> "$BUILD.log" 2>&1 || { tail -40 "$BUILD.log"; fail "build failed (log: $BUILD.log)"; }
ART="$BUILD/${NAME}_artefacts/Release"
for b in "AU/$AU" "VST3/$VST3" "Standalone/$APP"; do
    [ -d "$ART/$b" ] || fail "not built: $ART/$b"
done
$AAX_WANTED && { [ -d "$ART/AAX/$AAX" ] || fail "not built: $ART/AAX/$AAX"; }
lipo -info "$ART/AU/$AU/Contents/MacOS/$NAME"

# --- 2. Stage in /tmp, away from the iCloud-synced Desktop, with every extended attribute cleared ---
step "Stage in $STAGE"
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp -R "$ART/AU/$AU" "$ART/VST3/$VST3" "$ART/Standalone/$APP" "$STAGE/"
$AAX_WANTED && cp -R "$ART/AAX/$AAX" "$STAGE/"
xattr -cr "$STAGE"
find "$STAGE" \( -name '._*' -o -name '.DS_Store' \) -delete

# The AU's tags (Apple's standard ones; JUCE writes none for an AU v2), as ./build-install.sh adds them, before signing.
PLIST="$STAGE/$AU/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Delete :AudioComponents:0:tags" "$PLIST" 2> /dev/null || true
/usr/libexec/PlistBuddy -c "Add :AudioComponents:0:tags array" -c "Add :AudioComponents:0:tags:0 string Effects" -c "Add :AudioComponents:0:tags:1 string Guitar" \
                        -c "Add :AudioComponents:0:tags:2 string Synthesizer" -c "Add :AudioComponents:0:tags:3 string Pitch" "$PLIST"

# --- 3. Sign ---
step "Sign"
sign_apple() { # $1: bundle in $STAGE; $2: entitlements (optional)
    if $DRY; then
        codesign --force --deep --sign - ${2:+--entitlements "$2"} "$STAGE/$1"
    else
        codesign --force --deep --timestamp --options runtime --sign "$DEVELOPER_ID_APP" ${2:+--entitlements "$2"} "$STAGE/$1"
    fi
    codesign --verify --deep --strict "$STAGE/$1"
    echo "✓ $1 signed$($DRY && echo " (ad hoc: dry run)")"
}
sign_apple "$AU"
sign_apple "$VST3"
sign_apple "$APP" "$ENTITLEMENTS"
if $AAX_WANTED; then
    if $DRY; then
        sign_apple "$AAX"
        echo "  (dry run: the AAX is not PACE-wrapped, so Pro Tools won't load it)"
    else
        # wraptool applies the PACE signature and Apple's codesign in one step, on an unsigned copy.
        echo "AAX: PACE wraptool. Plug in the iLok; enter the iLok password for $PACE_ACCOUNT (it isn't shown, logged or stored)."
        read -r -s -p "iLok password: " ILOK_PASS
        echo
        UNSIGNED="/tmp/ShortOrder-aax-unsigned"
        rm -rf "$UNSIGNED"
        mkdir -p "$UNSIGNED"
        mv "$STAGE/$AAX" "$UNSIGNED/"
        codesign --remove-signature "$UNSIGNED/$AAX" 2> /dev/null || true
        if ! wraptool sign --verbose --account "$PACE_ACCOUNT" --password "$ILOK_PASS" --wcguid "$PACE_WCGUID" --signid "$DEVELOPER_ID_APP" \
                           --dsig1-compat off --dsigharden --in "$UNSIGNED/$AAX" --out "$STAGE/$AAX"; then
            unset ILOK_PASS
            fail "wraptool sign failed"
        fi
        unset ILOK_PASS
        rm -rf "$UNSIGNED"
        wraptool verify --verbose --in "$STAGE/$AAX"
        codesign --verify --deep --strict "$STAGE/$AAX"
        echo "✓ $AAX signed (PACE + Apple)"
    fi
fi

# --- 4. Notarize each piece, then staple ---
notarize() { # $1: a file to submit (a zip of a bundle, or the pkg)
    local log
    log="$(mktemp)"
    xcrun notarytool submit "$1" --keychain-profile "$NOTARY_PROFILE" --wait 2>&1 | tee "$log"
    grep "status: Accepted" "$log" > /dev/null || { rm -f "$log"; fail "notarization of $(basename "$1") wasn't accepted (xcrun notarytool log <id> --keychain-profile \"\$NOTARY_PROFILE\" says why)"; }
    rm -f "$log"
}
BUNDLES=("$AU" "$VST3" "$APP")
$AAX_WANTED && BUNDLES+=("$AAX")
if $DRY; then
    step "Notarize: skipped (dry run)"
else
    step "Notarize and staple"
    for b in "${BUNDLES[@]}"; do
        echo "Notarizing $b (1-5 minutes)..."
        ditto -c -k --keepParent "$STAGE/$b" "$STAGE/notarize.zip"
        notarize "$STAGE/notarize.zip"
        rm -f "$STAGE/notarize.zip"
        xcrun stapler staple "$STAGE/$b"
        xcrun stapler validate "$STAGE/$b"
        echo "✓ $b notarized and stapled"
    done
    spctl --assess --type execute --verbose "$STAGE/$APP"
fi

# --- 5. Package into $OUT ---
step "Package into $OUT"
rm -rf "$OUT"
mkdir -p "$OUT"
# The notices beside each bundle: the files every zip carries, and what the installer shows.
NOTES="$STAGE/notes"
mkdir -p "$NOTES"
for d in "${DOCS[@]}"; do cp "$d" "$NOTES/"; done

# Zips, one per format: a folder with the bundle and the notices (ditto keeps the signatures and the stapled tickets; no
# resource forks or extended attributes are added).
zip_format() { # $1: format label; $2: bundle
    local dir="$STAGE/zip/$NAME-$1"
    rm -rf "$STAGE/zip"
    mkdir -p "$dir"
    ditto "$STAGE/$2" "$dir/$2"
    cp "$NOTES"/* "$dir/"
    ditto -c -k --norsrc --noextattr --keepParent "$dir" "$OUT/$NAME-$1.zip"
    echo "✓ $OUT/$NAME-$1.zip"
}
zip_format AU "$AU"
zip_format VST3 "$VST3"
$AAX_WANTED && zip_format AAX "$AAX"
zip_format Standalone "$APP"

# The installer: one component package per format (not relocatable, so it installs where it says and never "upgrades" a copy
# found elsewhere), then a product archive with the choices.
if $PKG_WANTED; then
PKGS="$STAGE/pkgs"
mkdir -p "$PKGS"
component_pkg() { # $1: id suffix; $2: bundle; $3: install location
    local root="$STAGE/root-$1"
    rm -rf "$root"
    mkdir -p "$root"
    ditto "$STAGE/$2" "$root/$2"
    pkgbuild --analyze --root "$root" "$STAGE/$1.plist" > /dev/null
    local n=0
    while /usr/libexec/PlistBuddy -c "Print :$n" "$STAGE/$1.plist" > /dev/null 2>&1; do
        /usr/libexec/PlistBuddy -c "Delete :$n:BundleIsRelocatable" "$STAGE/$1.plist" > /dev/null 2>&1 || true
        /usr/libexec/PlistBuddy -c "Add :$n:BundleIsRelocatable bool false" "$STAGE/$1.plist"
        n=$((n + 1))
    done
    pkgbuild --root "$root" --component-plist "$STAGE/$1.plist" --identifier "$BUNDLE_ID.$1" --version "$VERSION" \
             --install-location "$3" "$PKGS/$NAME-$1.pkg" > /dev/null
}
component_pkg au "$AU" "/Library/Audio/Plug-Ins/Components"
component_pkg vst3 "$VST3" "/Library/Audio/Plug-Ins/VST3"
$AAX_WANTED && component_pkg aax "$AAX" "/Library/Application Support/Avid/Audio/Plug-Ins"
component_pkg app "$APP" "/Applications"

# What the installer shows: a welcome, and as its licence the MIT licence, the notice, the third-party notices and the font's OFL.
RES="$STAGE/pkg-resources"
mkdir -p "$RES"
cat > "$RES/welcome.txt" << EOF
ShortOrder $VERSION, The 8ms Guitar Synthesizer, by Yacht Pockets.

This installs ShortOrder's AU, VST3$($AAX_WANTED && echo ", AAX") and Standalone app (choose which under Customize):
  AU:         /Library/Audio/Plug-Ins/Components/$AU
  VST3:       /Library/Audio/Plug-Ins/VST3/$VST3
$($AAX_WANTED && echo "  AAX:        /Library/Application Support/Avid/Audio/Plug-Ins/$AAX")
  Standalone: /Applications/$APP

Requires macOS 11 or later (Apple silicon or Intel). ShortOrder-Install-Guide.md has the setup for each host and the tips.
EOF
{
    cat LICENSE
    printf '\n\n==== NOTICE ====\n\n'
    cat NOTICE
    printf '\n\n==== THIRD-PARTY-NOTICES.md ====\n\n'
    cat THIRD-PARTY-NOTICES.md
    printf '\n\n==== OFL.txt (Didact Gothic, the faceplate font) ====\n\n'
    cat Resources/Fonts/OFL.txt
} > "$RES/license.txt"

choice() { # $1: id suffix; $2: title; $3: description
    cat << EOF
    <choice id="$1" title="$2" description="$3" start_selected="true">
        <pkg-ref id="$BUNDLE_ID.$1"/>
    </choice>
EOF
}
pkgref() { echo "    <pkg-ref id=\"$BUNDLE_ID.$1\" version=\"$VERSION\" onConclusion=\"none\">$NAME-$1.pkg</pkg-ref>"; }
{
    cat << EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>$NAME $VERSION</title>
    <organization>$BUNDLE_ID</organization>
    <welcome file="welcome.txt" mime-type="text/plain"/>
    <license file="license.txt" mime-type="text/plain"/>
    <options customize="allow" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <domains enable_anywhere="false" enable_currentUserHome="false" enable_localSystem="true"/>
    <volume-check>
        <allowed-os-versions>
            <os-version min="11.0"/>
        </allowed-os-versions>
    </volume-check>
    <choices-outline>
        <line choice="au"/>
        <line choice="vst3"/>
EOF
    $AAX_WANTED && echo '        <line choice="aax"/>'
    cat << EOF
        <line choice="app"/>
    </choices-outline>
EOF
    choice au "Audio Unit (AU)" "For Logic Pro, GarageBand, MainStage and other AU hosts: /Library/Audio/Plug-Ins/Components"
    choice vst3 "VST3" "For REAPER, Ableton Live, Cubase, Studio One and other VST3 hosts: /Library/Audio/Plug-Ins/VST3"
    $AAX_WANTED && choice aax "AAX (Pro Tools)" "For Pro Tools: /Library/Application Support/Avid/Audio/Plug-Ins"
    choice app "Standalone app" "ShortOrder without a DAW: /Applications"
    pkgref au
    pkgref vst3
    $AAX_WANTED && pkgref aax
    pkgref app
    echo "</installer-gui-script>"
} > "$STAGE/distribution.xml"

PKG="$OUT/$NAME-$VERSION-macOS.pkg"
if $DRY; then
    productbuild --distribution "$STAGE/distribution.xml" --resources "$RES" --package-path "$PKGS" "$PKG"
else
    productbuild --distribution "$STAGE/distribution.xml" --resources "$RES" --package-path "$PKGS" --sign "$DEVELOPER_ID_INSTALLER" --timestamp "$PKG"
    pkgutil --check-signature "$PKG"
    echo "Notarizing the installer (1-5 minutes)..."
    notarize "$PKG"
    xcrun stapler staple "$PKG"
    xcrun stapler validate "$PKG"
    spctl --assess --type install --verbose "$PKG"
fi
echo "✓ $PKG"
fi # PKG_WANTED

# The guide's <!--PKG--> ... <!--/PKG--> blocks are about the installer: kept (markers dropped) with a pkg, removed without one.
# Likewise <!--AAX--> ... <!--/AAX--> blocks and lines ending in <!--AAXLINE--> for the AAX (Pro Tools) build.
GUIDE_SED=(-e "s/@VERSION@/$VERSION/g")
if $PKG_WANTED; then GUIDE_SED+=(-e '/^<!--\/\{0,1\}PKG-->$/d'); else GUIDE_SED+=(-e '/^<!--PKG-->$/,/^<!--\/PKG-->$/d'); fi
if $AAX_WANTED; then GUIDE_SED+=(-e '/^<!--\/\{0,1\}AAX-->$/d' -e 's/ <!--AAXLINE-->$//'); else GUIDE_SED+=(-e '/^<!--AAX-->$/,/^<!--\/AAX-->$/d' -e '/ <!--AAXLINE-->$/d'); fi
sed "${GUIDE_SED[@]}" Resources/Installer/ShortOrder-Install-Guide.md | cat -s > "$OUT/$NAME-Install-Guide.md"
echo "✓ $OUT/$NAME-Install-Guide.md"

# Checksums of every file.
( cd "$OUT" && find . -maxdepth 1 -type f ! -name SHA256SUMS.txt | sed 's|^\./||' | sort | xargs shasum -a 256 > SHA256SUMS.txt )
echo "✓ $OUT/SHA256SUMS.txt"

if $DRY; then
    echo "DRY RUN: unsigned (ad hoc) and not notarized; not for distribution." > "$OUT/DRY-RUN-UNSIGNED.txt"
fi
rm -rf "$STAGE"

step "Done: $NAME $VERSION$($DRY && echo " (DRY RUN: unsigned, not for distribution)")"
ls -l "$OUT"
