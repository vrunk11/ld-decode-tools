add_executable(testframe
    testframe.cpp
    ../frame.cpp
)

target_link_libraries(testframe PRIVATE Qt::Core)
target_include_directories(testframe PRIVATE ..)

add_test(NAME testframe COMMAND testframe)
set_tests_properties(testframe PROPERTIES LABELS unit)
