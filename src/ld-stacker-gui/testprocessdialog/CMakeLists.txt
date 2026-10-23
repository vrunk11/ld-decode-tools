add_executable(testprocessdialog
    testprocessdialog.cpp
    ../processdialog.cpp
    ../streamsdialog.cpp
)

target_include_directories(testprocessdialog PRIVATE ..)
target_compile_definitions(testprocessdialog PRIVATE TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/test-data")
target_link_libraries(testprocessdialog PRIVATE Qt::Core Qt::Gui Qt::Widgets lddecode-library)
add_dependencies(testprocessdialog ld-disc-stacker)

add_test(NAME testprocessdialog COMMAND testprocessdialog)
set_tests_properties(testprocessdialog PROPERTIES LABELS unit ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
