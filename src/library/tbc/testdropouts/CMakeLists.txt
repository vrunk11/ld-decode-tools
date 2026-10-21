add_executable(testdropouts
    testdropouts.cpp
)

target_link_libraries(testdropouts PRIVATE Qt::Core lddecode-library)

add_test(NAME testdropouts COMMAND testdropouts)
set_tests_properties(testdropouts PROPERTIES LABELS unit)
