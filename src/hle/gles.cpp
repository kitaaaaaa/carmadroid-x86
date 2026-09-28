// OpenGL ES 1.1 (common profile) -> desktop OpenGL compatibility profile.
// Nearly 1:1; the main work is translating guest pointers and loading >1.1 entry points.
#include "../platform.h"
#include "hle_common.h"
#include <windows.h>
#include <GL/gl.h>
#include <mutex>

#ifndef APIENTRY
#define APIENTRY __stdcall
#endif

namespace {

enum : GLenum {
    GL_ARRAY_BUFFER = 0x8892,
    GL_ELEMENT_ARRAY_BUFFER = 0x8893,
    GL_ARRAY_BUFFER_BINDING = 0x8894,
    GL_ELEMENT_ARRAY_BUFFER_BINDING = 0x8895,
};

using PFN_ActiveTexture = void(APIENTRY*)(GLenum);
using PFN_ClientActiveTexture = void(APIENTRY*)(GLenum);
using PFN_CompressedTexImage2D = void(APIENTRY*)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void*);
using PFN_BindBuffer = void(APIENTRY*)(GLenum, GLuint);
using PFN_BufferData = void(APIENTRY*)(GLenum, ptrdiff_t, const void*, GLenum);
using PFN_BufferSubData = void(APIENTRY*)(GLenum, ptrdiff_t, ptrdiff_t, const void*);
using PFN_GenBuffers = void(APIENTRY*)(GLsizei, GLuint*);
using PFN_DeleteBuffers = void(APIENTRY*)(GLsizei, const GLuint*);
using PFN_BlendEquation = void(APIENTRY*)(GLenum);

PFN_ActiveTexture pActiveTexture;
PFN_ClientActiveTexture pClientActiveTexture;
PFN_CompressedTexImage2D pCompressedTexImage2D;
PFN_BindBuffer pBindBuffer;
PFN_BufferData pBufferData;
PFN_BufferSubData pBufferSubData;
PFN_GenBuffers pGenBuffers;
PFN_DeleteBuffers pDeleteBuffers;
PFN_BlendEquation pBlendEquation;

std::once_flag g_load_once;
void load_procs() {
    std::call_once(g_load_once, [] {
        pActiveTexture = (PFN_ActiveTexture)platform::gl_proc("glActiveTexture");
        pClientActiveTexture = (PFN_ClientActiveTexture)platform::gl_proc("glClientActiveTexture");
        pCompressedTexImage2D = (PFN_CompressedTexImage2D)platform::gl_proc("glCompressedTexImage2D");
        pBindBuffer = (PFN_BindBuffer)platform::gl_proc("glBindBuffer");
        pBufferData = (PFN_BufferData)platform::gl_proc("glBufferData");
        pBufferSubData = (PFN_BufferSubData)platform::gl_proc("glBufferSubData");
        pGenBuffers = (PFN_GenBuffers)platform::gl_proc("glGenBuffers");
        pDeleteBuffers = (PFN_DeleteBuffers)platform::gl_proc("glDeleteBuffers");
        pBlendEquation = (PFN_BlendEquation)platform::gl_proc("glBlendEquation");
        if (!pBindBuffer || !pCompressedTexImage2D) fatal("OpenGL 1.5 entry points missing");
    });
}

GLuint g_array_buffer = 0, g_element_buffer = 0;

float F(Cpu& c, int i) { u32 v = c.r(i); float f; memcpy(&f, &v, 4); return f; }
float Fa(Cpu& c, int i) { u32 v = c.arg(i); float f; memcpy(&f, &v, 4); return f; }
// Attribute pointer: VBO offset if a buffer is bound, else guest memory.
const void* attrib_ptr(u32 g) { return g_array_buffer ? (const void*)(uintptr_t)g : (const void*)mem::ptr(g); }
const void* index_ptr(u32 g) { return g_element_buffer ? (const void*)(uintptr_t)g : (const void*)mem::ptr(g); }

u32 g_str_vendor, g_str_renderer, g_str_version, g_str_ext;

}  // namespace

// --- state ---------------------------------------------------------------
HLE(glEnable) { glEnable(c.r(0)); }
HLE(glDisable) { glDisable(c.r(0)); }
HLE(glIsEnabled) { c.ret(glIsEnabled(c.r(0))); }
HLE(glEnableClientState) { glEnableClientState(c.r(0)); }
HLE(glDisableClientState) { glDisableClientState(c.r(0)); }
HLE(glHint) { glHint(c.r(0), c.r(1)); }
HLE(glFlush) { glFlush(); }
HLE(glFinish) { glFinish(); }
HLE(glGetError) { c.ret(glGetError()); }
HLE(glGetIntegerv) {
    GLenum p = c.r(0);
    if (p == GL_ARRAY_BUFFER_BINDING) { mem::w32(c.r(1), g_array_buffer); return; }
    if (p == GL_ELEMENT_ARRAY_BUFFER_BINDING) { mem::w32(c.r(1), g_element_buffer); return; }
    glGetIntegerv(p, mem::ptr<GLint>(c.r(1)));
}
HLE(glGetFloatv) { glGetFloatv(c.r(0), mem::ptr<GLfloat>(c.r(1))); }
HLE(glGetString) {
    load_procs();
    if (!g_str_vendor) {
        g_str_vendor = mem::strdup("carmadroid");
        g_str_renderer = mem::strdup((const char*)glGetString(GL_RENDERER));
        g_str_version = mem::strdup("OpenGL ES-CM 1.1");
        // Only what the engine knows how to use. It checks these desktop names.
        g_str_ext = mem::strdup(
            "GL_ARB_vertex_buffer_object GL_ARB_texture_compression GL_EXT_texture_compression_s3tc "
            "GL_EXT_bgra GL_ARB_texture_env_crossbar GL_EXT_texture_lod_bias GL_OES_blend_subtract "
            "GL_OES_texture_npot GL_OES_compressed_paletted_texture");
    }
    switch (c.r(0)) {
        case GL_VENDOR: c.ret(g_str_vendor); break;
        case GL_RENDERER: c.ret(g_str_renderer); break;
        case GL_VERSION: c.ret(g_str_version); break;
        case GL_EXTENSIONS: c.ret(g_str_ext); break;
        default: c.ret(0);
    }
}

// --- framebuffer ---------------------------------------------------------
HLE(glViewport) { glViewport((GLint)c.r(0), (GLint)c.r(1), (GLsizei)c.r(2), (GLsizei)c.r(3)); }
HLE(glScissor) { glScissor((GLint)c.r(0), (GLint)c.r(1), (GLsizei)c.r(2), (GLsizei)c.r(3)); }
HLE(glClear) { glClear(c.r(0)); }
HLE(glClearColor) { glClearColor(F(c, 0), F(c, 1), F(c, 2), F(c, 3)); }
HLE(glClearDepthf) { glClearDepth(F(c, 0)); }
HLE(glClearStencil) { glClearStencil((GLint)c.r(0)); }
HLE(glColorMask) { glColorMask((GLboolean)c.r(0), (GLboolean)c.r(1), (GLboolean)c.r(2), (GLboolean)c.r(3)); }
HLE(glDepthMask) { glDepthMask((GLboolean)c.r(0)); }
HLE(glDepthFunc) { glDepthFunc(c.r(0)); }
HLE(glDepthRangef) { glDepthRange(F(c, 0), F(c, 1)); }
HLE(glStencilFunc) { glStencilFunc(c.r(0), (GLint)c.r(1), c.r(2)); }
HLE(glStencilOp) { glStencilOp(c.r(0), c.r(1), c.r(2)); }
HLE(glStencilMask) { glStencilMask(c.r(0)); }
HLE(glBlendFunc) { glBlendFunc(c.r(0), c.r(1)); }
HLE(glBlendEquation) { load_procs(); if (pBlendEquation) pBlendEquation(c.r(0)); }
HLE(glAlphaFunc) { glAlphaFunc(c.r(0), F(c, 1)); }
HLE(glCullFace) { glCullFace(c.r(0)); }
HLE(glFrontFace) { glFrontFace(c.r(0)); }
HLE(glShadeModel) { glShadeModel(c.r(0)); }
HLE(glLineWidth) { glLineWidth(F(c, 0)); }
HLE(glPolygonOffset) { glPolygonOffset(F(c, 0), F(c, 1)); }
HLE(glPixelStorei) { glPixelStorei(c.r(0), (GLint)c.r(1)); }
HLE(glReadPixels) {
    glReadPixels((GLint)c.r(0), (GLint)c.r(1), (GLsizei)c.r(2), (GLsizei)c.r(3), c.arg(4), c.arg(5), mem::ptr(c.arg(6)));
}

// --- matrices ------------------------------------------------------------
HLE(glMatrixMode) { glMatrixMode(c.r(0)); }
HLE(glLoadIdentity) { glLoadIdentity(); }
HLE(glLoadMatrixf) { glLoadMatrixf(mem::ptr<GLfloat>(c.r(0))); }
HLE(glMultMatrixf) { glMultMatrixf(mem::ptr<GLfloat>(c.r(0))); }
HLE(glPushMatrix) { glPushMatrix(); }
HLE(glPopMatrix) { glPopMatrix(); }
HLE(glTranslatef) { glTranslatef(F(c, 0), F(c, 1), F(c, 2)); }
HLE(glScalef) { glScalef(F(c, 0), F(c, 1), F(c, 2)); }
HLE(glRotatef) { glRotatef(F(c, 0), F(c, 1), F(c, 2), F(c, 3)); }
HLE(glOrthof) { glOrtho(F(c, 0), F(c, 1), F(c, 2), F(c, 3), Fa(c, 4), Fa(c, 5)); }
HLE(glFrustumf) { glFrustum(F(c, 0), F(c, 1), F(c, 2), F(c, 3), Fa(c, 4), Fa(c, 5)); }

// --- fixed-function lighting / fog / material ------------------------------
HLE(glColor4f) { glColor4f(F(c, 0), F(c, 1), F(c, 2), F(c, 3)); }
HLE(glColor4ub) { glColor4ub((GLubyte)c.r(0), (GLubyte)c.r(1), (GLubyte)c.r(2), (GLubyte)c.r(3)); }
HLE(glNormal3f) { glNormal3f(F(c, 0), F(c, 1), F(c, 2)); }
HLE(glFogf) { glFogf(c.r(0), F(c, 1)); }
HLE(glFogfv) { glFogfv(c.r(0), mem::ptr<GLfloat>(c.r(1))); }
HLE(glFogx) { glFogi(c.r(0), (GLint)c.r(1)); }
HLE(glLightf) { glLightf(c.r(0), c.r(1), F(c, 2)); }
HLE(glLightfv) { glLightfv(c.r(0), c.r(1), mem::ptr<GLfloat>(c.r(2))); }
HLE(glLightModelf) { glLightModelf(c.r(0), F(c, 1)); }
HLE(glLightModelfv) { glLightModelfv(c.r(0), mem::ptr<GLfloat>(c.r(1))); }
HLE(glMaterialf) { glMaterialf(c.r(0), c.r(1), F(c, 2)); }
HLE(glMaterialfv) { glMaterialfv(c.r(0), c.r(1), mem::ptr<GLfloat>(c.r(2))); }

// --- textures ------------------------------------------------------------
HLE(glGenTextures) { glGenTextures((GLsizei)c.r(0), mem::ptr<GLuint>(c.r(1))); }
HLE(glDeleteTextures) { glDeleteTextures((GLsizei)c.r(0), mem::ptr<GLuint>(c.r(1))); }
HLE(glBindTexture) { glBindTexture(c.r(0), c.r(1)); }
HLE(glActiveTexture) { load_procs(); pActiveTexture(c.r(0)); }
HLE(glClientActiveTexture) { load_procs(); pClientActiveTexture(c.r(0)); }
HLE(glTexParameteri) { glTexParameteri(c.r(0), c.r(1), (GLint)c.r(2)); }
HLE(glTexParameterx) { glTexParameteri(c.r(0), c.r(1), (GLint)c.r(2)); }  // enum params are not scaled
HLE(glTexParameterf) { glTexParameterf(c.r(0), c.r(1), F(c, 2)); }
HLE(glTexEnvi) { glTexEnvi(c.r(0), c.r(1), (GLint)c.r(2)); }
HLE(glTexEnvx) { glTexEnvi(c.r(0), c.r(1), (GLint)c.r(2)); }
HLE(glTexEnvf) { glTexEnvf(c.r(0), c.r(1), F(c, 2)); }
HLE(glTexEnvfv) { glTexEnvfv(c.r(0), c.r(1), mem::ptr<GLfloat>(c.r(2))); }
HLE(glTexImage2D) {
    // (target, level, internalformat, width, height, border, format, type, pixels)
    glTexImage2D(c.r(0), (GLint)c.r(1), (GLint)c.r(2), (GLsizei)c.r(3), (GLsizei)c.arg(4), (GLint)c.arg(5), c.arg(6),
                 c.arg(7), mem::ptr(c.arg(8)));
}
HLE(glTexSubImage2D) {
    // (target, level, xoffset, yoffset, width, height, format, type, pixels)
    glTexSubImage2D(c.r(0), (GLint)c.r(1), (GLint)c.r(2), (GLint)c.r(3), (GLsizei)c.arg(4), (GLsizei)c.arg(5), c.arg(6),
                    c.arg(7), mem::ptr(c.arg(8)));
}
HLE(glCompressedTexImage2D) {
    // (target, level, internalformat, width, height, border, imageSize, data)
    load_procs();
    pCompressedTexImage2D(c.r(0), (GLint)c.r(1), c.r(2), (GLsizei)c.r(3), (GLsizei)c.arg(4), (GLint)c.arg(5),
                          (GLsizei)c.arg(6), mem::ptr(c.arg(7)));
}
HLE(glCopyTexImage2D) {
    glCopyTexImage2D(c.r(0), (GLint)c.r(1), c.r(2), (GLint)c.r(3), (GLint)c.arg(4), (GLsizei)c.arg(5),
                     (GLsizei)c.arg(6), (GLint)c.arg(7));
}
HLE(glCopyTexSubImage2D) {
    glCopyTexSubImage2D(c.r(0), (GLint)c.r(1), (GLint)c.r(2), (GLint)c.r(3), (GLint)c.arg(4), (GLint)c.arg(5),
                        (GLsizei)c.arg(6), (GLsizei)c.arg(7));
}

// --- buffers -------------------------------------------------------------
HLE(glGenBuffers) { load_procs(); pGenBuffers((GLsizei)c.r(0), mem::ptr<GLuint>(c.r(1))); }
HLE(glDeleteBuffers) {
    load_procs();
    const GLuint* ids = mem::ptr<GLuint>(c.r(1));
    for (u32 i = 0; i < c.r(0); i++) {
        if (ids[i] == g_array_buffer) g_array_buffer = 0;
        if (ids[i] == g_element_buffer) g_element_buffer = 0;
    }
    pDeleteBuffers((GLsizei)c.r(0), ids);
}
HLE(glBindBuffer) {
    load_procs();
    if (c.r(0) == GL_ARRAY_BUFFER) g_array_buffer = c.r(1);
    else if (c.r(0) == GL_ELEMENT_ARRAY_BUFFER) g_element_buffer = c.r(1);
    pBindBuffer(c.r(0), c.r(1));
}
HLE(glBufferData) { load_procs(); pBufferData(c.r(0), (s32)c.r(1), mem::ptr(c.r(2)), c.r(3)); }
HLE(glBufferSubData) { load_procs(); pBufferSubData(c.r(0), (s32)c.r(1), (s32)c.r(2), mem::ptr(c.r(3))); }

// --- vertex arrays + drawing ----------------------------------------------
HLE(glVertexPointer) { glVertexPointer((GLint)c.r(0), c.r(1), (GLsizei)c.r(2), attrib_ptr(c.r(3))); }
HLE(glColorPointer) { glColorPointer((GLint)c.r(0), c.r(1), (GLsizei)c.r(2), attrib_ptr(c.r(3))); }
HLE(glTexCoordPointer) { glTexCoordPointer((GLint)c.r(0), c.r(1), (GLsizei)c.r(2), attrib_ptr(c.r(3))); }
HLE(glNormalPointer) { glNormalPointer(c.r(0), (GLsizei)c.r(1), attrib_ptr(c.r(2))); }
HLE(glDrawArrays) { glDrawArrays(c.r(0), (GLint)c.r(1), (GLsizei)c.r(2)); }
HLE(glDrawElements) { glDrawElements(c.r(0), (GLsizei)c.r(1), c.r(2), index_ptr(c.r(3))); }
