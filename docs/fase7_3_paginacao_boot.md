# Fase 7.3 — Paginação sob demanda (GT5) e HLE de boot

## O crash
O GT5 reserva uma área do `sys_mmapper` (0x70000000 no log), registra uma fila de eventos com a
syscall 327 `sys_mmapper_enable_page_fault_notification` e escreve na área sem mapear nada
(`stw r28,0(r10)`). No console isso é paginação sob demanda feita pelo próprio jogo: a escrita
gera uma falta de página, o kernel suspende a thread e manda um evento para a fila; a thread
paginadora do jogo mapeia memória ali e libera a thread, que refaz o acesso.

## Decisão: implementar o protocolo, não auto-commit
Auto-commit (mapear a área inteira na reserva, ou a página na primeira falta) quebraria o
protocolo: o paginador do jogo depois chama `sys_mmapper_map_shared_memory` naquele endereço e
receberia `EBUSY`; e qualquer conteúdo que ele carrega na página (dados do disco) não chegaria.

Como os interpretadores já checam cada acesso contra as flags de página da arena, a falta é
detectada **antes** de acontecer — não precisa de SIGSEGV/VEH nem de desenrolar contexto:

| passo | onde |
|---|---|
| syscall 327: área deve ser a base de uma reserva (`EINVAL`), fila deve existir (`ESRCH`), uma fila por área (`EBUSY`) | `MmapperService::EnablePageFaultNotification` |
| PPU: load/store/lwarx/stwcx./stmw/stvx que falha a checagem chama `RecoverFault` → handler do kernel | `PpuInterpreter::RecoverFault`, `ThreadManager` instala `PpuPageFault` |
| SPU: GET/PUT/GETLLAR/PUTLLC com EA não acessível | `ArenaAccessible` em `spu_mfc.cpp` |
| evento: source `0xFFFE000000000000`, data1 = endereço, data2 = `(tipo << 32) \| id da thread` (0 PPU, 1 SPU), data3 = causa (2 não mapeado, 1 somente leitura) | `HandlePageFault` |
| a thread bloqueia **dentro da instrução** até `sys_ppu_thread_recover_page_fault` (57) / `sys_spu_thread_recover_page_fault` (198); então a checagem é refeita (sem mapeamento, nova falta e novo evento) | idem |
| sem notificação na área: falta fatal, com log dizendo que a área do mmapper não tem notificação | idem |
| `sys_mmapper_free_address` com thread suspensa na área: `EBUSY` | `FreeAddress` |
| fim do processo / parada do grupo SPU libera a thread suspensa (PPU termina como `Terminated`, sem relatório de falta falso) | `WakeForTermination`, `cancel` |
| relatório de travamento: thread PPU "SUSPENDED on a page fault", SPU idem; seção "Demand paging" | `stall_monitor`, `MmapperService::Describe` |

Numeração e constantes seguem a tabela do RPCS3. Ainda não: `sys_ppu_thread_get_page_fault_context` (58)
e raw SPU (199).

## Eventos (128/134/136)
Já existiam desde a Fase 3 (fila, porta, conexão local, envio, recebimento) e são exercitados
de novo pelos testes de paginação (a fila do paginador recebe os eventos acima).

## HLE de boot (preencher estruturas, não só devolver CELL_OK)
- `cellSysutil` — vídeo: estado (HDMI, RGB, 59,94 Hz progressivo, 16:9), resoluções 1/2/4/5/0xA–0xD,
  configure/get configuration, disponibilidade, `GetDeviceInfo`; `--resolution 720|1080` define o melhor
  modo da "TV". Áudio: LPCM 2 canais 48 kHz. Callbacks de status (4 slots, `PostEvent` +
  `cellSysutilCheckCallback`). Parâmetros do sistema (inglês EUA, X = confirmar, 24 h, sem controle
  parental, apelido "Player"). `cellSysCacheMount/Clear` em `/dev_hdd1/caches/<id>`
  (`/dev_hdd1` padrão numa pasta temporária).
- `cellGame` — `BootCheck` (disco ou HDD), `ContentPermit` (`/dev_bdvd/PS3_GAME/USRDIR`), `DataCheck`,
  `GetParamInt/String` do PARAM.SFO inteiro, `PatchCheck` → `NOTPATCH`.
- Rede offline — `cellNetCtlGetState` = Disconnected, `GetInfo` = NOT_CONNECTED, `sceNpManagerGetStatus`
  = OFFLINE, `sys_net_initialize_network_ex` OK.
- `cellGcmSys` já estava completo para o boot desde a Fase 5.

## Próximos candidatos (dependem do log do GT5)
`sceNpTrophy*` (registro de troféus chama callback), `cellMsgDialog*`, `cellSaveData*`, `cellPad*`,
`cellAudio*`, `cellFsAio*`.
