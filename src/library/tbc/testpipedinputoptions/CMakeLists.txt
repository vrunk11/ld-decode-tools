add_executable(testpipedinputoptions
    testpipedinputoptions.cpp
)

target_link_libraries(testpipedinputoptions PRIVATE Qt::Core lddecode-library)

add_test(NAME testpipedinputoptions COMMAND testpipedinputoptions)
set_tests_properties(testpipedinputoptions PROPERTIES LABELS unit)
