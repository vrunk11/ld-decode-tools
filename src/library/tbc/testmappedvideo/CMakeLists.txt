add_executable(testmappedvideo
    testmappedvideo.cpp
)

target_link_libraries(testmappedvideo PRIVATE Qt::Core lddecode-library)

add_test(NAME testmappedvideo COMMAND testmappedvideo)
set_tests_properties(testmappedvideo PROPERTIES LABELS unit)
