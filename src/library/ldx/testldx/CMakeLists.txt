add_executable(testldx
    testldx.cpp
)

target_link_libraries(testldx PRIVATE Qt::Core lddecode-ldx)

add_test(NAME testldx COMMAND testldx)
set_tests_properties(testldx PROPERTIES LABELS unit)
