add_executable(testldx
    testldx.cpp
)

target_link_libraries(testldx PRIVATE Qt::Core lddecode-ldx)
target_compile_definitions(testldx PRIVATE LDX_EXAMPLES_DIR="${CMAKE_SOURCE_DIR}/docs/Specifications/examples")

add_test(NAME testldx COMMAND testldx)
set_tests_properties(testldx PROPERTIES LABELS unit)
