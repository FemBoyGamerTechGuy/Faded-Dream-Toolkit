add_executable(fdk_showcase showcase.c)
target_link_libraries(fdk_showcase PRIVATE fdk)
target_include_directories(fdk_showcase PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
)
