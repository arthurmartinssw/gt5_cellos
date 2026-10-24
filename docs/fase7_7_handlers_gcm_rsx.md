# Fase 7.7 — Espera da thread principal, handlers do GCM e estado do RSX

Projeto: GT5 CellOS (BCES00569), port nativo não oficial. Esta fase **não** renderiza nada, **não**
integra Vulkan e **não** executa código x86-64 nativo: o código do jogo continua no interpretador PPU.

## 1. Causa da espera da thread principal — não verificada, hipóteses ranqueadas

Fatos do log da 7.6:
- `main` em `sys_lwcond_wait(0xd001f938, 0)`, lwcond na **pilha** da main, cond `0x970000c1`, sem timeout,
  **nunca sinalizada** (0 sinais de qualquer tipo nesse objeto).
- `9qstY` e `ksF3` em `sys_lwcond_wait` sobre lwconds no heap (`0x5ffc8080`, `0x5ffbc088`).
- `MpAnk` em `sys_event_queue_receive(0x8d000001)`: é o paginador do jogo (fase 7.3) esperando faltas de página — normal.
- Imediatamente antes, a main registrou 4 handlers do GCM (VBlank `0x0183d948`, SecondV `0x0183d940`,
  Queue `0x0183d930`, Flip `0x0183d928`) que o runtime **nunca chamava**.

O que já se pode descartar com evidência: perda de sinal dentro do `Lv2Cond` para esse objeto. O contador de
sinais do objeto é zero — ninguém chamou signal/signal_all/signal_to nele. Um sinal "perdido" por ponteiro/id
errado retornaria ESRCH em silêncio; agora isso é registrado (`sys_lwcond_signal(...) -> ESRCH`, até 16 vezes).

| # | Hipótese | Por que é plausível | Como confirmar/rejeitar no próximo log |
|---|---|---|---|
| H1 | A cadeia produtora depende de eventos de vblank/flip/queue que nunca chegavam | Handlers registrados logo antes; o padrão "main espera numa lwcond local enquanto as threads de render esperam o display" é comum; no console esses eventos acontecem 60×/s sem o jogo pedir | Rodar o build novo. Se a main passa de `0xd001f938` e o relatório do RSX mostra `vblank 0x0183d948 xN` crescendo → confirmada. Rodar de novo com `--no-gcm-interrupts`: se o stall volta → confirmada por A/B |
| H2 | O produtor dependia de saída não inicializada de um dos stubs (id do `sys_mempool_create`, `count` do `sys_prx_get_module_list`) | Os dois retornavam `CELL_OK` sem escrever nada; a main os chamou antes de parar | `--no-gcm-interrupts` mantém as correções dos imports: se a main progride **também** com essa flag → H2 |
| H3 | O produtor é uma thread esperando outra coisa ainda não implementada (callback do sysutil, job SPURS, porta de evento) | Há dezenas de threads bloqueadas | O relatório novo imprime, para cada espera em cond, quem já sinalizou aquela cond e o estado atual dessa thread (cadeia de espera), e as 8 chamadas que a main completou antes de esperar |
| H4 | Erro de mapeamento guest↔host num lwcond copiado | Pouco provável (0 sinais no objeto) | Linha `sys_lwcond_signal(...) -> ESRCH` no log |

## 2. Dependência de sincronização e produtor esperado

- Dependência: `lwcond 0xd001f938` (pilha da main) + o lwmutex no campo +0 do struct; cond id `0x970000c1`.
- Produtor esperado: **não identificado** com os dados da 7.6 (a cond nunca recebeu sinal, então não há
  "sinalizador anterior"). O relatório agora diz exatamente isso (`expected producer: unknown — no thread has
  ever signaled this condition`) e adiciona: quem criou a cond (thread + LR), quem já esperou nela, e as últimas
  chamadas da main (por exemplo, um `sys_ppu_thread_create`, `sys_event_port_send` ou `cellGcmSetFlip` logo
  antes da espera aponta o produtor).
- Para as conds de `9qstY`/`ksF3` o relatório lista os sinalizadores anteriores e o que eles fazem agora —
  isso fecha a cadeia (ex.: "past signaler 0x010000xx _gcm_intr_thread: BLOCKED in ...").

## 3. Os handlers do GCM participam?

Provavelmente (H1), mas não está provado. O que mudou é independente do stall e necessário de qualquer forma:
o jogo registrou os handlers e um runtime correto precisa chamá-los.

Semântica verificada no RPCS3:
- `cellGcmSet{Flip,VBlank,Queue,User}Handler` apenas guardam o OPD (`Modules/cellGcmSys.cpp`).
- `_cellGcmInitBody` cria `_gcm_intr_thread` (prioridade 1, pilha 0x4000, `SYS_PPU_THREAD_CREATE_INTERRUPT`).
- vblank: `post_vblank_event` → `set_args 1, u64{1}` + `lle_call vblank_handler` (`RSXThread.cpp` linhas 1069–1077).
- flip: `handle_emu_flip` → `flip_status = DONE` (linha 3727) e depois `lle_call flip_handler` com 1 (3745–3755).
- queue: `flip_command` (HLE) chama `queue_handler(1)` quando o RSX chega ao comando de flip (`rsx_methods.cpp`).
- user: `user_command` chama `user_handler(arg)`.
- `cellGcmSetSecondVHandler` e `cellGcmSetGraphicsHandler`: `todo` no RPCS3 — **não** entregues aqui também.

Implementação (`hle/gcm_interrupts.{h,cpp}`): uma thread PPU de verdade, `_gcm_intr_thread`, criada no
`_cellGcmInitBody`, executa um trampolim de 16 instruções em memória guest:

```
loop: r3,r4 = gcm_interrupt_wait()   ; HLE: bloqueia no host até haver evento
      if r3 == 0: return              ; encerramento
      r2 = opd->toc; ctr = opd->code; r3 = r4; bctrl   ; handler do jogo, ABI do guest
      b loop
```

Os handlers só rodam nessa thread PPU (pilha e TLS próprios, mesmo interpretador) — nunca na thread do RSX nem
em outra thread do host. O lado host só enfileira eventos: o RSX chama `OnFlipCommand`/`OnFlipDone`/`OnUserCommand`;
os prazos de vblank (60 Hz, mesmo relógio de `cellGcmGetVBlankCount`) são calculados pela própria thread que espera.
Regras: handler capturado no momento do evento; handler 0 desliga (eventos já na fila são descartados);
flip/queue/user um chamado por evento, em ordem (fila de 64, excesso contado); vblank é nível — atraso de k
vblanks = 1 chamada + k−1 "missed". O objeto fica na tabela de objetos do kernel: o término do processo acorda a
espera, e ele sobrevive ao `CellGcmSys`.

Efeito colateral: a criação de `_gcm_intr_thread` no init desloca em +1 os ids das threads criadas depois
(o RPCS3 cria a mesma thread no mesmo ponto, então os ids ficam mais próximos dos dele).

Divergência conhecida, não alterada: o RPCS3 avança `control->put` no `cellGcmSetFlip` do contexto padrão;
nós não. Agora é mensurável: `flip command(s) written by cellGcmSetFlip*` maior que `reached in the FIFO`
com `put == get` indicaria um flip escrito e nunca enviado.

## 4. Os cinco imports

| NID | Nome (SHA-1 FNID verificado) | Assinatura | Antes do stall? | Pode afetar o boot? | Nesta fase |
|---|---|---|---|---|---|
| sysPrxForUser 0xCA9A60BF | `sys_mempool_create` | `(sys_mempool_t* pool, void* chunk, u64 chunk_size, u64 block_size, u64 ralignment)` | sim (log da main) | sim: o id ficava sem escrever | família inteira implementada (create/destroy/allocate/try_allocate/free/get_count), contrato do RPCS3 |
| sys_io 0x433F6EC0 | `cellKbInit` | `(u32 max_connect)` | sim | baixo | Init/End/GetInfo, sem teclado |
| cellKey2char 0xABF629C1 | `cellKey2CharOpen` | `(CellKey2CharHandle* handle)` (128 bytes opacos) | sim | nenhum (texto de teclado) | só as checagens do RPCS3; o handle não é escrito (layout desconhecido; o RPCS3 também não escreve) — dito no log |
| sysPrxForUser 0xA5D06BF0 | `sys_prx_get_module_list` | `(u64 flags, sys_prx_get_module_list_t* {be64 size; be32 max; be32 count; be32 idlist*})` | sim | médio: `count` ficava com lixo | implementado + syscall 494 |
| sys_io 0xC9030138 | `cellMouseInit` | `(u32 max_connect)` | sim | baixo | Init/End/GetInfo, sem mouse |

`sys_prx_get_module_list` (RPCS3 `Modules/sys_prx_.cpp` + `lv2/sys_prx.cpp`): EINVAL se `info` ou `idlist` nulos;
a biblioteca pede ao lv2 com flags 2 (ignora as do chamador); o lv2 só escreve se `flags & 2` e `size == 0x20`;
escreve até `max` ids (todos os PRX carregados exceto `/dev_flash/sys/external/liblv2.sprx`) e `count`; sempre
`CELL_OK`. Aqui só a libsre (LLE) é um PRX — as bibliotecas HLE não são, como no RPCS3 — então a lista é menor
que no console. Onde o RPCS3 tem comportamento indefinido (ponteiro ruim) retornamos EFAULT.

Os argumentos observados de cada import estão na linha `hle unimplemented ...` do log da 7.6 (r3–r6 e LR),
que o resumo enviado não incluía.

## 5. RSX: estado verificado

- Consome o command buffer real do jogo: sim, desde a fase 5 (thread "RSX" lendo `put`/`get`).
- Decodifica: saltos/call/return, referência, semáforos, flips (HLE 0xFEAC), user command (0xEB00) e conta
  draws (`NV4097_SET_BEGIN_END` 0x1808, `DRAW_ARRAYS` 0x1814, `INLINE_ARRAY` 0x1818, `DRAW_INDEX_ARRAY` 0x1824)
  e clears (`CLEAR_SURFACE` 0x1D94) — valores conferidos em `RSX/gcm_enums.h` do RPCS3.
- Se um draw é alcançado antes do stall: **desconhecido** — o log enviado não tinha a seção RSX. Agora há
  `first draw` / `first clear` / `first FIFO flip` / `first command-buffer submission` no log (uma vez cada).
- Rasterização ou API gráfica do host: **nenhuma**. `NullRsxBackend` conta métodos e não apresenta nada:
  "frames presented to a host surface: 0".
- Um quadro gerado pelo jogo não pode ser montado com o que existe hoje.

## 6. Arquivos

| Arquivo | Mudança |
|---|---|
| `src/hle/gcm_interrupts.{h,cpp}` (novo) | `_gcm_intr_thread`, trampolim, fila de eventos, vblank 60 Hz, relatório |
| `src/hle/cell_gcm_sys.{h,cpp}` | cria a thread no init; handlers entregues/gravados; contagem de flips escritos; relatório RSX completo; `set_interrupts_enabled` |
| `src/rsx/rsx_fifo.{h,cpp}` | `IRsxEventSink` (queue/flip/user), `RsxStats`, contadores, logs "first ..." |
| `src/rsx/rsx_methods.h` | `kGcmSetUserCommand`, métodos NV4097 de draw/clear |
| `src/rsx/rsx_backend.h` | `FramesPresented()` (0 no back end nulo) |
| `src/hle/sys_prx_for_user.cpp` | `sys_prx_get_module_list`, `sys_mempool_*`, procedência de lwcond, log de ESRCH |
| `src/lv2/sys_prx.{h,cpp}` | `ListModules`, `GetModuleList`, syscall 494, caminho guest do módulo |
| `src/hle/cell_pad.{h,cpp}` | `cellKb*`, `cellMouse*`, `cellKey2CharOpen` |
| `src/lv2/sys_sync.{h,cpp}` | `Lv2Cond`: criador, histórico de quem esperou/sinalizou |
| `src/ppu/ppu_interpreter.{h,cpp}`, `src/lv2/sys_ppu_thread.{h,cpp}` | anel lock-free das 8 últimas chamadas por thread |
| `src/lv2/stall_monitor.cpp` | cadeia de espera ("past signaler"), chamadas antes da espera, aridades novas |
| `src/main.cpp` | `--no-gcm-interrupts` |
| `tests/gcm_tests.cpp`, `tests/hle_output_tests.cpp` (novo), `tests/prx_tests.cpp`, `tests/timer_pad_sync_tests.cpp`, `CMakeLists.txt` | testes |

## 7. Build e testes (Linux x86-64 apenas; Windows não foi executado nesta fase)

- GCC 13.3 Release: 15/15 testes passam, sem avisos.
- GCC Debug + `-fsanitize=address,undefined`: 15/15 (`CELLOS_TEST_ITERATIONS=2000`).
- GCC RelWithDebInfo + `-fsanitize=thread`: 15/15, 0 relatórios do TSan (os avisos `-Wtsan` sobre
  `atomic_thread_fence` são antigos).
- Clang 18 Release: 15/15, sem avisos.
- O `-Warray-bounds` do GCC do MSYS2 nos testes de paginação continua não reproduzido aqui (GCC 13.3 e Clang 18).

## 8. Próximo comando

```
./build/gt5_cellos.exe "A:\Downloads\gt5_cellos\gt5_cellos\PS3_GAME\USRDIR\EBOOT.elf" --dev-hdd0 "A:\Downloads\gt5_cellos\dev_hdd0" --lle libsre.prx --resolution 1080 --max-insns 0
```
Comparação A/B (mesma linha + `--no-gcm-interrupts`) se a main ainda parar.

## 9. Assinaturas de log

Confirmam H1:
- `cellGcmSys _gcm_intr_thread 0x010000xx started (trampoline 0x...)`
- `cellGcmSetVBlankHandler(0x0183d948): delivered on _gcm_intr_thread ... (every vblank, 60 Hz, argument 1)`
- Seção RSX: `gcm interrupts (thread ...): vblank 0x0183d948 xN; flip 0x0183d928 x...; queue 0x0183d930 x...` com N crescendo
- Nenhum `primary thread made no progress`, ou a main em outro lugar que não `sys_lwcond_wait(0xd001f938, 0x0)`
- Depois: `rsx first command-buffer submission`, `first draw`, `first FIFO flip`

Rejeitam H1 (e o que olhar):
- Stall igual com `vblank ... x600+` → ler `expected producer`, `created by ... (lr ...)` e
  `completed calls before this one` da main.
- `vblank ... x0` com handler registrado → a thread de interrupção não roda; ver a linha `_gcm_intr_thread` nas threads PPU.
- `FATAL in thread ... _gcm_intr_thread` → o handler precisa de algo que falta (o relatório mostra a instrução).
- `sys_lwcond_signal(...) -> ESRCH` → mapeamento de objeto (H4).

## 10. Próximo marco e plano Vulkan

Roteiro de CPU (separado): o interpretador continua sendo a execução. Execução nativa x86-64 exige um recompilador
PPU (por bloco, em tempo de execução, ou estático sobre o EBOOT descriptografado) com o interpretador como
referência/fallback. Nada disso existe ainda.

Marco gráfico mensurável: **M1** — log com `first FIFO flip` e `draw begin(s) > 0` (o jogo chega a renderizar);
**M2** — primeira imagem gerada pelo jogo numa janela via Vulkan, `frames presented > 0`.

Plano de integração (nenhum código Vulkan nesta fase; o repositório não tem Vulkan nem janela):
1. Dependências/CMake: opção `CELLOS_VULKAN` (OFF por padrão); `find_package(Vulkan REQUIRED)` (MSYS2:
   `mingw-w64-ucrt-x86_64-vulkan-devel`; Linux: `libvulkan-dev`); `glslang` para os shaders no início; volk opcional.
2. Janela e superfície: Win32 nativo (`CreateWindowExW` + `vkCreateWin32SurfaceKHR`) sob `#ifdef _WIN32`,
   Xlib/Wayland sob `#ifdef __linux__` (ou GLFW para os dois); loop de mensagens numa thread de UI.
3. GPU e device: preferir `VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU`, vendor 0x1002 (AMD), nome com "RX 7600";
   Vulkan 1.3 (dynamic rendering, synchronization2, timeline semaphores); fila gráfica com present.
4. Swapchain: `VK_KHR_swapchain`, `B8G8R8A8_UNORM`, 1920×1080 do `cellVideoOutConfigure`, FIFO (60 Hz, casa com
   o vblank), recriação ao redimensionar.
5. Tradução RSX: `VulkanRsxBackend : IRsxBackend` na thread RSX (única que grava command buffers, em ordem FIFO):
   arquivo de registradores NV4097; superfícies (0x0208 formato e offsets/pitches) → imagens por offset de memória
   local; `BEGIN_END`/`DRAW_*` → `vkCmdDraw[Indexed]` com pipeline por hash de estado; vertex arrays (0x1680/0x1740)
   lidos da memória local (0xC0000000) ou main via mapa IO.
6. Shaders: fragment program (0x08E4) e vertex program (0x1E9C) → SPIR-V, cache por hash.
7. Recursos/texturas: 0x1A00+, formatos incl. DXT1/3/5 (BC1–3 nativos) e swizzle; cache por endereço+formato,
   invalidado por escrita do guest (reaproveitar o hook de invalidação de páginas).
8. Sincronização: labels/reports (`BACK_END_WRITE_SEMAPHORE_RELEASE`) só depois da GPU terminar (timeline
   semaphore; no início, flush+espera nesses pontos); flip = submit + present; `flip_status DONE` e o flip handler
   depois do present; readback de render targets que o jogo lê.
9. Menor teste com imagem do jogo: no flip, copiar o display buffer registrado por `cellGcmSetDisplayBuffer`
   (offset na memória local, pitch 8192, 1920×1080) para a imagem do swapchain (staging + `vkCmdCopyBufferToImage`)
   e apresentar; contar `FramesPresented`. Mostra pixels que o jogo (ou SPU) escreveu; com `CLEAR_SURFACE`
   implementado, a cor de limpeza escolhida pelo jogo é a primeira imagem gerada pelo GT5. Um triângulo Vulkan
   isolado só prova o host — não é renderização do GT5.

Fontes: RPCS3 `rpcs3/Emu/Cell/Modules/{cellGcmSys,sys_prx_,sys_mempool,cellKb,cellMouse,cellKey2char}.cpp`,
`rpcs3/Emu/Cell/lv2/{sys_prx.h,sys_prx.cpp,lv2.cpp,sys_rsx.cpp}`, `rpcs3/Emu/RSX/{RSXThread.cpp,rsx_methods.cpp,gcm_enums.h}`,
`rpcs3/Emu/Io/Null/{NullKeyboardHandler.h,NullMouseHandler.h}`.
