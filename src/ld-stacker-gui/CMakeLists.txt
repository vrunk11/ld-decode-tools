set(ld-stacker-gui_SOURCES
    main.cpp
    mainwindow.cpp
    framebackend.cpp
    sourceinfopanel.cpp
    mapdialog.cpp
    snrplot.cpp
    toolwindows.cpp
    optionsdialogs.cpp
    processdialog.cpp
    streamsdialog.cpp
)

qt_add_resources(ld-stacker-gui_SOURCES ld-stacker-gui-resources.qrc)

# On Windows the icon of the program itself (the one of the window comes from the resources)
if(WIN32)
    list(APPEND ld-stacker-gui_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/appicon.rc)
endif()

add_executable(ld-stacker-gui ${ld-stacker-gui_SOURCES})

target_include_directories(ld-stacker-gui PRIVATE ${FFTW_INCLUDE_DIR})

target_link_libraries(ld-stacker-gui PRIVATE
    Qt::Core Qt::Gui Qt::Widgets Qt::Svg
    lddecode-library
    lddecode-chroma
    lddecode-gui
)

if(WIN32)
    target_compile_definitions(ld-stacker-gui PRIVATE NOMINMAX)
    set_property(TARGET ld-stacker-gui PROPERTY WIN32_EXECUTABLE true)
endif()

install(TARGETS ld-stacker-gui)
