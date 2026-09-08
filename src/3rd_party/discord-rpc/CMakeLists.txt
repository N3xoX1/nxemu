add_library(discord-rpc STATIC
    src/discord_rpc.cpp
    src/rpc_connection.cpp
    src/serialization.cpp
)

target_include_directories(discord-rpc
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rapidjson/include
)

if(WIN32)
    target_sources(discord-rpc PRIVATE
        src/connection_win.cpp
        src/discord_register_win.cpp
    )
    target_compile_definitions(discord-rpc PRIVATE DISCORD_WINDOWS)
    target_link_libraries(discord-rpc PUBLIC psapi advapi32)
elseif(APPLE)
    target_sources(discord-rpc PRIVATE
        src/connection_unix.cpp
        src/discord_register_osx.m
    )
    target_compile_definitions(discord-rpc PRIVATE DISCORD_OSX)
    target_link_libraries(discord-rpc PUBLIC "-framework AppKit")
else()
    target_sources(discord-rpc PRIVATE
        src/connection_unix.cpp
        src/discord_register_linux.cpp
    )
    target_compile_definitions(discord-rpc PRIVATE DISCORD_LINUX)
    target_link_libraries(discord-rpc PUBLIC Threads::Threads)
endif()

if(UNIX)
    target_compile_options(discord-rpc PRIVATE
        -Wno-unknown-pragmas
        -Wno-deprecated-declarations
    )
    # GCC 15+ diagnoses uninstantiated RapidJSON templates (const member assignment).
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 15)
        target_compile_options(discord-rpc PRIVATE -Wno-template-body)
    endif()
endif()
