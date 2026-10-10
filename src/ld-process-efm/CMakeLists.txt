add_executable(ld-process-efm
    efmprocess.cpp
    main.cpp
    Datatypes/audio.cpp
    Datatypes/f1frame.cpp
    Datatypes/f2frame.cpp
    Datatypes/f3frame.cpp
    Datatypes/section.cpp
    Datatypes/sector.cpp
    Datatypes/tracktime.cpp
    Decoders/c1circ.cpp
    Decoders/c2circ.cpp
    Decoders/c2deinterleave.cpp
    Decoders/efmtof3frames.cpp
    Decoders/f1toaudio.cpp
    Decoders/f1todata.cpp
    Decoders/f2tof1frames.cpp
    Decoders/f3tof2frames.cpp
    Decoders/syncf3frames.cpp
)

# Reed-Solomon headers: the shared ezpwd (EZPWD_DIR, see the root CMakeLists.txt)
target_include_directories(ld-process-efm PRIVATE . ${EZPWD_DIR})

target_link_libraries(ld-process-efm PRIVATE Qt::Core lddecode-library)

install(TARGETS ld-process-efm)

# MSVC does not define ssize_t (a POSIX type), which the ezpwd headers use
target_compile_definitions(ld-process-efm PRIVATE $<$<CXX_COMPILER_ID:MSVC>:ssize_t=ptrdiff_t>)
