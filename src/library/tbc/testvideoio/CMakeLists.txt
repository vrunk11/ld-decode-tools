add_executable(testvideoio
    testvideoio.cpp
)

target_link_libraries(testvideoio PRIVATE Qt::Core lddecode-library)

add_test(NAME testvideoio COMMAND testvideoio)
set_tests_properties(testvideoio PROPERTIES LABELS unit)
