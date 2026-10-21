add_executable(testdropoutdrawing
    testdropoutdrawing.cpp
)

target_link_libraries(testdropoutdrawing PRIVATE lddecode-gui)

add_test(NAME testdropoutdrawing COMMAND testdropoutdrawing)
set_tests_properties(testdropoutdrawing PROPERTIES LABELS unit ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
