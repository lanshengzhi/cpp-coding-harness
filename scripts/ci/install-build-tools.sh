#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 || -z "$1" || "$1" == "/" ]]; then
	echo "Usage: scripts/ci/install-build-tools.sh INSTALL_ROOT" >&2
	exit 2
fi
if [[ "$(uname -s)" != "Linux" || "$(uname -m)" != "x86_64" ]]; then
	echo "error: pinned CI build tools support Linux x86-64 only" >&2
	exit 1
fi

cmake_version="4.4.2"
cmake_sha256="3ada9a3f5d8a85413579bdd0ea6aa8e8da86efdd6d15c91a1afa517f2021956c"
ninja_version="1.13.2"
ninja_sha256="5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6"

install_root="$1"
download_dir="$install_root/downloads"
bin_dir="$install_root/bin"
mkdir -p "$download_dir" "$bin_dir"

cmake_archive="$download_dir/cmake-${cmake_version}-linux-x86_64.tar.gz"
ninja_archive="$download_dir/ninja-linux-${ninja_version}.zip"

curl --fail --location --retry 3 --output "$cmake_archive" \
	"https://github.com/Kitware/CMake/releases/download/v${cmake_version}/cmake-${cmake_version}-linux-x86_64.tar.gz"
echo "$cmake_sha256  $cmake_archive" | sha256sum --check --status || {
	echo "error: CMake ${cmake_version} archive checksum mismatch" >&2
	exit 1
}

tar --extract --gzip --file "$cmake_archive" --directory "$install_root"
cmake_root="$install_root/cmake-${cmake_version}-linux-x86_64"
for tool in cmake cpack ctest; do
	ln -sfn "$cmake_root/bin/$tool" "$bin_dir/$tool"
done

curl --fail --location --retry 3 --output "$ninja_archive" \
	"https://github.com/ninja-build/ninja/releases/download/v${ninja_version}/ninja-linux.zip"
echo "$ninja_sha256  $ninja_archive" | sha256sum --check --status || {
	echo "error: Ninja ${ninja_version} archive checksum mismatch" >&2
	exit 1
}
unzip -oq "$ninja_archive" -d "$bin_dir"
chmod +x "$bin_dir/ninja"

[[ "$($bin_dir/cmake --version | awk 'NR == 1 { print $3 }')" == "$cmake_version" ]]
[[ "$($bin_dir/ninja --version)" == "$ninja_version" ]]

if [[ -n "${GITHUB_PATH:-}" ]]; then
	echo "$bin_dir" >> "$GITHUB_PATH"
fi

echo "Installed CMake $cmake_version and Ninja $ninja_version in $bin_dir"
