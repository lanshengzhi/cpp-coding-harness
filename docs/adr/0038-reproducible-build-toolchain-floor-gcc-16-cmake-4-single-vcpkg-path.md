---
status: accepted
---

# Reproducible build toolchain floor: GCC 16+, CMake 4.0, single vcpkg path

The harness pins its dependencies to the `builtin-baseline` commit in `vcpkg.json`, but the host toolchain was only documented as "a C++23-capable compiler" with `cmake_minimum_required(VERSION 3.25)`. A fresh checkout could fail at any of several implicit host dependencies: a system CMake older than the preset floor, a compiler that does not satisfy the C++23 requirements, a vcpkg clone older than the baseline commit (manifest resolution breaks with `git show <baseline>:versions/baseline.json`), or the unmaintained `system` dependency preset. This ADR makes the toolchain floor explicit and reproducible.

## Considered options

- Keep CMake 3.25 and a loose compiler floor, fixing only the vcpkg baseline pin: rejected because the C++23 codebase already requires 2023-era compilers in practice, the 3.25 floor only preserved compatibility with toolchains that cannot build the project, and the CMP0167 policy guard existed solely for that compatibility.
- Raise to GCC 16.1.1 exactly: rejected because Ubuntu 24.04's toolchain PPA ships `16-20260315` snapshot builds (reported as 16.0.1 by `g++ -dumpversion`), so an exact-version check would reject the only installable GCC 16 on the primary target platform.
- Vendor or download a prebuilt compiler: rejected because GCC binaries are coupled to the host glibc and cannot be distributed reliably across Ubuntu/Arch; vcpkg does not provide compilers.
- Use a Docker/container build for full hermeticity: rejected because the user develops on Linux directly and wants a container-free path; environment prechecks make the host toolchain explicit instead.
- Adopt GCC 16+ as the floor, pin vcpkg to `builtin-baseline`, raise CMake to 4.0, remove the `system` preset, and precheck the host: accepted.

## Consequences

- `CMakeLists.txt` requires CMake 4.0 (`cmake_minimum_required(VERSION 4.0)`), and the CMP0167 `if(POLICY ...)` guard is replaced by an unconditional `cmake_policy(SET CMP0167 NEW)`.
- `CMakePresets.json` minimum is 4.0.0 and the `system` preset is removed; vcpkg is the only supported dependency source.
- `scripts/bootstrap.sh` prechecks git, curl, zip, unzip, tar, and `g++` major version >= 16, printing a distribution-specific install command before any build work, then pins the vcpkg checkout to the `builtin-baseline` commit with `git fetch` + checkout every run, and prefers the vcpkg-cached CMake over any system CMake.
- GCC version checks accept any 16.x (including PPA snapshot builds), not an exact patch version.
- The supported platform range is unchanged (Linux primary; macOS/Windows scripts remain best-effort); the compiler floor is raised to GCC 16+.
