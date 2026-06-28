add_executable(fdk_hello hello.c)

# Link against whichever fdk target was built
# If installed system-wide, apps would instead do:
#   find_package(FDK REQUIRED)
#   target_link_libraries(myapp PRIVATE fdk::fdk)
target_link_libraries(fdk_hello PRIVATE fdk)

target_include_directories(fdk_hello PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
)
