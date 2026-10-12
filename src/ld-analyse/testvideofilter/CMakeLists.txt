add_executable(testvideofilter
    testvideofilter.cpp
    ../videofilter.cpp
)

target_link_libraries(testvideofilter PRIVATE Qt::Core Qt::Gui)
target_include_directories(testvideofilter PRIVATE ..)

if(FFMPEG_FOUND)
    target_link_libraries(testvideofilter PRIVATE PkgConfig::FFMPEG)
    target_compile_definitions(testvideofilter PRIVATE LDDECODE_HAVE_FFMPEG)
endif()

add_test(NAME testvideofilter COMMAND testvideofilter)
set_tests_properties(testvideofilter PROPERTIES LABELS unit)
