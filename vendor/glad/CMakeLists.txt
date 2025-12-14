set(SOURCES "")

if (USE_GLES)
    # GLES2 / EGL
    list(APPEND SOURCES
            include/glad/gles2.h
            include/glad/egl.h
            src/gles2.c
            src/egl.c
    )
else()
    # Desktop OpenGL
    list(APPEND SOURCES
            include/glad/gl.h
            src/gl.c
    )

    if (WIN32)
        list(APPEND SOURCES
                include/glad/wgl.h
                src/wgl.c
        )
    elseif(APPLE)
        # macOS uses CGL. GLAD provides no separate loader file
    else()
        list(APPEND SOURCES
                include/glad/glx.h
                src/glx.c
        )
    endif()
endif()

# build glad objects
add_library(glad_obj
        OBJECT
        ${SOURCES}
        )

target_include_directories(glad_obj
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        )

set_target_properties(glad_obj PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        )

# wrap the objects in a static library
add_library(glad STATIC $<TARGET_OBJECTS:glad_obj>)

# propagate include dirs via library target
target_include_directories(glad
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        )

set_target_properties(glad PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        )

# platform link dependencies
if (USE_GLES)
    # EGL is not a system lib on all platforms
    # on Linux, GLAD's egl.c needs dl for dlsym/dlopen
    if (UNIX AND NOT APPLE)
        target_link_libraries(glad PUBLIC dl)
    endif()
else()
    # Desktop GL
    if (WIN32)
        target_link_libraries(glad PUBLIC opengl32)
    elseif(APPLE)
        target_link_libraries(glad PUBLIC "-framework OpenGL")
    else()
        # Linux/Unix with GLX: need X11 for XOpenDisplay etc.
        find_package(X11 REQUIRED)
        target_link_libraries(glad PUBLIC X11::X11 dl)
    endif()
endif()
