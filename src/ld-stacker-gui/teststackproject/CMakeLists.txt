add_executable(teststackproject
    teststackproject.cpp
    ../stackproject.cpp
)

target_include_directories(teststackproject PRIVATE ..)
target_link_libraries(teststackproject PRIVATE Qt::Core)

add_test(NAME teststackproject COMMAND teststackproject)
set_tests_properties(teststackproject PROPERTIES LABELS unit)
