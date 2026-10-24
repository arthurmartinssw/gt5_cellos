# Fases 1–7 — Loader (EBOOT + PRX), memória, PPU, kernel lv2, SPU, mmapper, GCM/RSX (MGS4 / CellOS)

Decisões que as próximas fases (JIT PPU, executor/JIT SPU, SPURS, RSX→Vulkan) assumem.

## Mapa de endereços (EA de 32 bits, arena única de 4 GiB, host = base + EA)
| Faixa | Área | Uso |
|---|---|---|
| 0x00000000–0x0000FFFF | — | não mapeada (null trap) |
| 0x00010000–0x1FFFFFFF | main (64K) | PT_LOAD do EBOOT, PRX, blocos TLS, thunks HLE, imagens SPU importadas |
| 0x20000000–0x2FFFFFFF | user64k | sys_memory_allocate(0x200) |
| 0x30000000–0x3FFFFFFF | user1m | sys_memory_allocate(0x400) |
| 0x40000000–0xBFFFFFFF | mmapper (256M) | áreas de endereço do sys_mmapper (reservadas; memória mapeada dentro) |
| 0xC0000000–0xCFFFFFFF | rsx_local | VRAM RSX: 256 MiB comprometidos no _cellGcmInitBody; o jogo vê 0x0F900000; reports locais em +0x0FE00000 |
| 0xD0000000–0xDFFFFFFF | stack (64K) | pilhas PPU, guarda de 64K |
| 0xE0000000–0xEFFFFFFF | raw_spu | MMIO de raw SPU (não suportado ainda) |
| 0xF0000000–0xFFFFFFFF | spu_thread | LS de SPU threads: janela de 1 MiB por thread, 256 KiB RW + 768 KiB guarda |

Linhas a conferir contra RPCS3 (vm.cpp / sys_memory.cpp) se a primeira alocação do jogo cair em lugar estranho: divisão user64k/user1m e base do mmapper.

## Convenções
- Duas visões da mesma memória: `base()` (proteção do guest; usada pelo JIT) e `shadow()` (sempre RW; loader/HLE/DMA SPU/RSX). Equivale ao `backing_base` do runtime_memory.c.
- Página de rastreio 4K com flag `kPageCode` para invalidação de código (SMC). DMA SPU (PUT/PUTLLC) em página de código invalida o bloco PPU.
- Imports: cada FNID vira thunk `0x04000000|índice` + `blr` + OPD; o slot do libstub recebe o endereço do OPD.
- FNID = 4 primeiros bytes de SHA-1(nome + sufixo fixo), lidos em little-endian; verificado contra 16 NIDs conhecidos. Nomes ficam em `hle/nid_names.cpp` e os FNIDs são calculados, nunca digitados.
- Registros de entrada (convenção do RPCS3 ppu_load_exec): r3 argc, r4 argv (u64[]), r5 envp (u64[]), r7 id da thread (0x01000000), r8–r10 imagem TLS, r11 OPD de entrada, r12 malloc_pagesize, r13 = bloco TLS + 0x30 + 0x7000, LR = thunk de saída.
- Sem descriptografia: EBOOT retail precisa ser descriptografado do dump próprio antes; SELF debug (key revision 0x8000) carrega direto, com zlib.
- Código só com C++20 padrão; APIs do SO apenas com `#ifdef _WIN32` / `__linux__` explícitos: arena de memória e nome de thread (`common/host_thread.*`, `SetThreadDescription` resolvida em tempo de execução / `pthread_setname_np`). Afinidade de CPU não é fixada: o escalonador do host decide.

## Fase 2 — interpretador PPU
- Lista única de instruções (`ppu_instructions.h`, X-macro) gera enum, nomes e tabela de handlers; o JIT vai reutilizar o mesmo `Decode()`.
- Decodificação em dois níveis (opcode primário → tabela do formato estendido: 4 VMX, 19, 30, 31, 58/62, 59, 63).
- Checagem explícita de memória por flags de página → parada precisa (PC, EA, leitura/escrita). O JIT usará falhas do host + `GuestAddressOf()`.
- 64-bit mode (SF=1) com EA truncado a 32 bits; v128 em ordem big-endian (lvx/stvx copiam bytes direto).
- Timebase 79,8 MHz. FPSCR[RN] ainda não respeitado (assume arredondamento ao mais próximo).
- Desempenho medido: ~180 MIPS em laço simples (referência; o JIT é a Fase 2b).

## Fase 3 — kernel lv2 e concorrência
- 1 thread PPU = 1 `std::thread` do host com seu próprio `PpuInterpreter`; a thread primária roda na thread chamadora. Sem escalonador de guest: o SO do host escalona; prioridades lv2 são gravadas, não aplicadas. Threads do host nomeadas "PPU 0x0100000N" / "SPU 0x0200000N".
- `PpuCodeCache` compartilhado: ponteiro atômico por página, publicação por CAS, contador de geração por página contra a corrida decode × store; páginas invalidadas vão para lista de aposentadas (liberadas no shutdown).
- Acessos escalares alinhados ao guest via `std::atomic_ref` relaxado; `sync` = fence seq_cst, `lwsync`/`eieio` = acq_rel. TSan limpo nos testes.
- `HleRegistry`: slots fixos + contagem atômica publicada; Dispatch sem lock.
- Objetos lv2 em `ObjectTable` como `shared_ptr` (quem espera mantém o objeto vivo). Bloqueio = condition_variable do objeto; todo predicado inclui `terminating`, avaliado sob o lock do objeto.
- Término do processo (exit, exitspawn, falha em qualquer thread PPU ou SPU): `RequestTermination` levanta a flag, acorda todos os objetos e as threads; o relatório mostra a thread e a causa. `Kernel::Shutdown()` junta threads PPU e depois SPU.
- Números de syscall usados (tabela do lv2 reimplementada pelo RPCS3): 41 thread_exit, 43 yield, 44 **join**, 45 detach, 46 get_join_state, 47/48 prioridade, 49 stack info, 52 _create, 53 start, 56 rename; 82–87/118/132/139 event flag; 90–94/114 semáforo; **100–104 mutex** (create/destroy/lock/trylock/unlock); 105–110 cond; **128–138 event queue/port**; 141/142 timer sleep; **169 sys_spu_initialize**; 165–194 SPU (Fase 4).
- sysPrxForUser HLE: `sys_ppu_thread_create` (TLS + 52 + 53), lwmutex/lwcond bloqueantes sobre Lv2Mutex/Lv2Cond (com espelho dos campos no `sys_lwmutex_t` do guest), `sys_prx_exitspawn_with_level` = OK.
- cellSysmodule HLE: load/unload/isLoaded com registro; o ID 0x2E pedido pelo MGS4 ainda não foi identificado com segurança.

## Fase 3.1 — SPU init e diagnóstico
- 169 `sys_spu_initialize(max_usable_spu, max_raw_spu)`: só registra limites (EINVAL se raw > 5); não devolve ponteiro.
- Syscalls não implementadas: aviso uma vez por número, com nome quando conhecido (faixa SPU 160–194, sys_fs 801–818).
- Desmontador PPU (`ppu_disasm`): relatório de falha mostra a instrução desmontada, como o EA foi formado, as últimas 64 instruções e as últimas 16 chamadas HLE/syscalls da thread com argumentos e resultado. O loader avisa quando um módulo importa variáveis (vnids) ainda não ligadas.

## Fase 4 — base do runtime SPU
Mapeamento PS3 → PC (sem simular hardware):
- SPU thread → `std::thread` do host criada em `sys_spu_thread_group_start` (uma por thread do grupo).
- Local store 256 KiB → início de uma janela de 1 MiB na área spu_thread; ponteiro de host `ctx.ls` (= shadow + EA). O executor/JIT acessa `ls[addr & 0x3FFFF]`. Acessos concorrentes (read_ls/write_ls do PPU, DMA de outra SPU) usam `atomic_ref` relaxado; o executor deve fazer o mesmo (no JIT, loads/stores simples).
- MFC → síncrono: o comando roda inteiro na escrita de `MFC_Cmd`; tags concluem na hora (`MFC_RdTagStat` = máscara). GET/PUT (1/2/4/8 ou múltiplos de 16 até 16 KiB, alinhamento checado), listas (elemento {S|tamanho, EAL}; stall-and-notify só é reportado, a lista não pausa), GETLLAR/PUTLLC/PUTLLUC/PUTQLLUC com 256 mutexes listrados por linha de 128 B, SNDSIG, barreiras = fence.
- EA ≥ 0xF0000000 no DMA = relativo ao grupo (SPU n em 0xF0000000 + n·1 MiB; SNR1/SNR2 em +0x5400C/+0x5C00C).
- Canais 0–30 do CBEA; bloqueantes esperam no cv da thread via `Kernel::Wait` (acordam por término do processo ou parada do grupo). Eventos: MB/S1/S2 por nível; TM (decrementador a 79,8 MHz) e LR por polling com timeout; LE/ME/TG na borda.
- Protocolo lv2: out-intr-mbox código <64 send_event (reply em in_mbox), 64–127 throw_event, 128 event_flag_set_bit (reply), 192 impatient; evento = {0xFFFFFFFF53505501, id da SPU thread, spup<<32|data0, data1}. Stop 0x100 yield, 0x101 group exit, 0x102 thread exit, 0x110/0x111 receive/tryreceive em fila SPU ligada (reply {CELL_OK, d1, d2, d3}). in_mbox ocupado no pedido → reply CELL_EBUSY.
- Syscalls: 165, 166, 170–173, 176–182, 184–188, 190–194. IDs: SPU thread 0x02xxxxxx, grupo 0x04xxxxxx. Grupo: NotInitialized → Initialized (todas as threads) → Running → Stopped → join → Initialized (reinício permitido). Causas de join: 1 group exit, 2 todas saíram, 4 terminate. Limite de 6 SPUs somando grupos (EBUSY).
- HLE sysPrxForUser: `sys_spu_image_import` (PROTECT copia os bytes para um bloco na área main; DIRECT referencia o ELF; tabela de segmentos no início do bloco), `sys_spu_image_close`, `sys_spu_elf_get_information/segments`, `sys_spu_printf_*` aceitos, `sys_raw_spu_load` aborta.
- cellSpurs HLE: atributos/initialize/finalize registrados no host; criação de taskset/task/job chain/workload encerra o processo com motivo explícito. Plano: SPURS vira escalonador no host (pool de workers por instância) executando o código SPU pelo executor/JIT, não o kernel SPURS em SPU.
- `ISpuExecutor` (spu_thread.h) é o ponto de encaixe do interpretador/JIT SPU (próxima fase): `Run()` usa `ReadChannel/WriteChannel/ChannelCount/StopAndSignal`. Sem executor, iniciar um grupo termina o processo com "no SPU executor is installed".
- Lacuna conhecida: stores PPU não pegam o mutex listrado, então um store PPU concorrente com PUTLLC na mesma linha não é detectado (resolver no JIT).
- Testes: `spu_tests` (executor de script em C++), limpo em ASan/UBSan e TSan.

## Fase 5 — sys_mmapper, cellGcmSys e front end do RSX
- Crash em 0x002ed878 (`lwz r9,0(r11)` → `lwz r3,8(r9)`): `cellGcmInit` é inline do libgcm_cmd e chama `_cellGcmInitBody(&gCellGcmCurrentContext, cmdSize, ioSize, ioAddress)`; o stub não gravava o ponteiro do contexto e o primeiro inline lia `context->current` de NULL. Antes disso a syscall 330 falhava (ENOSYS) e os stubs de sys_mmapper/cellGcm devolviam CELL_OK sem fazer nada.
- Arena: `ReserveRange/ReserveFixedRange/Unreserve/CommitRange/DecommitRange` (reserva sem commit; commit/decommit por faixa).
- sys_mmapper (lv2/sys_mmapper): 326, 329–332, 334, 335, 337 + wrappers sysPrxForUser (`sys_mmapper_{allocate,allocate_memory_from_container,map,unmap,free}_memory`). Área = reserva de 256 MiB na região mmapper; objeto de memória cobrado do orçamento de 213 MiB; conteúdo preservado entre unmap/map; um mapeamento por objeto (EBUSY). IDs 0x08xxxxxx.
- cellGcmSys HLE (hle/cell_gcm_sys): bloco de 64 KiB na área main com CellGcmContextData (+0), CellGcmControl {put,get,ref} (+0x40), labels 256×16 (+0x1000), display info (+0x2000), tile/zcull (+0x2100/+0x2200, ainda zerados), tabelas de offset ioAddress[3072]/eaAddress[512] (+0x3000/+0x5000), reports em main (+0x8000). Área io = offset IO 0; buffer de comandos [io+0x1000, io+cmdSize) em segmentos de 32 KiB (ou o de cellGcmSetDefaultFifoSize); `context->callback` é o OPD de `cellGcmCallback` (HLE interno): escreve jump para o próximo segmento, put = início dele, espera o RSX sair.
- ~60 exports: init, map/unmap de IO, address↔offset, configuração, labels/reports/control, display buffers, flip (comando privado 0xFEAC no FIFO, como o GCM_FLIP_COMMAND do RPCS3), flip status, tiled pitch, tile/zcull validados, handlers registrados mas ainda não chamados (sem threads de interrupção PPU).
- Front end RSX (rsx/rsx_fifo): thread do host "RSX" lê put/get do CellGcmControl; old/new jump, call/return (1 nível), NOP; NV406E SET_REFERENCE/SEMAPHORE_OFFSET/ACQUIRE/RELEASE, NV4097 SET_SEMAPHORE_OFFSET/BACK_END_WRITE_SEMAPHORE_RELEASE (bytes trocados)/TEXTURE_READ_SEMAPHORE_RELEASE; flip; o resto vai para `IRsxBackend` (hoje `NullRsxBackend`, que conta métodos; o renderer Vulkan implementa essa interface). Comando inválido ou offset IO não mapeado encerra o processo com relatório.
- Relatório final mostra a seção "RSX" (io, comandos, flips, métodos mais frequentes).
- Testes: gcm_tests (mmapper, init/IO, FIFO, wrap, falha, e a sequência do MGS4 como código PPU no interpretador). ASan/UBSan e TSan limpos.
- Fase 5.1 (Windows): `std::this_thread::sleep_for` abaixo de 1 ms não serve no Windows (MinGW trunca para Sleep(0) = yield; MSVC arredonda para o tick de 15,6 ms). Polling do RSX/GCM usa `host::SleepMicroseconds` (timer de alta resolução por thread no Windows 10 1803+, clock_nanosleep no Linux). Esperas de teste por prazo (steady_clock), nunca por contagem de iterações. Validado no Linux com um shim LD_PRELOAD que imita os dois comportamentos do Windows.

## Fase 6 — interpretador SPU
- Lista única do ISA SPU (`spu_instructions.h`) → decodificador (tabela 2048), interpretador de referência (`SpuInterpreter`, instalado como executor padrão no `main`), desmontador. Detalhes e o plano do SPURS em `claude/fase6_spu_spurs_plano.md`.
- Recomendação registrada: SPURS por LLE da `libsre.sprx` do firmware do próprio console (loader de PRX na Fase 7), não HLE do lado SPU.

## Fase 7 — loader de PRX, linker LLE/HLE, sys_prx, eventos de grupo SPU
- `ParsePrx` (e_type 0xFFA4); payload do segmento de relocação `0x700000A4` preservado; a checagem `p_filesz <= p_memsz` vale só para segmentos mapeados (a tabela de relocação pode ter `p_memsz = 0`).
- `loader/prx_loader`: segmentos num bloco da área main mantendo o layout relativo (separados se se sobrepõem); relocações de 24 bytes {u64 offset; u16; u8 index_value (0xFF = absoluto); u8 index_addr; u32 type; u64 addend}, tipos ADDR32 (1), ADDR16_LO/HI/HA (4/5/6), REL24 (10), REL14 (11), ADDR64 (38), REL64 (44), ADDR16_LO_DS (57); module info em base(seg0) + p_paddr − p_offset (52 bytes); exports libent de 28 bytes, biblioteca com atributo 0x8000 = entradas especiais por NID fixo (module_start 0xBC9A0086, module_stop 0xAB779874, module_exit 0x3AB9A95E, prologue 0x0D10FD3F, epilogue 0x330F7005, var module_info 0xD7F43016 — não são gerados pelo esquema SHA-1 dos FNIDs); texto read-only + kPageCode depois das relocações.
- `loader/ppu_linker`: (biblioteca, NID) → export LLE de um módulo carregado, senão thunk HLE; registra todos os slots de import (EBOOT incluído) e religa ao carregar/descarregar um módulo. O EBOOT passou a usar o mesmo linker.
- `lv2/sys_prx` (PrxManager no Kernel): ids 0x23xxxxxx, estados Loaded/Starting/Started/Stopping/Stopped; syscalls 480 (load → id), 481/482 (protocolo em duas fases cmd 1/cmd 2), 483 (unload), 496 (id por nome); sysPrxForUser `sys_prx_load/start/stop/unload_module`, `sys_prx_get_module_id_by_name`. module_start/module_stop rodam via `ThreadManager::CallGuest` (thread PPU nova + join do lado host; funciona chamado de dentro de HLE). Avisa por nome os imports do módulo que caem em stubs.
- `lv2/vfs`: `--dev-flash`, `--dev-bdvd`, `--dev-hdd0`, `--app-home` (padrão: pasta do EBOOT); prefixo mais longo, ".." recusado.
- Boot LLE (`--lle libsre` ou `--lle caminho.prx`): carregado depois do EBOOT (slots religados para o módulo), module_start antes da entrada do jogo.
- Eventos de grupo SPU: RUN (chave 0xFFFFFFFF53505500, data1 = grupo) no start; EXCEPTION (…503, data1 = grupo, data2 = thread) na falha de SPU (o processo ainda termina com relatório); syscalls 250 (cooperative victims, aceito), 251/252 `connect/disconnect_event_all_threads` (usado por `cellSpursAttachLv2EventQueue`).
- Testes `prx_tests`: PRX sintético no formato real (código PPU executado, relocações de todos os tipos, module_start, chamadas EBOOT → PRX → HLE, relink ao descarregar, VFS, syscalls e protocolo em duas fases, eventos RUN/usuário/EXCEPTION).

## Observações do boot do MGS4
- Executável lançado tem 0x8588 bytes de texto e só importa sysPrxForUser → launcher.
- Executável principal: cria mutexes (100/102/104), cria a thread "MGS4 MAIN" e faz join (44).
- "MGS4 MAIN" chama 169 (OK) e 147, e caía em `lbz r0,0(r9)` com r9 = 0 (pc 0x10710). Causa (Fase 4.1): o loader passava argv[]/envp[] com elementos de 32 bits; o lv2 passa elementos de **64 bits** (EA de 32 bits no word baixo, envp logo após o NULL de argv). O crt0 estreita o vetor no lugar (ld do slot i*8 → stw no slot i*4) e guarda {argv, argc} num global (0x4125A8 no MGS4); com 32 bits, argv[0] virava 0. Corrigido em `PpuLoader::SetupStack` + teste de regressão em loader_tests.
- Depois do argv: "MGS4 MAIN" carrega 15 módulos via cellSysmodule (NET, HTTP, SSL, RTC, FS, FONT, JPGDEC...), chama 330 + sys_mmapper_allocate_memory(0x1200000) + map, `_cellGcmInitBody(0x9e6f18, ...)` e `cellGcmMapMainMemory`, e caía em 0x002ed878 (corrigido na Fase 5).
- Cadeia do crash: TOC (r2 0x400090) → r30 = 0x3E9BC0 → r28 = 0x4125A8 → r29 = argv (0xD001FFC0, topo da pilha primária), r31 = argc = 1 → r9 = argv[0].

## Pendências
- Executor SPU: decodificador do ISA SPU + interpretador de referência, depois JIT SPU → x86-64.
- Variáveis importadas/exportadas entre módulos (vnids/vstubs) ainda não ligadas; TLS de PRX não suportado.
- `sys_ppu_thread_once` (precisa chamar código do guest a partir da HLE).
- Threads de interrupção, IPC, protocolos de prioridade; raw SPUs; suspend/resume de grupo; eventos de grupo (RUN/EXCEPTION) só registrados.
- SPURS no host; sys_fs.
- MGS4 depois da Fase 5.1: mapeia dezenas de blocos de memória compartilhada (0x50000000–0x59800000), envia 26 comandos ao FIFO, cria `GV_PAD`, `GV_MOUSE` e `spu_printf_handler`, e para em `cellSpursCreateTasksetWithAttribute` (guarda HLE).
- Threads de interrupção PPU para chamar flip/vblank handlers do cellGcm; empacotamento de tile/zcull; renderer Vulkan (IRsxBackend).
