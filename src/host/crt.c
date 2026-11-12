/* CRT display: the Lola Retro shader chain on SDL's OpenGL renderer.
 * See crt.h and docs/16-crt.md. */
#include "crt.h"
#include "crt_shaders.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <string.h>

/* ---- GL entry points (the renderer's context) ---- */
#define GLFN(ret, name, args) typedef ret (APIENTRY *name##_t) args; static name##_t p_##name;
GLFN(GLuint, glCreateShader, (GLenum))
GLFN(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *))
GLFN(void, glCompileShader, (GLuint))
GLFN(void, glGetShaderiv, (GLuint, GLenum, GLint *))
GLFN(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))
GLFN(void, glDeleteShader, (GLuint))
GLFN(GLuint, glCreateProgram, (void))
GLFN(void, glAttachShader, (GLuint, GLuint))
GLFN(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *))
GLFN(void, glLinkProgram, (GLuint))
GLFN(void, glGetProgramiv, (GLuint, GLenum, GLint *))
GLFN(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))
GLFN(void, glDeleteProgram, (GLuint))
GLFN(void, glUseProgram, (GLuint))
GLFN(GLint, glGetUniformLocation, (GLuint, const GLchar *))
GLFN(void, glGetActiveUniform, (GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *))
GLFN(void, glUniform1i, (GLint, GLint))
GLFN(void, glUniform1f, (GLint, GLfloat))
GLFN(void, glUniform2f, (GLint, GLfloat, GLfloat))
GLFN(void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *))
GLFN(void, glGenFramebuffers, (GLsizei, GLuint *))
GLFN(void, glDeleteFramebuffers, (GLsizei, const GLuint *))
GLFN(void, glBindFramebuffer, (GLenum, GLuint))
GLFN(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))
GLFN(GLenum, glCheckFramebufferStatus, (GLenum))
GLFN(void, glGenTextures, (GLsizei, GLuint *))
GLFN(void, glDeleteTextures, (GLsizei, const GLuint *))
GLFN(void, glBindTexture, (GLenum, GLuint))
GLFN(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))
GLFN(void, glTexParameteri, (GLenum, GLenum, GLint))
GLFN(void, glTexParameterfv, (GLenum, GLenum, const GLfloat *))
GLFN(void, glActiveTexture, (GLenum))
GLFN(void, glViewport, (GLint, GLint, GLsizei, GLsizei))
GLFN(void, glGenBuffers, (GLsizei, GLuint *))
GLFN(void, glBindBuffer, (GLenum, GLuint))
GLFN(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum))
GLFN(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))
GLFN(void, glEnableVertexAttribArray, (GLuint))
GLFN(void, glDisableVertexAttribArray, (GLuint))
GLFN(void, glGenVertexArrays, (GLsizei, GLuint *))
GLFN(void, glBindVertexArray, (GLuint))
GLFN(void, glDrawArrays, (GLenum, GLint, GLsizei))
GLFN(void, glGetIntegerv, (GLenum, GLint *))
GLFN(void, glEnable, (GLenum))
GLFN(void, glDisable, (GLenum))
GLFN(GLboolean, glIsEnabled, (GLenum))
GLFN(GLenum, glGetError, (void))
GLFN(const GLubyte *, glGetString, (GLenum))

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#define GL_HALF_FLOAT 0x140B
#endif
#ifndef GL_VERTEX_ARRAY_BINDING
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#endif
#ifndef GL_CLAMP_TO_BORDER
#define GL_CLAMP_TO_BORDER 0x812D
#endif

static int load_gl(void)
{
#define L(name) if (!(p_##name = (name##_t)SDL_GL_GetProcAddress(#name))) { SDL_Log("crt: no %s", #name); return 0; }
    L(glCreateShader) L(glShaderSource) L(glCompileShader) L(glGetShaderiv) L(glGetShaderInfoLog) L(glDeleteShader)
    L(glCreateProgram) L(glAttachShader) L(glBindAttribLocation) L(glLinkProgram) L(glGetProgramiv)
    L(glGetProgramInfoLog) L(glDeleteProgram) L(glUseProgram) L(glGetUniformLocation) L(glGetActiveUniform)
    L(glUniform1i) L(glUniform1f) L(glUniform2f) L(glUniformMatrix4fv)
    L(glGenFramebuffers) L(glDeleteFramebuffers) L(glBindFramebuffer) L(glFramebufferTexture2D) L(glCheckFramebufferStatus)
    L(glGenTextures) L(glDeleteTextures) L(glBindTexture) L(glTexImage2D) L(glTexParameteri) L(glTexParameterfv)
    L(glActiveTexture) L(glViewport) L(glGenBuffers) L(glBindBuffer) L(glBufferData) L(glVertexAttribPointer)
    L(glEnableVertexAttribArray) L(glDisableVertexAttribArray) L(glGenVertexArrays) L(glBindVertexArray)
    L(glDrawArrays) L(glGetIntegerv) L(glEnable) L(glDisable) L(glIsEnabled) L(glGetError) L(glGetString)
#undef L
    return 1;
}

/* ---- the chain (kodi/lola-retro/*.glslp: the same for every preset) ---- */
enum { SC_SOURCE, SC_ABSOLUTE, SC_VIEWPORT };
static const struct {
    int sx_type; float sx; int sy_type; float sy;
    int linear;                     /* how this pass samples its Source */
    int border;                     /* clamp to border (black) instead of edge */
    int fp;                         /* float target */
    const char *alias;
} chain[CRT_PASSES] = {
    { SC_SOURCE, 1, SC_SOURCE, 1, 0, 0, 1, NULL },              /* native */
    { SC_ABSOLUTE, 1280, SC_SOURCE, 1, 0, 1, 1, "LR_Signal" },  /* encode */
    { SC_SOURCE, 1, SC_SOURCE, 1, 1, 1, 1, "LR_Decoded" },      /* decode */
    { SC_SOURCE, 0.125f, SC_SOURCE, 0.5f, 1, 0, 1, NULL },      /* glow-h */
    { SC_SOURCE, 1, SC_SOURCE, 1, 1, 0, 1, "LR_Glow" },         /* glow-v */
    { SC_ABSOLUTE, 8192, SC_SOURCE, 2.05f, 1, 0, 1, "LR_Beam" },/* beam */
    { SC_VIEWPORT, 1, SC_VIEWPORT, 1, 1, 0, 1, NULL },          /* crt */
    { SC_VIEWPORT, 1, SC_VIEWPORT, 1, 0, 0, 0, NULL },          /* present: into the caller's texture */
};

static struct {
    int state;                      /* 0 untried, 1 ready, -1 failed */
    GLuint prog[CRT_PASSES], copy_prog;
    GLuint fbo, vbo, vao;
    GLuint orig;                    /* the frame, exact size */
    int orig_w, orig_h;
    GLuint tex[CRT_PASSES];         /* outputs of passes 0..6 */
    int w[CRT_PASSES], h[CRT_PASSES];
    int vp_w, vp_h;
    char pname[96][32];             /* parameters: name, value */
    float pval[96];
    int np;
} C;

static GLuint compile(const char *src, const char *stage_define, GLenum type)
{
    /* the source starts with "#version 330": the define goes after that line */
    const char *nl = strchr(src, '\n');
    char ver[64];
    size_t vn = nl ? (size_t)(nl - src + 1) : 0;
    if (vn >= sizeof ver) vn = 0;
    memcpy(ver, src, vn); ver[vn] = 0;
    const char *parts[3] = { ver, stage_define, src + vn };
    GLuint s = p_glCreateShader(type);
    p_glShaderSource(s, 3, parts, NULL);
    p_glCompileShader(s);
    GLint ok = 0;
    p_glGetShaderiv(s, 0x8B81 /* GL_COMPILE_STATUS */, &ok);
    if (!ok) {
        char log[2048]; GLsizei n = 0;
        p_glGetShaderInfoLog(s, sizeof log, &n, log);
        SDL_Log("crt: shader error: %.*s", (int)n, log);
        p_glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint program(const char *src)
{
    GLuint vs = compile(src, "#define VERTEX\n", 0x8B31), fs = compile(src, "#define FRAGMENT\n", 0x8B30);
    if (!vs || !fs) return 0;
    GLuint p = p_glCreateProgram();
    p_glAttachShader(p, vs); p_glAttachShader(p, fs);
    p_glBindAttribLocation(p, 0, "VertexCoord");
    p_glBindAttribLocation(p, 1, "TexCoord");
    p_glLinkProgram(p);
    p_glDeleteShader(vs); p_glDeleteShader(fs);
    GLint ok = 0;
    p_glGetProgramiv(p, 0x8B82 /* GL_LINK_STATUS */, &ok);
    if (!ok) {
        char log[2048]; GLsizei n = 0;
        p_glGetProgramInfoLog(p, sizeof log, &n, log);
        SDL_Log("crt: link error: %.*s", (int)n, log);
        p_glDeleteProgram(p);
        return 0;
    }
    return p;
}

/* "#pragma parameter NAME "text" default min max step" in every pass */
static void collect_params(void)
{
    C.np = 0;
    for (int k = 0; k < CRT_PASSES; k++)
        for (const char *s = crt_pass_src[k]; (s = strstr(s, "#pragma parameter ")); s++) {
            char name[32]; float def;
            const char *q = s + 18;
            if (SDL_sscanf(q, "%31s", name) != 1) continue;
            const char *e = strchr(q, '"');
            e = e ? strchr(e + 1, '"') : NULL;
            if (!e || SDL_sscanf(e + 1, "%f", &def) != 1) continue;
            int have = 0;
            for (int i = 0; i < C.np; i++) have |= !strcmp(C.pname[i], name);
            if (!have && C.np < 96) { SDL_strlcpy(C.pname[C.np], name, 32); C.pval[C.np++] = def; }
        }
}

static void set_param(const char *name, float v)
{
    for (int i = 0; i < C.np; i++) if (!strcmp(C.pname[i], name)) { C.pval[i] = v; return; }
    if (C.np < 96) { SDL_strlcpy(C.pname[C.np], name, 32); C.pval[C.np++] = v; }
}

static const char copy_src[] =
    "#version 330\n"
    "#if defined(VERTEX)\n"
    "in vec4 VertexCoord; in vec2 TexCoord; out vec2 vTex; uniform vec2 Scale;\n"
    "void main() { gl_Position = vec4(VertexCoord.xy * 2.0 - 1.0, 0.0, 1.0); vTex = TexCoord * Scale; }\n"
    "#elif defined(FRAGMENT)\n"
    "in vec2 vTex; out vec4 FragColor; uniform sampler2D Texture;\n"
    "void main() { FragColor = vec4(texture(Texture, vTex).rgb, 1.0); }\n"
    "#endif\n";

static int init(void)
{
    if (!load_gl()) return 0;
    const GLubyte *ver = p_glGetString(0x1F02 /* GL_VERSION */);
    SDL_Log("crt: OpenGL %s", ver ? (const char *)ver : "?");
    for (int k = 0; k < CRT_PASSES; k++)
        if (!(C.prog[k] = program(crt_pass_src[k]))) { SDL_Log("crt: pass %d failed", k); return 0; }
    if (!(C.copy_prog = program(copy_src))) return 0;
    collect_params();
    p_glGenFramebuffers(1, &C.fbo);
    p_glGenBuffers(1, &C.vbo);
    p_glGenVertexArrays(1, &C.vao);
    /* a quad: VertexCoord (x, y, 0, 1) and TexCoord (u, v) over 0..1 */
    static const float quad[] = {
        0, 0, 0, 1, 0, 0,   1, 0, 0, 1, 1, 0,   0, 1, 0, 1, 0, 1,   1, 1, 0, 1, 1, 1,
    };
    GLint vao_was = 0, buf_was = 0;
    p_glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao_was);
    p_glGetIntegerv(0x8894 /* GL_ARRAY_BUFFER_BINDING */, &buf_was);
    p_glBindVertexArray(C.vao);
    p_glBindBuffer(0x8892 /* GL_ARRAY_BUFFER */, C.vbo);
    p_glBufferData(0x8892, sizeof quad, quad, 0x88E4 /* GL_STATIC_DRAW */);
    p_glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)0);
    p_glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)(4 * sizeof(float)));
    p_glEnableVertexAttribArray(0);
    p_glEnableVertexAttribArray(1);
    p_glBindVertexArray((GLuint)vao_was);
    p_glBindBuffer(0x8892, (GLuint)buf_was);
    return 1;
}

static void make_tex(GLuint *t, int w, int h, int fp)
{
    if (!*t) p_glGenTextures(1, t);
    p_glBindTexture(GL_TEXTURE_2D, *t);
    p_glTexImage2D(GL_TEXTURE_2D, 0, fp ? GL_RGBA16F : GL_RGBA8, w, h, 0, GL_RGBA, fp ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, NULL);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static void sampling(GLuint t, int linear, int border)
{
    p_glBindTexture(GL_TEXTURE_2D, t);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    GLint wrap = border ? GL_CLAMP_TO_BORDER : GL_CLAMP_TO_EDGE;
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    static const float black[4] = { 0, 0, 0, 0 };
    if (border) p_glTexParameterfv(GL_TEXTURE_2D, 0x1004 /* GL_TEXTURE_BORDER_COLOR */, black);
}

static int attach(GLuint t)
{
    p_glBindFramebuffer(GL_FRAMEBUFFER, C.fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    return p_glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

int crt_supported(SDL_Renderer *ren)
{
    const char *name = ren ? SDL_GetRendererName(ren) : NULL;
    return name && !strcmp(name, "opengl");
}

int crt_render(SDL_Renderer *ren, SDL_Texture *src, int src_w, int src_h, SDL_Texture *dst,
               const crt_settings *set, unsigned frame)
{
    if (!crt_supported(ren) || !src || !dst || C.state < 0) return 0;
    SDL_FlushRenderer(ren);
    if (C.state == 0) C.state = init() ? 1 : -1;
    if (C.state < 0) return 0;
    SDL_PropertiesID sp = SDL_GetTextureProperties(src), dp = SDL_GetTextureProperties(dst);
    GLuint src_gl = (GLuint)SDL_GetNumberProperty(sp, SDL_PROP_TEXTURE_OPENGL_TEXTURE_NUMBER, 0);
    GLuint dst_gl = (GLuint)SDL_GetNumberProperty(dp, SDL_PROP_TEXTURE_OPENGL_TEXTURE_NUMBER, 0);
    float stex_w = (float)SDL_GetNumberProperty(sp, SDL_PROP_TEXTURE_OPENGL_TEX_W_FLOAT, 1.0);   /* used part of the GL texture */
    float stex_h = (float)SDL_GetNumberProperty(sp, SDL_PROP_TEXTURE_OPENGL_TEX_H_FLOAT, 1.0);
    if (!src_gl || !dst_gl) return 0;
    float tw, th;
    SDL_GetTextureSize(src, &tw, &th);
    SDL_GetTextureSize(dst, &(float){0}, &(float){0});
    float vw_f = 0, vh_f = 0;
    SDL_GetTextureSize(dst, &vw_f, &vh_f);
    int vw = (int)vw_f, vh = (int)vh_f;

    /* save the renderer's GL state */
    GLint prog_was = 0, fbo_was = 0, vao_was = 0, vp_was[4], tex0_was = 0, tex1_was = 0, active_was = 0;
    p_glGetIntegerv(0x8B8D /* GL_CURRENT_PROGRAM */, &prog_was);
    p_glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo_was);
    p_glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao_was);
    p_glGetIntegerv(GL_VIEWPORT, vp_was);
    p_glGetIntegerv(0x84E0 /* GL_ACTIVE_TEXTURE */, &active_was);
    p_glActiveTexture(GL_TEXTURE0); p_glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0_was);
    p_glActiveTexture(GL_TEXTURE1); p_glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex1_was);
    GLboolean blend_was = p_glIsEnabled(GL_BLEND), scissor_was = p_glIsEnabled(GL_SCISSOR_TEST);
    p_glDisable(GL_BLEND);
    p_glDisable(GL_SCISSOR_TEST);
    p_glBindVertexArray(C.vao);

    /* sizes */
    if (C.orig_w != src_w || C.orig_h != src_h || C.vp_w != vw || C.vp_h != vh) {
        C.orig_w = src_w; C.orig_h = src_h; C.vp_w = vw; C.vp_h = vh;
        make_tex(&C.orig, src_w, src_h, 0);
        int iw = src_w, ih = src_h;
        for (int k = 0; k < CRT_PASSES - 1; k++) {
            int w = chain[k].sx_type == SC_SOURCE ? (int)(iw * chain[k].sx + 0.5f) : chain[k].sx_type == SC_ABSOLUTE ? (int)chain[k].sx : vw;
            int h = chain[k].sy_type == SC_SOURCE ? (int)(ih * chain[k].sy + 0.5f) : chain[k].sy_type == SC_ABSOLUTE ? (int)chain[k].sy : vh;
            if (w < 1) w = 1;
            if (h < 1) h = 1;
            C.w[k] = w; C.h[k] = h;
            make_tex(&C.tex[k], w, h, chain[k].fp);
            iw = w; ih = h;
        }
        C.w[CRT_PASSES - 1] = vw; C.h[CRT_PASSES - 1] = vh;
    }

    /* the frame at its exact size */
    int ok = attach(C.orig);
    if (ok) {
        p_glViewport(0, 0, src_w, src_h);
        p_glUseProgram(C.copy_prog);
        p_glActiveTexture(GL_TEXTURE0);
        sampling(src_gl, 0, 0);
        p_glUniform1i(p_glGetUniformLocation(C.copy_prog, "Texture"), 0);
        p_glUniform2f(p_glGetUniformLocation(C.copy_prog, "Scale"), stex_w * src_w / tw, stex_h * src_h / th);
        p_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    /* the parameters for this picture */
    collect_params();
    for (const crt_param *p = crt_md_params; p->name; p++) set_param(p->name, p->value);
    set_param("LR_CABLE", (float)set->cable);
    set_param("LR_CURV_X", set->curved ? 0.045f : 0);
    set_param("LR_CURV_Y", set->curved ? 0.06f : 0);
    set_param("LR_CORNER", set->curved ? 0.045f : 0);
    if (set->mask_type >= 0) set_param("LR_MASK_TYPE", (float)set->mask_type);

    static const float mvp[16] = { 2, 0, 0, 0,  0, 2, 0, 0,  0, 0, 1, 0,  -1, -1, 0, 1 };
    GLuint in = C.orig;
    int in_w = src_w, in_h = src_h;
    for (int k = 0; k < CRT_PASSES && ok; k++) {
        GLuint out = k == CRT_PASSES - 1 ? dst_gl : C.tex[k];
        if (!(ok = attach(out))) { SDL_Log("crt: pass %d target incomplete", k); break; }
        p_glViewport(0, 0, C.w[k], C.h[k]);
        GLuint pr = C.prog[k];
        p_glUseProgram(pr);
        p_glActiveTexture(GL_TEXTURE0);
        sampling(in, chain[k].linear, chain[k].border);
        p_glUniform1i(p_glGetUniformLocation(pr, "Texture"), 0);
        p_glActiveTexture(GL_TEXTURE1);
        sampling(C.orig, 0, 0);
        p_glUniform1i(p_glGetUniformLocation(pr, "OrigTexture"), 1);
        int unit = 2;
        for (int a = 0; a < k; a++) {
            if (!chain[a].alias) continue;
            char nm[48];
            SDL_snprintf(nm, sizeof nm, "%sTexture", chain[a].alias);
            GLint loc = p_glGetUniformLocation(pr, nm);
            if (loc < 0) continue;
            p_glActiveTexture(GL_TEXTURE0 + unit);
            sampling(C.tex[a], 1, 0);
            p_glUniform1i(loc, unit++);
        }
        p_glActiveTexture(GL_TEXTURE0);
        p_glUniformMatrix4fv(p_glGetUniformLocation(pr, "MVPMatrix"), 1, GL_FALSE, mvp);
        p_glUniform1i(p_glGetUniformLocation(pr, "FrameCount"), (GLint)frame);
        p_glUniform2f(p_glGetUniformLocation(pr, "TextureSize"), (float)in_w, (float)in_h);
        p_glUniform2f(p_glGetUniformLocation(pr, "OrigTextureSize"), (float)src_w, (float)src_h);
        p_glUniform2f(p_glGetUniformLocation(pr, "OutputSize"), (float)C.w[k], (float)C.h[k]);
        for (int i = 0; i < C.np; i++) {
            GLint loc = p_glGetUniformLocation(pr, C.pname[i]);
            if (loc >= 0) p_glUniform1f(loc, C.pval[i]);
        }
        p_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        in = out; in_w = C.w[k]; in_h = C.h[k];
    }
    if (ok && SDL_getenv("DS_CRT_DEBUG")) { GLenum e = p_glGetError(); if (e) SDL_Log("crt: GL error 0x%X", e); }

    /* restore */
    for (int u = 7; u >= 1; u--) { p_glActiveTexture(GL_TEXTURE0 + u); p_glBindTexture(GL_TEXTURE_2D, u == 1 ? (GLuint)tex1_was : 0); }
    p_glActiveTexture(GL_TEXTURE0); p_glBindTexture(GL_TEXTURE_2D, (GLuint)tex0_was);
    p_glActiveTexture((GLenum)active_was);
    p_glBindVertexArray((GLuint)vao_was);
    p_glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo_was);
    p_glViewport(vp_was[0], vp_was[1], vp_was[2], vp_was[3]);
    p_glUseProgram((GLuint)prog_was);
    if (blend_was) p_glEnable(GL_BLEND);
    if (scissor_was) p_glEnable(GL_SCISSOR_TEST);
    return ok;
}
