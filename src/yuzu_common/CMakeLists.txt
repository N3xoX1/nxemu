add_library(yuzu_common STATIC
    address_space.cpp
    algorithm.h
    alignment.h
    announce_multiplayer_room.h
    atomic_helpers.h
    atomic_ops.h
    bit_cast.h
    bit_field.h
    bit_set.h
    bit_util.h
    bounded_threadsafe_queue.h
    cityhash.cpp
    cityhash.h
    common_funcs.h
    common_types.h
    concepts.h
    container_hash.h
    div_ceil.h
    dynamic_library.cpp
    dynamic_library.h
    error.cpp
    error.h
    expected.h
    fiber.cpp
    fiber.h
    fixed_point.h
    free_region_manager.h
    fs/file.cpp
    fs/file.h
    fs/filesystem_interfaces.cpp
    fs/filesystem_interfaces.h
    fs/fs.cpp
    fs/fs.h
    fs/fs_paths.h
    fs/fs_types.h
    fs/fs_util.cpp
    fs/fs_util.h
    fs/path_util.cpp
    fs/path_util.h
    hash.h
    heap_tracker.cpp
    heap_tracker.h
    hex_util.cpp
    hex_util.h
    host_memory.cpp
    host_memory.h
    input.h
    intrusive_red_black_tree.h
    literals.h
    logging/backend.cpp
    logging/backend.h
    logging/filter.cpp
    logging/filter.h
    logging/formatter.h
    logging/log.h
    logging/log_entry.h
    logging/text_formatter.cpp
    logging/text_formatter.h
    lz4_compression.cpp
    lz4_compression.h
    make_unique_for_overwrite.h
    math_util.h
    microprofile.cpp
    microprofile.h
    multi_level_page_table.cpp
    multi_level_page_table.h
    overflow.h
    page_table.cpp
    page_table.h
    param_package.cpp
    param_package.h
    parent_of_member.h
    point.h
    quaternion.h
    range_map.h
    range_mutex.h
    range_sets.h
    range_sets.inc
    reader_writer_queue.h
    ring_buffer.h
    scope_exit.h
    scratch_buffer.h
    settings.cpp
    settings.h
    settings_common.cpp
    settings_common.h
    settings_enums.h
    settings_input.cpp
    settings_input.h
    settings_setting.h
    slot_vector.h
    socket_types.h
    spin_lock.cpp
    spin_lock.h
    stb.cpp
    stb.h
    steady_clock.cpp
    steady_clock.h
    stream.cpp
    stream.h
    string_util.cpp
    string_util.h
    swap.h
    thread.cpp
    thread.h
    thread_worker.h
    threadsafe_queue.h
    time_zone.cpp
    time_zone.h
    tiny_mt.h
    tree.h
    typed_address.h
    unique_function.h
    uuid.cpp
    uuid.h
    vector_math.h
    virtual_buffer.cpp
    virtual_buffer.h
    wall_clock.cpp
    wall_clock.h
    yuzu_assert.cpp
    yuzu_assert.h
    zstd_compression.cpp
    zstd_compression.h
)

if (WIN32)
  target_sources(yuzu_common PRIVATE
    windows/timer_resolution.cpp
    windows/timer_resolution.h
  )
  target_link_libraries(yuzu_common PRIVATE ntdll)
endif()

if(ANDROID)
    target_sources(yuzu_common PRIVATE
        android/java_bridge.cpp
        android/java_bridge.h
        fs/fs_android.cpp
        fs/fs_android.h
    )
endif()

if(ARCHITECTURE_x86_64)
    target_sources(yuzu_common
        PRIVATE
            x64/cpu_detect.cpp
            x64/cpu_detect.h
            x64/cpu_wait.cpp
            x64/cpu_wait.h
            x64/native_clock.cpp
            x64/native_clock.h
            x64/rdtsc.cpp
            x64/rdtsc.h
            x64/xbyak_abi.h
            x64/xbyak_util.h
    )
    target_link_libraries(yuzu_common PRIVATE xbyak::xbyak)
endif()

set_source_files_properties(stb.cpp PROPERTIES COMPILE_OPTIONS "-Wno-implicit-fallthrough;-Wno-missing-declarations;-Wno-missing-field-initializers")

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  target_compile_options(yuzu_common PRIVATE
    -fsized-deallocation
    -Werror=unreachable-code-aggressive
  )
  target_compile_definitions(yuzu_common PRIVATE
    # Clang 14 and earlier have errors when explicitly instantiating Settings::Setting
    $<$<VERSION_LESS:$<CXX_COMPILER_VERSION>,15>:CANNOT_EXPLICITLY_INSTANTIATE>
  )
endif()

# So #include "yuzu_common/..." resolves to src/yuzu_common/...
target_include_directories(yuzu_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(yuzu_common PUBLIC Boost::context Boost::headers fmt::fmt microprofile stb::headers Threads::Threads)
target_link_libraries(yuzu_common PRIVATE lz4::lz4 zstd::zstd)

create_target_directory_groups(yuzu_common)