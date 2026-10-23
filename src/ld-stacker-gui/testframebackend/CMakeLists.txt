add_executable(testframebackend
    testframebackend.cpp
    ../framebackend.cpp
)

target_include_directories(testframebackend PRIVATE .. ${FFTW_INCLUDE_DIR})
target_link_libraries(testframebackend PRIVATE Qt::Core Qt::Gui Qt::Widgets lddecode-library lddecode-chroma lddecode-gui)
if(WIN32)
    target_compile_definitions(testframebackend PRIVATE NOMINMAX)
endif()

add_test(NAME testframebackend COMMAND testframebackend)
set_tests_properties(testframebackend PROPERTIES LABELS unit ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
