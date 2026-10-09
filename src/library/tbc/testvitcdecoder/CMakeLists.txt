add_executable(testvitcdecoder
    testvitcdecoder.cpp
)

target_link_libraries(testvitcdecoder PRIVATE Qt::Core lddecode-library)

add_test(NAME testvitcdecoder COMMAND testvitcdecoder)
set_tests_properties(testvitcdecoder PROPERTIES LABELS unit)
