add_library(SOIL2 OBJECT
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/common.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/common.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SOIL2/image_DXT.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SOIL2/image_helper.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SOIL2/pkm_helper.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SOIL2/SOIL2.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SOIL2/wfETC.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SOIL2/SOIL2_gl_bridge.c
        )

target_include_directories(SOIL2
        PUBLIC
        "${PROJECT_SOURCE_DIR}/vendor/glad/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        )

if (ANDROID)
    target_compile_definitions(SOIL2
            PRIVATE
            SOIL_PLATFORM_ANDROID=1
            )
elseif (APPLE)
    target_compile_definitions(SOIL2
            PRIVATE
            SOIL_PLATFORM_OSX=1
            )
elseif (WIN32)
    target_compile_definitions(SOIL2
            PRIVATE
            SOIL_PLATFORM_WIN32=1
            )
else()
    target_compile_definitions(SOIL2
            PRIVATE
            SOIL_X11_PLATFORM=1
            )
endif()

# GLES / EGL settings
if (USE_GLES)
    target_compile_definitions(SOIL2
            PRIVATE
            SOIL_GLES2
            )

    target_link_libraries(SOIL2
            PUBLIC
            ${CMAKE_DL_LIBS}
            )
else()
    target_compile_definitions(SOIL2
            PRIVATE
            SOIL_NO_EGL
            )
endif()

if (EMSCRIPTEN)
    target_compile_definitions(SOIL2
            PRIVATE
            SOIL_GLES2
            SOIL_NO_EGL
            )
elseif (NOT TARGET OpenGL::EGL)
    target_compile_definitions(SOIL2
            PRIVATE
            SOIL_NO_EGL
            )
endif()

set_target_properties(SOIL2 PROPERTIES
        FOLDER vendor/SOIL2
        )

target_link_libraries(SOIL2
        PRIVATE
        glad
        )
