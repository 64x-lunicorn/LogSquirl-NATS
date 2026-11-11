# ============================================================================
# nats.c, the official NATS C client, at the pinned version: the static
# library nats_static, with TLS against the OpenSSL of OpenSSL.cmake.
#
# Without libsodium: nats.c signs NKeys with its own ed25519 code.  Without
# NATS Streaming (deprecated, needs protobuf-c), examples and tests.
# ============================================================================
include(FetchContent)

# nats.c's options (option() honours these normal variables, CMP0077).
set(NATS_BUILD_WITH_TLS            ON)
set(NATS_BUILD_OPENSSL_STATIC_LIBS ON)
set(NATS_BUILD_USE_SODIUM          OFF)
set(NATS_BUILD_STREAMING           OFF)
set(NATS_BUILD_EXAMPLES            OFF)
set(NATS_BUILD_LIB_STATIC          ON)
set(NATS_BUILD_LIB_SHARED          OFF)
set(BUILD_TESTING                  OFF)

FetchContent_Declare(natsc
    URL      "${NATSC_PIN_URL}"
    URL_HASH "SHA256=${NATSC_PIN_SHA256}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    # Its headers are not ours to warn about; it installs nothing of ours.
    SYSTEM
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(natsc)

if(NOT TARGET nats_static)
    message(FATAL_ERROR "nats.c ${NATSC_PIN_VERSION} defined no target nats_static")
endif()
