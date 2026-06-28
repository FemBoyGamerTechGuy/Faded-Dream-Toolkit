# Headless tests — no window, no compositor required.
# Run with: ctest --output-on-failure   (or just execute the binary directly)

add_executable(test_theme test_theme.c)
target_link_libraries(test_theme PRIVATE fdk)
target_include_directories(test_theme PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
)

# test_theme looks for "themes/<name>.fdktheme" relative to its CWD,
# falling back to "../themes/<name>.fdktheme". Running it from the build
# directory (where this target lives) makes the fallback path resolve to
# ${CMAKE_SOURCE_DIR}/themes via the source tree symlink/copy below.
#
# Simplest: just copy the bundled themes next to the test binary so the
# primary "themes/<name>.fdktheme" lookup succeeds with zero path guessing.
add_custom_command(TARGET test_theme POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:test_theme>/themes
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/themes
            $<TARGET_FILE_DIR:test_theme>/themes
    COMMENT "Copying bundled .fdktheme files next to test_theme"
)

enable_testing()
add_test(NAME theme_parser COMMAND test_theme)
