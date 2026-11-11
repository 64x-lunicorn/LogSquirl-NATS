# 1. OpenSSL is built by the plugin's CMake and linked in statically

Status: accepted (#1)

## Context

nats.c needs OpenSSL for TLS. LogSquirl ships no OpenSSL (Qt uses
SecureTransport and Schannel on macOS and Windows), the shared CI
(LogSquirl-Plugin-CI) can bundle only Qt libraries next to a plugin, and it
runs nothing but `cmake`, `cmake --build` and `ctest` on the plugin. A system
OpenSSL on Linux differs between distributions, and one loaded into the host
process by Qt's TLS backend could clash with another copy.

## Decision

The plugin's own CMake builds OpenSSL 3.x LTS from the pinned,
SHA-256-checked release tarball at configure time, static only and the same
on Linux, macOS and Windows (`no-asm` there, so no NASM). nats.c is built
against it (`NATS_BUILD_WITH_TLS`, `NATS_BUILD_OPENSSL_STATIC_LIBS`, no
libsodium), and both end up inside `logsquirl_nats`. The library exports only
its entry points, so on Linux the OpenSSL symbols stay local.

## Consequences

- The package carries no extra library and runs on every distribution; a
  ctest checks that the library links no OpenSSL shared library.
- Nothing changes in LogSquirl-Plugin-CI or the other plugins.
- The first configure takes a few minutes longer (longest on Windows).
- An OpenSSL security fix reaches users only with a plugin release: bump the
  version and hash in `cmake/Dependencies.cmake` and release.
- The package carries the Apache License 2.0 text and NOTICE.

## Considered

- OpenSSL from vcpkg or Homebrew static libraries in CI: needs changes to the
  shared workflows, and differs per platform.
- Teaching LogSquirl-Plugin-CI to bundle non-Qt shared libraries: a change
  for every plugin for one plugin's need, and a shared OpenSSL next to the
  plugin still risks clashing with the host's.
