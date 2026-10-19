add_executable(testremote
    testremote.cpp
    ../remotelogic.cpp
    ../remotewidget.cpp
    ../jogwheel.cpp
    ../infopanel.cpp
    ../pintitlebar.cpp
    ../chaptermarks.cpp
    ../chapterlistdialog.cpp
    ../seekbar.cpp
)

target_link_libraries(testremote PRIVATE Qt::Core Qt::Gui Qt::Widgets)
target_include_directories(testremote PRIVATE ..)

add_test(NAME testremote COMMAND testremote)
set_tests_properties(testremote PROPERTIES LABELS unit ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
