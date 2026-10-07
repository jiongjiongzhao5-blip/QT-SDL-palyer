
cmake_minimum_required(VERSION 3.21)

project(voice_player_learn VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)   
set(CMAKE_CXX_EXTENSIONS OFF)         

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "构建类型" FORCE)
endif()

if(DEFINED ENV{QT_ROOT})
    list(APPEND CMAKE_PREFIX_PATH "$ENV{QT_ROOT}")
endif()

set(CMAKE_PREFIX_PATH "C:/Apps/QTnew/6.7.3/msvc2022_64" ${CMAKE_PREFIX_PATH})

find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets)

set(FFMPEG_ROOT "" CACHE PATH "FFmpeg 开发包根目录（含 include/、lib/、bin/）")

if(NOT FFMPEG_ROOT AND DEFINED ENV{FFMPEG_ROOT})
    set(FFMPEG_ROOT "$ENV{FFMPEG_ROOT}")
endif()

if(NOT FFMPEG_ROOT AND
   EXISTS "C:/vcpkg/installed/x64-windows/include/libavformat/avformat.h")
    set(FFMPEG_ROOT "C:/vcpkg/installed/x64-windows")
endif()

if(NOT FFMPEG_ROOT)
    message(FATAL_ERROR
        "未找到 FFmpeg 开发包。请安装 FFmpeg 7+（如 vcpkg install ffmpeg），\n"
        "并用 -DFFMPEG_ROOT=<dev包根目录> 指向它（目录下需有 include/、lib/、bin/）。")
endif()
message(STATUS "FFmpeg 根目录: ${FFMPEG_ROOT}")

set(_FFMPEG_COMPONENTS avformat avcodec avutil swresample swscale)

set(_FFMPEG_LIBS "")
foreach(_comp IN LISTS _FFMPEG_COMPONENTS)
    find_path(_FFMPEG_${_comp}_INC NAMES lib${_comp}/${_comp}.h
              HINTS "${FFMPEG_ROOT}/include" NO_DEFAULT_PATH)
    find_library(_FFMPEG_${_comp}_REL NAMES ${_comp}
                 HINTS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
    find_library(_FFMPEG_${_comp}_DBG NAMES ${_comp}
                 HINTS "${FFMPEG_ROOT}/debug/lib" NO_DEFAULT_PATH)

    if(NOT _FFMPEG_${_comp}_INC OR NOT _FFMPEG_${_comp}_REL)
        message(FATAL_ERROR "未找到 FFmpeg 组件 ${_comp}（检查 FFMPEG_ROOT 下的 include/lib）。")
    endif()

    if(_FFMPEG_${_comp}_DBG)
        list(APPEND _FFMPEG_LIBS
             "$<IF:$<CONFIG:Debug>,${_FFMPEG_${_comp}_DBG},${_FFMPEG_${_comp}_REL}>")
    else()
        list(APPEND _FFMPEG_LIBS "${_FFMPEG_${_comp}_REL}")
    endif()
endforeach()

message(STATUS "FFmpeg 组件: ${_FFMPEG_COMPONENTS}")

add_library(vpl_ffmpeg INTERFACE)
target_include_directories(vpl_ffmpeg INTERFACE "${FFMPEG_ROOT}/include")
target_link_libraries(vpl_ffmpeg INTERFACE ${_FFMPEG_LIBS})

function(vpl_apply_common_options tgt)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /utf-8 /W3 /permissive-)
        target_compile_definitions(${tgt} PRIVATE
            _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
endfunction()

function(vpl_copy_runtime_dlls tgt)
    if(WIN32)
        file(GLOB _FFMPEG_DLLS "${FFMPEG_ROOT}/bin/*.dll")
        if(_FFMPEG_DLLS)
            add_custom_command(TARGET ${tgt} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        ${_FFMPEG_DLLS} "$<TARGET_FILE_DIR:${tgt}>"
                COMMENT "拷贝 FFmpeg 运行时 DLL 到 ${tgt} 输出目录")
        endif()
    endif()
endfunction()

add_executable(voice_player_learn
    src/main.cpp
    src/av_utils.h
    .gitignore
)
target_include_directories(voice_player_learn PRIVATE src)
target_link_libraries(voice_player_learn PRIVATE
    Qt6::Widgets
    vpl_ffmpeg
)
vpl_apply_common_options(voice_player_learn)
vpl_copy_runtime_dlls(voice_player_learn)

add_executable(test_av_utils
    tests/test_av_utils.cpp
)
target_include_directories(test_av_utils PRIVATE src)
target_link_libraries(test_av_utils PRIVATE vpl_ffmpeg)
vpl_apply_common_options(test_av_utils)
vpl_copy_runtime_dlls(test_av_utils)
