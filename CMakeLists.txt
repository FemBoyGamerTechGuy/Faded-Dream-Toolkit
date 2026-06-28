cmake_minimum_required(VERSION 3.16)
project(fdk VERSION 0.0.1 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ── Options ───────────────────────────────────────────────────────────────────
option(FDK_ENABLE_X11      "Build X11 platform backend"              ON)
option(FDK_ENABLE_WAYLAND  "Build Wayland platform backend"          ON)
option(FDK_ENABLE_OPENGL   "Build OpenGL render backend"             ON)
option(FDK_BUILD_SHARED    "Build libfdk.so alongside libfdk.a"      ON)
option(FDK_BUILD_EXAMPLES  "Build example applications"              ON)
option(FDK_BUILD_STATIC    "Build libfdk.a static library"           ON)
option(FDK_WITH_STB_IMAGE  "Enable PNG/JPEG loading via stb_image.h" OFF)

# ── Dependencies ──────────────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
find_package(Freetype  REQUIRED)

set(FDK_SOURCES
    src/core/core.c
    src/core/tween.c
    src/core/theme.c
    src/render/software.c
    src/widgets/widget.c
)
set(FDK_COMPILE_DEFS "")
set(FDK_LINK_LIBS    Freetype::Freetype m pthread)
set(FDK_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(FDK_EXTRA_TARGETS "")

# Pkg-config requires a flat list of -l flags for the .pc file
set(FDK_PC_LIBS      "-lm -lfreetype")
set(FDK_PC_REQUIRES  "freetype2")

# ── stb_image ─────────────────────────────────────────────────────────────────
if(FDK_WITH_STB_IMAGE)
    # The caller must make stb_image.h available on the include path.
    # A typical setup:  -DFDK_WITH_STB_IMAGE=ON -DFDK_STB_DIR=/path/to/stb
    if(DEFINED FDK_STB_DIR)
        list(APPEND FDK_INCLUDE_DIRS ${FDK_STB_DIR})
    endif()
    list(APPEND FDK_COMPILE_DEFS FDK_WITH_STB_IMAGE)
    message(STATUS "FDK: stb_image PNG/JPEG loader enabled")
endif()

# ── X11 ───────────────────────────────────────────────────────────────────────
if(FDK_ENABLE_X11)
    find_package(X11)
    if(X11_FOUND)
        list(APPEND FDK_SOURCES      src/platform/x11.c)
        list(APPEND FDK_COMPILE_DEFS FDK_HAVE_X11)
        list(APPEND FDK_LINK_LIBS    ${X11_LIBRARIES})
        list(APPEND FDK_INCLUDE_DIRS ${X11_INCLUDE_DIR})
        string(APPEND FDK_PC_LIBS    " -lX11")
        pkg_check_modules(XCURSOR xcursor)
        if(XCURSOR_FOUND)
            list(APPEND FDK_LINK_LIBS    ${XCURSOR_LIBRARIES})
            list(APPEND FDK_INCLUDE_DIRS ${XCURSOR_INCLUDE_DIRS})
            string(APPEND FDK_PC_LIBS    " -lXcursor")
        endif()
        message(STATUS "FDK: X11 backend enabled")
    else()
        message(WARNING "FDK: X11 not found — skipping")
    endif()
endif()

# ── Wayland ───────────────────────────────────────────────────────────────────
if(FDK_ENABLE_WAYLAND)
    pkg_check_modules(WAYLAND       wayland-client)
    pkg_check_modules(WAYLAND_PROTO wayland-protocols)
    pkg_check_modules(XKBCOMMON     xkbcommon)
    pkg_check_modules(WAYLAND_CURSOR wayland-cursor)
    find_program(WAYLAND_SCANNER    wayland-scanner)

    if(WAYLAND_FOUND AND WAYLAND_PROTO_FOUND AND WAYLAND_SCANNER)
        execute_process(
            COMMAND pkg-config --variable=pkgdatadir wayland-protocols
            OUTPUT_VARIABLE WP_DIR OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        set(XDG_XML "${WP_DIR}/stable/xdg-shell/xdg-shell.xml")
        set(XDG_H   "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol.h")
        set(XDG_C   "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c")

        add_custom_command(
            OUTPUT  ${XDG_H}
            COMMAND ${WAYLAND_SCANNER} client-header ${XDG_XML} ${XDG_H}
            DEPENDS ${XDG_XML}
            COMMENT "wayland-scanner: xdg-shell client header"
        )
        add_custom_command(
            OUTPUT  ${XDG_C}
            COMMAND ${WAYLAND_SCANNER} private-code ${XDG_XML} ${XDG_C}
            DEPENDS ${XDG_XML}
            COMMENT "wayland-scanner: xdg-shell private code"
        )

        add_custom_target(fdk_xdg_shell_gen DEPENDS ${XDG_H} ${XDG_C})
        list(APPEND FDK_EXTRA_TARGETS fdk_xdg_shell_gen)

        list(APPEND FDK_SOURCES      src/platform/wayland.c ${XDG_C})
        list(APPEND FDK_COMPILE_DEFS FDK_HAVE_WAYLAND)
        list(APPEND FDK_LINK_LIBS    ${WAYLAND_LIBRARIES})
        list(APPEND FDK_INCLUDE_DIRS
            ${WAYLAND_INCLUDE_DIRS}
            ${CMAKE_CURRENT_BINARY_DIR}
        )
        string(APPEND FDK_PC_REQUIRES " wayland-client")

        if(XKBCOMMON_FOUND)
            list(APPEND FDK_LINK_LIBS    ${XKBCOMMON_LIBRARIES})
            list(APPEND FDK_INCLUDE_DIRS ${XKBCOMMON_INCLUDE_DIRS})
            string(APPEND FDK_PC_REQUIRES " xkbcommon")
        endif()
        if(WAYLAND_CURSOR_FOUND)
            list(APPEND FDK_LINK_LIBS    ${WAYLAND_CURSOR_LIBRARIES})
            list(APPEND FDK_INCLUDE_DIRS ${WAYLAND_CURSOR_INCLUDE_DIRS})
            string(APPEND FDK_PC_REQUIRES " wayland-cursor")
        endif()

        message(STATUS "FDK: Wayland backend enabled")
    else()
        message(WARNING "FDK: Wayland disabled (need wayland-client, wayland-protocols, wayland-scanner)")
    endif()
endif()

# ── OpenGL ────────────────────────────────────────────────────────────────────
if(FDK_ENABLE_OPENGL)
    find_package(OpenGL)
    pkg_check_modules(EGL        egl)
    pkg_check_modules(WAYLAND_EGL wayland-egl)
    if(OpenGL_FOUND)
        list(APPEND FDK_SOURCES      src/render/opengl.c)
        list(APPEND FDK_COMPILE_DEFS FDK_HAVE_OPENGL)
        list(APPEND FDK_LINK_LIBS    OpenGL::GL)
        string(APPEND FDK_PC_LIBS    " -lGL")
        if(EGL_FOUND)
            list(APPEND FDK_LINK_LIBS    ${EGL_LIBRARIES})
            list(APPEND FDK_INCLUDE_DIRS ${EGL_INCLUDE_DIRS})
            string(APPEND FDK_PC_REQUIRES " egl")
        endif()
        if(WAYLAND_EGL_FOUND)
            list(APPEND FDK_LINK_LIBS    ${WAYLAND_EGL_LIBRARIES})
            list(APPEND FDK_INCLUDE_DIRS ${WAYLAND_EGL_INCLUDE_DIRS})
            string(APPEND FDK_PC_REQUIRES " wayland-egl")
        endif()
        message(STATUS "FDK: OpenGL backend enabled")
    else()
        message(WARNING "FDK: OpenGL not found — skipping")
    endif()
endif()

# ── Shared library macro (avoids repeating target setup) ──────────────────────
# We use a helper function so both targets share identical settings.
function(fdk_configure_target target_name)
    if(FDK_EXTRA_TARGETS)
        add_dependencies(${target_name} ${FDK_EXTRA_TARGETS})
    endif()
    target_include_directories(${target_name}
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:include>
        PRIVATE ${FDK_INCLUDE_DIRS}
                ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(${target_name} PRIVATE ${FDK_COMPILE_DEFS})
    target_link_libraries(${target_name} PUBLIC ${FDK_LINK_LIBS})
    target_compile_options(${target_name} PRIVATE
        -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
        $<$<CONFIG:Release>:-O2>
        $<$<CONFIG:Debug>:-g -O0>
    )
    # Shared libraries need -fPIC; static can use it too with no cost
    set_target_properties(${target_name} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
    )
endfunction()

# ── Static library ────────────────────────────────────────────────────────────
if(FDK_BUILD_STATIC)
    add_library(fdk_static STATIC ${FDK_SOURCES})
    set_target_properties(fdk_static PROPERTIES OUTPUT_NAME fdk)
    fdk_configure_target(fdk_static)
    # Alias so existing code using target 'fdk' still works
    add_library(fdk::fdk_static ALIAS fdk_static)
endif()

# ── Shared library ────────────────────────────────────────────────────────────
if(FDK_BUILD_SHARED)
    add_library(fdk_shared SHARED ${FDK_SOURCES})
    set_target_properties(fdk_shared PROPERTIES
        OUTPUT_NAME   fdk
        # SO versioning: libfdk.so.0.3.4  →  libfdk.so.0  →  libfdk.so
        VERSION       ${PROJECT_VERSION}
        SOVERSION     ${PROJECT_VERSION_MAJOR}
    )
    fdk_configure_target(fdk_shared)
    add_library(fdk::fdk ALIAS fdk_shared)
endif()

# Convenience alias 'fdk' — prefers shared, falls back to static
if(FDK_BUILD_SHARED)
    add_library(fdk ALIAS fdk_shared)
elseif(FDK_BUILD_STATIC)
    add_library(fdk ALIAS fdk_static)
endif()

# ── pkg-config file ───────────────────────────────────────────────────────────
# Generated from a template so paths are correct after install
set(FDK_PC_PREFIX      "${CMAKE_INSTALL_PREFIX}")
set(FDK_PC_INCLUDEDIR  "${CMAKE_INSTALL_PREFIX}/include")
set(FDK_PC_LIBDIR      "${CMAKE_INSTALL_PREFIX}/lib")
set(FDK_PC_VERSION     "${PROJECT_VERSION}")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/fdk.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/fdk.pc"
    @ONLY
)

# ── CMake package config ──────────────────────────────────────────────────────
include(CMakePackageConfigHelpers)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FDKConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/FDKConfig.cmake"
    INSTALL_DESTINATION lib/cmake/FDK
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/FDKConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

# ── Install ───────────────────────────────────────────────────────────────────
include(GNUInstallDirs)

# Headers
install(DIRECTORY include/fdk
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Libraries
if(FDK_BUILD_STATIC)
    install(TARGETS fdk_static
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()
if(FDK_BUILD_SHARED)
    install(TARGETS fdk_shared
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            NAMELINK_COMPONENT devel   # libfdk.so symlink → devel component
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

# pkg-config
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/fdk.pc"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)

# CMake package config
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/FDKConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/FDKConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FDK
)

# ── Examples ──────────────────────────────────────────────────────────────────
if(FDK_BUILD_EXAMPLES)
    add_subdirectory(examples/hello)
    add_subdirectory(examples/widgets)
    add_subdirectory(examples/showcase)
    add_subdirectory(examples/theme-switcher)
    add_subdirectory(examples/systemwide-test)
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
option(FDK_BUILD_TESTS "Build headless test binaries" ON)
if(FDK_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# ── Tools ─────────────────────────────────────────────────────────────────────
option(FDK_BUILD_TOOLS "Build companion CLI tools (fdk-theme)" ON)
if(FDK_BUILD_TOOLS)
    add_subdirectory(tools/fdk-theme)
endif()
