# Switch Final Linker Optimization
set(_FFmpeg_COMPONENTS avcodec avfilter avformat avutil swresample swscale)

foreach(c ${_FFmpeg_COMPONENTS})
    set(FFmpeg_INCLUDE_${c} "/opt/devkitpro/portlibs/switch/include")
    set(FFmpeg_LIBRARY_${c} "/opt/devkitpro/portlibs/switch/lib/lib${c}.a")

    if(NOT TARGET FFmpeg::${c})
        add_library(FFmpeg::${c} UNKNOWN IMPORTED)
        set_target_properties(FFmpeg::${c} PROPERTIES
            IMPORTED_LOCATION ${FFmpeg_LIBRARY_${c}}
            INTERFACE_INCLUDE_DIRECTORIES ${FFmpeg_INCLUDE_${c}}
        )

        # Manually attach the missing dependencies to the targets
        if(${c} STREQUAL "avcodec")
            set_target_properties(FFmpeg::avcodec PROPERTIES
                INTERFACE_LINK_LIBRARIES "/opt/devkitpro/portlibs/switch/lib/libdav1d.a")
        endif()
        if(${c} STREQUAL "avformat")
            set_target_properties(FFmpeg::avformat PROPERTIES
                INTERFACE_LINK_LIBRARIES "/opt/devkitpro/portlibs/switch/lib/libbz2.a")
        endif()
    endif()
    set(FFmpeg_${c}_FOUND TRUE PARENT_SCOPE)
endforeach()

set(FFmpeg_FOUND TRUE PARENT_SCOPE)
