# The HVD decoders of ld-chroma-decoder on a capture of the test data: they write the frames they are asked for (also at the end of the capture, where the
# 3D decoder has no fields to look at beyond it), and a wrong option is a usage error. Run by CTest with DECODER (the program), CAPTURE (a TBC file with
# its metadata beside it) and WORK (a folder to write in).

function(expect code description)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
    if(NOT "${result}" STREQUAL "${code}")
        message(FATAL_ERROR "${description}: the exit code is ${result}, ${code} was expected (${ARGN})")
    endif()
endfunction()

file(MAKE_DIRECTORY "${WORK}")

# A RGB48 frame of the NTSC capture is 760 x 488 pixels
set(frameBytes 2225280)

foreach(decoder hvd2d hvd3d)
    set(output "${WORK}/${decoder}.rgb")
    file(REMOVE "${output}")
    expect(0 "${decoder} on 2 frames" ${DECODER} -f ${decoder} --start 3 --length 2 "${CAPTURE}" "${output}")
    file(SIZE "${output}" size)
    math(EXPR expected "2 * ${frameBytes}")
    if(NOT size EQUAL expected)
        message(FATAL_ERROR "${decoder}: ${size} bytes were written for 2 frames, ${expected} were expected")
    endif()

    # More frames are asked than the capture holds: the last ones have no neighbours, and the decoder still writes them all
    file(REMOVE "${output}")
    expect(0 "${decoder} to the end of the capture" ${DECODER} -f ${decoder} --start 3 --length 100 "${CAPTURE}" "${output}")
    file(SIZE "${output}" size)
    if(size LESS expected OR NOT size EQUAL 4450560)
        message(FATAL_ERROR "${decoder}: ${size} bytes were written to the end of the capture, 4450560 were expected")
    endif()
endforeach()

# The 3D decoder at the end of the capture gives what the 2D one would not be far from (a black neighbour used to break it: 1.8 dB against 35 dB)
set(a "${WORK}/hvd2d.rgb")
set(b "${WORK}/hvd3d.rgb")
file(READ "${a}" bytesA HEX LIMIT 400000)
file(READ "${b}" bytesB HEX LIMIT 400000)
if("${bytesA}" STREQUAL "${bytesB}")
    message(FATAL_ERROR "hvd2d and hvd3d wrote the same pictures: the 3D decoder did not use its neighbours")
endif()

# Wrong options are 2, whatever the decoder
expect(2 "iterations that are not a number" ${DECODER} -f hvd2d --hvd-iterations x "${CAPTURE}" "${WORK}/x.rgb")
expect(2 "negative iterations" ${DECODER} -f hvd2d --hvd-iterations -1 "${CAPTURE}" "${WORK}/x.rgb")
expect(2 "a negative strength" ${DECODER} -f hvd3d --hvd-strength -0.5 "${CAPTURE}" "${WORK}/x.rgb")
expect(2 "an overlap of 0" ${DECODER} -f hvd3d --hvd-overlap 0 "${CAPTURE}" "${WORK}/x.rgb")
expect(2 "a decoder that does not exist" ${DECODER} -f hvd4d "${CAPTURE}" "${WORK}/x.rgb")

message(STATUS "testhvdcli: ok")
