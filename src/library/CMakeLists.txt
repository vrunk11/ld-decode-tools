add_library(lddecode-library STATIC
    tbc/cvbs.cpp
    tbc/cvbsaudio.cpp
    tbc/biphasecode.cpp
    tbc/closedcaption.cpp
    tbc/dropouts.cpp
    tbc/efmchain.cpp
    tbc/filters.cpp
    tbc/fieldvbi.cpp
    tbc/fmcode.cpp
    tbc/jsonio.cpp
    tbc/ldxbytetrack.cpp
    tbc/ldxcapture.cpp
    tbc/ldxfilter.cpp
    tbc/ldxstream.cpp
    tbc/sqliteio.cpp
    tbc/lddecodemetadata.cpp
    tbc/metadataoptions.cpp
    tbc/logging.cpp
    tbc/navigation.cpp
    tbc/sourceaudio.cpp
    tbc/sourcevideo.cpp
    tbc/stackercore.cpp
    tbc/stackedvideo.cpp
    tbc/stackproject.cpp
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

# The LDX container (docs/Specifications/container-format.md), when libebml and libmatroska are there
if(LDDECODE_LDX_AVAILABLE)
    add_library(lddecode-ldx STATIC
        ldx/ldx.cpp
        ldx/ldxcodec.cpp
    )
    target_include_directories(lddecode-ldx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/ldx)
    target_link_libraries(lddecode-ldx PUBLIC Qt::Core EBML::ebml Matroska::matroska)
    if(LDDECODE_LDX_FFMPEG)
        target_link_libraries(lddecode-ldx PUBLIC PkgConfig::LDX_FFMPEG)
        target_compile_definitions(lddecode-ldx PUBLIC LDX_WITH_FFMPEG)
    endif()
    # and the tools read captures kept in LDX files through the TBC library
    target_link_libraries(lddecode-library PUBLIC lddecode-ldx)
    target_compile_definitions(lddecode-library PUBLIC LDDECODE_WITH_LDX)
endif()
