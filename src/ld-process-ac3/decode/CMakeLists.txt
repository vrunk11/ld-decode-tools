add_executable(ld-ac3-decode
    ac3_parsing.cpp
    main.cpp
)
if(MSVC)
    target_link_libraries(ld-ac3-decode PRIVATE ${GETOPT_LIBRARY})
    target_include_directories(ld-ac3-decode PRIVATE ${GETOPT_INCLUDE_DIR})
    # MSVC does not define ssize_t (a POSIX type), which the ezpwd headers use
    target_compile_definitions(ld-ac3-decode PRIVATE ssize_t=ptrdiff_t)
endif()
# Reed-Solomon headers: the shared ezpwd (EZPWD_DIR, see the root CMakeLists.txt)
target_include_directories(ld-ac3-decode PRIVATE . ${EZPWD_DIR})

install(TARGETS ld-ac3-decode)
