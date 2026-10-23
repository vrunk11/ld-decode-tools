add_executable(testthemedisabled
    testthemedisabled.cpp
)

target_include_directories(testthemedisabled PRIVATE ..)
target_link_libraries(testthemedisabled PRIVATE lddecode-gui)

add_test(NAME testthemedisabled COMMAND testthemedisabled)
set_tests_properties(testthemedisabled PROPERTIES LABELS unit ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
