add_executable(testweave
    testweave.cpp
    ../tbcsource.cpp
    ../filesnr.cpp
    ../livesnr.cpp
)

target_link_libraries(testweave PRIVATE Qt::Core Qt::Gui Qt::Widgets lddecode-library lddecode-chroma)
target_include_directories(testweave PRIVATE ..)
if(WIN32)
    target_compile_definitions(testweave PRIVATE)
endif()

add_test(NAME testweave COMMAND testweave)
set_tests_properties(testweave PROPERTIES LABELS unit ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
