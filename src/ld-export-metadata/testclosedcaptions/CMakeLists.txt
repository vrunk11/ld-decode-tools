add_executable(testclosedcaptions
    testclosedcaptions.cpp
    ../closedcaptions.cpp
)

target_link_libraries(testclosedcaptions PRIVATE Qt::Core lddecode-library)
target_include_directories(testclosedcaptions PRIVATE ..)

add_test(NAME testclosedcaptions COMMAND testclosedcaptions)
set_tests_properties(testclosedcaptions PROPERTIES LABELS unit)
