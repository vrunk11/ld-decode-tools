add_executable(testmapdialog
    testmapdialog.cpp
    ../mapdialog.cpp
)

target_include_directories(testmapdialog PRIVATE ..)
target_compile_definitions(testmapdialog PRIVATE TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/test-data")
target_link_libraries(testmapdialog PRIVATE Qt::Core Qt::Gui Qt::Widgets lddecode-library)
add_dependencies(testmapdialog ld-discmap)

add_test(NAME testmapdialog COMMAND testmapdialog)
set_tests_properties(testmapdialog PROPERTIES LABELS unit ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
