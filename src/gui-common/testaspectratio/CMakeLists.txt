add_executable(testaspectratio
    testaspectratio.cpp
)

target_link_libraries(testaspectratio PRIVATE lddecode-gui)

add_test(NAME testaspectratio COMMAND testaspectratio)
set_tests_properties(testaspectratio PROPERTIES LABELS unit)
