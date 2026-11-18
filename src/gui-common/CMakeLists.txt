# What the GUI programs share: the colour themes and widget styles (with their menus), the editor of the custom theme, and the drawing of dropouts on a picture
add_library(lddecode-gui STATIC
    appearancemenus.cpp
    chromadecoderconfigdialog.cpp
    chromadecoderconfigdialog.ui
    coverimage.cpp
    coverwidget.cpp
    dropoutdrawing.cpp
    theme.cpp
    themeeditordialog.cpp
)

target_include_directories(lddecode-gui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lddecode-gui PUBLIC Qt::Core Qt::Gui Qt::Widgets lddecode-library lddecode-chroma)

if(LDDECODE_NETWORK_AVAILABLE)
    target_link_libraries(lddecode-gui PUBLIC Qt::Network)
    target_compile_definitions(lddecode-gui PUBLIC LDDECODE_WITH_NETWORK)
endif()
