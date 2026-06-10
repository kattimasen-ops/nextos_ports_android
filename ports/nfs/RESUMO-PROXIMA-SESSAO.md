# NFS Most Wanted — RESUMO p/ próxima sessão (handoff)

## ONDE PAROU (~95% do boot — engine boota e LÊ o OBB; falta 1 bug no parse de asset)

### Rodar (device nextos-87, ES mascarado, Mali fbdev)
```
cd ~/nextos_ports_android/ports/nfs && ./build.sh && scp -q nfs nextos-87:/storage/roms/nfs/
ssh nextos-87 'cd /storage/roms/nfs && export LD_LIBRARY_PATH="/usr/lib32:/storage/roms/nfs" \
  SDL_VIDEODRIVER=mali NFS_INIT=1 NFS_SKIPCTOR="186,187,188"; ./nfs 2>&1 | tail -40'
```
OBB (623MB) já no device: `/storage/roms/nfs/data/Android/obb/com.ea.games.nfs13_row/main.1003128.com.ea.games.nfs13_row.obb`

### O QUE JÁ FUNCIONA (não re-investigar)
- Build armhf (toolchain NextOS armv8a-emuelec-gnueabihf, glibc 2.43=device, auto-detect glob).
- Multi-módulo: libc++_shared + libNimble + FMOD + libapp, 0 unresolved (so_snapshot_symbols+fallback dlsym).
- init_array COMPLETO com `NFS_INIT=1` + padding malloc +64B (overflow do ctor 11) + `NFS_SKIPCTOR="186,187,188"`.
- JNI_OnLoad(0x10002) + nativeOnCreate + lifecycle (onStart/SurfaceCreated/SurfaceChanged 1280x720/onResume) + render loop (RunLoop tick) em main.c.
- **softfp ABI shim** (src/softfp_shim.c, igual RE4): engine é SOFTFP, glibc HARDFP → wrappers pcs("aapcs"). Roteado em so_resolve (softfp_resolve antes do dlsym) + my_dlsym.
- jni_shim: paths (File→getAbsolutePath/getObbFullPath/getFilesDir/getExternalStorageDirectory), isObbAssets→1, DisplayMetrics (GetIntField widthPixels=1280/heightPixels=720, GetFloatField density=2.0), config strings (ApplicationVersion=1.3.128/DeviceName/Locale/Language/OsVersion), getPackageName.
- **A engine ABRE+LÊ+PARSEIA o OBB** (fopen OK; fread header 1028B + parse byte-a-byte do índice).
- Crash handler: ignora `si_code<=0` (asserts deliberados raise SIGSEGV), maps lookup do PC, recover null-copy, tick-recovery (sigsetjmp).

### O MURO (1 bug isolado) — objeto GARBAGE no parse do índice do OBB
A engine cria um objeto de asset garbage (vtable/state inválido) e ASSERT nele (raise SIGSEGV
si_code=-6=SI_TKILL, debug build = breakpoint). Ignorar o assert → usar o objeto → crash real
(`blx r1` com r1 lixo, PC 0xe12fff36). Só ~1046 bytes lidos do OBB (header + início do dir) antes do crash.

**JÁ DESCARTADO COM TESTE (não repetir):** relocações (todas aplicadas, .rel.dyn cobre DT_REL
388264B), softfp (todas math+f-variants), arquivos faltando (só OBB, 0 MISS), asserts (deliberados),
version/config/screen strings, OBB path/abertura/leitura.

### PRÓXIMOS PASSOS (RE focada do objeto garbage)
1. **Instrumentar a CRIAÇÃO do objeto** entre o read do header (1028B) e o crash. Cadeia:
   `0x47a920`(release refcontado) ← `0x624a98`(0x624a40 cria stack-struct+chama 0x626e50) ←
   `0x4c971c`(0x4c9700) ← `0x4c966c`(0x4c9640). O objeto garbage é arg0 de 0x624a40 (vem de 0x4c9700).
   `0x626e50` = tokenizer (split por '.'=0x2e e ','=0x2c) com callback `blx r7`(global).
   → desmontar 0x4c9640/0x4c9700 (onde o objeto é criado) p/ ver a fonte do garbage.
2. **Entender o formato do índice do OBB EA** (hexdump dos 1028B header + o que vem depois):
   `xxd -l 1100 ~/Downloads/obb_extract/com.ea.games.nfs13_row/main.1003128.com.ea.games.nfs13_row.obb`
   Ver magic/versão/contagem/offsets. Comparar com o que a engine lê (hookar fread/fseek mostra o padrão).
3. Hipóteses do garbage: objeto alocado-mas-não-construído (vtable=lixo do malloc, chamada virtual antes do ctor),
   OU asset-não-achado no índice → objeto-vazio retornado. Checar `fseek`/`ftell` no OBB (hook).
4. Estudar como Bully/DYSMANTLE/reVC tratam o asset/resource system + libc++ iostream (DYSMANTLE expôs basic_filebuf::open).

### Flags de debug (env)
`NFS_INIT` `NFS_SKIPCTOR` `NFS_FOPENLOG`(fopen/fread/open) `NFS_PTLOG`(pthread) `NFS_DLSYMLOG`
`NFS_TICKRECOVER` `NFS_FRAMES=N` `NFS_NOASSERTIGNORE`(volta a morrer no assert) `NFS_INITDBG`(log por ctor).

### Recon local
`~/Downloads/nfs-analysis/lib/armeabi-v7a/` (libapp.so etc, desmontar com `aarch64-linux-gnu-objdump -d`).
libc do device: `/tmp/devlibc32.so` (crash em libc+0x7c720 = pthread_kill/raise).
