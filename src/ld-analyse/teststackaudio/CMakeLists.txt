add_executable(teststackaudio
    teststackaudio.cpp
    ../stackaudio.cpp
)

target_link_libraries(teststackaudio PRIVATE Qt::Core lddecode-library)
target_include_directories(teststackaudio PRIVATE ..)

add_test(NAME teststackaudio COMMAND teststackaudio)
set_tests_properties(teststackaudio PROPERTIES LABELS unit)
