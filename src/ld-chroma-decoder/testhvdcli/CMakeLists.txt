add_test(NAME testhvdcli
    COMMAND ${CMAKE_COMMAND}
        -DDECODER=$<TARGET_FILE:ld-chroma-decoder>
        -DCAPTURE=${CMAKE_SOURCE_DIR}/test-data/ntsc/issue176.tbc
        -DWORK=${CMAKE_CURRENT_BINARY_DIR}/work
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check.cmake)
set_tests_properties(testhvdcli PROPERTIES LABELS unit)
