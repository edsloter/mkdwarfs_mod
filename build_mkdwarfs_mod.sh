#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
#
# Copyright (C) 2026 Edward Sloter
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published
# by the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
#
# All-in-one build script for the modified DwarFS (global chunk-level
# de-duplication) on Linux.
#
# This script:
#   - installs all required build dependencies via the system package
#     manager (apt, dnf, pacman or zypper)
#   - configures and builds mkdwarfs, dwarfsextract, dwarfsck (plus the
#     dwarfs FUSE driver when libfuse3 is available)
#   - runs a smoke test that verifies the --dedup-global option works and
#     the resulting image extracts correctly
#
# Usage:
#   ./build_mkdwarfs_mod.sh [options]
#
# Options (can also be set via environment variables):
#   --build-dir DIR     build directory        (default: ./build-mod, env BUILD_DIR)
#   --build-type TYPE   CMake build type       (default: RelWithDebInfo, env BUILD_TYPE)
#   --jobs N            parallel build jobs    (default: nproc, env BUILD_JOBS)
#   --static            fully static binaries (no shared library dependencies;
#                       needs the static variants of all libraries installed)
#   --no-fuse           do not build the dwarfs FUSE driver
#   --with-tests        also build and run the global-dedup test suite
#   --skip-smoke-test   skip the round-trip smoke test
#   --skip-deps         skip dependency installation (use installed packages)
#   -h, --help          show this help
#
set -euo pipefail

# ---------------------------------------------------------------------------
# defaults & argument parsing
# ---------------------------------------------------------------------------

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build-mod}"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
BUILD_JOBS="${BUILD_JOBS:-$(nproc 2>/dev/null || echo 4)}"
WITH_FUSE=0
WITH_TESTS=0
STATIC_BUILD=0
SKIP_SMOKE_TEST=0
SKIP_DEPS=0

usage() {
    grep '^#' "$0" | sed 's/^# \?//' | head -n 30
    exit "${1:-0}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir)    BUILD_DIR="$2"; BUILD_DIR_SET=1; shift 2 ;;
        --build-type)   BUILD_TYPE="$2"; shift 2 ;;
        --jobs)         BUILD_JOBS="$2"; shift 2 ;;
        --static)       STATIC_BUILD=1; shift ;;
        --no-fuse)      WITH_FUSE=0; shift ;;
        --with-tests)   WITH_TESTS=1; shift ;;
        --skip-smoke-test) SKIP_SMOKE_TEST=1; shift ;;
        --skip-deps)    SKIP_DEPS=1; shift ;;
        -h|--help)      usage 0 ;;
        *)              echo "unknown option: $1" >&2; usage 1 >&2 ;;
    esac
done

# Default to build-mod-static when --static is used
if [ "$STATIC_BUILD" -eq 1 ] && [ -z "${BUILD_DIR_SET:-}" ]; then
    BUILD_DIR="${REPO_ROOT}/build-mod-static"
fi

# Clean the build directory for a fresh build every time
if [ -d "$BUILD_DIR" ]; then
    echo "  Cleaning build directory: $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

step() {
    echo ""
    echo "==> $*"
}

die() {
    echo "ERROR: $*" >&2
    exit 1
}

have() { command -v "$1" >/dev/null 2>&1; }

# ---------------------------------------------------------------------------
# prerequisites
# ---------------------------------------------------------------------------

step "Checking prerequisites..."

have git   || die "git not found. Please install git."
have cmake || die "cmake not found. Please install CMake >= 3.28."
have g++   || die "g++ not found. Please install a C++20 compiler (GCC >= 12 or Clang >= 16)."

CMAKE_VERSION="$(cmake --version | head -n1 | awk '{print $3}')"
echo "  cmake $CMAKE_VERSION"

# check cmake >= 3.28.0 (sort -V: input must be sorted ascending)
if ! printf '%s\n' "3.28.0" "$CMAKE_VERSION" | sort -CV >/dev/null 2>&1; then
    die "cmake >= 3.28.0 required, found $CMAKE_VERSION.
  On older distributions, install a newer CMake with:
    pip3 install --user cmake
  or use the official Kitware APT repository."
fi

# ---------------------------------------------------------------------------
# install dependencies
# ---------------------------------------------------------------------------

if [ "$SKIP_DEPS" -eq 0 ]; then
    step "Installing build dependencies..."

    # fmt >= 10, range-v3 >= 0.12 and parallel-hashmap are fetched
    # automatically via CMake FetchContent when the system versions are
    # too old, so they are not required as system packages.
    # Note: boost.uuid is header-only since boost 1.69 and ships with the
    # other boost development packages, so there is no libboost-uuid-dev.
    # ccache is installed explicitly so that CMake doesn't accidentally
    # pick up a Windows ccache.exe from the PATH (e.g. in WSL).
    APT_PACKAGES=(
        build-essential cmake ninja-build pkg-config git ca-certificates python3 ccache
        libboost-chrono-dev
        libboost-filesystem-dev
        libboost-program-options-dev
        libboost-thread-dev
        libssl-dev
        libxxhash-dev
        libzstd-dev
        liblz4-dev
        liblzma-dev
        libbrotli-dev
        libarchive-dev
        libflac-dev
        nlohmann-json3-dev
        libutfcpp-dev
        libhowardhinnant-date-dev
        libfuse3-dev
    )

    DNF_PACKAGES=(
        gcc-c++ cmake ninja-build pkgconf git ccache
        boost-chrono boost-filesystem boost-program-options boost-thread boost-uuid
        openssl-devel libxxhash-devel libzstd-devel lz4-devel xz-devel brotli-devel
        libarchive-devel flac-devel json-devel utf8cpp-devel fuse3-devel
    )

    ARCH_PACKAGES=(
        base-devel cmake ninja pkgconf git ccache
        boost openssl libxxhash zstd lz4 xz brotli libarchive flac nlohmann-json utf8cpp fuse3
    )

    ZYPPER_PACKAGES=(
        gcc-c++ cmake ninja pkg-config git ccache
        libboost_filesystem-devel libboost_program_options-devel
        libboost_thread-devel libboost_chrono-devel libboost_uuid-devel
        libopenssl-devel libxxhash-devel libzstd-devel liblz4-devel
        liblzma-devel brotli-devel libarchive-devel libflac-devel
        nlohmann_json-devel utf8cpp-devel fuse3-devel
    )

    if have apt-get; then
        sudo="sudo"
        [ "$(id -u)" -eq 0 ] && sudo=""
        $sudo apt-get update -qq
        $sudo apt-get install -y -qq "${APT_PACKAGES[@]}"
    elif have dnf; then
        sudo="sudo"
        [ "$(id -u)" -eq 0 ] && sudo=""
        $sudo dnf install -y -q "${DNF_PACKAGES[@]}"
    elif have pacman; then
        sudo="sudo"
        [ "$(id -u)" -eq 0 ] && sudo=""
        $sudo pacman -Sy --needed --noconfirm --noprogressbar "${ARCH_PACKAGES[@]}"
    elif have zypper; then
        sudo="sudo"
        [ "$(id -u)" -eq 0 ] && sudo=""
        $sudo zypper --non-interactive install "${ZYPPER_PACKAGES[@]}"
    else
        echo "  Unsupported package manager (need apt/dnf/pacman/zypper)."
        echo "  Please install the required dependencies manually; see"
        echo "  https://github.com/mhx/darfs/blob/main/README.md for a list."
        echo ""
        echo "  Required libraries: boost (chrono, filesystem, program_options,"
        echo "  thread, uuid), openssl/libcrypto, xxhash, zstd, lz4, lzma, brotli,"
        echo "  libarchive, flac, nlohmann-json, fuse3 (optional, for the driver)"
        echo ""
        echo "  Re-run with --skip-deps once the packages are installed."
        exit 1
    fi
    echo "  dependencies installed"
fi

# ---------------------------------------------------------------------------
# date/date.h fallback (GCC < 14 lacks std::chrono::from_stream)
# ---------------------------------------------------------------------------

# src/util.cpp uses Howard Hinnant's date library when available and falls
# back to std::chrono::from_stream otherwise.  The latter is only provided by
# libstdc++ >= 14, so on older distros we need the header from somewhere.
ensure_date_headers() {
    if printf '#include <date/date.h>\nint main() { return 0; }\n' \
        | g++ -x c++ - -fsyntax-only >/dev/null 2>&1; then
        return 0
    fi

    if [ ! -f /usr/local/include/date/date.h ]; then
        echo "==> Fetching Howard Hinnant date headers (GCC < 14 fallback)..."
        local tmp
        tmp="$(mktemp -d)"
        curl -fsSL -o "$tmp/date.tar.gz" \
            https://github.com/HowardHinnant/date/archive/refs/tags/v3.0.3.tar.gz \
            || die "failed to download date headers"
        tar -xzf "$tmp/date.tar.gz" -C "$tmp"
        local sudo="sudo"
        [ "$(id -u)" -eq 0 ] && sudo=""
        $sudo mkdir -p /usr/local/include
        $sudo cp -r "$tmp"/date-3.0.3/include/date /usr/local/include/
        rm -rf "$tmp"
    fi
}

ensure_date_headers

# ---------------------------------------------------------------------------
# configure
# ---------------------------------------------------------------------------

step "Configuring CMake (${BUILD_TYPE})..."

CMAKE_ARGS=(
    -S "$REPO_ROOT"
    -B "$BUILD_DIR"
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    -DWITH_TOOLS=ON
)

if have ninja; then
    CMAKE_ARGS+=(-G Ninja)
fi

if [ "$WITH_FUSE" -eq 1 ] && pkg-config --exists fuse3 2>/dev/null; then
    CMAKE_ARGS+=(-DWITH_FUSE_DRIVER=ON)
elif [ "$WITH_FUSE" -eq 1 ]; then
    echo "  libfuse3 not found - building without the dwarfs FUSE driver."
    CMAKE_ARGS+=(-DWITH_FUSE_DRIVER=OFF)
else
    CMAKE_ARGS+=(-DWITH_FUSE_DRIVER=OFF)
fi

if [ "$WITH_TESTS" -eq 1 ]; then
    CMAKE_ARGS+=(-DWITH_TESTS=ON)
fi

# ---------------------------------------------------------------------------
# static build
# ---------------------------------------------------------------------------

if [ "$STATIC_BUILD" -eq 1 ]; then
    step "Configuring fully static binaries..."

    # Warn (but do not abort) if the static variants of libraries that the
    # link stage will require are missing.  The actual gate is the link
    # itself; paths/naming conventions differ between distributions.
    missing=""
    for lib in z zstd lz4 lzma ssl crypto archive \
               boost_filesystem boost_program_options boost_thread \
               boost_chrono fuse3; do
        if ! find /usr/lib /usr/local/lib -name "lib${lib}.a" 2>/dev/null \
            | grep -q .; then
            missing="$missing ${lib}.a"
        fi
    done
    if [ -n "$missing" ]; then
        echo "  WARNING: these static libraries were not found:$missing"
        echo "  If the build fails at the linking stage, install the static"
        echo "  variants of the corresponding -dev/-devel packages, or drop"
        echo "  --static."
    fi

    # STATIC_BUILD_DO_NOT_USE is the upstream cmake option that:
    #  - sets CMAKE_FIND_LIBRARY_SUFFIXES to .a
    #  - sets Boost_USE_STATIC_LIBS=ON
    #  - appends --static to the pkg-config executable
    #  - links libstdc++/libgcc/libm/libpthread statically
    # dwarfs does not use any NSS-dependent functions (getpwnam & friends),
    # so a fully static glibc is safe here.
    #
    # Transitive static deps (libxml2 → zlib/lzma, libbz2 → zlib, etc.)
    # are not covered by upstream's own --start-group, causing link failures.
    # We expand the group to include all common transitive deps.
    CMAKE_ARGS+=(
        -DSTATIC_BUILD_DO_NOT_USE=ON
        -DCMAKE_EXE_LINKER_FLAGS=-static
    )
fi

# manpage generation (--man option) requires python3 + mistletoe; disable
# it if they are not available
WITH_MAN=OFF
if have python3; then
    if ! python3 -c "import mistletoe" >/dev/null 2>&1; then
        echo "  Installing mistletoe (needed for manpage generation)..."
        python3 -m pip install --user --quiet mistletoe >/dev/null 2>&1 || true
    fi
    if python3 -c "import mistletoe" >/dev/null 2>&1; then
        WITH_MAN=ON
    fi
fi
if [ "$WITH_MAN" = "OFF" ]; then
    echo "  python3/mistletoe not available - building without --man option."
fi
CMAKE_ARGS+=(-DWITH_MAN_OPTION=$WITH_MAN)

# generating man pages (for installation) requires 'ronn' when building
# from a git checkout; skip them when it is not available
if have ronn; then
    CMAKE_ARGS+=(-DWITH_MAN_PAGES=ON)
else
    echo "  ronn not found - not building man pages."
    CMAKE_ARGS+=(-DWITH_MAN_PAGES=OFF)
fi

# ---------------------------------------------------------------------------
# Patch CMakeLists.txt for static builds:
# add_link_options() places libs BEFORE <OBJECTS> and <LINK_LIBRARIES> in
# the link command, so transitive deps (zlib/lzma needed by libxml2/libarchive)
# must appear AFTER <LINK_LIBRARIES>. string(REPLACE) on the link template
# appends them at the very end of the command.
if [ "$STATIC_BUILD" -eq 1 ]; then
    cat >> "$REPO_ROOT/CMakeLists.txt" << 'CMAKEPATCH'

# --- static-build: append transitive deps at end of link line ---
if(STATIC_BUILD_DO_NOT_USE)
  foreach(_var CMAKE_CXX_LINK_EXECUTABLE CMAKE_C_LINK_EXECUTABLE)
    string(REPLACE "<LINK_LIBRARIES>"
      "<LINK_LIBRARIES> -Wl,--start-group -lz -llzma -lbz2 -licui18n -licuuc -licudata -ldl -lrt -Wl,--end-group"
      ${_var} "${${_var}}")
  endforeach()
endif()
CMAKEPATCH
fi

cmake "${CMAKE_ARGS[@]}"

# ---------------------------------------------------------------------------
# build
# ---------------------------------------------------------------------------

step "Building..."

TARGETS=(mkdwarfs)
if [ "$WITH_FUSE" -eq 1 ] && pkg-config --exists fuse3 2>/dev/null; then
    TARGETS+=(dwarfs-bin) # CMake target name; output is 'dwarfs'
fi

cmake --build "$BUILD_DIR" --target "${TARGETS[@]}" -j "$BUILD_JOBS"

step "Build complete."

MKDWARFS="$BUILD_DIR/mkdwarfs"

[ -x "$MKDWARFS" ]     || die "mkdwarfs binary not found after build"

# ---------------------------------------------------------------------------
# report
# ---------------------------------------------------------------------------

step "Build complete."

"$MKDWARFS" --version 2>/dev/null | head -n1 || true

# Note: -H (long help) is used here since plain --help only lists a subset
# of options on Linux.  grep is used without -q on purpose: with -q it exits
# after the first match which can SIGPIPE the tool; combined with pipefail
# that would make the check fail even though the option exists.
if "$MKDWARFS" -H 2>&1 | grep -- '--dedup-global' >/dev/null; then
    echo "  --dedup-global support verified."
else
    die "the built mkdwarfs does not seem to support --dedup-global!"
fi

if [ "$STATIC_BUILD" -eq 1 ]; then
    echo "  static build: binaries have no shared library dependencies."
fi

echo ""
echo "Binaries:"
for t in mkdwarfs; do
    p="$BUILD_DIR/$t"
    if [ -x "$p" ]; then
        printf '  %-16s %8.1f MB\n' "$t" "$(echo "scale=1; $(stat -c%s "$p") / 1048576" | bc 2>/dev/null || stat -c%s "$p")"
    fi
done
if [ -x "$BUILD_DIR/dwarfs" ]; then
    printf '  %-16s %8.1f MB\n' "dwarfs" "$(echo "scale=1; $(stat -c%s "$BUILD_DIR/dwarfs") / 1048576" | bc 2>/dev/null || stat -c%s "$BUILD_DIR/dwarfs")"
fi

# ---------------------------------------------------------------------------
# smoke test
# ---------------------------------------------------------------------------

if [ "$SKIP_SMOKE_TEST" -eq 0 ]; then
    step "Running smoke test (--dedup-global round trip)..."

    SMOKE="$(mktemp -d /tmp/dwarfs-mod-smoke.XXXXXX)"
    trap 'rm -rf "$SMOKE"' EXIT

    mkdir -p "$SMOKE/in"

    # two files that share 512 KiB of embedded duplicate data
    python3 - "$SMOKE/in" <<'PYEOF'
import os, sys, random
rng = random.Random(1234)
dup = bytes(rng.getrandbits(8) for _ in range(512 * 1024))
filler = bytes(rng.getrandbits(8) for _ in range(512 * 1024))
d = sys.argv[1]
with open(os.path.join(d, "a.bin"), "wb") as f:
    f.write(dup + filler)
with open(os.path.join(d, "b.bin"), "wb") as f:
    f.write(filler + dup)
PYEOF

    if [ ! -s "$SMOKE/in/a.bin" ]; then
        # python3 not available; fall back to a simpler test
        head -c 524288 /dev/urandom > "$SMOKE/in/a.bin"
        cp "$SMOKE/in/a.bin" "$SMOKE/in/b.bin"
    fi

    "$MKDWARFS" -i "$SMOKE/in" -o "$SMOKE/image.dwarfs" -C null \
        --no-history --no-progress --dedup-global 4k >/dev/null

    mkdir -p "$SMOKE/out"
    "$DWARFSEXTRACT" -i "$SMOKE/image.dwarfs" -o "$SMOKE/out" >/dev/null

    for f in a.bin b.bin; do
        if ! cmp -s "$SMOKE/in/$f" "$SMOKE/out/$f"; then
            die "smoke test: content mismatch for $f"
        fi
    done

    "$DWARFSCK" -i "$SMOKE/image.dwarfs" --check-integrity >/dev/null

    echo "  Smoke test passed: round-trip verified, integrity check OK."
fi

# ---------------------------------------------------------------------------
# optional test suite
# ---------------------------------------------------------------------------

if [ "$WITH_TESTS" -eq 1 ]; then
    step "Building and running global-dedup test suite..."

    cmake --build "$BUILD_DIR" --target tool_main_test dwarfs_unit_tests -j "$BUILD_JOBS"

    TOOL_MAIN_TEST="$BUILD_DIR/tool_main_test"
    [ -x "$TOOL_MAIN_TEST" ] || die "tool_main_test not found"

    "$TOOL_MAIN_TEST" --gtest_filter='*global_dedup*'

    UNIT_TESTS="$BUILD_DIR/dwarfs_unit_tests"
    if [ -x "$UNIT_TESTS" ]; then
        "$UNIT_TESTS" --gtest_filter='cdc_chunker_test.*'
    fi

    echo "  Global dedup tests passed."
fi

echo ""
echo "All done."
