add_executable(testldx
    testldx.cpp
)

target_link_libraries(testldx PRIVATE Qt::Core lddecode-ldx)

# assert() is what these tests check with: a release build (-DNDEBUG) would remove them
target_compile_options(testldx PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)
target_compile_definitions(testldx PRIVATE LDX_EXAMPLES_DIR="${CMAKE_SOURCE_DIR}/docs/Specifications/examples")

add_test(NAME testldx COMMAND testldx)
set_tests_properties(testldx PROPERTIES LABELS unit)
