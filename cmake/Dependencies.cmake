# ============================================================================
# Pinned third-party versions and their SHA-256s — the one place to bump.
#
# A security fix in one of them is a change here (version and hash) plus a
# plugin release: OpenSSL and nats.c are linked statically into
# logsquirl_nats, so users get the fix only with a new plugin version.
#
# The hashes are those of the archives the URLs below name:
#   OpenSSL      openssl-<v>.tar.gz of the release (its .sha256 file)
#   nats.c       GitHub's source archive of the tag
#   nats-server  the release's SHA256SUMS, one entry per platform
#   Catch2       GitHub's source archive of the tag
# ============================================================================

# OpenSSL 3.5 is an LTS release (supported until April 2030).
set(OPENSSL_PIN_VERSION 3.5.9)
set(OPENSSL_PIN_SHA256  603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a)
set(OPENSSL_PIN_URL
    "https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_PIN_VERSION}/openssl-${OPENSSL_PIN_VERSION}.tar.gz")

set(NATSC_PIN_VERSION 3.14.0)
set(NATSC_PIN_SHA256  1f8b450bc295d0c94be201e34713ca0b515aae2c0d1b279273c3e6e0e72fe005)
set(NATSC_PIN_URL
    "https://github.com/nats-io/nats.c/archive/refs/tags/v${NATSC_PIN_VERSION}.tar.gz")

# Only for the tests: the server they start.
set(NATS_SERVER_PIN_VERSION 2.15.2)
set(NATS_SERVER_PIN_SHA256_darwin-arm64  cbd2219d01502c38733485ac684269da585ee479fa3b1377264beecdefdf9e9d)
set(NATS_SERVER_PIN_SHA256_linux-amd64   67cb8e10bc6f0c4b47f75a728529a20953dbae2a4f9a4a52ae8517d65ae303ff)
set(NATS_SERVER_PIN_SHA256_windows-amd64 f6398d5d11093778f870db70242cb50d20a58134c937c805ba8d114e7e398766)
# Where the releases are; the version directory v<version> is added to it.
set(NATS_SERVER_PIN_RELEASES_URL "https://github.com/nats-io/nats-server/releases/download")

# Only for the tests: the test framework (v2, as LogSquirl's own tests).
set(CATCH2_PIN_VERSION 2.13.10)
set(CATCH2_PIN_SHA256  d54a712b7b1d7708bc7a819a8e6e47b2fde9536f487b89ccbca295072a7d9943)
set(CATCH2_PIN_URL
    "https://github.com/catchorg/Catch2/archive/refs/tags/v${CATCH2_PIN_VERSION}.tar.gz")
