#!/usr/bin/env bash
# SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
set -euo pipefail

stage="${1:?Usage: collect-windows-notices.sh STAGED_PAYLOAD [BUILD_DIR]}"
build_dir="${2:-build-offline/release/client}"
prefix=/mingw64
notices="$stage/LICENSES/MSYS2"
mkdir -p "$notices"

# Resolve deployed DLLs back to the installed package database in one query.
declare -a libraries=()
while IFS= read -r -d '' dll; do
    relative=${dll#"$stage"/}
    for library in "$prefix/bin/${dll##*/}" "$prefix/share/qt6/plugins/$relative" "$prefix/share/qt6/$relative"; do
        [[ -f "$library" ]] || continue
        libraries+=("$library")
        break
    done
done < <(find "$stage" -type f -iname '*.dll' -print0)

mapfile -t package_names < <(
    {
        pacman -Qqo "${libraries[@]}"
        printf '%s\n' mingw-w64-x86_64-qt6-base mingw-w64-x86_64-qt6-declarative mingw-w64-x86_64-qt6-svg
    } | sort -u
)
packages=$(pacman -Q "${package_names[@]}")
{
    printf '# MSYS2 MINGW64 runtime\n\n'
    printf '| Package | Version | Source recipe & patches |\n| --- | --- | --- |\n'
    while read -r pkg version; do
        archive_version="${version##*:}"
        desc="/var/lib/pacman/local/$pkg-$archive_version/desc"
        base=$(awk '$0 == "%BASE%" { getline; print; exit }' "$desc")
        printf '| %s | %s | [source archive](https://mirror.msys2.org/mingw/sources/%s-%s.src.tar.zst) · [package](https://packages.msys2.org/packages/%s) |\n' "$pkg" "$version" "$base" "$archive_version" "$pkg"
    done <<< "$packages"
    printf '\nThe MSYS2 FFmpeg binary is distributed under GPL-3.0-or-later. '
    printf 'The matching build recipe above identifies the FFmpeg source, patches and build options.\n'
    printf '\nPS5Link source and build scripts are at https://github.com/Q1W1RTY/PS5Link.\n'
} > "$notices/Runtime-Sources.md"

while IFS= read -r license; do
    [[ -f "$license" ]] || continue
    rel="${license#"$prefix"/share/licenses/}"
    mkdir -p "$notices/$(dirname "$rel")"
    cp "$license" "$notices/$rel"
done < <(pacman -Qlq "${package_names[@]}" | awk '/^\/mingw64\/share\/licenses\//')

# The MSYS2 FFmpeg package publishes its license in the corresponding sources.
if grep -q "^mingw-w64-x86_64-ffmpeg " <<< "$packages"; then
    read -r _ ffmpeg_package < <(pacman -Q mingw-w64-x86_64-ffmpeg)
    ffmpeg_version=${ffmpeg_package%-*}
    mkdir -p "$notices/ffmpeg"
    for file in LICENSE.md COPYING.GPLv3 COPYING.LGPLv3; do
        curl --fail --silent --show-error --location --retry 2 \
            "https://raw.githubusercontent.com/FFmpeg/FFmpeg/n${ffmpeg_version##*[:~]}/$file" \
            --output "$notices/ffmpeg/$file"
    done
fi

# Licenses for components built as part of the native client.
cp "$build_dir/_deps/ps5link_imgui-src/LICENSE.txt" "$stage/LICENSES/Dear-ImGui.txt"
cp third-party/curl/COPYING "$stage/LICENSES/curl.txt"
cp third-party/nanopb/LICENSE.txt "$stage/LICENSES/nanopb.txt"
cp third-party/cpp-steam-tools/LICENSE "$stage/LICENSES/cpp-steam-tools.txt"
for project in jerasure gf-complete; do
    mkdir -p "$stage/LICENSES/$project"
    cp "third-party/$project/"{COPYING,License.txt} "$stage/LICENSES/$project/"
done
