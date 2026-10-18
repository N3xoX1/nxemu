add_library(glad STATIC
    src/glad.c
)
target_include_directories(glad PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if(WIN32)
    target_sources(glad PRIVATE src/glad_wgl.c)
    target_link_libraries(glad PUBLIC opengl32)
endif()
