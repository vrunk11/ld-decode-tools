add_executable(testaudio
    testaudio.cpp
)

target_link_libraries(testaudio PRIVATE Qt::Core lddecode-library)

add_test(NAME testaudio COMMAND testaudio)
set_tests_properties(testaudio PROPERTIES LABELS unit)
