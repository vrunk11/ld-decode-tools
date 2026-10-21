add_executable(testvitsmetrics
    testvitsmetrics.cpp
)

target_link_libraries(testvitsmetrics PRIVATE Qt::Core lddecode-library)

add_test(NAME testvitsmetrics COMMAND testvitsmetrics)
set_tests_properties(testvitsmetrics PROPERTIES LABELS unit)
