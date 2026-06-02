add_library(common STATIC
    base64.cpp
    dynamic_library.cpp
    file.cpp
    json.cpp
    json_util.cpp
    path.cpp
    path_finder.cpp
    platform.cpp
    shell_open_url.cpp
    std_string.cpp
)

target_include_directories(common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}
)