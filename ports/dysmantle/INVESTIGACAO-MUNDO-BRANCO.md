# DYSMANTLE — Investigação MUNDO BRANCO (diário de testes)

Objetivo: o jogo roda 100% (imagem+som+controle) mas no gameplay o **mundo não renderiza**:
chão branco, árvores/barris/matos/cabeça/armas do player NÃO aparecem; corpo+calça do player
e tampa do bonker renderizam (com cor). "algo surreal".

> ⚠️ NÃO REPETIR testes já feitos abaixo. Atualizar este MD a cada teste novo.

## FATOS CONFIRMADOS (pipeline)
- Cena renderiza num **FBO** (color=tex19 GL_COLOR_ATTACHMENT0, depth=tex20 GL_DEPTH_ATTACHMENT).
  FBO é **COMPLETO** (glCheckFramebufferStatus=0x8CD5). Depois é **composto fullscreen** na tela
  com o shader diffuse: `gl_FragColor = _vary_color * texture2D(_tex_diffuse, uv)`.
- A tela visível = o FBO composto (clear da TELA nunca aparece).
- **60% do FBO fica na cor de clear** = 60% do mundo nunca é desenhado no FBO.
- Draws: começam na tela (cur_fbo=0, menus), depois gameplay desenha no FBO (cur_fbo=1).
  No gameplay há draws no FBO (fbo>4000) E na tela (16000+).

## CAUSA-RAIZ (em apuração): vertex buffers format=0 falham
- `NX_Graphics_CreateVertexBufferWithVertices(fmt, verts, count, flags)` @0x4837d4:
  chama `Legacy::ConvertVertexFormatToVertexElements(fmt)` @0x574034; se fmt=0 → 0 elementos →
  "Can't create a vertex buffer with an unknown vertex format" → retorna NULL → objeto não desenha.
- ConvertVertexFormat trata formatos: 0x1,0x7,0xe,0xf,0x1f,0x60,0x61,0x67,0x6e,0x6f,0x7f (bitfield).
  Bits: 0x1=pos 0x2=uv 0x4=normal 0x8=cor 0x10=tangent (ordem de GetVertexComponentFlags).
- **2 CALLERS de createvb(fmt=0):**
  1. `ModelSurface::GenerateVerticesByFormat(this,fmt)` @0xa025b8 (call @0xa0268c): passa
     createvb(fmt, NULL, count, 4); verts preenchidos depois por GenerateStreamDataForVertexFormat.
  2. `ModelSurface::InitializeVertexAndIndexBuffers()` @0xa03b70 (call @0xa03c20). ← NOVO
- `GetVertexComponentFlagsAkaVertexFormat` @0xa05108 computa fmt dos 5 stream ptrs em
  this+64(pos) +72(cor) +80(uv) +88(normal) +96(tangent).

## FIX PARCIAL APLICADO (ON por default no código)
- `hook_genverts` (main.c, detour @0xa025b8): quando fmt==0, computa o formato real dos 5 stream
  ptrs e passa esse no lugar de 0. **Reduziu erros 41633→2587.** Surfaces com 4 streams
  (pos|cor|uv|normal) viram 0xF. **MAS visualmente NÃO mudou** (FBO segue 60% clear).
- Restam **2587** falhas: vêm de `InitializeVertexAndIndexBuffers`@0xa03c20 + surfaces do
  genverts com **streams TODOS nulos** (GENV-NULLSTREAMS: count=78/32 mas ptrs todos null).

## TESTES JÁ FEITOS (NÃO REPETIR)
| Teste | Env/método | Resultado |
|---|---|---|
| Remap format 0→0x7 | DYSMANTLE_VB_FMT0=7 | PIOR: bolas pretas (garbage, x1 é NULL p/ esses) |
| Clear TELA magenta | CLEAR_TEST=2 | 0% magenta → tela nunca mostra clear próprio (FBO cobre) |
| Clear FBO magenta | CLEAR_TEST=3 | **60% magenta** → 60% do FBO não é desenhado (chão falta) |
| Clear tudo magenta | CLEAR_TEST=1 | chão fica magenta (confirma chão=clear) |
| Shaders compile/link | SHADER_DUMP + hooks | 0 falhas (124 shaders OK) |
| Shader força vermelho | SHADER_RED | tela 100% vermelha (composite usa shader diffuse) |
| Shader só textura | SHADER_TEX | tela 100% branca = ENGANOSO (FBO vazio, não textura branca) |
| Texturas comprimidas | hook glCompressedTexImage2D | 0 (nenhuma comprimida) |
| Texture arrays GLES3 | hook glTexImage3D/Storage3D | 0 (não usa) |
| Dados das texturas | TEX_LOG + dump pixels | RGBA8 uncompressed, DADOS REAIS (só 3/57 brancas) |
| Params de textura | TEXPARAM_LOG | engine já usa MIN/MAG=LINEAR + WRAP=CLAMP (NPOT-safe!) |
| Force NPOT CLAMP/LINEAR | (default on) / NPOT_OFF | sem diferença (já era safe) |
| Force ETC2 OFF | (default) vs FORCE_ETC2 | idêntico (não era isso) |
| GLVER 3.0/3.1 | DYSMANTLE_GLVER=3.0 | remove erros "unknown vertex" MAS chão segue branco |
| Atributos de vértice | ATTR_LOG | pos→0 cor→1 uv→2 normal→3 (glBindAttribLocation); layout pos vec3@0 + cor rgba8@12 + uv vec2@16 stride24 ✓ |
| Textura bound no draw | DRAW_LOG | texturas REAIS bound unit0 (tex25/26/30); tex[1]=11 fixo unit1 |
| Sampler→unit | UNIF_LOG | _tex_diffuse→loc1, setado val=0 ou 1 (investigar) |
| FBO completo? | hook CheckFramebufferStatus | COMPLETO (0 incompletos) |
| Fix format-0 (genv) | hook_genverts (default) | erros 41633→2587, FBO ainda 60% clear |

## HIPÓTESES ABERTAS (a testar)
- [ ] Os 2587 restantes (InitializeVertexAndIndexBuffers + null-streams) = os objetos visíveis?
- [ ] Surfaces com streams nulos: por que não alocados? (count>0 mas ptrs null) →
      ver AllocateVertexStreamsForVertexFormat @0xa04e84 / InitializeForVertexFormat @0xa05150
- [ ] DEPTH: depth-texture attachment no Utgard quebra depth test? (forçar glDepthFunc ALWAYS)
- [ ] As surfaces 0xF "consertadas" pelo genv DESENHAM mesmo? (verificar se entram no FBO)
- [ ] Composite/lighting: _vary_color do quad fullscreen lava branco? (UNIF/blend)

## FERRAMENTAS
- `capgame.sh <fmt0>`: navega ao gameplay (PB_SELFTEST) + dd /dev/fb0 → fb_game.raw
- PC: converto fb_*.raw → PNG (BGRA, 1280x720, primeiros 8MB) e LEIO a imagem p/ julgar sozinho
- ENVS diag: VB_LOG/VB_FMT0/VB_DUMP/VB_CALLER, GENV_NOFIX, SHADER_DUMP/RED/TEX,
  TEX_LOG/TEXPARAM_LOG/NPOT_OFF, CLEAR_TEST=1/2/3, DRAW_LOG, ATTR_LOG, UNIF_LOG, GLVER,
  PB_SELFTEST (navega: 1× baixo + A/X)

## SESSÃO 2 (continuação)
| Teste | Env/método | Resultado |
|---|---|---|
| Fix format-0 entry e[0]=e[1]@4 | hook_initbufs (INITBUF_FIX) | VB-fail 2586→1347 MAS branco 0.58 (sem mudança) |
| Dump verts@8 das entradas | INITBUF dump | verts@8 começa com PONTEIRO (0x7f...) = OBJETO, não float cru! createvb gera lixo. Por isso 3 fixes de formato não mudaram nada |
| Depth ALWAYS | DEPTH_ALWAYS=1 | 0.58 branco (DESCARTADO) |
| Depth DISABLED | DEPTH_ALWAYS=2 | 0.58 branco (DESCARTADO) |

**CONCLUSÃO PARCIAL:** format-0 buffer creation NÃO é a causa visual (3 fixes, 0 mudança).
Streams da surface (this+64..96) são NULL p/ os objetos faltando. verts@8 das entradas é um
OBJETO (ponteiro), não dados crus. **HIPÓTESE FORTE NOVA: os STREAMS de vértice não são
alocados** (AllocateVertexStreamsForVertexFormat @0xa04e84 nunca roda ou falha) → sem geometria.
Possível causa: OOM/alocador, ou path de init que NÓS quebramos. PRÓXIMO: hookar
AllocateVertexStreams + ver se roda p/ as surfaces null-stream; comparar com ports que funcionam.

## SESSÃO 2 — PIVÔ IMPORTANTE
| Teste | Método | Resultado |
|---|---|---|
| Instancing GLES3? | readelf relocs | NÃO usa (descartado) |
| Worker thread sem ctx GL? | BUF_LOG tid | glBufferData SÓ na main thread (tid único), 0 erros → DESCARTADO |
| Formatos de textura | TEXFMT histograma | só RGBA + DEPTH_COMPONENT (FBO). SEM LUMINANCE → descartado fix Bully |
| tex 11 (unidade 1 fixa) | dump | cinza real (a9a9a9), não branca dummy |
| Comparar surface OK vs quebrada | INITBUF_DUMPALL | **OK: streams não-nulos, fmt@0 correto (0x7F), verts@8=NULL, buf criado. QUEBRADA: streams NULOS, fmt@0=0, verts@8=objeto** |

**🔑 CONCLUSÃO-CHAVE:** os 3 fixes de format-0 (genv, initbuf e[0]=e[1], remap) reduziram erros
41633→1347 mas **NENHUM mudou a tela (0.58 branco sempre)**. Logo **format-0 NÃO é o mundo
visível** — são surfaces à parte (LOD/partículas/decals?) que falham e a engine ignora.
O **mundo visível (chão/árvores) usa surfaces FUNCIONANDO** (streams ok) que **renderizam
branco/invisível**. Estava caçando a coisa errada 50 iterações.

**REDIRECIONAR p/:** por que surfaces FUNCIONANDO (streams ok, buffer ok) do chão/props
renderizam branco/invisível? Suspeitas: (a) lighting/blend lava branco (testar glBlendFunc,
ainda NÃO testado — clr3 só testou clear); (b) shader/textura específica por tipo de objeto;
(c) coordenadas/projeção. AllocateVertexStreamsForVertexFormat@0xa04e84 escreve streams
this+64..96 baseado no formato (bits). AllocateVertexStreams(0)=sem streams.
