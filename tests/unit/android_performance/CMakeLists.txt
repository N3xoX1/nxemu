# Production Android sources, with only platform APIs replaced for portable CI.
add_executable(test_android_refresh refresh.cpp
    "${CMAKE_SOURCE_DIR}/src/android/app/src/main/jni/android_render_window.cpp")
target_include_directories(test_android_refresh BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs" "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/android/app/src/main/jni")
target_compile_features(test_android_refresh PRIVATE cxx_std_20)
target_compile_options(test_android_refresh PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
foreach(mode strategy legacy missing rejected)
    add_test(NAME test_android_refresh_${mode} COMMAND test_android_refresh ${mode})
    set_tests_properties(test_android_refresh_${mode} PROPERTIES LABELS unit TIMEOUT 30)
endforeach()
