add_executable(testcvbs
    testcvbs.cpp
)

target_link_libraries(testcvbs PRIVATE Qt::Core lddecode-library)

add_test(NAME testcvbs COMMAND testcvbs)
set_tests_properties(testcvbs PROPERTIES LABELS unit)
