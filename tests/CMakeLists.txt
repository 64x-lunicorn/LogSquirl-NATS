# ============================================================================
# Unit tests for logsquirl-nats
#
# Catch2 v2 (BDD style) to match the host project's test conventions.
#
# Two ctest entries run the one test program:
#   logsquirl_nats_tests         everything that needs no nats-server
#   logsquirl_nats_server_tests  the tests against a real nats-server
#                                ([server]); reported as skipped, with the
#                                reason in its output, when no server could
#                                be provided (see cmake/NatsServer.cmake)
#
# Neither runs the hidden tests ([.benchmark]): they are started by hand.
# ============================================================================

include(FetchContent)
FetchContent_Declare(Catch2
    URL      "${CATCH2_PIN_URL}"
    URL_HASH "SHA256=${CATCH2_PIN_SHA256}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(Catch2)

include(NatsServer)  # LOGSQUIRL_NATS_TEST_SERVER, LOGSQUIRL_NATS_TEST_SERVER_UNAVAILABLE

find_package(Qt6 REQUIRED COMPONENTS Network)

# ── Test executable ───────────────────────────────────────────────────────

add_executable(logsquirl_nats_tests
    tests_main.cpp
    natsserver.cpp
    plugininfo_test.cpp
    plugin_test.cpp
    library_test.cpp
    natsserver_test.cpp
    lineformat_test.cpp
    logformat_test.cpp
    natssession_test.cpp
)

set_target_properties(logsquirl_nats_tests PROPERTIES
    AUTOMOC ON
)


# testconfig.h: the paths and the reason as C string literals.  (The reason
# quotes CMake's error messages, which compile definitions would not survive.)
function(_c_string out value)
    string(REPLACE "\\" "\\\\" value "${value}")
    string(REPLACE "\"" "\\\"" value "${value}")
    string(REPLACE "\n" " " value "${value}")
    set(${out} "${value}" PARENT_SCOPE)
endfunction()
_c_string(TESTCONFIG_SERVER "${LOGSQUIRL_NATS_TEST_SERVER}")
_c_string(TESTCONFIG_SERVER_UNAVAILABLE "${LOGSQUIRL_NATS_TEST_SERVER_UNAVAILABLE}")
_c_string(TESTCONFIG_CERTS "${CMAKE_CURRENT_SOURCE_DIR}/certs")
_c_string(TESTCONFIG_SOURCE_DIR "${CMAKE_SOURCE_DIR}")
configure_file(testconfig.h.in "${CMAKE_CURRENT_BINARY_DIR}/testconfig.h" @ONLY)
target_include_directories(logsquirl_nats_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

# The built plugin library, which library_test.cpp loads as the host would.
add_dependencies(logsquirl_nats_tests logsquirl_nats)
target_compile_definitions(logsquirl_nats_tests
    PRIVATE LOGSQUIRL_NATS_LIBRARY="$<TARGET_FILE:logsquirl_nats>")

target_link_libraries(logsquirl_nats_tests
    PRIVATE
        logsquirl_nats_common   # the plugin's includes and libraries
        Catch2::Catch2
        Qt6::Network
)

# The plugin sources go into the test program, so tests reach the plugin's
# entry points and internals without loading the library.
list(TRANSFORM LOGSQUIRL_NATS_SOURCES PREPEND "${CMAKE_SOURCE_DIR}/" OUTPUT_VARIABLE _plugin_sources)
target_sources(logsquirl_nats_tests PRIVATE ${_plugin_sources})

# "~[.]": a filter of exclusions alone would also select the hidden tests,
# the opt-in benchmark ([.benchmark]) among them.
add_test(NAME logsquirl_nats_tests COMMAND logsquirl_nats_tests "~[server]~[.]")

# Exit code 77: every server test was skipped (tests_main.cpp).
add_test(NAME logsquirl_nats_server_tests COMMAND logsquirl_nats_tests "[server]")
set_tests_properties(logsquirl_nats_server_tests PROPERTIES SKIP_RETURN_CODE 77)

# ── The plugin library links no OpenSSL shared library ────────────────────
# With the platform's own tool; under MSVC dumpbin sits next to link.exe.
if(APPLE)
    find_program(DEPENDENCY_TOOL otool)
elseif(WIN32)
    get_filename_component(_linker_dir "${CMAKE_LINKER}" DIRECTORY)
    find_program(DEPENDENCY_TOOL dumpbin HINTS "${_linker_dir}")
else()
    find_program(DEPENDENCY_TOOL objdump HINTS "${CMAKE_OBJDUMP}")
endif()
if(DEPENDENCY_TOOL)
    add_test(NAME no_shared_openssl
        COMMAND "${CMAKE_COMMAND}" "-DBINARY=$<TARGET_FILE:logsquirl_nats>"
                "-DTOOL=${DEPENDENCY_TOOL}" -P "${CMAKE_SOURCE_DIR}/cmake/CheckNoSharedOpenSSL.cmake")
else()
    # Never a silent pass: without the tool the test fails, saying why.
    add_test(NAME no_shared_openssl
        COMMAND "${CMAKE_COMMAND}" -E echo "no otool, objdump or dumpbin to list the dependencies with")
    set_tests_properties(no_shared_openssl PROPERTIES WILL_FAIL TRUE)
endif()
