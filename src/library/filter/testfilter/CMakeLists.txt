add_executable(testfilter
    testfilter.cpp
)

target_link_libraries(testfilter PRIVATE Qt::Core lddecode-library)

# assert() is what these tests check with: a release build (-DNDEBUG) would remove them
target_compile_options(testfilter PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)

add_test(NAME testfilter COMMAND testfilter)
set_tests_properties(testfilter PROPERTIES LABELS unit)
