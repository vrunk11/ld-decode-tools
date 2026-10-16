add_executable(testwaveform
    testwaveform.cpp
    ../audioengine.cpp
    ../waveformpanel.cpp
    ../audioconfigdialog.cpp
    ../efmdecoding.cpp
)

target_link_libraries(testwaveform PRIVATE Qt::Core Qt::Gui Qt::Widgets Qt6::Multimedia lddecode-library lddecode-gui)
target_include_directories(testwaveform PRIVATE ..)

add_test(NAME testwaveform COMMAND testwaveform)
set_tests_properties(testwaveform PROPERTIES LABELS unit ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
