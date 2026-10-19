// gltest - native OpenGL (WGL ICD) check for the Mali ICD (notes/gl-icd.md).
// SPDX-License-Identifier: GPL-2.0
//
// gltest [seconds]   (default 3; 0 = no swap loop)
// First it lists, per adapter, what D3DKMTQueryAdapterInfo(KMTQAITYPE_UMOPENGLINFO) returns - the call MS Learn
// "Loading an OpenGL Installable Client Driver" says the OpenGL runtime uses to find the ICD.
// Prints the GL strings, then one PASS/FAIL line per case, read back with glReadPixels:
//   default   clear the window's back buffer, read it back
//   tri       fixed-function triangle (compatibility profile, what MonoGame-era code still uses)
//   fbo       ARB_framebuffer_object render-to-texture, then the texture drawn to the window
//   glsl      a GLSL 1.20 program drawing a quad
//   core      wglCreateContextAttribsARB core 3.1 context + glClear + read back
// and with seconds > 0 a spinning-triangle swap loop with the frame rate (SwapBuffers -> the ICD's present).
// A last line "GLTEST n/m PASS" sums the cases. Run in the user's session (a window is needed).
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <winternl.h>
#include <d3dkmthk.h>

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

// The few GL 2.x / 3.x names this test needs (glext.h is not in the SDK).
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_NUM_EXTENSIONS 0x821D
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x0001

typedef void(APIENTRY *PFNGENFB)(GLsizei, GLuint *);
typedef void(APIENTRY *PFNBINDFB)(GLenum, GLuint);
typedef void(APIENTRY *PFNFBTEX2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum(APIENTRY *PFNCHECKFB)(GLenum);
typedef void(APIENTRY *PFNDELFB)(GLsizei, const GLuint *);
typedef GLuint(APIENTRY *PFNCREATESHADER)(GLenum);
typedef void(APIENTRY *PFNSHADERSOURCE)(GLuint, GLsizei, const GLchar *const *, const GLint *);
typedef void(APIENTRY *PFNCOMPILESHADER)(GLuint);
typedef void(APIENTRY *PFNGETSHADERIV)(GLuint, GLenum, GLint *);
typedef void(APIENTRY *PFNGETSHADERINFOLOG)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef GLuint(APIENTRY *PFNCREATEPROGRAM)(void);
typedef void(APIENTRY *PFNATTACHSHADER)(GLuint, GLuint);
typedef void(APIENTRY *PFNLINKPROGRAM)(GLuint);
typedef void(APIENTRY *PFNGETPROGRAMIV)(GLuint, GLenum, GLint *);
typedef void(APIENTRY *PFNUSEPROGRAM)(GLuint);
typedef const GLubyte *(APIENTRY *PFNGETSTRINGI)(GLenum, GLuint);
typedef HGLRC(WINAPI *PFNCREATECTXATTRIBS)(HDC, HGLRC, const int *);

static const int W = 256, H = 256;
static int g_pass, g_total;

static void Result(const char *name, bool ok, const char *detail)
{
    g_total++;
    if (ok)
        g_pass++;
    printf("%-8s %s %s\n", name, ok ? "PASS" : "FAIL", detail ? detail : "");
}

// Reads one pixel; true if within 8 of r,g,b.
static bool PixelIs(int x, int y, int r, int g, int b, char *detail, size_t n)
{
    unsigned char p[4] = {0};
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    snprintf(detail, n, "(%d,%d) = %u %u %u want %d %d %d, glGetError 0x%x", x, y, p[0], p[1], p[2], r, g, b,
             glGetError());
    return abs(p[0] - r) <= 8 && abs(p[1] - g) <= 8 && abs(p[2] - b) <= 8;
}

static PROC Get(const char *name)
{
    PROC p = wglGetProcAddress(name);
    if (p == NULL || p == (PROC)1 || p == (PROC)2 || p == (PROC)3 || p == (PROC)-1)
        return NULL;
    return p;
}

static void SetupOrtho(void)
{
    glViewport(0, 0, W, H);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

static void CaseDefault(void)
{
    char d[160];
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    Result("default", PixelIs(W / 2, H / 2, 255, 0, 0, d, sizeof(d)), d);
}

static void CaseTri(void)
{
    char d[160];
    SetupOrtho();
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glColor3f(0.0f, 1.0f, 0.0f);
    glVertex2f(-0.9f, -0.9f);
    glVertex2f(0.9f, -0.9f);
    glVertex2f(0.0f, 0.9f);
    glEnd();
    glFinish();
    bool in = PixelIs(W / 2, H / 2, 0, 255, 0, d, sizeof(d));
    char d2[160];
    bool out = PixelIs(4, H - 4, 0, 0, 255, d2, sizeof(d2));
    char both[340];
    snprintf(both, sizeof(both), "inside %s; corner %s", d, d2);
    Result("tri", in && out, both);
}

static void CaseFbo(void)
{
    char d[200];
    PFNGENFB GenFramebuffers = (PFNGENFB)Get("glGenFramebuffers");
    PFNBINDFB BindFramebuffer = (PFNBINDFB)Get("glBindFramebuffer");
    PFNFBTEX2D FramebufferTexture2D = (PFNFBTEX2D)Get("glFramebufferTexture2D");
    PFNCHECKFB CheckFramebufferStatus = (PFNCHECKFB)Get("glCheckFramebufferStatus");
    PFNDELFB DeleteFramebuffers = (PFNDELFB)Get("glDeleteFramebuffers");
    if (!GenFramebuffers || !BindFramebuffer || !FramebufferTexture2D || !CheckFramebufferStatus || !DeleteFramebuffers) {
        Result("fbo", false, "glGenFramebuffers & co. not exported (no ARB_framebuffer_object)");
        return;
    }
    GLuint tex = 0, fb = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    GenFramebuffers(1, &fb);
    BindFramebuffer(GL_FRAMEBUFFER, fb);
    FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    GLenum st = CheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        snprintf(d, sizeof(d), "status 0x%x", st);
        Result("fbo", false, d);
        BindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }
    glViewport(0, 0, 64, 64);
    glClearColor(1.0f, 1.0f, 0.0f, 1.0f);   // yellow into the texture
    glClear(GL_COLOR_BUFFER_BIT);
    BindFramebuffer(GL_FRAMEBUFFER, 0);
    // draw the texture over the whole window
    SetupOrtho();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D);
    glColor3f(1.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(-1, -1);
    glTexCoord2f(1, 0); glVertex2f(1, -1);
    glTexCoord2f(1, 1); glVertex2f(1, 1);
    glTexCoord2f(0, 1); glVertex2f(-1, 1);
    glEnd();
    glDisable(GL_TEXTURE_2D);
    glFinish();
    Result("fbo", PixelIs(W / 3, H / 3, 255, 255, 0, d, sizeof(d)), d);
    DeleteFramebuffers(1, &fb);
    glDeleteTextures(1, &tex);
}

static void CaseGlsl(void)
{
    char d[600];
    PFNCREATESHADER CreateShader = (PFNCREATESHADER)Get("glCreateShader");
    PFNSHADERSOURCE ShaderSource = (PFNSHADERSOURCE)Get("glShaderSource");
    PFNCOMPILESHADER CompileShader = (PFNCOMPILESHADER)Get("glCompileShader");
    PFNGETSHADERIV GetShaderiv = (PFNGETSHADERIV)Get("glGetShaderiv");
    PFNGETSHADERINFOLOG GetShaderInfoLog = (PFNGETSHADERINFOLOG)Get("glGetShaderInfoLog");
    PFNCREATEPROGRAM CreateProgram = (PFNCREATEPROGRAM)Get("glCreateProgram");
    PFNATTACHSHADER AttachShader = (PFNATTACHSHADER)Get("glAttachShader");
    PFNLINKPROGRAM LinkProgram = (PFNLINKPROGRAM)Get("glLinkProgram");
    PFNGETPROGRAMIV GetProgramiv = (PFNGETPROGRAMIV)Get("glGetProgramiv");
    PFNUSEPROGRAM UseProgram = (PFNUSEPROGRAM)Get("glUseProgram");
    if (!CreateShader || !ShaderSource || !CompileShader || !GetShaderiv || !GetShaderInfoLog || !CreateProgram ||
        !AttachShader || !LinkProgram || !GetProgramiv || !UseProgram) {
        Result("glsl", false, "GL 2.0 shader entry points missing");
        return;
    }
    static const char *vs = "#version 120\nvoid main() { gl_Position = gl_Vertex; }\n";
    static const char *fs = "#version 120\nvoid main() { gl_FragColor = vec4(1.0, 0.0, 1.0, 1.0); }\n";
    GLuint s[2] = {CreateShader(GL_VERTEX_SHADER), CreateShader(GL_FRAGMENT_SHADER)};
    const char *src[2] = {vs, fs};
    for (int i = 0; i < 2; i++) {
        GLint ok = 0;
        ShaderSource(s[i], 1, &src[i], NULL);
        CompileShader(s[i]);
        GetShaderiv(s[i], GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[400] = {0};
            GetShaderInfoLog(s[i], sizeof(log), NULL, log);
            snprintf(d, sizeof(d), "%s shader: %s", i ? "fragment" : "vertex", log);
            Result("glsl", false, d);
            return;
        }
    }
    GLuint p = CreateProgram();
    AttachShader(p, s[0]);
    AttachShader(p, s[1]);
    LinkProgram(p);
    GLint linked = 0;
    GetProgramiv(p, GL_LINK_STATUS, &linked);
    if (!linked) {
        Result("glsl", false, "link failed");
        return;
    }
    SetupOrtho();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    UseProgram(p);
    glBegin(GL_QUADS);
    glVertex2f(-0.5f, -0.5f);
    glVertex2f(0.5f, -0.5f);
    glVertex2f(0.5f, 0.5f);
    glVertex2f(-0.5f, 0.5f);
    glEnd();
    UseProgram(0);
    glFinish();
    bool in = PixelIs(W / 2, H / 2, 255, 0, 255, d, sizeof(d));
    char d2[200];
    bool out = PixelIs(4, 4, 0, 0, 0, d2, sizeof(d2));
    char both[820];
    snprintf(both, sizeof(both), "inside %s; corner %s", d, d2);
    Result("glsl", in && out, both);
}

static void CaseCore(HDC dc, HGLRC compat)
{
    char d[200];
    PFNCREATECTXATTRIBS CreateContextAttribs = (PFNCREATECTXATTRIBS)Get("wglCreateContextAttribsARB");
    if (!CreateContextAttribs) {
        Result("core", false, "no wglCreateContextAttribsARB");
        return;
    }
    const int attrs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 1,
                         WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
    HGLRC core = CreateContextAttribs(dc, NULL, attrs);
    if (!core) {
        snprintf(d, sizeof(d), "3.1 core context refused (GetLastError %lu)", GetLastError());
        Result("core", false, d);
        return;
    }
    wglMakeCurrent(dc, core);
    printf("core     GL_VERSION %s\n", (const char *)glGetString(GL_VERSION));
    glViewport(0, 0, W, H);
    glClearColor(0.0f, 1.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    Result("core", PixelIs(W / 2, H / 2, 0, 255, 255, d, sizeof(d)), d);
    wglMakeCurrent(dc, compat);
    wglDeleteContext(core);
}

static void SwapLoop(HWND wnd, HDC dc, double seconds)
{
    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    unsigned frames = 0;
    SetupOrtho();
    for (;;) {
        MSG m;
        while (PeekMessage(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessage(&m);
        }
        QueryPerformanceCounter(&t);
        double el = (double)(t.QuadPart - t0.QuadPart) / f.QuadPart;
        if (el >= seconds)
            break;
        glClearColor(0.1f, 0.1f, 0.2f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glLoadIdentity();
        glRotatef((float)(el * 90.0), 0, 0, 1);
        glBegin(GL_TRIANGLES);
        glColor3f(1, 0, 0); glVertex2f(-0.7f, -0.6f);
        glColor3f(0, 1, 0); glVertex2f(0.7f, -0.6f);
        glColor3f(0, 0, 1); glVertex2f(0.0f, 0.8f);
        glEnd();
        SwapBuffers(dc);
        frames++;
    }
    QueryPerformanceCounter(&t);
    double el = (double)(t.QuadPart - t0.QuadPart) / f.QuadPart;
    printf("swap     %u frames in %.2f s = %.1f fps (%dx%d window)\n", frames, el, frames / el, W, H);
    (void)wnd;
}

static void ListIcds(void)
{
    D3DKMT_ADAPTERINFO info[16] = {};
    D3DKMT_ENUMADAPTERS2 en = {};
    en.NumAdapters = 16;
    en.pAdapters = info;
    NTSTATUS st = D3DKMTEnumAdapters2(&en);
    if (st < 0) {
        printf("icd      D3DKMTEnumAdapters2 0x%08lx\n", (unsigned long)st);
        return;
    }
    for (ULONG i = 0; i < en.NumAdapters; i++) {
        D3DKMT_OPENGLINFO gl = {};
        D3DKMT_QUERYADAPTERINFO q = {};
        q.hAdapter = info[i].hAdapter;
        q.Type = KMTQAITYPE_UMOPENGLINFO;
        q.pPrivateDriverData = &gl;
        q.PrivateDriverDataSize = sizeof(gl);
        st = D3DKMTQueryAdapterInfo(&q);
        printf("icd      adapter %lu luid %08lx:%08lx: UMOPENGLINFO 0x%08lx name '%ls' version %lu flags 0x%lx\n", i,
               (unsigned long)info[i].AdapterLuid.HighPart, (unsigned long)info[i].AdapterLuid.LowPart,
               (unsigned long)st, gl.UmdOpenGlIcdFileName, gl.Version, gl.Flags);
        D3DKMT_CLOSEADAPTER c = {info[i].hAdapter};
        D3DKMTCloseAdapter(&c);
    }
}

int main(int argc, char **argv)
{
    double seconds = argc > 1 ? atof(argv[1]) : 3.0;
    ListIcds();
    WNDCLASSA wc = {};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "gltest";
    RegisterClassA(&wc);
    RECT r = {0, 0, W, H};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND wnd = CreateWindowA("gltest", "gltest", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, r.right - r.left,
                             r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (!wnd) {
        printf("CreateWindow failed %lu\n", GetLastError());
        return 1;
    }
    HDC dc = GetDC(wnd);
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    int pf = ChoosePixelFormat(dc, &pfd);
    PIXELFORMATDESCRIPTOR got = {};
    DescribePixelFormat(dc, pf, sizeof(got), &got);
    printf("pixelfmt %d flags 0x%lx (%s) color %u depth %u stencil %u\n", pf, got.dwFlags,
           (got.dwFlags & PFD_GENERIC_FORMAT) ? "GENERIC = Microsoft GDI GL, no ICD" : "ICD", got.cColorBits,
           got.cDepthBits, got.cStencilBits);
    if (!pf || !SetPixelFormat(dc, pf, &pfd)) {
        printf("SetPixelFormat failed %lu\n", GetLastError());
        return 1;
    }
    HGLRC rc = wglCreateContext(dc);
    if (!rc || !wglMakeCurrent(dc, rc)) {
        printf("wglCreateContext/MakeCurrent failed %lu\n", GetLastError());
        return 1;
    }
    printf("vendor   %s\n", (const char *)glGetString(GL_VENDOR));
    printf("renderer %s\n", (const char *)glGetString(GL_RENDERER));
    printf("version  %s\n", (const char *)glGetString(GL_VERSION));
    const char *sl = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
    printf("glsl     %s\n", sl ? sl : "(none)");
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    printf("ext      %zu bytes; ARB_framebuffer_object %s, EXT_framebuffer_object %s\n", ext ? strlen(ext) : 0,
           ext && strstr(ext, "GL_ARB_framebuffer_object") ? "yes" : "no",
           ext && strstr(ext, "GL_EXT_framebuffer_object") ? "yes" : "no");

    CaseDefault();
    CaseTri();
    CaseFbo();
    CaseGlsl();
    CaseCore(dc, rc);
    if (seconds > 0)
        SwapLoop(wnd, dc, seconds);

    printf("GLTEST %d/%d PASS\n", g_pass, g_total);
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rc);
    ReleaseDC(wnd, dc);
    DestroyWindow(wnd);
    return g_pass == g_total ? 0 : 2;
}
