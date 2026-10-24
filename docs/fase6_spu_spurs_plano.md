# Fase 6 — Motor de execução SPU e plano do SPURS

## Entregue nesta fase (6a): execução de código SPU
- `spu/spu_instructions.h`: lista única do ISA SPU v1.2 (≈200 instruções, formas RR/RRR/RI7/RI8/RI10/RI16/RI18). Decodificador, interpretador, desmontador e o montador dos testes são gerados dela.
- `spu/spu_decoder.*`: tabela de 2048 entradas pelos 11 bits do topo; detecção de conflitos; desmontador (`ai $1,$1,-32`, `brsl $0,0x00120`...). Opcodes conferidos contra codificações reais conhecidas (`1cf80081`, `24004080`, `40200000`, `35000000`, `40800003`).
- `spu/spu_interpreter.*`: executor de referência (`ISpuExecutor`). Registradores em ordem big-endian arquitetural; LS acessado com atômicos relaxados (PPU e DMA de outras SPUs podem tocar ao mesmo tempo); canais, stop-and-signal e falhas via `SpuThread`. Ponto flutuante simples: IEEE do host com flush de denormais e saturação no lugar de infinito (o hardware trunca; pode diferir no último bit). ~146 M instr/s neste sandbox.
- `main.cpp` instala o interpretador como executor padrão: `sys_spu_thread_group_start` agora roda código SPU de verdade.
- Testes `spu_isa_tests`: 90 casos de semântica com valores derivados à mão do ISA, cadeias de carry de 64 bits, laço + chamada/retorno, loads/stores, DMA via canais, falha com desmontagem, e o caminho completo ELF SPU → `sys_spu_image_import` → syscalls de grupo → thread do host → join pelo PPU.

## Por que o SPURS não foi reimplementado (HLE) nesta fase
O SPURS tem duas metades:
- **lado PPU** (libsre.sprx, que hoje é HLE nosso): cria o grupo SPU do kernel SPURS, inicializa `CellSpurs`, `CellSpursTaskset`, filas, event flags etc. em memória do guest;
- **lado SPU**: o kernel SPURS e o policy module de taskset (código SPU dentro da libsre) e a libspurs SPU que está **linkada dentro dos ELFs de task do próprio MGS4**.

O código SPU do jogo espera o ABI exato do policy module (contexto em LS fixo, endereço de trap das syscalls de task, registradores de entrada) e os layouts exatos das estruturas em memória principal que ele lê e altera com DMA atômico. Reescrever isso de memória significa adivinhar layouts byte a byte — erros aparecem como travamentos silenciosos dentro de tasks, muito difíceis de diagnosticar. O RPCS3 mantém a sua reimplementação HLE do lado SPU do SPURS incompleta e roda a libsre real (LLE) por padrão.

## Caminho escolhido (Fase 7, base entregue): LLE da libsre
Status: itens 1, 2 (start/stop/load/unload/id por nome) e os eventos de grupo foram implementados na Fase 7 e validados com a `libsre.sprx` real (carregou, 36 slots religados, `module_start` = 0). A Fase 7.1 (abaixo) trata o travamento logo depois da inicialização do SPURS.

Rodar a `libsre.sprx` real — o lado PPU no nosso interpretador PPU, o kernel SPURS e as tasks no nosso interpretador SPU, sobre as syscalls lv2 que já existem (grupos SPU, imagens, event queues/ports, lwmutex, DMA atômico, eventos LR).

1. **Loader de PRX** (`e_type 0xFFA4`): segmentos em base dinâmica, relocações SCE (seção `0x700000A4`: ADDR32, ADDR16_LO/HI/HA, REL24, ADDR64...), `sys_prx_module_info`, tabelas de export (libent) e import (libstub); ligar os imports `cellSpurs*` do jogo aos exports da PRX; resolver os imports da PRX contra nossa HLE; chamar `module_start` numa thread PPU.
2. **Syscalls `sys_prx_*`** (load/start/stop/unload/register_library) e `sys_spu_thread_group_connect_event` gerando os eventos que o SPURS escuta.
3. **Arquivo do firmware do próprio console**: `dev_flash/sys/external/libsre.sprx`, descriptografado pelo usuário com a mesma ferramenta usada no EBOOT (ex.: *Utilities → Decrypt PS3 Binaries* do RPCS3). Sem chaves no nosso código, como no resto do projeto.
4. **Seleção LLE/HLE**: quando a PRX existir, os imports `cellSpurs*` vão para ela; o guarda HLE atual continua como diagnóstico quando ela faltar.
5. **Desempenho**: o kernel SPURS gira em eventos LR (reserva perdida) — hoje por polling de 20 µs; o JIT SPU e notificação de escrita na linha reservada vêm depois.

## Fase 7.1 — o que a libsre real pediu e não tinha
Log do usuário: `unimplemented syscall 14` e sete imports sem implementação. Identificação por FNID (SHA-1 do nome + sufixo), conferida em `libsre_support_tests`:

| NID | função | situação |
|---|---|---|
| syscall 14 | `sys_process_is_spu_lock_line_reservation_address(addr, flags)` | **implementada** (semântica do RPCS3: segmentos 0/1/2/C/E OK, D (pilha) EPERM, F EPERM só para raw SPU, demais OK se mapeado; flags fora de RAW_SPU\|SPU_THR = EINVAL). O `cellSpursInitialize` pergunta isso para o endereço do `CellSpurs` antes de criar o grupo; com ENOSYS a inicialização falha. |
| 0x0618936B | `_sys_vsnprintf` | implementada (va_list = ponteiro para slots de 8 bytes) |
| 0xFA7F693D | `_sys_vprintf` | implementada (+ `_sys_vsprintf`) |
| 0x996F7CF8 | `_sys_strncat` | implementada |
| 0xE75C40F2 | `sys_process_get_paramsfo` | implementada + syscall 30 `_sys_process_get_paramsfo`; título do PARAM.SFO (`PS3_GAME/PARAM.SFO`, pasta do EBOOT, `/dev_bdvd`) ou `--title-id` |
| 0xDD0C1E09 | `_sys_spu_printf_attach_group` | **bug nosso**: registrávamos sem o `_`. Agora chama o callback do servidor spu_printf do jogo (guardado por `_sys_spu_printf_initialize`) via `CallGuest` e devolve o resultado |
| 0x5FDFB2FE | `_sys_spu_printf_detach_group` | idem |
| 0x05893E7C / 0x6D045C2E | `cellLibprof::cellUserTraceRegister / Unregister` | aceitas (CELL_OK; sem profiler conectado) + Init/Terminate |
| 0x9FB6228E | ? | não identificado (≈340 nomes de sysPrxForUser/libc testados); continua stub com log |

As syscalls de SPU thread (165–194, 250–252) já existiam desde a Fase 4; faltam só 174/175 (suspend/resume), que aparecem no log se forem chamadas.

**Relatório de travamento** (`--stall-report N`, padrão 10 s; `--status-every N` periódico): quando nenhuma thread PPU avança por N s, imprime cada thread PPU (syscall/HLE em que está bloqueada, argumentos, LR de quem chamou, PC), cada grupo/thread SPU (PC, canal ou serviço stop-and-signal em que está, mailboxes, eventos), objetos lv2 com waiters/dono e o estado do RSX. Leitura sem locks (atômicos atualizados pelos interpretadores) e `try_lock` nos objetos: o relatório nunca trava o processo. Continua ativo durante o encerramento, para apontar uma thread que não obedece à terminação.

## Alternativa: SPURS HLE no host
Pool de threads do host por instância (uma por SPU), tabela de workloads com prioridade/contenção, policy module de taskset em C++ (carregar ELF da task no LS, salvar/restaurar contexto via `ls_pattern`, syscalls de task exit/yield/wait signal/poll), e os layouts de `CellSpursTaskset`, `CellSpursEventFlag`, `CellSpursQueue`, `CellSpursSemaphore`, `CellSpursBarrier`, `CellSpursLFQueue` iguais aos do SDK. Mais rápido em execução, mas exige os layouts exatos; sem a PRX para comparar, cada divergência vira depuração às cegas.

## Depois: JIT SPU → x86-64
Mesmo decodificador; tradução por bloco básico; cache indexado por endereço LS + hash do conteúdo (código SPU chega por DMA e é sobrescrito por overlays); invalidação em `stq*` e DMA para o LS; base SSE4.1 com caminhos AVX2; modo de verificação que executa JIT e interpretador lado a lado e compara registradores.
