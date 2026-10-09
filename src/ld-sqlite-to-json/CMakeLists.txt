add_executable(ld-sqlite-to-json
    main.cpp
)

target_link_libraries(ld-sqlite-to-json PRIVATE Qt::Core lddecode-library)

install(TARGETS ld-sqlite-to-json)
