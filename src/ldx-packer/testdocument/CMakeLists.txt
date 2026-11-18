add_executable(testdocument
    testdocument.cpp
)

target_link_libraries(testdocument PRIVATE ldx-packer-gui)

# assert() is what these tests check with: a release build (-DNDEBUG) would remove them
target_compile_options(testdocument PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)

add_test(NAME testdocument COMMAND testdocument)
set_tests_properties(testdocument PROPERTIES LABELS unit)
