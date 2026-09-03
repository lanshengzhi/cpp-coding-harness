#!/usr/bin/env bash
set -euo pipefail

usage() {
	cat <<'EOF'
Usage: scripts/bootstrap.sh [options]

Prepares the host environment and the pinned vcpkg dependency checkout.
It does not configure, build, or test: CMake and CTest own those steps.

Environment precheck: native Linux x86-64 with glibc, git, curl, zip, unzip,
tar, CMake 4.4+, Ninja 1.11+, and a GCC 16.x compiler are required. When a
tool is missing or unsupported, the script prints an actionable diagnostic and
exits before touching anything. vcpkg is pinned to the builtin-baseline commit
recorded in vcpkg.json.

Options:
  --vcpkg-root DIR Use DIR for vcpkg instead of $VCPKG_ROOT or .deps/vcpkg.
  -h, --help       Show this help.

After bootstrap, configure, build, and test with CMake directly:

  cmake --preset vcpkg --fresh     # Fresh Validation configure
  cmake --build --preset vcpkg
  ctest --preset vcpkg

(export VCPKG_ROOT=<vcpkg root> is only required when using a custom checkout outside .deps/vcpkg)
Use the vcpkg-release preset for the Release configuration.
EOF
}

removed_option() {
	echo "error: $1 is no longer handled by bootstrap.sh; it only prepares the host and pinned vcpkg" >&2
	echo "run CMake/CTest directly instead, for example:" >&2
	echo "  export VCPKG_ROOT=<vcpkg root>" >&2
	if [[ "$1" == "--release" ]]; then
		echo "  cmake --preset vcpkg-release --fresh && cmake --build --preset vcpkg-release && ctest --preset vcpkg-release" >&2
	else
		echo "  cmake --preset vcpkg --fresh && cmake --build --preset vcpkg && ctest --preset vcpkg" >&2
	fi
	exit 2
}

vcpkg_root_arg=""

while [[ $# -gt 0 ]]; do
	case "$1" in
	--vcpkg-root)
		if [[ $# -lt 2 ]]; then
			echo "error: --vcpkg-root requires a directory" >&2
			exit 2
		fi
		vcpkg_root_arg="$2"
		shift 2
		;;
	--no-build | --test | --release)
		removed_option "$1"
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

version_at_least() {
	local actual="$1"
	local minimum="$2"
	[[ "$(printf '%s\n%s\n' "$minimum" "$actual" | sort -V | head -1)" == "$minimum" ]]
}

distro="$(detect_distro)"

if [[ "$(uname -s)" != "Linux" ]]; then
	precheck_fail "unsupported operating system '$(uname -s)'; native Linux x86-64 with glibc is required"
fi
if [[ "$(uname -m)" != "x86_64" ]]; then
	precheck_fail "unsupported architecture '$(uname -m)'; native Linux x86-64 is required"
fi
if ! getconf GNU_LIBC_VERSION >/dev/null 2>&1; then
	precheck_fail "unsupported C library; glibc is required (musl and other C libraries are unsupported)"
fi

missing=0
for tool in git curl zip unzip tar cmake ninja; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		echo "error: required command '$tool' was not found on PATH" >&2
		missing=1
	fi
done

if [[ "$missing" -eq 1 ]]; then
	case "$distro" in
	ubuntu)
		echo "Install the missing tools, for example:" >&2
		echo "  sudo apt install curl zip unzip tar ninja-build" >&2
		echo "Install CMake 4.4+ from an official Kitware release." >&2
		;;
	arch)
		echo "Install the missing tools, for example:" >&2
		echo "  sudo pacman -Syu curl zip unzip tar cmake ninja" >&2
		;;
	*)
		echo "Install the missing tools using your distribution's package manager." >&2
		;;
	esac
	exit 1
fi

cmake_version="$(cmake --version | awk 'NR == 1 { print $3 }')"
if ! version_at_least "$cmake_version" "4.4"; then
	precheck_fail "CMake 4.4 or newer is required (found $cmake_version)"
fi

ninja_version="$(ninja --version)"
if ! version_at_least "$ninja_version" "1.11"; then
	precheck_fail "Ninja 1.11 or newer is required (found $ninja_version)"
fi

# GCC 16.x is the sole supported build/release compiler (ADR 0039). The vcpkg
# configure preset pins CMAKE_CXX_COMPILER to the g++ driver on PATH, so verify
# gcc/g++ are 16.x.
if ! command -v gcc >/dev/null 2>&1 || ! command -v g++ >/dev/null 2>&1; then
	echo "error: GCC build compilers (gcc and g++) were not found on PATH" >&2
	case "$distro" in
	ubuntu)
		echo "Install GCC 16.x, for example:" >&2
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
		echo "Install a GCC 16.x toolchain using your distribution's package manager." >&2
		;;
	esac
	exit 1
fi

gcc_major="$(gcc -dumpversion | cut -d. -f1)"
gxx_major="$(g++ -dumpversion | cut -d. -f1)"
if [[ "$gcc_major" != "16" || "$gxx_major" != "16" ]]; then
	echo "error: GCC 16.x is required (found gcc $gcc_major.x and g++ $gxx_major.x)" >&2
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
		echo "Install a GCC 16.x toolchain using your distribution's package manager." >&2
		;;
	esac
	exit 1
fi

echo "Environment precheck passed: native Linux x86-64 glibc, CMake $cmake_version, Ninja $ninja_version, GCC $gcc_major.x"

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

tracked_changes="$(git -C "$vcpkg_root" status --porcelain --untracked-files=no)"
if [[ -n "$tracked_changes" ]]; then
	echo "error: the pinned vcpkg checkout has tracked modifications:" >&2
	echo "$tracked_changes" >&2
	precheck_fail "use a clean vcpkg checkout at $baseline"
fi

# The vcpkg binary version follows scripts/vcpkg-tool-metadata.txt, which
# changes with the checkout. bootstrap-vcpkg.sh always downloads (or builds)
# the binary matching that metadata and overwrites vcpkg_root/vcpkg, so run it
# unconditionally: a stale binary rejects the newer tool schema. This also
# self-heals a clone whose binary predates the pinned baseline.
echo "Building/updating vcpkg for baseline $baseline"
"$vcpkg_root/bootstrap-vcpkg.sh" -disableMetrics

cat <<EOF
Bootstrap complete. Configure, build, and test with CMake directly:

  cmake --preset vcpkg --fresh
  cmake --build --preset vcpkg
  ctest --preset vcpkg

Use the vcpkg-release preset for the Release configuration.
EOF
