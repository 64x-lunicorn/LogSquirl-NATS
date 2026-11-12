# ============================================================================
# logsquirl-nats — NATS Monitor plugin for LogSquirl
#
# A standalone CMake project.  It does NOT depend on the LogSquirl build
# tree — only on the vendored logsquirl_plugin_api.h header (MIT).
#
# Build:
#   cmake -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
#   cmake --build build
#   ctest --test-dir build --output-on-failure
#
# The output is a shared library (liblogsquirl_nats.dylib / .so /
# logsquirl_nats.dll) with nats.c and OpenSSL linked in statically: the
# package carries no other library.
# ============================================================================
cmake_minimum_required(VERSION 3.28)

project(logsquirl_nats
    VERSION   0.1.0
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json for tooling
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The static libraries linked into the plugin must be position independent.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# The version logsquirl_plugin_get_info() reports: project(VERSION), which CI
# requires to equal plugin.json's version.
add_compile_definitions(LOGSQUIRL_PLUGIN_VERSION="${PROJECT_VERSION}")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Dependencies)   # pinned versions and SHA-256s
include(OpenSSL)        # OpenSSL::SSL, OpenSSL::Crypto (static, built here)
include(NatsC)          # nats_static, built against them

# ── Find Qt6 ──────────────────────────────────────────────────────────────
find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
qt_standard_project_setup()

# ── Plugin shared library ─────────────────────────────────────────────────
set(LOGSQUIRL_NATS_SOURCES
    src/plugin.cpp
    src/connectiondialog.cpp
    src/connectiontile.cpp
    src/lineformat.cpp
    src/natsoptions.cpp
    src/natssession.cpp
    src/nkey.cpp
    src/profiles.cpp
    src/sessions.cpp
    src/sidebarwidget.cpp
    src/subjects.cpp
    src/tempdirs.cpp
)

# What the plugin's sources need to compile and link: shared by the plugin
# library and the test program, which compiles the same sources.
add_library(logsquirl_nats_common INTERFACE)
target_include_directories(logsquirl_nats_common
    INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"   # Vendored SDK header
        "${CMAKE_CURRENT_SOURCE_DIR}/src"       # Internal headers
)
target_link_libraries(logsquirl_nats_common
    INTERFACE
        Qt6::Core
        Qt6::Widgets
        nats_static
        OpenSSL::Crypto
)
# nats.h pulls in <windows.h>, whose min/max macros break std::min/std::max
# and std::numeric_limits<T>::max() in every source that includes it.
if(WIN32)
    target_compile_definitions(logsquirl_nats_common INTERFACE NOMINMAX)
endif()

add_library(logsquirl_nats SHARED ${LOGSQUIRL_NATS_SOURCES})

set_target_properties(logsquirl_nats PROPERTIES
    AUTOMOC ON
    # CMake chooses the platform's prefix and suffix; plugin.json names the
    # library without them, and QLibrary resolves the platform's name.
)

# Default symbol visibility: hidden.  Only LOGSQUIRL_PLUGIN_EXPORT symbols
# are visible (matches the __attribute__((visibility("default"))) macro).
if(NOT WIN32)
    target_compile_options(logsquirl_nats PRIVATE -fvisibility=hidden)
endif()

# -fvisibility=hidden does not reach the static OpenSSL and nats.c: export
# only the plugin's entry points.  On Linux this matters beyond tidiness: the
# host process may already have loaded the system's libssl (Qt's TLS backend
# does), and symbols kept local can neither bind to that OpenSSL nor have
# others bind to the plugin's copy.  (Windows exports only dllexport.)
if(APPLE)
    target_link_options(logsquirl_nats PRIVATE "LINKER:-exported_symbol,_logsquirl_plugin_*")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_exports "${CMAKE_CURRENT_BINARY_DIR}/exports.map")
    file(WRITE "${_exports}"
        "{ global: logsquirl_plugin_*; local: *; };\n")
    target_link_options(logsquirl_nats PRIVATE
        "LINKER:--exclude-libs,ALL"
        "LINKER:--version-script=${_exports}")
    set_property(TARGET logsquirl_nats APPEND PROPERTY LINK_DEPENDS "${_exports}")
endif()

target_link_libraries(logsquirl_nats PRIVATE logsquirl_nats_common)

# ── macOS rpath ───────────────────────────────────────────────────────────
# When loaded from Contents/PlugIns/<id>/, the plugin resolves Qt frameworks
# via @loader_path/../../Frameworks (→ Contents/Frameworks/).
if(APPLE)
    set_target_properties(logsquirl_nats PROPERTIES
        INSTALL_RPATH "@loader_path;@loader_path/../../Frameworks"
    )
endif()

# ── Install rule ──────────────────────────────────────────────────────────
include(GNUInstallDirs)

set(PLUGIN_INSTALL_DIR
    "${CMAKE_INSTALL_DATADIR}/LogSquirl/plugins/io.github.logsquirl.nats"
    CACHE PATH "Plugin installation directory"
)

install(TARGETS logsquirl_nats LIBRARY DESTINATION "${PLUGIN_INSTALL_DIR}"
                               RUNTIME DESTINATION "${PLUGIN_INSTALL_DIR}")
# The licence texts as the CI package carries them (package_files in
# .github/plugin-ci.json): the library contains Apache-2.0 code.
install(FILES   plugin.json icon.png LICENSE NOTICE LICENSES/Apache-2.0.txt
        DESTINATION "${PLUGIN_INSTALL_DIR}")
# The Log Format plugin.json names, beside it as in the CI package.
install(FILES   formats/nats_log.json DESTINATION "${PLUGIN_INSTALL_DIR}")

# ── Tests (opt-in) ────────────────────────────────────────────────────────
option(BUILD_TESTS "Build unit tests" OFF)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
