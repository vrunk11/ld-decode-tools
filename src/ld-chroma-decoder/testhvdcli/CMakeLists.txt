# ld-container makes the VideoDisc capture of the test (it is there when the LDX library is built)
set(containerArgument "")
if(TARGET ld-container)
    set(containerArgument -DCONTAINER=$<TARGET_FILE:ld-container>)
endif()
add_test(NAME testhvdcli
    COMMAND ${CMAKE_COMMAND}
        -DDECODER=$<TARGET_FILE:ld-chroma-decoder>
        ${containerArgument}
        -DCAPTURE=${CMAKE_SOURCE_DIR}/test-data/ntsc/issue176.tbc
        -DWORK=${CMAKE_CURRENT_BINARY_DIR}/work
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check.cmake)
set_tests_properties(testhvdcli PROPERTIES LABELS unit)
