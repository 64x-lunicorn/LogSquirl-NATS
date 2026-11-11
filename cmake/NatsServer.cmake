# ============================================================================
# A nats-server for the tests (only included with BUILD_TESTS).
#
# Downloads the official release archive for the host platform at the
# pinned version, verifies its SHA-256 and unpacks the binary into the build
# tree.  Sets, for the test program:
#
#   LOGSQUIRL_NATS_TEST_SERVER              path of the nats-server binary,
#                                           empty when there is none
#   LOGSQUIRL_NATS_TEST_SERVER_UNAVAILABLE  why there is none, else empty
#
# Without a server (offline, no pinned build for the platform, or switched
# off) the build still succeeds and the server tests report as skipped with
# that reason.  A download whose hash does not match fails the configure:
# that is not "offline", that is a wrong file.
# ============================================================================

set(NATS_SERVER_EXECUTABLE "" CACHE FILEPATH
    "A nats-server binary for the tests; empty: download the pinned release")
option(NATS_SERVER_DOWNLOAD "Download the pinned nats-server for the tests" ON)
# Without the version: a pin bump in cmake/Dependencies.cmake takes effect
# also in an existing build directory, where this cache entry stays.
set(NATS_SERVER_RELEASES_URL "${NATS_SERVER_PIN_RELEASES_URL}" CACHE STRING
    "Where the nats-server releases are (a mirror, say): <this>/v<version>/<archive>; the SHA-256 must match")

set(LOGSQUIRL_NATS_TEST_SERVER "")
set(LOGSQUIRL_NATS_TEST_SERVER_UNAVAILABLE "")

function(_nats_server_provide)
    if(NATS_SERVER_EXECUTABLE)
        if(EXISTS "${NATS_SERVER_EXECUTABLE}")
            set(LOGSQUIRL_NATS_TEST_SERVER "${NATS_SERVER_EXECUTABLE}" PARENT_SCOPE)
        else()
            set(LOGSQUIRL_NATS_TEST_SERVER_UNAVAILABLE
                "NATS_SERVER_EXECUTABLE ${NATS_SERVER_EXECUTABLE} does not exist" PARENT_SCOPE)
        endif()
        return()
    endif()

    # The platforms the plugin ships for.
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _cpu)
    set(_platform "")
    if(APPLE AND _cpu MATCHES "^(arm64|aarch64)$")
        set(_platform darwin-arm64)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND _cpu MATCHES "^(x86_64|amd64)$")
        set(_platform linux-amd64)
    elseif(WIN32 AND _cpu MATCHES "^(amd64|x86_64)$")
        set(_platform windows-amd64)
    endif()
    if(NOT _platform)
        set(LOGSQUIRL_NATS_TEST_SERVER_UNAVAILABLE
            "no pinned nats-server for ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}; set NATS_SERVER_EXECUTABLE"
            PARENT_SCOPE)
        return()
    endif()

    if(WIN32)
        set(_archive_ext zip)
        set(_exe nats-server.exe)
    else()
        set(_archive_ext tar.gz)
        set(_exe nats-server)
    endif()
    set(_name "nats-server-v${NATS_SERVER_PIN_VERSION}-${_platform}")
    set(_sha256 "${NATS_SERVER_PIN_SHA256_${_platform}}")
    set(_dir "${CMAKE_BINARY_DIR}/nats-server")
    set(_binary "${_dir}/${_exe}")
    set(_stamp "${_dir}/.stamp")

    # Already there from an earlier configure: also works offline.
    if(EXISTS "${_binary}" AND EXISTS "${_stamp}")
        file(READ "${_stamp}" _previous)
        if(_previous STREQUAL _sha256)
            set(LOGSQUIRL_NATS_TEST_SERVER "${_binary}" PARENT_SCOPE)
            return()
        endif()
    endif()

    if(NOT NATS_SERVER_DOWNLOAD)
        set(LOGSQUIRL_NATS_TEST_SERVER_UNAVAILABLE
            "the nats-server download is switched off (NATS_SERVER_DOWNLOAD=OFF) and NATS_SERVER_EXECUTABLE is not set"
            PARENT_SCOPE)
        return()
    endif()

    set(_url "${NATS_SERVER_RELEASES_URL}/v${NATS_SERVER_PIN_VERSION}/${_name}.${_archive_ext}")
    set(_archive "${CMAKE_BINARY_DIR}/${_name}.${_archive_ext}")
    message(STATUS "Downloading nats-server ${NATS_SERVER_PIN_VERSION} for the tests")
    file(DOWNLOAD "${_url}" "${_archive}"
        STATUS _status
        TLS_VERIFY ON
        INACTIVITY_TIMEOUT 30
        TIMEOUT 300
    )
    list(GET _status 0 _code)
    if(NOT _code EQUAL 0)
        list(GET _status 1 _message)
        file(REMOVE "${_archive}")
        set(_reason "downloading ${_url} failed: ${_message}")
        message(WARNING "No nats-server for the tests (${_reason}); the server tests will be skipped")
        set(LOGSQUIRL_NATS_TEST_SERVER_UNAVAILABLE "${_reason}" PARENT_SCOPE)
        return()
    endif()

    file(SHA256 "${_archive}" _actual)
    if(NOT _actual STREQUAL _sha256)
        file(REMOVE "${_archive}")
        message(FATAL_ERROR "${_url} has SHA-256 ${_actual}, expected ${_sha256} "
                            "(cmake/Dependencies.cmake)")
    endif()

    set(_unpack "${CMAKE_BINARY_DIR}/nats-server-unpack")
    file(REMOVE_RECURSE "${_unpack}" "${_dir}")
    file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_unpack}")
    if(NOT EXISTS "${_unpack}/${_name}/${_exe}")
        message(FATAL_ERROR "${_url} holds no ${_name}/${_exe}")
    endif()
    file(MAKE_DIRECTORY "${_dir}")
    file(COPY "${_unpack}/${_name}/${_exe}" DESTINATION "${_dir}"
         FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
                          WORLD_READ WORLD_EXECUTE)
    file(REMOVE_RECURSE "${_unpack}")
    file(REMOVE "${_archive}")
    file(WRITE "${_stamp}" "${_sha256}")
    set(LOGSQUIRL_NATS_TEST_SERVER "${_binary}" PARENT_SCOPE)
endfunction()

_nats_server_provide()

if(LOGSQUIRL_NATS_TEST_SERVER)
    message(STATUS "nats-server for the tests: ${LOGSQUIRL_NATS_TEST_SERVER}")
elseif(NOT "$ENV{CI}" STREQUAL "" AND NOT "$ENV{CI}" MATCHES "^(0|[Ff][Aa][Ll][Ss][Ee]|[Nn][Oo]|[Oo][Ff][Ff])$")
    # In CI a skip would pass the build with the server tests (the round
    # trip, TLS) never run.
    message(FATAL_ERROR "No nats-server for the tests, and CI is set: "
                        "${LOGSQUIRL_NATS_TEST_SERVER_UNAVAILABLE}")
else()
    message(STATUS "No nats-server for the tests: ${LOGSQUIRL_NATS_TEST_SERVER_UNAVAILABLE}")
endif()
