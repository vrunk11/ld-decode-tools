# The holographic-variational (HVD) Y/C separator: the numerical engine and the bridge from fields to frames.
# Single-precision FFTW (fftw3f) and, when the compiler has it, OpenMP for the solver loops.

add_library(lddecode-hvd STATIC
    engine/ntsc_geometry.cpp
    engine/lockin.cpp
    engine/fft2d.cpp
    engine/holographic_init.cpp
    engine/variational.cpp
    engine/colour.cpp
    engine/motion.cpp
    engine/temporal.cpp
    engine/sequence.cpp
    engine/engine.cpp
    gpu/gpu_solver.cpp
    fieldwindow.cpp
    frame_bridge.cpp
)

target_include_directories(lddecode-hvd PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lddecode-hvd PUBLIC PkgConfig::FFTW3F Threads::Threads)

# The GPU path (OpenCL, optional): without the headers and a loader the library is built without it and the decoder runs on the CPU.
option(LDDECODE_WITH_OPENCL "Build the HVD decoder with its OpenCL (GPU) solver when OpenCL is found" ON)
if(LDDECODE_WITH_OPENCL)
    find_package(OpenCL QUIET)
    if(OpenCL_FOUND)
        target_compile_definitions(lddecode-hvd PUBLIC LDDECODE_HVD_OPENCL)
        target_link_libraries(lddecode-hvd PUBLIC OpenCL::OpenCL)
        message(STATUS "OpenCL found: the HVD decoder has its GPU option")
    else()
        message(STATUS "OpenCL not found: the HVD decoder has no GPU option")
    endif()
endif()

find_package(OpenMP QUIET)
if(OpenMP_CXX_FOUND)
    target_link_libraries(lddecode-hvd PUBLIC OpenMP::OpenMP_CXX)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # MinGW's FindOpenMP finds the flag but no library name, and with LTO the link needs the flag itself
        target_link_options(lddecode-hvd PUBLIC ${OpenMP_CXX_FLAGS})
    endif()
else()
    message(STATUS "OpenMP not found: the HVD decoder runs its solver loops on one thread")
endif()

set_target_properties(lddecode-hvd PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The solver is arithmetic on planes of floats and std::complex. Without these, every sqrt keeps its errno branch and every complex product
# calls the library for the NaN and infinity cases, and the loops do not vectorise; the data has neither.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(lddecode-hvd PRIVATE -fno-math-errno -fno-trapping-math -fcx-limited-range -fopenmp-simd)
endif()
