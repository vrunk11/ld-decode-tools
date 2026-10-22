add_executable(testsnrplot
    testsnrplot.cpp
    ../snrplot.cpp
)

target_include_directories(testsnrplot PRIVATE ..)
target_link_libraries(testsnrplot PRIVATE Qt::Core Qt::Gui Qt::Widgets)

add_test(NAME testsnrplot COMMAND testsnrplot)
set_tests_properties(testsnrplot PROPERTIES LABELS unit ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
