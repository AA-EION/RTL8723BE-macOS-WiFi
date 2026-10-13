#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Issen Software Group
#
# Static checks that a build can start on macOS 10.13 High Sierra and later. Real High Sierra
# hardware/VMs are not available in CI, so this verifies what the loader and kext linker enforce:
# deployment target, architecture, and that nothing newer than 10.13 is linked unconditionally.
#
#   scripts/verify_compat.sh kext   path/to/RTL8723BEWiFi.kext
#   scripts/verify_compat.sh tool   path/to/binary            # CLI / classic UI: minos 10.13, no Swift/SwiftUI
#   scripts/verify_compat.sh modern path/to/binary            # SwiftUI UI: minos 14.0
#   scripts/verify_compat.sh app    "path/to/App.app"         # bundle layout + Info.plist
set -euo pipefail

fail() { echo "FAIL: $*" >&2; exit 1; }
ok()   { echo "ok:   $*"; }

# Prints the minimum macOS version of a Mach-O (LC_BUILD_VERSION minos or LC_VERSION_MIN_MACOSX version).
minos() {
    otool -l "$1" 2>/dev/null | awk '
        /cmd LC_BUILD_VERSION/      { mode = "build" }
        /cmd LC_VERSION_MIN_MACOSX/ { mode = "min" }
        !done && mode == "build" && $1 == "minos"   { print $2; done = 1 }
        !done && mode == "min"   && $1 == "version" { print $2; done = 1 }'
}

# a <= b for dotted versions
ver_le() { [[ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | head -1)" == "$1" ]]; }

check_arch() {
    local archs; archs="$(lipo -archs "$1")"
    [[ "$archs" == "x86_64" ]] || fail "$1: expected x86_64 only, got '$archs'"
    ok "$1 architecture x86_64"
}

check_minos() {
    local m; m="$(minos "$1")"
    [[ -n "$m" ]] || { otool -l "$1" | grep -A6 -E 'LC_BUILD_VERSION|LC_VERSION_MIN' >&2 || true; fail "$1: cannot read deployment target"; }
    ver_le "$m" "$2" || fail "$1: deployment target $m is newer than $2"
    ok "$1 deployment target $m (<= $2)"
}

case "${1:-}" in
kext)
    kext="$2"
    bin="$kext/Contents/MacOS/RTL8723BEWiFi"
    [[ -f "$bin" ]] || fail "missing $bin"
    check_arch "$bin"
    # A linked kext carries no LC_BUILD_VERSION / LC_VERSION_MIN, so check the objects it was built from.
    if [[ -n "$(minos "$bin")" ]]; then check_minos "$bin" 10.13; fi
    for o in build/RTL8723BE*.o; do check_minos "$o" 10.13; done
    # A kext may only reference kernel symbols through com.apple.kpi.* / iokit families.
    bad="$(otool -L "$bin" 2>/dev/null | tail -n +2 | grep -v '^[[:space:]]*$' || true)"
    [[ -z "$bad" ]] || fail "kext links user-space libraries: $bad"
    # The plist must declare dependencies that exist on High Sierra (Darwin 17).
    lib() { /usr/libexec/PlistBuddy -c "Print :OSBundleLibraries:$1" "$kext/Contents/Info.plist"; }
    for k in com.apple.iokit.IOPCIFamily com.apple.iokit.IONetworkingFamily com.apple.kpi.bsd com.apple.kpi.iokit \
             com.apple.kpi.libkern com.apple.kpi.mach; do
        v="$(lib "$k")" || fail "OSBundleLibraries missing $k"
        ok "OSBundleLibraries $k >= $v"
    done
    # Symbols the kext needs, resolved against the SDK's kernel interface (best effort, informational).
    if command -v kextlibs >/dev/null; then kextlibs -all-symbols -undef-symbols "$kext" 2>&1 | head -20 || true; fi
    ;;
tool)
    check_arch "$2"
    check_minos "$2" 10.13
    # The classic UI and CLI must not need the Swift runtime or any framework added after 10.13.
    if otool -L "$2" | grep -Eq 'libswift|SwiftUI|Combine|Observation|_Concurrency'; then
        otool -L "$2" >&2; fail "$2 links Swift/SwiftUI libraries that do not exist on macOS 10.13"
    fi
    ok "$2 has no Swift/SwiftUI dependency"
    ;;
modern)
    check_arch "$2"
    check_minos "$2" 14.0
    ;;
app)
    app="$2"
    plist="$app/Contents/Info.plist"
    [[ "$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "$plist")" == "10.13" ]] || fail "LSMinimumSystemVersion must be 10.13"
    exe="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$plist")"
    "$0" tool "$app/Contents/MacOS/$exe"
    "$0" modern "$app/Contents/MacOS/${exe}Modern"
    "$0" kext "$app/Contents/Resources/RTL8723BEWiFi.kext"
    "$0" tool "$app/Contents/Resources/rtl8723be_cli"
    codesign --verify --deep --strict "$app" && ok "bundle signature valid"
    ;;
*)
    sed -n 4,13p "$0"; exit 2 ;;
esac
