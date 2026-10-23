add_executable(testmetadataoptions
    testmetadataoptions.cpp
)

target_link_libraries(testmetadataoptions PRIVATE Qt::Core lddecode-library)

add_test(NAME testmetadataoptions COMMAND testmetadataoptions)
set_tests_properties(testmetadataoptions PROPERTIES LABELS unit)
