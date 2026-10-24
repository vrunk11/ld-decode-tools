# The tests of the HVD engine: each file is a program of its own that checks the numerical core.

set(HVD_ENGINE_TESTS
    gradients_test
    lossless_identity_test
    lockin_test
    colour_test
    frame_bridge_test
    motion_test
    motion_v2_test
    temporal_test
    sequence_test
    field_order_test
    variational_v2_test
    pal_lockin_test
    chroma_reference_test
    gain_chain_test
    pal_aniso_group_test
    preview_aspect_test)

foreach(t ${HVD_ENGINE_TESTS})
    add_executable(hvd_${t} ${t}.cpp)
    target_include_directories(hvd_${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(hvd_${t} PRIVATE lddecode-hvd)
    add_test(NAME hvd_${t} COMMAND hvd_${t})
    set_tests_properties(hvd_${t} PROPERTIES LABELS unit)
endforeach()
