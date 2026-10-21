add_executable(testlumacoring
    testlumacoring.cpp
)

target_include_directories(testlumacoring PRIVATE ..)
target_link_libraries(testlumacoring PRIVATE Qt::Core lddecode-library)

add_test(NAME testlumacoring COMMAND testlumacoring)
set_tests_properties(testlumacoring PROPERTIES LABELS unit)
