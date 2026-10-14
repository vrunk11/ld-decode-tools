# What the GUI programs share: the colour themes and widget styles, and the editor of the custom theme
add_library(lddecode-gui STATIC
    theme.cpp
    themeeditordialog.cpp
)

target_include_directories(lddecode-gui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lddecode-gui PUBLIC Qt::Core Qt::Gui Qt::Widgets)
