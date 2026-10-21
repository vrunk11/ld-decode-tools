add_executable(teststandardoptions
    teststandardoptions.cpp
)

target_include_directories(teststandardoptions PRIVATE ..)

add_test(NAME teststandardoptions COMMAND teststandardoptions)
set_tests_properties(teststandardoptions PROPERTIES LABELS unit)
