add_executable(ld-ac3-demodulate
    main.cpp
)
if(MSVC)
    target_link_libraries(ld-ac3-demodulate PRIVATE ${GETOPT_LIBRARY})
    target_include_directories(ld-ac3-demodulate PRIVATE ${GETOPT_INCLUDE_DIR})
endif()

install(TARGETS ld-ac3-demodulate)
