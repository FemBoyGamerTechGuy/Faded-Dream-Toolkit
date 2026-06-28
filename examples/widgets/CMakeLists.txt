add_executable(fdk_widgets widgets.c)
target_link_libraries(fdk_widgets PRIVATE fdk)
target_include_directories(fdk_widgets PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
)
