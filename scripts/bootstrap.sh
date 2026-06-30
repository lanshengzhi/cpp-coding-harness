#!/usr/bin/env bash
set -euo pipefail

usage() {
	cat <<'EOF'
Usage: scripts/bootstrap.sh [options]

Bootstraps vcpkg, configures CMake with the vcpkg preset, and optionally builds/tests.

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

require_command() {
	if ! command -v "$1" >/dev/null 2>&1; then
		echo "error: required command '$1' was not found on PATH" >&2
		exit 1
	fi
}

require_command git
require_command cmake

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

if [[ ! -x "$vcpkg_root/vcpkg" ]]; then
	echo "Building vcpkg"
	"$vcpkg_root/bootstrap-vcpkg.sh" -disableMetrics
fi

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

# CMake presets resolve CMakePresets.json relative to the current working
# directory, so run from repo_root regardless of where the script is invoked.
(
	cd "$repo_root" || exit 1

	echo "Configuring with CMake preset '$preset'"
	cmake --preset "$preset"

	if [[ "$build" -eq 1 ]]; then
		echo "Building with CMake preset '$preset'"
		cmake --build --preset "$preset"
	fi

	if [[ "$run_tests" -eq 1 ]]; then
		echo "Running tests with CTest preset '$preset'"
		ctest --preset "$preset"
	fi
)

echo "Bootstrap complete. VCPKG_ROOT=$VCPKG_ROOT"
