#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <SDL2/SDL.h>
#include <signal.h>
#include <ucontext.h>
#include <pthread.h>
#include "so_util.h"
#include "imports.h"
#include "jni_shim.h"
extern void re4_fill(void);
extern void recon_wire_pthread(void (*)(const char *, void *));
extern void *jni_find_native(const char *);
extern void egl_shim_create_window(void);
FILE *stderr_fake;
void *asset_open(const char *p){ (void)p; return NULL; }
typedef int jint; typedef jint (*JNI_OnLoad_t)(void*,void*);
#define SO_NAME "libunity.so"
static void re4_set_import(const char *name, void *fn){
  for(size_t i=0;i<dynlib_numfunctions;i++)
    if(!strcmp(dynlib_functions[i].symbol,name)){ dynlib_functions[i].func=(uintptr_t)fn; return; }
}
static void *N(const char*n){ void*p=jni_find_native(n); fprintf(stderr,"  native %s = %p\n",n,p); return p; }
/* TLS bionic-safe: bionic ignora delete de key invalida; glibc crasha. Rastreamos. */
static unsigned char g_key_valid[4096];
static int sh_key_create(pthread_key_t *k, void(*d)(void*)){ int r=pthread_key_create(k,d); if(r==0 && *k<4096) g_key_valid[*k]=1; return r; }
static int sh_key_delete(pthread_key_t k){ if(k<4096){ if(!g_key_valid[k]) return 0; g_key_valid[k]=0; } return pthread_key_delete(k); }
extern void *text_virtbase;
static void on_segv(int sig, siginfo_t *si, void *uc_){
  ucontext_t *uc=(ucontext_t*)uc_;
  unsigned long pc=uc->uc_mcontext.arm_pc, lr=uc->uc_mcontext.arm_lr;
  unsigned long base=(unsigned long)text_virtbase;
  fprintf(stderr,"\n[SEGV] fault=%p pc=0x%lx lr=0x%lx",si->si_addr,pc,lr);
  if(pc>=base && pc<base+0x2000000) fprintf(stderr," (unity+0x%lx)",pc-base);
  fprintf(stderr," lr_off=0x%lx\n",lr-base);
  FILE *m=fopen("/proc/self/maps","r"); char ln[300];
  while(m && fgets(ln,sizeof ln,m)){ unsigned long a,b; if(sscanf(ln,"%lx-%lx",&a,&b)==2 && pc>=a && pc<b){ fprintf(stderr,"[SEGV-LIB] %s",ln); break; } }
  if(m) fclose(m);
  fprintf(stderr,"[REGS] r0=0x%lx r1=0x%lx r2=0x%lx r3=0x%lx r4=0x%lx\n",
    uc->uc_mcontext.arm_r0,uc->uc_mcontext.arm_r1,uc->uc_mcontext.arm_r2,uc->uc_mcontext.arm_r3,uc->uc_mcontext.arm_r4);
  unsigned long sp=uc->uc_mcontext.arm_sp;
  fprintf(stderr,"[STACK->unity] ");
  for(int k=0;k<128;k++){ unsigned long v=*(unsigned long*)(sp+k*4);
    if(v>=base && v<base+0x2000000) fprintf(stderr,"unity+0x%lx ",v-base); }
  fprintf(stderr,"\n");
  _exit(139);
}
int main(void){
  struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_sigaction=on_segv; sa.sa_flags=SA_SIGINFO; sigaction(SIGSEGV,&sa,0); sigaction(SIGBUS,&sa,0);
  fprintf(stderr,"=== RE4 Unity 2018 (ARM32 GLES2) ===\n");
  size_t hs=192*1024*1024;
  void *heap=mmap(NULL,hs,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  if(heap==MAP_FAILED){perror("mmap");return 1;}
  if(so_load(SO_NAME,heap,hs)<0){fprintf(stderr,"load FALHOU\n");return 1;}
  if(so_relocate()<0){fprintf(stderr,"reloc FALHOU\n");return 1;}
  re4_fill(); recon_wire_pthread(re4_set_import);
  re4_set_import("pthread_key_create",(void*)sh_key_create);
  re4_set_import("pthread_key_delete",(void*)sh_key_delete);
  so_resolve(dynlib_functions,dynlib_numfunctions,0); so_finalize();
  so_execute_init_array();
  fprintf(stderr,"[A] engine init OK (372 ctors)\n");
  void *vm=NULL,*env=NULL; jni_shim_init(&vm,&env);
  uintptr_t onload=so_find_addr_safe("JNI_OnLoad");
  jint ver=((JNI_OnLoad_t)onload)(vm,NULL);
  fprintf(stderr,"[B] JNI_OnLoad=0x%x\n",ver);
  static long t=0xA1, surf=0x5F, ctx=0xC0;
  /* janela GLES2 do device (egl_shim do core, provado) */
  if(SDL_Init(SDL_INIT_VIDEO)!=0) fprintf(stderr,"SDL_Init: %s\n",SDL_GetError());
  egl_shim_create_window();
  fprintf(stderr,"[C] janela SDL/GLES2 criada\n");
  void *fn;
  if((fn=N("initJni"))) ((void(*)(void*,void*,void*))fn)(env,&t,&ctx);
  fprintf(stderr,"[D] initJni OK\n");
  if((fn=N("nativeRecreateGfxState"))) ((void(*)(void*,void*,int,void*))fn)(env,&t,0,&surf);
  if((fn=N("nativeResume"))) ((void(*)(void*,void*))fn)(env,&t);
  if((fn=N("nativeSendSurfaceChangedEvent"))) ((void(*)(void*,void*))fn)(env,&t);
  if((fn=N("nativeFocusChanged"))) ((void(*)(void*,void*,int))fn)(env,&t,1);
  fprintf(stderr,"[E] lifecycle OK -> nativeRender loop\n");
  void *render=N("nativeRender");
  for(int f=0; render && f<1200; f++){
    ((unsigned char(*)(void*,void*))render)(env,&t);
    if(f<5||f%100==0) fprintf(stderr,"[render %d]\n",f);
  }
  fprintf(stderr,"=== render loop terminou ===\n");
  return 0;
}
