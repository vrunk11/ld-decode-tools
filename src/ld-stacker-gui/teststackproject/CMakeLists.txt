add_executable(teststackproject
    teststackproject.cpp
)

target_link_libraries(teststackproject PRIVATE Qt::Core lddecode-library)

add_test(NAME teststackproject COMMAND teststackproject)
set_tests_properties(teststackproject PROPERTIES LABELS unit)
