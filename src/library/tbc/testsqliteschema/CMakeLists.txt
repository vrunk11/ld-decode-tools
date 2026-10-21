add_executable(testsqliteschema
    testsqliteschema.cpp
)

target_link_libraries(testsqliteschema PRIVATE Qt::Core Qt::Sql lddecode-library)

# compared with a .tbc.db that ld-decode made
add_test(NAME testsqliteschema COMMAND testsqliteschema ${CMAKE_SOURCE_DIR}/test-data/ntsc/issue176.tbc.db)
set_tests_properties(testsqliteschema PROPERTIES LABELS unit)
