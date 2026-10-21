add_executable(testdiscnumbering
    testdiscnumbering.cpp
)

target_link_libraries(testdiscnumbering PRIVATE Qt::Core lddecode-library)

add_test(NAME testdiscnumbering COMMAND testdiscnumbering)
set_tests_properties(testdiscnumbering PROPERTIES LABELS unit)
