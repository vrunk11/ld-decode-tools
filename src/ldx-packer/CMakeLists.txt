# The window as a library, so that the tests can drive it; the program is main.cpp. The work is done by ld-container, which
# the window runs (it is installed beside it).
add_library(ldx-packer-gui STATIC
    document.cpp
    mainwindow.cpp
    addtrackdialog.cpp
    metadatadialog.cpp
    rfsourcedialog.cpp
    parametersdialog.cpp
    processdialog.cpp
    trackrow.cpp
    ldx-packer-resources.qrc)
set_target_properties(ldx-packer-gui PROPERTIES AUTORCC ON)
target_include_directories(ldx-packer-gui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ldx-packer-gui PUBLIC lddecode-library lddecode-gui Qt::Widgets Qt::Gui Qt::Core)

if(WIN32)
    # The icon of the program in Windows (the window's own comes from the resources).
    add_executable(ldx-packer main.cpp appicon.rc)
    target_compile_definitions(ldx-packer PRIVATE NOMINMAX)
else()
    add_executable(ldx-packer main.cpp)
endif()
target_link_libraries(ldx-packer PRIVATE ldx-packer-gui)

# A program with a window: no console on Windows.
if(WIN32)
    set_target_properties(ldx-packer PROPERTIES WIN32_EXECUTABLE ON)
endif()

install(TARGETS ldx-packer)

# The icon and the menu entry on Linux.
if(UNIX AND NOT APPLE)
    foreach(size 16 32 64 128 256)
        install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/${size}-ldx-packer.png
            DESTINATION share/icons/hicolor/${size}x${size}/apps RENAME ldx-packer.png)
    endforeach()
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/install/ldx-packer.desktop DESTINATION share/applications)
endif()
