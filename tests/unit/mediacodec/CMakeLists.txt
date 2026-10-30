if(ANDROID)
    return()
endif()
add_executable(test_mediacodec mediacodec.cpp
    "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/host1x/ffmpeg/ffmpeg.cpp"
    "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/host1x/codecs/codec.cpp")
target_include_directories(test_mediacodec BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs" "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/src/nxemu-video"
    "${CMAKE_SOURCE_DIR}/external/boost" "${CMAKE_SOURCE_DIR}/external/fmt/include")
target_compile_definitions(test_mediacodec PRIVATE __ANDROID__ NXEMU_ENABLE_ANDROID_MEDIACODEC=1)
target_compile_features(test_mediacodec PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(test_mediacodec PRIVATE /UNDEBUG "/FI${CMAKE_CURRENT_SOURCE_DIR}/hooks.h")
else()
    target_compile_options(test_mediacodec PRIVATE -UNDEBUG -include "${CMAKE_CURRENT_SOURCE_DIR}/hooks.h")
    target_compile_options(test_mediacodec PRIVATE -O1 -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(test_mediacodec PRIVATE -fsanitize=address,undefined)
endif()
target_link_libraries(test_mediacodec PRIVATE nxemu_ffmpeg nxemu_test_support fmt::fmt Threads::Threads)
nxemu_unit_test(test_mediacodec)

# No Android defines or API substitutes: execute the unchanged software selection.
add_executable(test_ffmpeg_software software.cpp
    "${CMAKE_SOURCE_DIR}/src/yuzu_video_core/host1x/ffmpeg/ffmpeg.cpp")
target_include_directories(test_ffmpeg_software PRIVATE "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/nxemu-video" "${CMAKE_SOURCE_DIR}/external/boost"
    "${CMAKE_SOURCE_DIR}/external/fmt/include")
target_compile_features(test_ffmpeg_software PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(test_ffmpeg_software PRIVATE /UNDEBUG)
else()
    target_compile_options(test_ffmpeg_software PRIVATE -UNDEBUG -O1 -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(test_ffmpeg_software PRIVATE -fsanitize=address,undefined)
endif()
target_link_libraries(test_ffmpeg_software PRIVATE nxemu_ffmpeg nxemu_test_support fmt::fmt Threads::Threads)
nxemu_unit_test(test_ffmpeg_software)
