# ============================================================================
# OpenSSL, built from the pinned source tarball at configure time.
#
# Static libraries only, the same on every platform (also on Linux: no system
# OpenSSL, so the package runs on every distribution).  They end up inside
# logsquirl_nats.  Defines the imported targets OpenSSL::SSL and
# OpenSSL::Crypto, and the cache variables FindOpenSSL reads, so that nats.c's
# find_package(OpenSSL) takes this build and nothing installed on the machine.
#
# Built at configure time, not as an ExternalProject, because nats.c's
# CMakeLists needs the headers and libraries when it is configured.  A stamp
# keyed on the version, hash, options and compiler skips the build when
# nothing changed.
#
# Needs Perl (and on Windows a native Perl such as Strawberry Perl, not the
# MSYS perl of Git for Windows, plus nmake from the MSVC environment).
# ============================================================================
include(FetchContent)

FetchContent_Declare(openssl_source
    URL      "${OPENSSL_PIN_URL}"
    URL_HASH "SHA256=${OPENSSL_PIN_SHA256}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# No CMakeLists.txt in it: this only downloads, verifies and unpacks.
FetchContent_MakeAvailable(openssl_source)

set(_ossl_build   "${CMAKE_BINARY_DIR}/openssl-build")
set(_ossl_prefix  "${CMAKE_BINARY_DIR}/openssl")
set(_ossl_stamp   "${_ossl_prefix}/.logsquirl-nats-stamp")

# ── Perl, make ────────────────────────────────────────────────────────────
if(WIN32)
    # Git for Windows puts its MSYS perl on the PATH; OpenSSL's VC build
    # needs a native one, which the runners carry as Strawberry Perl.
    find_program(OPENSSL_BUILD_PERL perl
        HINTS "C:/Strawberry/perl/bin" "$ENV{SystemDrive}/Strawberry/perl/bin"
        NO_DEFAULT_PATH)
endif()
find_program(OPENSSL_BUILD_PERL perl REQUIRED)
if(WIN32 AND OPENSSL_BUILD_PERL MATCHES "/(usr|Git)/")
    message(FATAL_ERROR "OpenSSL needs a native Perl on Windows (e.g. Strawberry Perl), "
                        "not ${OPENSSL_BUILD_PERL}; set OPENSSL_BUILD_PERL")
endif()

if(WIN32)
    find_program(OPENSSL_BUILD_MAKE nmake REQUIRED)
    set(_ossl_make "${OPENSSL_BUILD_MAKE}")
else()
    find_program(OPENSSL_BUILD_MAKE NAMES gmake make REQUIRED)
    cmake_host_system_information(RESULT _ossl_jobs QUERY NUMBER_OF_LOGICAL_CORES)
    set(_ossl_make "${OPENSSL_BUILD_MAKE}" "-j${_ossl_jobs}")
endif()

# ── Configure options ─────────────────────────────────────────────────────
# Only what a TLS client needs: no shared libraries, no loadable modules or
# engines, no programs, tests or documentation, and no automatic loading of
# a system openssl.cnf (whose contents would differ from machine to machine).
set(_ossl_options
    no-shared no-module no-engine no-legacy no-apps no-tests no-docs no-quic
    no-autoload-config
    --libdir=lib
    "--prefix=${_ossl_prefix}"
)

if(WIN32)
    # no-asm: no NASM needed.
    list(APPEND _ossl_options VC-WIN64A no-asm)
    # Where the default trust store would be; the plugin never relies on it.
    list(APPEND _ossl_options "--openssldir=${_ossl_prefix}/ssl")
    set(_ossl_libs "${_ossl_prefix}/lib/libssl.lib" "${_ossl_prefix}/lib/libcrypto.lib")
else()
    # The system's certificate directory, so a later default trust store
    # finds the CA certificates (/etc/ssl/certs on Debian-like Linux,
    # /etc/ssl/cert.pem on macOS).  Never a path of the build machine.
    list(APPEND _ossl_options "--openssldir=/etc/ssl" -fPIC)
    if(APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET)
        list(APPEND _ossl_options "-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()
    set(_ossl_libs "${_ossl_prefix}/lib/libssl.a" "${_ossl_prefix}/lib/libcrypto.a")
endif()

set(_ossl_signature "${OPENSSL_PIN_VERSION};${OPENSSL_PIN_SHA256};${_ossl_options};${CMAKE_C_COMPILER}")

set(_ossl_up_to_date FALSE)
if(EXISTS "${_ossl_stamp}")
    file(READ "${_ossl_stamp}" _ossl_previous)
    set(_ossl_up_to_date TRUE)
    if(NOT _ossl_previous STREQUAL _ossl_signature)
        set(_ossl_up_to_date FALSE)
    endif()
    foreach(_lib IN LISTS _ossl_libs)
        if(NOT EXISTS "${_lib}")
            set(_ossl_up_to_date FALSE)
        endif()
    endforeach()
endif()

# Run one build step, its output into a log file; on failure, show the end
# of the log.
function(_openssl_step name)
    set(_log "${_ossl_build}/${name}.log")
    execute_process(
        COMMAND ${ARGN}
        WORKING_DIRECTORY "${_ossl_build}"
        OUTPUT_FILE "${_log}"
        ERROR_FILE  "${_log}"
        RESULT_VARIABLE _rc
    )
    if(NOT _rc EQUAL 0)
        file(READ "${_log}" _text)
        string(LENGTH "${_text}" _len)
        if(_len GREATER 6000)
            math(EXPR _start "${_len} - 6000")
            string(SUBSTRING "${_text}" ${_start} -1 _text)
        endif()
        message(FATAL_ERROR "OpenSSL ${name} failed (${_rc}); full log: ${_log}\n…${_text}")
    endif()
endfunction()

if(NOT _ossl_up_to_date)
    message(STATUS "Building OpenSSL ${OPENSSL_PIN_VERSION} (static) — this takes a few minutes")
    file(REMOVE_RECURSE "${_ossl_build}" "${_ossl_prefix}")
    file(MAKE_DIRECTORY "${_ossl_build}")

    if(NOT WIN32)
        # The same C compiler as the rest of the build.
        set(ENV{CC} "${CMAKE_C_COMPILER}")
    endif()
    _openssl_step(configure "${OPENSSL_BUILD_PERL}" "${openssl_source_SOURCE_DIR}/Configure" ${_ossl_options})
    _openssl_step(build ${_ossl_make} build_libs)
    _openssl_step(install ${_ossl_make} install_dev)

    file(WRITE "${_ossl_stamp}" "${_ossl_signature}")
    message(STATUS "Building OpenSSL ${OPENSSL_PIN_VERSION} (static) — done")
endif()

# ── Imported targets ──────────────────────────────────────────────────────
list(GET _ossl_libs 0 _ossl_ssl_lib)
list(GET _ossl_libs 1 _ossl_crypto_lib)
set(_ossl_include "${_ossl_prefix}/include")

find_package(Threads REQUIRED)

add_library(OpenSSL::Crypto STATIC IMPORTED GLOBAL)
set_target_properties(OpenSSL::Crypto PROPERTIES
    IMPORTED_LOCATION "${_ossl_crypto_lib}"
    INTERFACE_INCLUDE_DIRECTORIES "${_ossl_include}"
)
if(WIN32)
    set_property(TARGET OpenSSL::Crypto PROPERTY
        INTERFACE_LINK_LIBRARIES ws2_32 gdi32 advapi32 crypt32 user32)
else()
    set_property(TARGET OpenSSL::Crypto PROPERTY
        INTERFACE_LINK_LIBRARIES Threads::Threads ${CMAKE_DL_LIBS})
endif()

add_library(OpenSSL::SSL STATIC IMPORTED GLOBAL)
set_target_properties(OpenSSL::SSL PROPERTIES
    IMPORTED_LOCATION "${_ossl_ssl_lib}"
    INTERFACE_INCLUDE_DIRECTORIES "${_ossl_include}"
    INTERFACE_LINK_LIBRARIES OpenSSL::Crypto
)

# What FindOpenSSL looks for, pointing at this build: nats.c's
# find_package(OpenSSL) then finds it and nothing else.  (LIB_EAY_* and
# SSL_EAY_* are FindOpenSSL's names under MSVC.)
set(OPENSSL_ROOT_DIR       "${_ossl_prefix}"     CACHE PATH     "" FORCE)
set(OPENSSL_INCLUDE_DIR    "${_ossl_include}"    CACHE PATH     "" FORCE)
set(OPENSSL_CRYPTO_LIBRARY "${_ossl_crypto_lib}" CACHE FILEPATH "" FORCE)
set(OPENSSL_SSL_LIBRARY    "${_ossl_ssl_lib}"    CACHE FILEPATH "" FORCE)
foreach(_cfg DEBUG RELEASE)
    set(LIB_EAY_${_cfg} "${_ossl_crypto_lib}" CACHE FILEPATH "" FORCE)
    set(SSL_EAY_${_cfg} "${_ossl_ssl_lib}"    CACHE FILEPATH "" FORCE)
endforeach()
set(OPENSSL_USE_STATIC_LIBS TRUE)

find_package(OpenSSL ${OPENSSL_PIN_VERSION} EXACT REQUIRED)
