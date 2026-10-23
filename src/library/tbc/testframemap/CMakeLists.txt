add_executable(testframemap
    testframemap.cpp
)

target_link_libraries(testframemap PRIVATE Qt::Core lddecode-library)

add_test(NAME testframemap COMMAND testframemap)
set_tests_properties(testframemap PROPERTIES LABELS unit)
