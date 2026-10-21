add_executable(testvbidecoder
    testvbidecoder.cpp
)

target_link_libraries(testvbidecoder PRIVATE Qt::Core lddecode-library)

# assert() is what these tests check with: a release build (-DNDEBUG) would remove them
target_compile_options(testvbidecoder PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)

add_test(NAME testvbidecoder COMMAND testvbidecoder)
set_tests_properties(testvbidecoder PROPERTIES LABELS unit)
