add_executable(testshortcuts
    testshortcuts.cpp
    ../shortcutsdialog.cpp
)

target_link_libraries(testshortcuts PRIVATE Qt::Core Qt::Gui Qt::Widgets)
target_include_directories(testshortcuts PRIVATE ..)

add_test(NAME testshortcuts COMMAND testshortcuts)
set_tests_properties(testshortcuts PROPERTIES LABELS unit ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
