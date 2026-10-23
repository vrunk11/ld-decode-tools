add_executable(teststreamcopy
    teststreamcopy.cpp
)

target_link_libraries(teststreamcopy PRIVATE Qt::Core lddecode-library)

add_test(NAME teststreamcopy COMMAND teststreamcopy)
set_tests_properties(teststreamcopy PROPERTIES LABELS unit)
