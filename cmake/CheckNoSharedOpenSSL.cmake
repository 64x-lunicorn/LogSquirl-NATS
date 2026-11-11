# ============================================================================
# Fails if a binary depends on an OpenSSL shared library (libssl, libcrypto).
#
#   cmake -DBINARY=<file> -DTOOL=<otool|objdump|dumpbin> -P CheckNoSharedOpenSSL.cmake
#
# The ctest no_shared_openssl runs it on the plugin library: OpenSSL must be
# linked in statically, so the package carries no OpenSSL and never picks up
# whichever one a user machine has.
# ============================================================================
if(NOT BINARY OR NOT TOOL)
    message(FATAL_ERROR "usage: cmake -DBINARY=<file> -DTOOL=<tool> -P ${CMAKE_SCRIPT_MODE_FILE}")
endif()

get_filename_component(_tool_name "${TOOL}" NAME_WE)
string(TOLOWER "${_tool_name}" _tool_name)
# What each tool lists, and a dependency every such binary has: if that is
# missing, the listing was not understood, and the check would pass blindly.
if(_tool_name STREQUAL "otool")
    set(_args -L)
    set(_always "libsystem")
elseif(_tool_name MATCHES "objdump$")
    set(_args -p)
    set(_always "needed")
elseif(_tool_name STREQUAL "dumpbin")
    set(_args -nologo -dependents)
    set(_always "kernel32.dll")
else()
    message(FATAL_ERROR "unknown tool ${TOOL}")
endif()

execute_process(
    COMMAND "${TOOL}" ${_args} "${BINARY}"
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE  _err
    RESULT_VARIABLE _rc
)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "${TOOL} ${_args} ${BINARY} failed (${_rc}):\n${_err}")
endif()
message("${_out}")

string(TOLOWER "${_out}" _lower)
if(NOT _lower MATCHES "${_always}")
    message(FATAL_ERROR "the output of ${TOOL} lists no ${_always}: not understood")
endif()
# libssl.3.dylib, libcrypto.so.3, libssl-3-x64.dll, ...
if(_lower MATCHES "lib(ssl|crypto)[-_.0-9a-z]*\\.(dylib|so|dll)")
    message(FATAL_ERROR "${BINARY} depends on the OpenSSL shared library ${CMAKE_MATCH_0}")
endif()
message("${BINARY}: no OpenSSL shared library")
