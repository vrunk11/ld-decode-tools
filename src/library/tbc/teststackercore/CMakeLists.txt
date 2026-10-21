add_executable(teststackercore
    teststackercore.cpp
)

target_link_libraries(teststackercore PRIVATE Qt::Core lddecode-library)

add_test(NAME teststackercore COMMAND teststackercore)
set_tests_properties(teststackercore PROPERTIES LABELS unit)
