# Production Android sources, with only platform APIs replaced for portable CI.
add_executable(test_android_adpf adpf.cpp
    "${CMAKE_SOURCE_DIR}/src/yuzu_common/android/performance_hint.cpp")
target_include_directories(test_android_adpf BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs" "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(test_android_adpf PRIVATE __ANDROID__ NXEMU_ENABLE_ANDROID_ADPF=1)
target_link_libraries(test_android_adpf PRIVATE Threads::Threads)
nxemu_unit_test(test_android_adpf)

add_executable(test_android_refresh refresh.cpp
    "${CMAKE_SOURCE_DIR}/src/android/app/src/main/jni/android_render_window.cpp")
target_include_directories(test_android_refresh BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs" "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/android/app/src/main/jni")
foreach(mode strategy legacy missing rejected)
    add_test(NAME test_android_refresh_${mode} COMMAND test_android_refresh ${mode})
    set_tests_properties(test_android_refresh_${mode} PROPERTIES LABELS unit TIMEOUT 30)
endforeach()
foreach(name test_android_adpf test_android_refresh)
    target_compile_features(${name} PRIVATE cxx_std_20)
    target_compile_options(${name} PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
endforeach()
