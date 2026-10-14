set(ld-stacker-gui_SOURCES
    main.cpp
    mainwindow.cpp
    framebackend.cpp
    snrplot.cpp
    toolwindows.cpp
    optionsdialogs.cpp
    processdialog.cpp
)

add_executable(ld-stacker-gui ${ld-stacker-gui_SOURCES})

target_include_directories(ld-stacker-gui PRIVATE ${FFTW_INCLUDE_DIR})

target_link_libraries(ld-stacker-gui PRIVATE
    Qt::Core Qt::Gui Qt::Widgets
    lddecode-library
    lddecode-chroma
)

if(WIN32)
    target_compile_definitions(ld-stacker-gui PRIVATE NOMINMAX)
    set_property(TARGET ld-stacker-gui PROPERTY WIN32_EXECUTABLE true)
endif()

install(TARGETS ld-stacker-gui)
