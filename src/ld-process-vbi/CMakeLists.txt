add_executable(ld-process-vbi
    decoderpool.cpp
    main.cpp
    vbilinedecoder.cpp
)

target_link_libraries(ld-process-vbi PRIVATE Qt::Core lddecode-library)

install(TARGETS ld-process-vbi)
