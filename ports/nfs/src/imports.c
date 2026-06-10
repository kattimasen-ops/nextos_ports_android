/*
 * imports.c — shims bionic→glibc do NFS (os 18 imports que o dlsym fallback
 * não cobre: __sF/__errno/__assert2/__android_log/_ctype_/_tolower_tab_/
 * __cxa_type_match/__dso_handle/sigsetjmp/AndroidBitmap_*).
 * Exporta nfs_shims[] — main.c usa como base da tabela combinada.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <dlfcn.h>
#include <string.h>

#include "so_util.h"
#include "egl_shim.h"

/* ---- bionic __sF[3] = stdin/out/err (libc++ usa p/ std::cerr/cout) ---- */
static char bionic_sF[3][512];
static FILE *map_sF(void *fp) {
  if (fp == (void *)&bionic_sF[0]) return stdin;
  if (fp == (void *)&bionic_sF[1]) return stdout;
  if (fp == (void *)&bionic_sF[2]) return stderr;
  return (FILE *)fp;
}
static int w_fprintf(void *fp, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt); int r = vfprintf(map_sF(fp), fmt, ap); va_end(ap); return r;
}
static int w_vfprintf(void *fp, const char *fmt, va_list ap) { return vfprintf(map_sF(fp), fmt, ap); }
static size_t w_fwrite(const void *p, size_t s, size_t n, void *fp) { return fwrite(p, s, n, map_sF(fp)); }
static int w_fputs(const char *str, void *fp) { return fputs(str, map_sF(fp)); }
static int w_fputc(int c, void *fp) { return fputc(c, map_sF(fp)); }
static int w_fflush(void *fp) { return fflush(fp ? map_sF(fp) : NULL); }

/* ---- errno / assert / log ---- */
static int *b_errno(void) { extern int *__errno_location(void); return __errno_location(); }
static void b_assert2(const char *f, int l, const char *fn, const char *m) {
  fprintf(stderr, "[assert] %s:%d %s: %s\n", f ? f : "?", l, fn ? fn : "?", m ? m : "?");
  abort();
}
static int b_log_print(int prio, const char *tag, const char *fmt, ...) {
  (void)prio; va_list ap; va_start(ap, fmt);
  fprintf(stderr, "[%s] ", tag ? tag : "nfs"); vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n"); va_end(ap); return 0;
}
static int b_log_write(int prio, const char *tag, const char *msg) {
  (void)prio; fprintf(stderr, "[%s] %s\n", tag ? tag : "nfs", msg ? msg : ""); return 0;
}
static void b_log_assert(const char *cond, const char *tag, const char *fmt, ...) {
  (void)cond; va_list ap; va_start(ap, fmt);
  fprintf(stderr, "[%s ASSERT] ", tag ? tag : "nfs"); vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n"); va_end(ap); abort();
}

/* ---- ctype tables bionic (_ctype_[c+1], _tolower_tab_[c+1]) ----
 * bionic flags: _U=1 _L=2 _N=4 _S=8 _P=0x10 _C=0x20 _X=0x40 _B=0x80.
 * Preenchidas da classificação glibc no constructor. */
static unsigned char b_ctype[1 + 256];
static unsigned char b_tolower[1 + 256];
static unsigned char b_toupper[1 + 256];
__attribute__((constructor)) static void init_ctype(void) {
  for (int c = 0; c < 256; c++) {
    unsigned char f = 0;
    if (isupper(c)) f |= 0x01;
    if (islower(c)) f |= 0x02;
    if (isdigit(c)) f |= 0x04;
    if (isspace(c)) f |= 0x08;
    if (ispunct(c)) f |= 0x10;
    if (iscntrl(c)) f |= 0x20;
    if (isxdigit(c)) f |= 0x40;
    if (c == ' ')   f |= 0x80;
    b_ctype[c + 1] = f;
    b_tolower[c + 1] = (unsigned char)tolower(c);
    b_toupper[c + 1] = (unsigned char)toupper(c);
  }
}

/* ---- malloc com PADDING (+64B) ----
 * A engine NFS faz pequenos overflows que o malloc do bionic tolera (layout de
 * chunk diferente) mas o glibc detecta ("malloc(): invalid size"). Over-alocar
 * absorve o overflow na folga, sem corromper a metadata do glibc. free/realloc
 * usam o mesmo ponteiro base → consistentes. */
#define NFS_PAD 64
static void *pad_malloc(size_t n) { return malloc(n + NFS_PAD); }
static void *pad_calloc(size_t a, size_t b) {
  size_t t = a * b; void *p = malloc(t + NFS_PAD); if (p) memset(p, 0, t + NFS_PAD); return p;
}
static void *pad_realloc(void *p, size_t n) { return realloc(p, n + NFS_PAD); }
static void *pad_memalign(size_t al, size_t n) { return memalign(al, n + NFS_PAD); }

/* ---- pthread_create hook: loga a função de entrada de cada thread (p/
 * identificar a worker thread que crasha no init). NFS_PTLOG=1 liga. ---- */
static int (*real_pthread_create)(void *, const void *, void *(*)(void *), void *);
static int my_pthread_create(void *th, const void *attr, void *(*fn)(void *), void *arg) {
  if (!real_pthread_create) {
    real_pthread_create = (int (*)(void *, const void *, void *(*)(void *), void *))
        dlsym(RTLD_DEFAULT, "pthread_create");
  }
  if (getenv("NFS_PTLOG")) {
    static int n = 0;
    if (n < 40) { fprintf(stderr, "[pthread_create] #%d fn=%p arg=%p attr=%p\n", n, (void *)fn, arg, (void *)attr); n++; }
  }
  /* IGNORA o attr bionic (layout ≠ glibc → glibc lê lixo de stack-size). Passa
   * NULL = thread glibc default (8MB). NFS_PT_KEEPATTR=1 mantém o attr original. */
  if (!getenv("NFS_PT_KEEPATTR")) attr = NULL;
  return real_pthread_create(th, attr, fn, arg);
}

/* ---- fopen/open hook: loga acessos a arquivo (ver se a engine abre o OBB) ---- */
static void *(*real_fopen)(const char *, const char *);
static void *my_fopen(const char *path, const char *mode) {
  if (!real_fopen) real_fopen = (void *(*)(const char *, const char *))dlsym(RTLD_DEFAULT, "fopen");
  void *fp = real_fopen(path, mode);
  if (getenv("NFS_FOPENLOG")) {
    static int n = 0;
    if (n < 60) { fprintf(stderr, "[fopen] '%s' (%s) -> %s\n", path ? path : "?", mode ? mode : "?", fp ? "OK" : "MISS"); n++; }
  }
  return fp;
}
static size_t (*real_fread)(void *, size_t, size_t, void *);
static size_t my_fread(void *p, size_t sz, size_t n, void *fp) {
  if (!real_fread) real_fread = (size_t(*)(void*,size_t,size_t,void*))dlsym(RTLD_DEFAULT, "fread");
  size_t r = real_fread(p, sz, n, fp);
  if (getenv("NFS_FOPENLOG")) { static int c = 0; static size_t tot = 0; tot += r * sz;
    if (c < 30) { fprintf(stderr, "[fread] %zu*%zu -> %zu (fp=%p, total=%zu)\n", sz, n, r, fp, tot); c++; } }
  return r;
}
static int (*real_open)(const char *, int, ...);
static int my_open(const char *path, int flags, ...) {
  if (!real_open) real_open = (int (*)(const char *, int, ...))dlsym(RTLD_DEFAULT, "open");
  int fd = real_open(path, flags, 0666);
  if (getenv("NFS_FOPENLOG")) {
    static int n = 0;
    if (n < 60) { fprintf(stderr, "[open] '%s' -> %s\n", path ? path : "?", fd >= 0 ? "OK" : "MISS"); n++; }
  }
  return fd;
}

/* ---- dlsym hook: a engine pode dlsym funções math em runtime e chamá-las
 * (softfp). Retornamos o wrapper softfp_resolve ANTES do dlsym real (senão
 * pega a versão HARDFP do glibc → crash). NFS_DLSYMLOG=1 loga. ---- */
extern void *softfp_resolve(const char *);
static void *(*real_dlsym)(void *, const char *);
static void *my_dlsym(void *handle, const char *name) {
  if (!real_dlsym) real_dlsym = (void *(*)(void *, const char *))dlsym(RTLD_DEFAULT, "dlsym");
  void *p = softfp_resolve(name);
  if (!p) p = real_dlsym(handle, name);
  if (getenv("NFS_DLSYMLOG")) {
    static int n = 0;
    if (n < 80) { fprintf(stderr, "[dlsym] '%s' -> %p%s\n", name ? name : "?", p, softfp_resolve(name) ? " (softfp)" : ""); n++; }
  }
  return p;
}

/* ---- stubs ---- */
static int b_dso_handle;                       /* __dso_handle = endereço dummy */
static void *b_cxa_type_match(void *a, void *b, char c) { (void)a; (void)b; (void)c; return (void *)0; }
/* __sigsetjmp: declarado por <setjmp.h> (via so_util.h); bionic sigsetjmp == isso */
/* AndroidBitmap (jnigraphics) — stub: sinaliza erro p/ a engine cair no fallback */
static int abm_getInfo(void *env, void *bmp, void *info) { (void)env; (void)bmp; (void)info; return -1; }
static int abm_lock(void *env, void *bmp, void **pix) { (void)env; (void)bmp; if (pix) *pix = 0; return -1; }
static int abm_unlock(void *env, void *bmp) { (void)env; (void)bmp; return 0; }

DynLibFunction nfs_shims[] = {
    /* EGL → SDL2 (Mali fbdev): a engine cria contexto/surface via egl_shim */
    {"eglGetDisplay", (uintptr_t)egl_shim_GetDisplay},
    {"eglInitialize", (uintptr_t)egl_shim_Initialize},
    {"eglTerminate", (uintptr_t)egl_shim_Terminate},
    {"eglChooseConfig", (uintptr_t)egl_shim_ChooseConfig},
    {"eglGetConfigAttrib", (uintptr_t)egl_shim_GetConfigAttrib},
    {"eglCreateWindowSurface", (uintptr_t)egl_shim_CreateWindowSurface},
    {"eglCreatePbufferSurface", (uintptr_t)egl_shim_CreatePbufferSurface},
    {"eglDestroySurface", (uintptr_t)egl_shim_DestroySurface},
    {"eglCreateContext", (uintptr_t)egl_shim_CreateContext},
    {"eglDestroyContext", (uintptr_t)egl_shim_DestroyContext},
    {"eglMakeCurrent", (uintptr_t)egl_shim_MakeCurrent},
    {"eglSwapBuffers", (uintptr_t)egl_shim_SwapBuffers},
    {"eglSwapInterval", (uintptr_t)egl_shim_SwapInterval},
    {"eglGetCurrentContext", (uintptr_t)egl_shim_GetCurrentContext},
    {"eglGetCurrentSurface", (uintptr_t)egl_shim_GetCurrentSurface},
    {"eglGetError", (uintptr_t)egl_shim_GetError},
    {"eglQueryString", (uintptr_t)egl_shim_QueryString},
    {"eglQuerySurface", (uintptr_t)egl_shim_QuerySurface},
    {"eglBindAPI", (uintptr_t)egl_shim_BindAPI},
    {"eglSurfaceAttrib", (uintptr_t)egl_shim_SurfaceAttrib},
    {"eglGetProcAddress", (uintptr_t)egl_shim_GetProcAddress},
    {"pthread_create", (uintptr_t)my_pthread_create},
    {"dlsym", (uintptr_t)my_dlsym},
    {"fopen", (uintptr_t)my_fopen},
    {"fread", (uintptr_t)my_fread},
    {"open", (uintptr_t)my_open},
    {"malloc", (uintptr_t)pad_malloc},
    {"calloc", (uintptr_t)pad_calloc},
    {"realloc", (uintptr_t)pad_realloc},
    {"memalign", (uintptr_t)pad_memalign},
    {"__sF", (uintptr_t)bionic_sF},
    {"fprintf", (uintptr_t)w_fprintf}, {"vfprintf", (uintptr_t)w_vfprintf},
    {"fwrite", (uintptr_t)w_fwrite}, {"fputs", (uintptr_t)w_fputs},
    {"fputc", (uintptr_t)w_fputc}, {"fflush", (uintptr_t)w_fflush},
    {"__errno", (uintptr_t)b_errno},
    {"__assert2", (uintptr_t)b_assert2},
    {"__android_log_print", (uintptr_t)b_log_print},
    {"__android_log_write", (uintptr_t)b_log_write},
    {"__android_log_assert", (uintptr_t)b_log_assert},
    {"_ctype_", (uintptr_t)b_ctype},
    {"_tolower_tab_", (uintptr_t)b_tolower},
    {"_toupper_tab_", (uintptr_t)b_toupper},
    {"__dso_handle", (uintptr_t)&b_dso_handle},
    {"__cxa_type_match", (uintptr_t)b_cxa_type_match},
    {"sigsetjmp", (uintptr_t)__sigsetjmp},
    {"AndroidBitmap_getInfo", (uintptr_t)abm_getInfo},
    {"AndroidBitmap_lockPixels", (uintptr_t)abm_lock},
    {"AndroidBitmap_unlockPixels", (uintptr_t)abm_unlock},
};
int nfs_shims_count = sizeof(nfs_shims) / sizeof(nfs_shims[0]);
