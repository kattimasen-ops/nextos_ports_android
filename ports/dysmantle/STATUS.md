# DYSMANTLE (10tons NX engine / GameActivity AGDK, Android) → NextOS aarch64 / Mali-450 (Utgard, GLES2)

so-loader do `libNativeGame.so` (arm64) + `libc++_shared.so`. APK: DYSMANTLE 1.4.1.12 (modado "APK_Award").
Device: Amlogic-old (S905X, Mali-450 Utgard, GLES2, fbdev), nextos-87.

## 🎉 MARCO: RENDERER GLES2 100% INICIALIZADO + JOGO CARREGANDO TEXTURAS

### O QUE FUNCIONA ✅ (evidência: gamedata/DYSMANTLE.log + run.log)
1. **Loader 2-módulos** libc++_shared + libNativeGame, 379 ctors, JNI fake, SDL2.
2. **🔑 GameActivity (AGDK), não NativeActivity** — struct android_app layout próprio
   (window@56, mutex@200, cond@240, msgread@288, pendingWindow@344, motionEventFilter@376)
   + process_cmd chama android_app_pre_exec_cmd/post_exec_cmd do glue. Destravou hang.
3. **CONTEXTO GLES2 NO MALI-450 + "Renderer Initialization done."** ← renderer COMPLETO.
   - Engine: "Using OpenGL renderer", "EGL context version is 1.4", glGetString/glGetIntegerv OK.
4. **Som**: Oboe falha (patch) → **fallback "Null" succeeded** → segue sem áudio.
5. **"Initializing Program 'DYSMANTLE'"** + **CARREGANDO TEXTURAS** (a maioria carrega OK,
   dados reais ffffff00) via fread no pak (data.pak fopen'd, índice parseado certo).

### FIXES-CHAVE (todos em main.c/imports.c)
- **🩹 STACK SMASH = falso-positivo de TLS**: a engine (bionic) lê a stack-canary de
  `tpidr_el0+0x28`; sob glibc colidia com `_Thread_local` do egl_shim (has_real_gl etc.) →
  guard mudava no meio da função. FIX = tirar `_Thread_local` do egl_shim + neutralizar
  `__stack_chk_fail` (override no-op) p/ os demais paths (libc++ TLS).
- **SwappyGL_init** retornava ptr par → tbz interpretava como falha → "Unable to init renderer".
  FIX = patch `SwappyGL_init`→return 1, `isEnabled`→0.
- **SoundImpOboe::Initialize** crasha (Oboe STL/JNI) → patch return 0 → fallback Null.
- **__sF / android_set_abort_message** UNRESOLVED no libc++ (std::cerr) → providos (bionic_sF
  + wrappers fwrite/fputs/fprintf, do bully).
- **setjmp/longjmp** → mapeados p/ `_setjmp`/`_longjmp` glibc (sem sigmask, casa bionic; libjpeg).
- **__register_frame** do .eh_frame do jogo (@0x349900) — módulo custom-loaded, unwinder C++.
- **AAsset_openFileDescriptor** = dup(fileno) (offset compartilhado). fopen/ifstream redirect→assets/.
- **NXI_GetProductValue("opengl_version")** GOT-hook → "2.0" (caminho ES2).
- **nx_run_no_popups=1** + NXD_ShowPopup no-op + ImageWriterJPEG::Initialize=0 (anti-crash falha textura).

### ✅ TEXTURAS RESOLVIDAS (ETC2→JPEG/PNG) + 🧱 MURO = compilador Squirrel (autoexec.nut)

**TEXTURAS (resolvido):** o APK modado "APK_Award" tem os JPEG/PNG de UI com **size=0** (vazios),
só a versão **`.ktx` (ETC2_RGB8, glInternalFormat=0x9274 = GLES3)**. Mali-450 Utgard é GLES2
(sem ETC2) → a engine cai no .jpg/.png vazio → "Not a JPEG: starts with 0xff 0xd9". `NX_Graphics_
IsTextureFormatSupported`→1 NÃO redireciona (a engine escolhe .jpg por GL caps). **FIX = decodificar
os ETC2 KTX → JPEG/PNG no PC e preencher os slots vazios no pak** (`tools/fix_empty_textures.py`,
usa texture2ddecoder+PIL). data.pak (primário): 466 .jpg in-place + 3 .jpg + 70 .png anexados.
grunge passou (carrega JPEG real 1024x1024, ver tools/grunge_decoded_proof.jpg). Formato pak:
magic"PAK\0V11\0"(8)+idx_offset(u32)+filesize(u32); índice = nome\0+offset+size+hash+pad (4×u32).
In-place (sobrescreve slot .ktx, mesmo offset) OU anexa (move índice). Paks corrigidos em stage/assets/.

**🧱 MURO ATUAL = compilador SQUIRREL crasha compilando `autoexec.nut`.** BRK-trap em sq_compile
(0x64454c) revelou: o 1º script `autoexec.nut` (sourcename) crasha no lexer ao criar o token da
string literal **"UI AUTOEXEC"** (exatamente **11 chars = n do memcpy**). Crash = `memcpy(dst=NULL,
src=0x2964, n=11)` na worker thread (`ScreenLoading_LoadingThread`). Stack: SQLexer::ReadNumber /
SQCompiler::ShiftExp / SQFuncState::GetConstant/PushTarget / SQString. dst=NULL + src=ponteiro
**truncado/uninit (~0x2900, varia por run)** = **SQLexer._longstr (sqvector) NÃO inicializado**
e/ou string-table do SQSharedState NULL → **VM/compilador Squirrel mal-construído na worker thread**.
(grunge era red herring — o crash sempre foi o Squirrel, logado logo após o erro da grunge.)

### PRÓXIMOS PASSOS (muro Squirrel)
1. Investigar a criação do VM Squirrel (sq_open/NX wrapper): SQSharedState/string-table/_longstr
   uninit → ver se falta função stub, ou se é construção C++ (operator new/ctor) na worker thread.
2. Conferir se SQCompiler/SQLexer roda construído (ctor) vs malloc cru; checar ABI sqvector.
3. Comparável a [[project_hollow_knight_unity_soloader_mali450]] em profundidade.

## Build / Deploy / Teste
`cd ports/dysmantle && ./build.sh` → `scp dysmantle nextos-87:/storage/roms/dysmantle/`.
Dados: `~/dysmantle-build/stage/` → rsync p/ device (libNativeGame.so + libc++_shared.so + assets/ 733MB).
`gamedata/` precisa existir (mkdir) p/ o log. Rodar: `bash diag5.sh` (detached) → run.log + gamedata/DYSMANTLE.log.
BRK-trap tracer (install_brk_traps, comentado) p/ rastrear funções locais. Crash handler faz stack-scan
(g_load_base, range 0x463000-0xd8e000). DYSMANTLE_ASSETS=assets, SDL_VIDEODRIVER=mali.
