add_executable(testmetadata
    testmetadata.cpp
)

target_link_libraries(testmetadata PRIVATE Qt::Core lddecode-library)

# assert() is what these tests check with: a release build (-DNDEBUG) would remove them
target_compile_options(testmetadata PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)

add_test(NAME testmetadata COMMAND testmetadata)
set_tests_properties(testmetadata PROPERTIES LABELS unit)
