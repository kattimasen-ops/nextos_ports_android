/*
 * main.c — loader ARMHF do NFS Most Wanted (com.ea.games.nfs13_row).
 *
 * Engine EA/Ironmonkey: entry = JNI_OnLoad em libapp.so + Java
 * GameActivityMain.nativeOnCreate. Multi-módulo (libapp + libc++_shared +
 * libNimble + FMOD). F0: carrega libapp, resolve imports (tabela + fallback
 * dlsym), acha JNI_OnLoad. F1+: multi-módulo + boot JNI/GameActivity.
 */
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include "android_shim.h"
#include "error.h"
#include "imports.h"
#include "jni_shim.h"
#include "so_util.h"
#include "util.h"

#define MEMORY_MB 64
#define SO_NAME "libapp.so"
/* 🔑 pad TLS do canary bionic (DYSMANTLE): as render threads batem no padrão
 * memcpy(0,tid,11) (thread-entry/canary instável sob glibc). Pad _Thread_local
 * 256B aligned(16) NUNCA escrito cobre o slot bionic e estabiliza. */
__attribute__((aligned(16))) _Thread_local char g_bionic_guard_pad[256];

/* ---- crash handler ARMHF (campos arm_pc/arm_r0/arm_lr do sigcontext 32-bit) ---- */
static void crash_handler(int sig, siginfo_t *info, void *uctx) {
  ucontext_t *uc = (ucontext_t *)uctx;
  mcontext_t *m = &uc->uc_mcontext;
  uintptr_t pc = m->arm_pc, lr = m->arm_lr, fault = (uintptr_t)info->si_addr;
  uintptr_t text = (uintptr_t)text_base;

  /* construtor do init_array crashou (precisa de ambiente bionic indisponível)
   * → pula ele e segue (so_execute_init_array armou o sigsetjmp). */
  if ((sig == SIGSEGV || sig == SIGBUS) && g_init_armed) {
    g_init_armed = 0;
    siglongjmp(g_init_jmp, 1);
  }

  /* 🩹 recupera o memcpy/op com DESTINO NULO e n pequeno (padrão recorrente da
   * engine: destrutor de objeto garbage no parse de asset) — "retorna" da função
   * que crashou (PC←LR) pulando a cópia, p/ a engine seguir. NFS_NORECOVER=1 off. */
  if (sig == SIGSEGV && !getenv("NFS_NORECOVER")) {
    uintptr_t r0 = m->arm_r0, r2 = m->arm_r2;
    static int recov = 0;
    if (r0 < 0x10000 && r2 <= 64 && lr > 0x10000 && recov < 200000) {
      m->arm_r0 = lr; m->arm_pc = lr; recov++; /* r0=retorno(dst), pc=lr */
      if (recov <= 4) fprintf(stderr, "[RECOVER] copy dst=%lx n=%lx -> skip (pc<-lr)\n",
                              (unsigned long)r0, (unsigned long)r2);
      return;
    }
  }

  fprintf(stderr, "\n=== CRASH sig=%d addr=%p ===\n", sig, (void *)fault);
  fprintf(stderr, "  PC=%p", (void *)pc);
  if (pc >= text && pc < text + text_size)
    fprintf(stderr, " (%s+0x%lx)", SO_NAME, (unsigned long)(pc - text));
  fprintf(stderr, "\n  LR=%p", (void *)lr);
  if (lr >= text && lr < text + text_size)
    fprintf(stderr, " (%s+0x%lx)", SO_NAME, (unsigned long)(lr - text));
  fprintf(stderr, "\n");
  fprintf(stderr, "  r0=%08lx r1=%08lx r2=%08lx r3=%08lx\n",
          (unsigned long)m->arm_r0, (unsigned long)m->arm_r1,
          (unsigned long)m->arm_r2, (unsigned long)m->arm_r3);
  fprintf(stderr, "  r4=%08lx r5=%08lx r6=%08lx r7=%08lx\n",
          (unsigned long)m->arm_r4, (unsigned long)m->arm_r5,
          (unsigned long)m->arm_r6, (unsigned long)m->arm_r7);
  fprintf(stderr, "  r8=%08lx r9=%08lx r10=%08lx fp=%08lx ip=%08lx sp=%08lx\n",
          (unsigned long)m->arm_r8, (unsigned long)m->arm_r9,
          (unsigned long)m->arm_r10, (unsigned long)m->arm_fp,
          (unsigned long)m->arm_ip, (unsigned long)m->arm_sp);

  /* backtrace simples via FP chain (ARM: [fp-4]=lr salvo, [fp-8]=fp ant.;
   * varia por -fomit-frame-pointer, então é best-effort) */
  fprintf(stderr, "  --- stack scan (retornos no .so) ---\n");
  uintptr_t sp = m->arm_sp;
  int n = 0;
  for (uintptr_t a = sp; a < sp + 0x2000 && n < 24; a += 4) {
    uintptr_t v = *(uintptr_t *)a;
    if (v >= text && v < text + text_size) {
      fprintf(stderr, "    [sp+0x%lx] %s+0x%lx\n", (unsigned long)(a - sp),
              SO_NAME, (unsigned long)(v - text));
      n++;
    }
  }
  fprintf(stderr, "=== END CRASH ===\n");
  fflush(stderr);
  _exit(128 + sig);
}

static void install_crash_handler(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_handler;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
  sigaction(SIGFPE, &sa, NULL);
}

/* tabela combinada acumulada (base + snapshots dos módulos já carregados) */
static DynLibFunction *g_comb;
static int g_comb_n;
static void comb_append(DynLibFunction *tbl, int n) {
  g_comb = realloc(g_comb, sizeof(DynLibFunction) * (g_comb_n + n));
  memcpy(g_comb + g_comb_n, tbl, sizeof(DynLibFunction) * n);
  g_comb_n += n;
}

/* carrega 1 módulo no seu próprio heap, reloca, resolve contra a tabela
 * combinada (+ fallback dlsym no so_resolve) e, se snapshot!=0, acumula os
 * símbolos exportados na combinada p/ os módulos seguintes. */
static int load_module(const char *name, int heap_mb, int snapshot) {
  size_t hs = (size_t)heap_mb * 1024 * 1024;
  void *heap = mmap(NULL, hs, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED) { fprintf(stderr, "mmap %s falhou\n", name); return -1; }
  debugPrintf("--- %s (heap %p, %d MB) ---\n", name, heap, heap_mb);
  if (so_load(name, heap, hs) < 0) { fprintf(stderr, "so_load %s falhou\n", name); return -1; }
  if (so_relocate() < 0) { fprintf(stderr, "so_relocate %s falhou\n", name); return -1; }
  if (so_resolve(g_comb ? g_comb : dynlib_functions,
                 g_comb ? g_comb_n : (int)dynlib_numfunctions, 0) < 0) {
    fprintf(stderr, "so_resolve %s falhou\n", name); return -1;
  }
  so_finalize();
  so_flush_caches();
  if (snapshot) {
    int n = 0;
    DynLibFunction *t = so_snapshot_symbols(&n);
    if (t && n > 0) { comb_append(t, n); debugPrintf("%s: +%d símbolos exportados\n", name, n); }
  }
  /* roda os construtores C++ estáticos (.init_array) deste módulo — necessário
   * antes de usar a engine (inicializa globais/singletons). Ordem = ordem de
   * carga (libc++ 1º). NFS_SKIPINIT="lib..." pula p/ bissecção de corrupção. */
  /* init_array (construtores C++): DEFAULT-OFF — os construtores do NFS
   * corrompem o heap/segfaltam sem o ambiente bionic completo (TODO F2.x:
   * rodar de forma segura). NFS_INIT=1 liga; NFS_SKIPINIT="lib..." filtra. */
  const char *skip = getenv("NFS_SKIPINIT");
  if (getenv("NFS_INIT") && !(skip && strstr(skip, name))) {
    so_execute_init_array();
    debugPrintf("%s: init_array OK\n", name);
  } else {
    debugPrintf("%s: init_array OFF\n", name);
  }
  return 0;
}

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;
  install_crash_handler();
  __asm__ volatile("" : : "r"(g_bionic_guard_pad) : "memory"); /* força o pad no TLS */
  setvbuf(stdout, NULL, _IONBF, 0); /* logs visíveis no crash (init_array era a causa, não isto) */
  debugPrintf("=== NFS Most Wanted — loader ARMHF (Mali-450) ===\n");

  /* base = shims bionic→glibc (os 18 que o dlsym fallback não cobre) */
  extern DynLibFunction nfs_shims[];
  extern int nfs_shims_count;
  g_comb = malloc(sizeof(DynLibFunction) * nfs_shims_count);
  memcpy(g_comb, nfs_shims, sizeof(DynLibFunction) * nfs_shims_count);
  g_comb_n = nfs_shims_count;

  /* dependências primeiro (cada uma vira fonte de símbolos p/ as seguintes) */
  if (load_module("libc++_shared.so", 24, 1) < 0) return 1; /* std::__ndk1 */
  if (load_module("libNimble.so", 4, 1) < 0) return 1;      /* bridge JNI */
  if (load_module("libfmodex.so", 8, 1) < 0) return 1;      /* áudio FMOD */
  if (load_module("libfmodevent.so", 8, 1) < 0) return 1;

  /* módulo principal: libapp (resolve contra tudo acima + dlsym) */
  if (load_module(SO_NAME, MEMORY_MB, 0) < 0) return 1;

  uintptr_t jni_onload = so_find_addr_safe("JNI_OnLoad");
  uintptr_t native_oncreate =
      so_find_addr_safe("Java_com_ea_ironmonkey_GameActivityMain_nativeOnCreate");
  debugPrintf("JNI_OnLoad=%p nativeOnCreate=%p (combinada=%d símbolos)\n",
              (void *)jni_onload, (void *)native_oncreate, g_comb_n);

  /* ---- F2: boot JNI ---- */
  jni_shim_set_package("com.ea.games.nfs13_row", 0);
  void *vm = 0, *env = 0;
  jni_shim_init(&vm, &env);
  debugPrintf("jni_shim: vm=%p env=%p\n", vm, env);

  if (jni_onload) {
    int (*JNI_OnLoad)(void *, void *) = (int (*)(void *, void *))jni_onload;
    int v = JNI_OnLoad(vm, 0);
    debugPrintf("JNI_OnLoad -> 0x%x\n", v);
  }

  static char fake_this[64], fake_surface[64];
#define NCALL0(sym) do { uintptr_t a = so_find_addr_safe(sym); \
    if (a) { debugPrintf(">> %s\n", sym); ((void(*)(void*,void*))a)(env, fake_this); } \
    else debugPrintf("!! %s nao achado\n", sym); } while (0)

  if (!getenv("NFS_NO_ONCREATE") && native_oncreate) {
    debugPrintf("chamando nativeOnCreate...\n");
    ((void(*)(void*,void*,void*,void*,void*,void*))native_oncreate)(env, fake_this, 0, 0, 0, 0);
    debugPrintf("nativeOnCreate retornou\n");

    /* ---- F3: ciclo de render (GameActivity lifecycle + RunLoop tick) ---- */
    extern void egl_shim_create_window(void);
    egl_shim_create_window(); /* SDL2 + EGL/GLES2 no Mali fbdev */

    NCALL0("Java_com_ea_ironmonkey_GameActivityMain_nativeOnStart");
    /* nativeSurfaceCreated(env, this, surface) — surface fake; a engine pega o
     * ANativeWindow via ANativeWindow_fromSurface (android_shim) */
    { uintptr_t a = so_find_addr_safe("Java_com_ea_ironmonkey_GameActivityMain_nativeSurfaceCreated");
      if (a) { debugPrintf(">> nativeSurfaceCreated\n");
        ((void(*)(void*,void*,void*))a)(env, fake_this, fake_surface); } }
    /* nativeSurfaceChanged(env, this, format, w, h) */
    { uintptr_t a = so_find_addr_safe("Java_com_ea_ironmonkey_GameActivityMain_nativeSurfaceChanged");
      if (a) { debugPrintf(">> nativeSurfaceChanged 1280x720\n");
        ((void(*)(void*,void*,int,int,int))a)(env, fake_this, 1, 1280, 720); } }
    NCALL0("Java_com_ea_ironmonkey_GameActivityMain_nativeOnResume");

    /* loop de render: RunLoop_nativeOnRunLoopTick a cada frame (a engine faz
     * eglSwapBuffers -> egl_shim -> SDL_GL_SwapWindow) */
    uintptr_t tick = so_find_addr_safe("Java_com_ea_ironmonkey_RunLoop_nativeOnRunLoopTick");
    debugPrintf(">> entrando no render loop (tick=%p)\n", (void *)tick);
    int frames = getenv("NFS_FRAMES") ? atoi(getenv("NFS_FRAMES")) : 600;
    int recov = getenv("NFS_TICKRECOVER") != NULL;
    for (int f = 0; f < frames && tick; f++) {
      if (recov) {
        /* tick recuperável: se crashar, pula esse frame e continua (a engine
         * pode avançar o estado/carregar assets nos frames seguintes) */
        if (sigsetjmp(g_init_jmp, 1) == 0) {
          g_init_armed = 1;
          ((void(*)(void*,void*))tick)(env, fake_this);
          g_init_armed = 0;
        } else {
          g_init_armed = 0;
          if (f < 3 || g_init_skips < 8) debugPrintf("[frame %d CRASHOU -> pulado]\n", f);
          g_init_skips++;
        }
      } else {
        ((void(*)(void*,void*))tick)(env, fake_this);
      }
      if (f < 5 || f % 60 == 0) debugPrintf("[frame %d]\n", f);
      usleep(16000);
    }
    debugPrintf("=== render loop terminou (%d frames) ===\n", frames);
  }

  debugPrintf("=== F3: render executado ===\n");
  return 0;
}
