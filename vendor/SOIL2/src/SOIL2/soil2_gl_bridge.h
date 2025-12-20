#ifndef SOIL2_GL_BRIDGE_H
#define SOIL2_GL_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(SOIL_GLES2) || defined(__ANDROID__) || defined(__EMSCRIPTEN__)
#include <glad/gles2.h>
#else
#include <glad/gl.h>
#endif

    typedef void* (*pm_soil_gl_resolver_t)(const char* name);

    void SOIL_GL_SetResolver(pm_soil_gl_resolver_t resolver);
    void* SOIL_GL_GetProcAddress(const char* proc);

#ifdef __cplusplus
}
#endif

#endif
