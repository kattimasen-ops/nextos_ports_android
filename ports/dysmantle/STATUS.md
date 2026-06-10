# DYSMANTLE (10tons NX engine, Android) → NextOS aarch64 / Mali-450 (Utgard, GLES2)

so-loader do `libNativeGame.so` (arm64) + `libc++_shared.so`. APK: DYSMANTLE 1.4.1.12.
Device: Amlogic-old (S905X, Mali-450 Utgard, GLES2, fbdev), nextos-87.

## Estado atual: render init trava em stack corruption (NÃO resolvido)

### O QUE JÁ FUNCIONA ✅
1. **Loader 2-módulos**: carrega libc++_shared.so (snapshot 2336 símbolos) + libNativeGame.so.
   Só 2 UNRESOLVED no libc++ (`__sF`, `android_set_abort_message`), não-críticos.
2. **379 construtores (init_array)** rodam completos.
3. **JNI fake** responde (GetCPUDescription, GetDeviceModel, GetRendererType, GetLanguage...).
4. **SDL2 + EGL→GLES2 (egl_shim) cria contexto GLES2 no Mali-450 Utgard** (muro principal vencido):
   `egl_shim: Window created 1280x720 / GL share-root context created`.
5. **🔑 GameActivity (AGDK), NÃO NativeActivity clássico** — DESTRAVE PRINCIPAL:
   a engine usa o glue **GameActivity**. Struct `android_app` tem layout DIFERENTE
   (offsets confirmados via disasm do glue estático): `onAppCmd@8, activity@16, config@24,
   savedState@32, looper@48, window@56, activityState@88, flag@92, mutex@200, cond@240,
   msgread@288, msgwrite@292, pendingWindow@344, motionEventFilter@376`.
   - Struct reescrita em `android_shim.h` p/ esse layout.
   - `process_cmd` chama os helpers exportados do glue `android_app_pre_exec_cmd`/`post_exec_cmd`
     (pre_exec faz `window=pendingWindow`, broadcast cond, seta flag@92 que o loop do android_main espera).
   - Isso destravou o hang em `android_app_set_motion_event_filter` (que lockava mutex@200 em lixo).
6. **Engine chega no render init**: escolhe "OpenGL renderer", "Initializing renderer 'OpenGL ES':
   1280x720 fullscreen", "EGL context version is 1.4".

### O MURO ATUAL 🧱 — stack corruption em RendererImplementationOpenGL::Initialize
`*** stack smashing detected ***` (canary). Cadeia (stack scan):
android_main → NXI_Init(0x4ea730) → NXI_InitRendImp(0x4e2d14) → 0x4e8324 →
**RendererImplementationOpenGL::Initialize (0x51abcc, frame 0x130, canary @ sp+0xe8)**.

- A função: NXI_GetProductValue("opengl_version") → Version::FromString → **ContextImpEGL::Initialize**
  (faz toda a sequência EGL: getdisplay/initialize/choose/createsurface/createcontext/makecurrent —
  TODA logada e bem-sucedida) → InitializeVersions → **GL::APIManager ctor (sp+8, ~220B)** →
  **GL::APIManager::GetFunctions** (GetStandardFunctions + GetExtensionNames) → VertexArrayManager/
  UniformManager::Initialize → GetInteger(GL_MAX_TEXTURE_SIZE) → ... canary smash no epílogo (0x51ae90).
- O smash é ANTES de qualquer `glGetString` (my_glGetString=0 chamadas) — confirmado que NÃO é a
  string de extensões longa do Mali.
- **Neutralizar `__stack_chk_fail`** (override→no-op) faz a execução PASSAR da canary, mas crasha
  logo depois com **SIGSEGV em libc+0x7d8b4 com ponteiro quase-nulo** → a corrupção é REAL (não só
  a canary): um ponteiro local é corrompido e usado adiante.
- Provável causa: `GL::APIManager`/`GetStandardFunctions` escrevendo ponteiros de função além do
  buffer de ~220B na pilha (engine dimensionada p/ ES3 vs nosso contexto ES2 Utgard), OU um valor
  de config (nx_state) lixo no nosso setup virando size/count de um buffer.

### Hipóteses testadas (não resolveram)
- glGetString override (strings curtas) — engine não chama antes do smash.
- eglGetProcAddress/dlsym override — engine resolve GL via GOT no init_array (dc8000→dc9000), não runtime.
- Forçar opengl_version="2.0" (caminho ES2) via GOT-hook NXI_GetProductValue — real era null, sem efeito.
- Neutralizar SwappyGL_init/isEnabled — smash é antes do SwappyGL.
- NOP na 2ª chamada virtual (vtable+64) do renderer — sem efeito.

### Próximos passos sugeridos
1. Achar o offset EXATO do overflow: instrumentar via trampolim no ENTRY de
   `GL::APIManager::GetStandardFunctions` (GOT-hook não pega função local — precisa patch de entry),
   ou watchpoint via mprotect na página da canary (sp+0xe8) p/ pegar quem escreve.
2. Investigar o que `GetStandardFunctions` escreve no APIManager e se o count depende de um valor
   de `nx_state`/Version mal-inicializado no nosso ambiente.
3. Conferir se algum valor do `egl_shim` (eglGetConfigAttrib p/ 0x303b/0x303c retornando 0, ou
   eglQuerySurface) é usado como size/count.

## Build / Deploy / Teste
```bash
cd ~/nextos_ports_android/ports/dysmantle && ./build.sh        # gera ./dysmantle (aarch64)
scp -q dysmantle nextos-87:/storage/roms/dysmantle/dysmantle
# dados (uma vez): rsync ~/dysmantle-build/stage/ -> /storage/roms/dysmantle/ (libNativeGame.so,
#   libc++_shared.so, assets/ = data.pak 543MB + data-gfx1200.pak + data-localizations.pak, 733MB)
ssh nextos-87 'cd /storage/roms/dysmantle; bash diag5.sh'      # roda detached, captura run.log/debug.log
```
Diagnóstico: `run.log` (stderr loader + crash bt), `debug.log` (egl_shim/jni_shim/process_cmd).
Stage local: `~/dysmantle-build/stage/`. Crash handler resolve frames via /proc/self/maps ("?+0xVADDR"
= offset no libNativeGame, já que so_load mmapeia anônimo).

## Arquivos-chave do port
- `src/main.c` — loader 2-módulos + entrada android_main + patches (SwappyGL, GOT-hook NXI_GetProductValue)
  + crash handler + SIGUSR1 backtrace.
- `src/android_shim.{c,h}` — **struct android_app layout GameActivity** + process_cmd c/ pre/post_exec_cmd.
- `src/imports.c` — tabela de overrides (EGL→egl_shim, AAsset/ANativeWindow/ALooper extras, OpenSL,
  bionic _chk, dlsym/glGetString/glGetIntegerv overrides, __stack_chk_fail neutralizado p/ diagnóstico).
- `src/egl_shim.c` — EGL→SDL2 GLES2 (força contexto ES2; proc override routing).
- `src/so_util.c`, `src/pthread_bridge.c` — do bully (aarch64 RELA, dlsym fallback, snapshot, bionic→glibc pthread).
