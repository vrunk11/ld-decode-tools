add_executable(ld-container
    main.cpp
)

target_link_libraries(ld-container PRIVATE Qt::Core lddecode-library lddecode-ldx)

install(TARGETS ld-container)
