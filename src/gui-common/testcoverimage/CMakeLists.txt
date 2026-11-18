add_executable(testcoverimage
    testcoverimage.cpp
)

target_link_libraries(testcoverimage PRIVATE lddecode-gui)

add_test(NAME testcoverimage COMMAND testcoverimage)
set_tests_properties(testcoverimage PROPERTIES LABELS unit)
