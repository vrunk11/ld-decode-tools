add_test(NAME testac3cli
    COMMAND ${CMAKE_COMMAND}
        -DDEMODULATE=$<TARGET_FILE:ld-ac3-demodulate>
        -DDECODE=$<TARGET_FILE:ld-ac3-decode>
        -DWORK=${CMAKE_CURRENT_BINARY_DIR}/work
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check.cmake)
set_tests_properties(testac3cli PROPERTIES LABELS unit)
