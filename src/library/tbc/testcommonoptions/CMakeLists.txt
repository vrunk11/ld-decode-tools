add_executable(testcommonoptions
    testcommonoptions.cpp
)

target_link_libraries(testcommonoptions PRIVATE Qt::Core lddecode-library)

add_test(NAME testcommonoptions COMMAND testcommonoptions)
set_tests_properties(testcommonoptions PROPERTIES LABELS unit)
