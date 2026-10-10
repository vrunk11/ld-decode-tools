# ld-process-ac3: ld-ac3-demodulate and ld-ac3-decode, the AC3 (Dolby Digital)
# RF decoder. Plain C++ using POSIX getopt, which MSVC does not provide: there
# it comes from vcpkg's "getopt" port (see vcpkg.json).
if(MSVC)
    find_path(GETOPT_INCLUDE_DIR getopt.h REQUIRED)
    find_library(GETOPT_LIBRARY NAMES getopt wingetopt REQUIRED)
endif()

add_subdirectory(demodulate)
add_subdirectory(decode)
