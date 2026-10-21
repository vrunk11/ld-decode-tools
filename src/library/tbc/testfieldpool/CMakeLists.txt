add_executable(testfieldpool
    testfieldpool.cpp
)

target_link_libraries(testfieldpool PRIVATE Qt::Core lddecode-library)

add_test(NAME testfieldpool COMMAND testfieldpool)
set_tests_properties(testfieldpool PROPERTIES LABELS unit)
