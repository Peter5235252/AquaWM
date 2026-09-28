cmake_minimum_required(VERSION 3.28)

project(aquawm VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(WLROOTS REQUIRED wlroots-0.20)
pkg_check_modules(WAYLAND_SERVER REQUIRED wayland-server)
pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)
pkg_check_modules(LUA REQUIRED lua)
pkg_check_modules(JPEG REQUIRED libjpeg)
pkg_check_modules(PNG REQUIRED libpng)
pkg_check_modules(DRM REQUIRED libdrm)
pkg_check_modules(XCB REQUIRED xcb xcb-ewmh xcb-icccm)
pkg_check_modules(FREETYPE REQUIRED freetype2)
pkg_check_modules(FONTCONFIG REQUIRED fontconfig)

# Layer-shell protocol (Phase 3b): wlroots' own layer-shell header includes
# the generated server header, which is not shipped in install prefixes
# (notably NixOS), so generate it at build time from the vendored XML
# (exact wlroots 0.20.2 copy in protocol/). Same result on Arch/Fedora.
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
set(LAYER_SHELL_XML ${CMAKE_CURRENT_SOURCE_DIR}/protocol/wlr-layer-shell-unstable-v1.xml)
set(LAYER_SHELL_HEADER ${CMAKE_CURRENT_BINARY_DIR}/protocol/wlr-layer-shell-unstable-v1-protocol.h)
add_custom_command(
  OUTPUT ${LAYER_SHELL_HEADER}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/protocol
  COMMAND ${WAYLAND_SCANNER} server-header ${LAYER_SHELL_XML} ${LAYER_SHELL_HEADER}
  DEPENDS ${LAYER_SHELL_XML}
  VERBATIM
)
add_custom_target(layer-shell-protocol DEPENDS ${LAYER_SHELL_HEADER})

add_executable(aquawm
  src/main.cpp
  src/config.cpp
  src/wallpaper.cpp
  src/xwayland_ffi.c
)

target_include_directories(aquawm PRIVATE
  ${WLROOTS_INCLUDE_DIRS}
  ${WAYLAND_SERVER_INCLUDE_DIRS}
  ${XKBCOMMON_INCLUDE_DIRS}
  ${LUA_INCLUDE_DIRS}
  ${JPEG_INCLUDE_DIRS}
  ${PNG_INCLUDE_DIRS}
  ${DRM_INCLUDE_DIRS}
  ${XCB_INCLUDE_DIRS}
  ${FREETYPE_INCLUDE_DIRS}
  ${FONTCONFIG_INCLUDE_DIRS}
  ${CMAKE_CURRENT_BINARY_DIR}/protocol
  src
)

target_link_directories(aquawm PRIVATE
  ${WLROOTS_LIBRARY_DIRS}
  ${WAYLAND_SERVER_LIBRARY_DIRS}
  ${XKBCOMMON_LIBRARY_DIRS}
  ${LUA_LIBRARY_DIRS}
  ${JPEG_LIBRARY_DIRS}
  ${PNG_LIBRARY_DIRS}
  ${DRM_LIBRARY_DIRS}
  ${FREETYPE_LIBRARY_DIRS}
  ${FONTCONFIG_LIBRARY_DIRS}
)

target_link_libraries(aquawm PRIVATE
  ${WLROOTS_LIBRARIES}
  ${WAYLAND_SERVER_LIBRARIES}
  ${XKBCOMMON_LIBRARIES}
  ${LUA_LIBRARIES}
  ${JPEG_LIBRARIES}
  ${PNG_LIBRARIES}
  ${DRM_LIBRARIES}
  ${FREETYPE_LIBRARIES}
  ${FONTCONFIG_LIBRARIES}
)

target_compile_options(aquawm PRIVATE
  -Wall -Wextra -Wpedantic
  ${WLROOTS_CFLAGS_OTHER}
  ${WAYLAND_SERVER_CFLAGS_OTHER}
)

# wlroots gates all of its headers behind this macro.
target_compile_definitions(aquawm PRIVATE WLR_USE_UNSTABLE)

# The generated layer-shell protocol header must exist before compiling.
add_dependencies(aquawm layer-shell-protocol)

# Pure-logic unit test for the tiling layout (no wlroots dependency).
add_executable(test_tiling
  tests/test_tiling.cpp
)
target_include_directories(test_tiling PRIVATE src)
target_compile_options(test_tiling PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_test(NAME tiling COMMAND test_tiling)

# Config module test (Lua + xkbcommon, no wlroots needed).
add_executable(test_config
  tests/test_config.cpp
  src/config.cpp
)
target_include_directories(test_config PRIVATE
  src
  ${LUA_INCLUDE_DIRS}
  ${XKBCOMMON_INCLUDE_DIRS}
)
target_link_directories(test_config PRIVATE
  ${LUA_LIBRARY_DIRS}
  ${XKBCOMMON_LIBRARY_DIRS}
)
target_link_libraries(test_config PRIVATE
  ${LUA_LIBRARIES}
  ${XKBCOMMON_LIBRARIES}
)
target_compile_options(test_config PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME config COMMAND test_config)

# Wallpaper decode test (libjpeg + libpng, no wlroots needed).
add_executable(test_wallpaper
  tests/test_wallpaper.cpp
  src/wallpaper.cpp
)
target_include_directories(test_wallpaper PRIVATE
  src
  ${JPEG_INCLUDE_DIRS}
  ${PNG_INCLUDE_DIRS}
)
target_link_directories(test_wallpaper PRIVATE
  ${JPEG_LIBRARY_DIRS}
  ${PNG_LIBRARY_DIRS}
)
target_link_libraries(test_wallpaper PRIVATE
  ${JPEG_LIBRARIES}
  ${PNG_LIBRARIES}
)
target_compile_options(test_wallpaper PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME wallpaper COMMAND test_wallpaper)

# Exclusive-zone geometry test (pure logic, no wlroots dependency).
add_executable(test_layers
  tests/test_layers.cpp
)
target_include_directories(test_layers PRIVATE src)
target_compile_options(test_layers PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME layers COMMAND test_layers)

# Tiling layout geometry test (pure logic, no wlroots dependency).
add_executable(test_layouts
  tests/test_layouts.cpp
)
target_include_directories(test_layouts PRIVATE src)
target_compile_options(test_layouts PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME layouts COMMAND test_layouts)

# Window-rule matcher test (pure logic, no wlroots dependency).
add_executable(test_rules
  tests/test_rules.cpp
)
target_include_directories(test_rules PRIVATE src)
target_compile_options(test_rules PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME rules COMMAND test_rules)

# Warning-bar message/layout test (pure logic, no dependencies).
add_executable(test_warnbar
  tests/test_warnbar.cpp
)
target_include_directories(test_warnbar PRIVATE src)
target_compile_options(test_warnbar PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME warnbar COMMAND test_warnbar)

include(GNUInstallDirs)
install(TARGETS aquawm RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS sessions/aquawm-session DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES sessions/aquawm.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/wayland-sessions)
