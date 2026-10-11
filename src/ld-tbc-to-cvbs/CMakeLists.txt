add_executable(ld-tbc-to-cvbs
    main.cpp
)

target_link_libraries(ld-tbc-to-cvbs PRIVATE Qt::Core lddecode-library)

install(TARGETS ld-tbc-to-cvbs)
