add_executable(ld-cvbs-to-tbc
    main.cpp
)

target_link_libraries(ld-cvbs-to-tbc PRIVATE Qt::Core lddecode-library)

install(TARGETS ld-cvbs-to-tbc)
