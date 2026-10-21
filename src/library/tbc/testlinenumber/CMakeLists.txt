add_executable(testlinenumber
    testlinenumber.cpp
)

target_link_libraries(testlinenumber PRIVATE Qt::Core lddecode-library)

# assert() is what these tests check with: a release build (-DNDEBUG) would remove them
target_compile_options(testlinenumber PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)

add_test(NAME testlinenumber COMMAND testlinenumber)
set_tests_properties(testlinenumber PROPERTIES LABELS unit)
