add_executable(testsourcefilechecks
    testsourcefilechecks.cpp
)

target_link_libraries(testsourcefilechecks PRIVATE Qt::Core lddecode-library)

add_test(NAME testsourcefilechecks COMMAND testsourcefilechecks)
set_tests_properties(testsourcefilechecks PROPERTIES LABELS unit)
