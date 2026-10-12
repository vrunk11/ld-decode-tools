add_library(lddecode-library STATIC
    tbc/cvbs.cpp
    tbc/cvbsaudio.cpp
    tbc/biphasecode.cpp
    tbc/closedcaption.cpp
    tbc/dropouts.cpp
    tbc/filters.cpp
    tbc/fieldvbi.cpp
    tbc/fmcode.cpp
    tbc/jsonio.cpp
    tbc/sqliteio.cpp
    tbc/lddecodemetadata.cpp
    tbc/metadataoptions.cpp
    tbc/logging.cpp
    tbc/navigation.cpp
    tbc/sourceaudio.cpp
    tbc/sourcevideo.cpp
    tbc/tbcfiles.cpp
    tbc/streamreader.cpp
    tbc/targetvideo.cpp
    tbc/vbidecoder.cpp
    tbc/vitsmetrics.cpp
    tbc/whiteflag.cpp
    tbc/videoiddecoder.cpp
    tbc/videoid.cpp
    tbc/vitccode.cpp
    tbc/vitcdecoder.cpp
)

target_include_directories(lddecode-library PUBLIC filter tbc)
target_include_directories(lddecode-library PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(lddecode-library PRIVATE Qt::Core Qt::Sql)
