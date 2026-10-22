add_executable(testsqliteread
    testsqliteread.cpp
)

target_link_libraries(testsqliteread PRIVATE Qt::Core lddecode-library)

add_test(NAME testsqliteread COMMAND testsqliteread)
set_tests_properties(testsqliteread PROPERTIES LABELS unit)
