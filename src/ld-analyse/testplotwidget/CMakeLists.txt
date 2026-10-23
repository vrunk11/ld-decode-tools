add_executable(testplotwidget
    testplotwidget.cpp
    ../plotwidget.cpp
)

target_link_libraries(testplotwidget PRIVATE Qt::Core Qt::Gui Qt::Widgets)
target_include_directories(testplotwidget PRIVATE ..)

add_test(NAME testplotwidget COMMAND testplotwidget)
set_tests_properties(testplotwidget PROPERTIES LABELS unit ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
