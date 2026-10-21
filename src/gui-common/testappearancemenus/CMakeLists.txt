add_executable(testappearancemenus
    testappearancemenus.cpp
)

target_link_libraries(testappearancemenus PRIVATE lddecode-gui)

add_test(NAME testappearancemenus COMMAND testappearancemenus)
set_tests_properties(testappearancemenus PROPERTIES LABELS unit ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
