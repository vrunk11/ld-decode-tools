# The command lines of ld-ac3-demodulate and ld-ac3-decode: what they do with arguments and inputs that are wrong, short or without a signal. Run by CTest with
# DEMODULATE, DECODE (the programs) and WORK (a folder to write in).

function(expect code description)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
    if(NOT "${result}" STREQUAL "${code}")
        message(FATAL_ERROR "${description}: the exit code is ${result}, ${code} was expected (${ARGN})")
    endif()
endfunction()

file(MAKE_DIRECTORY "${WORK}")
string(REPEAT "0123456789" 20000 samples)           # 200 000 samples: a signal of no use, which the demodulator still has to get through
file(WRITE "${WORK}/samples.rf" "${samples}")
file(WRITE "${WORK}/tiny.rf" "0123456789")
file(WRITE "${WORK}/empty.rf" "")
file(WRITE "${WORK}/symbols.sym" "0123012301230123")

# Wrong command lines: a usage error is 2
expect(2 "no argument" ${DEMODULATE})
expect(2 "no argument" ${DECODE})
expect(2 "a window that is not a number" ${DEMODULATE} -s abc "${WORK}/samples.rf" "${WORK}/a.sym")
expect(2 "a window of 0" ${DEMODULATE} -s 0 "${WORK}/samples.rf" "${WORK}/a.sym")
expect(2 "a negative window" ${DEMODULATE} -s -5 "${WORK}/samples.rf" "${WORK}/a.sym")
expect(2 "a window with text after the number" ${DEMODULATE} -s 12abc "${WORK}/samples.rf" "${WORK}/a.sym")
expect(2 "a logging level that is not a number" ${DEMODULATE} -v x "${WORK}/samples.rf" "${WORK}/a.sym")
expect(2 "a logging level out of range" ${DECODE} -v 7 "${WORK}/symbols.sym" "${WORK}/a.ac3")

# What cannot be read is 1
expect(1 "a missing input" ${DEMODULATE} "${WORK}/nothing.rf" "${WORK}/a.sym")
expect(1 "an input that is too short" ${DEMODULATE} "${WORK}/tiny.rf" "${WORK}/a.sym")
expect(1 "an empty input" ${DEMODULATE} "${WORK}/empty.rf" "${WORK}/a.sym")
expect(1 "a missing input" ${DECODE} "${WORK}/nothing.sym" "${WORK}/a.ac3")

# An input without a signal is read to its end (the program used to read its own buffer after it was gone, and stopped)
expect(0 "an input of 200 000 samples" ${DEMODULATE} "${WORK}/samples.rf" "${WORK}/b.sym")
file(SIZE "${WORK}/b.sym" symbolBytes)
if(symbolBytes LESS 1000)
    message(FATAL_ERROR "the demodulator wrote ${symbolBytes} symbols for 200 000 samples (about 1 250 were expected)")
endif()
expect(0 "symbols that hold no frame" ${DECODE} "${WORK}/symbols.sym" "${WORK}/b.ac3")

message(STATUS "testac3cli: ok")
