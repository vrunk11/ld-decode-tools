add_executable(testjsonio
    testjsonio.cpp
)

target_link_libraries(testjsonio PRIVATE Qt::Core lddecode-library)

add_test(NAME testjsonio COMMAND testjsonio)
set_tests_properties(testjsonio PROPERTIES LABELS unit)
