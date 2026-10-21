add_executable(teststackframe
    teststackframe.cpp
)

target_link_libraries(teststackframe PRIVATE Qt::Core lddecode-library)

add_test(NAME teststackframe COMMAND teststackframe)
set_tests_properties(teststackframe PROPERTIES LABELS unit)
