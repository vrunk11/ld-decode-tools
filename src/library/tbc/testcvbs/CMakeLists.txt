add_executable(testcvbs
    testcvbs.cpp
)

target_link_libraries(testcvbs PRIVATE Qt::Core lddecode-library)

# assert() is what these tests check with: a release build (-DNDEBUG) would remove them
target_compile_options(testcvbs PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)

add_test(NAME testcvbs COMMAND testcvbs)
set_tests_properties(testcvbs PROPERTIES LABELS unit)
