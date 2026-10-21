add_executable(testvitcdecoder
    testvitcdecoder.cpp
)

target_link_libraries(testvitcdecoder PRIVATE Qt::Core lddecode-library)

# assert() is what these tests check with: a release build (-DNDEBUG) would remove them
target_compile_options(testvitcdecoder PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)

add_test(NAME testvitcdecoder COMMAND testvitcdecoder)
set_tests_properties(testvitcdecoder PROPERTIES LABELS unit)
