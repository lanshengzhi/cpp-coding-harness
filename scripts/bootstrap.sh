#!/usr/bin/env bash
set -euo pipefail

usage() {
	cat <<'EOF'
Usage: scripts/bootstrap.sh [options]

Bootstraps vcpkg, configures CMake with the vcpkg preset, and optionally builds/tests.

Environment precheck: git, curl, zip, unzip, tar, and a GCC 16+ compiler are
required. When a tool is missing or too old, the script prints the install
command for the detected distribution and exits before touching anything.
vcpkg is pinned to the builtin-baseline commit recorded in vcpkg.json.

Options:
  --no-build       Configure dependencies/project only; do not build.
  --test           Build and run tests after configuration.
  --release        Use the vcpkg-release preset instead of Debug.
  --vcpkg-root DIR Use DIR for vcpkg instead of $VCPKG_ROOT or .deps/vcpkg.
  -h, --help       Show this help.
EOF
}

build=1
run_tests=0
release=0
vcpkg_root_arg=""

while [[ $# -gt 0 ]]; do
	case "$1" in
	--no-build)
		build=0
		shift
		;;
	--test)
		run_tests=1
		build=1
		shift
		;;
	--release)
		release=1
		shift
		;;
	--vcpkg-root)
		if [[ $# -lt 2 ]]; then
			echo "error: --vcpkg-root requires a directory" >&2
			exit 2
		fi
		vcpkg_root_arg="$2"
		shift 2
		;;
	-h | --help)
		usage
		exit 0
		;;
	*)
		echo "error: unknown option: $1" >&2
		usage >&2
		exit 2
		;;
	esac
done

# --- Environment precheck -------------------------------------------------
#
# The build is reproducible by construction: vcpkg pins every dependency to the
# builtin-baseline commit in vcpkg.json, and the script pins vcpkg itself to
# that commit. What cannot be vendored is the host toolchain, so the precheck
# fails fast with a distribution-specific install command instead of letting a
# missing or too-old tool fail halfway through the build.

precheck_fail() {
	echo "error: $1" >&2
	exit 1
}

detect_distro() {
	if [[ -f /etc/os-release ]]; then
		. /etc/os-release
		case "${ID:-}" in
		ubuntu | debian)
			echo "ubuntu"
			;;
		arch | manjaro)
			echo "arch"
			;;
		*)
			echo "other"
			;;
		esac
	else
		echo "other"
	fi
}

require_tool() {
	local tool="$1"
	if ! command -v "$tool" >/dev/null 2>&1; then
		precheck_fail "required command '$tool' was not found on PATH"
	fi
}

distro="$(detect_distro)"
missing=0

for tool in git curl zip unzip tar; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		echo "error: required command '$tool' was not found on PATH" >&2
		missing=1
	fi
done

if [[ "$missing" -eq 1 ]]; then
	case "$distro" in
	ubuntu)
		echo "Install the missing tools, for example:" >&2
		echo "  sudo apt install curl zip unzip tar" >&2
		;;
	arch)
		echo "Install the missing tools, for example:" >&2
		echo "  sudo pacman -Syu curl zip unzip tar" >&2
		;;
	*)
		echo "Install the missing tools using your distribution's package manager." >&2
		;;
	esac
	exit 1
fi

# GCC 16+ (major version >= 16) is the toolchain floor (ADR 0038). Use g++ so
# the compiler actually used for linking is the one checked.
if ! command -v g++ >/dev/null 2>&1; then
	echo "error: a C++ compiler (g++) was not found on PATH" >&2
	case "$distro" in
	ubuntu)
		echo "Install GCC 16+, for example:" >&2
		echo "  sudo add-apt-repository ppa:ubuntu-toolchain-r/test" >&2
		echo "  sudo apt install gcc-16 g++-16" >&2
		echo "  sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-16 100" >&2
		echo "  sudo update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-16 100" >&2
		;;
	arch)
		echo "Install GCC, for example:" >&2
		echo "  sudo pacman -Syu gcc" >&2
		;;
	*)
		echo "Install a GCC 16+ toolchain using your distribution's package manager." >&2
		;;
	esac
	exit 1
fi

gcc_major="$(g++ -dumpversion | cut -d. -f1)"
if [[ "$gcc_major" -lt 16 ]]; then
	echo "error: GCC 16+ is required (found g++ $gcc_major.x)" >&2
	case "$distro" in
	ubuntu)
		echo "Install GCC 16, for example:" >&2
		echo "  sudo add-apt-repository ppa:ubuntu-toolchain-r/test" >&2
		echo "  sudo apt install gcc-16 g++-16" >&2
		echo "  sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-16 100" >&2
		echo "  sudo update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-16 100" >&2
		;;
	arch)
		echo "Install a newer GCC, for example:" >&2
		echo "  sudo pacman -Syu gcc" >&2
		;;
	*)
		echo "Install a GCC 16+ toolchain using your distribution's package manager." >&2
		;;
	esac
	exit 1
fi

echo "Environment precheck passed: g++ $gcc_major.x, tools OK"

# --- vcpkg acquisition and pinning ----------------------------------------

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"

if [[ -n "$vcpkg_root_arg" ]]; then
	vcpkg_root="$vcpkg_root_arg"
elif [[ -n "${VCPKG_ROOT:-}" ]]; then
	vcpkg_root="$VCPKG_ROOT"
else
	vcpkg_root="$repo_root/.deps/vcpkg"
fi
if [[ "$vcpkg_root" != /* ]]; then
	vcpkg_root="$repo_root/$vcpkg_root"
fi

if [[ ! -d "$vcpkg_root/.git" ]]; then
	mkdir -p "$(dirname "$vcpkg_root")"
	echo "Bootstrapping vcpkg into $vcpkg_root"
	git clone https://github.com/microsoft/vcpkg.git "$vcpkg_root"
fi

# Pin vcpkg itself to the builtin-baseline commit recorded in vcpkg.json. A
# clone that is older than the baseline cannot resolve the dependency versions
# (git show <baseline>:versions/baseline.json fails), so fetch before checkout
# every time. This keeps dependency resolution reproducible and self-healing.
baseline="$(grep -oP '"builtin-baseline"\s*:\s*"\K[0-9a-f]+' "$repo_root/vcpkg.json")"
if [[ -z "$baseline" ]]; then
	echo "error: no builtin-baseline found in $repo_root/vcpkg.json" >&2
	exit 1
fi

current_head="$(git -C "$vcpkg_root" rev-parse HEAD 2>/dev/null || true)"
if [[ "$current_head" != "$baseline" ]]; then
	echo "Pinning vcpkg to builtin-baseline $baseline"
	git -C "$vcpkg_root" fetch origin "$baseline" || git -C "$vcpkg_root" fetch origin
	git -C "$vcpkg_root" checkout --detach "$baseline" >/dev/null 2>&1 || \
		precheck_fail "could not check out vcpkg baseline $baseline"
fi

# The vcpkg binary version follows scripts/vcpkg-tool-metadata.txt, which
# changes with the checkout. bootstrap-vcpkg.sh always downloads (or builds)
# the binary matching that metadata and overwrites vcpkg_root/vcpkg, so run it
# unconditionally: a stale binary rejects the newer tool schema. This also
# self-heals a clone whose binary predates the pinned baseline.
echo "Building/updating vcpkg for baseline $baseline"
"$vcpkg_root/bootstrap-vcpkg.sh" -disableMetrics

export VCPKG_ROOT="$vcpkg_root"

preset="vcpkg"
if [[ "$release" -eq 1 ]]; then
	preset="vcpkg-release"
fi

# If a prior system-package configure left a cache in build/, CMake will keep
# ignoring the vcpkg toolchain. Remove only the cache metadata, not build outputs.
cache_file="$repo_root/build/CMakeCache.txt"
if [[ -f "$cache_file" ]] && ! grep -Fq "scripts/buildsystems/vcpkg.cmake" "$cache_file"; then
	echo "Removing stale non-vcpkg CMake cache from build/"
	rm -f "$cache_file"
	rm -rf "$repo_root/build/CMakeFiles"
fi

# Prefer the CMake vcpkg cached into downloads/tools (fixed by the vcpkg
# version) over any system CMake; fall back to the system one when absent.
vcpkg_cmake="$(find "$vcpkg_root/downloads/tools" -maxdepth 4 -type f -name cmake -perm -u+x 2>/dev/null | sort -V | tail -1 || true)"
if [[ -n "$vcpkg_cmake" ]]; then
	cmake_bin="$vcpkg_cmake"
	echo "Using vcpkg-cached CMake: $cmake_bin"
else
	cmake_bin="cmake"
	echo "Using system CMake (vcpkg-cached CMake not found)"
fi

# CMake presets resolve CMakePresets.json relative to the current working
# directory, so run from repo_root regardless of where the script is invoked.
(
	cd "$repo_root" || exit 1

	echo "Configuring with CMake preset '$preset'"
	"$cmake_bin" --preset "$preset"

	if [[ "$build" -eq 1 ]]; then
		echo "Building with CMake preset '$preset'"
		"$cmake_bin" --build --preset "$preset"
	fi

	if [[ "$run_tests" -eq 1 ]]; then
		echo "Running tests with CTest preset '$preset'"
		ctest --preset "$preset"
	fi
)

echo "Bootstrap complete. VCPKG_ROOT=$VCPKG_ROOT"
