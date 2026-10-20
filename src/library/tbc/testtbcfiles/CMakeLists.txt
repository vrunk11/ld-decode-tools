add_executable(testtbcfiles
    testtbcfiles.cpp
)

target_link_libraries(testtbcfiles PRIVATE Qt::Core lddecode-library)

add_test(NAME testtbcfiles COMMAND testtbcfiles)
set_tests_properties(testtbcfiles PROPERTIES LABELS unit)
