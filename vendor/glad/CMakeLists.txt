add_library(glad_obj OBJECT)

target_include_directories(glad_obj
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_sources(glad_obj
        PRIVATE
        $<$<BOOL:ENABLE_GLES>:
        src/gles2.c
        src/egl.c
        >
        $<$<NOT:$<BOOL:ENABLE_GLES>>:
        src/gl.c
        >
)

if (WIN32)
    target_sources(glad_obj PRIVATE
            $<$<NOT:$<BOOL:ENABLE_GLES>>:src/wgl.c>
    )
elseif(UNIX AND NOT APPLE)
    target_sources(glad_obj PRIVATE
            $<$<NOT:$<BOOL:ENABLE_GLES>>:src/glx.c>
    )
endif()

add_library(glad STATIC $<TARGET_OBJECTS:glad_obj>)

target_include_directories(glad
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set_target_properties(glad PROPERTIES
        POSITION_INDEPENDENT_CODE ON
)

if (UNIX AND NOT APPLE)
    target_link_libraries(glad PUBLIC dl)
endif()
