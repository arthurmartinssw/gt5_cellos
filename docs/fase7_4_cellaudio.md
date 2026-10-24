# Fase 7.4 — cellAudio e stubs restantes do log do GT5

## O crash
A thread `sgx-audio-thr` lia `lwz r3,4(r3)` com `r3 = 0` (EA 0x4): `cellAudioPortOpen` e
`cellAudioGetPortConfig` eram stubs que devolviam `CELL_OK` sem preencher as saídas, e o jogo
seguiu um ponteiro do `CellAudioPortConfig` que nunca foi escrito.

## Modelo do cellAudio
No PS3 não existe "enviar amostras": o jogo escreve em buffers circulares na memória do guest e
o sistema os lê num relógio fixo. Implementado assim (`hle/cell_audio.*`):

| peça | detalhe |
|---|---|
| `cellAudioInit` | 8 buffers de porta de 256 KiB + tabela de 8 índices de leitura (u64 BE) na área main |
| `CellAudioPortParam` (32 B) | `u64 nChannel @0, u64 nBlock @8, u64 attr @16, float level @24`; canais 2 ou 8, blocos 2/4/8/16/32; `attr & 0x1000` usa `level` |
| `CellAudioPortConfig` (32 B) | `u32 readIndexAddr @0, u32 status @4, u64 nChannel @8, u64 nBlock @16, u32 portSize @24, u32 portAddr @28`; status CLOSE 0x1010 / READY 1 / RUN 2 |
| tamanho da porta | `nChannel * nBlock * 256 amostras * 4 bytes` (float BE intercalado) |
| relógio | thread do host a cada 256/48000 s = 5,333 ms: em cada porta iniciada consome o bloco do índice, zera-o, avança o índice e a tag, atualiza o timestamp; depois manda 1 evento para cada fila registrada |
| filas | `cellAudioCreateNotifyEventQueue(Ex)` cria fila com chave `0x80004d494f323221+n`; `cellAudioSetNotifyEventQueue(Ex)` aceita fila do jogo pela chave IPC; chave inexistente → `TRANS_EVENT` |
| tags / timestamps | `GetPortBlockTag` = tag em que o bloco será lido; `GetPortTimestamp` recua `period` por tag; tag futura → `TAG_NOT_FOUND` |
| saída no host | ainda não: o ponto de encaixe é `CellAudio::Tick()`, onde o bloco consumido é o que iria para WASAPI/PulseAudio/CoreAudio |
| relógio único | `sys_time_get_system_time` e os timestamps do áudio usam a mesma origem do timebase (`SystemTimeMicroseconds`) |

Também: Start/Stop/Close/Quit com os códigos de erro do SDK, `SetPortLevel`, `AddData/Add2chData/Add6chData`
(validam a porta), `MiscSetAccessoryVolume`, `SendAck`, `Set/UnsetPersonalDevice`.

## NIDs desconhecidos (identificados pelo FNID)
| NID | função | implementação |
|---|---|---|
| 0xD0E766FE | `cellUsbdInit` | OK, nenhum dispositivo USB conectado |
| 0x5C832BD7 | `cellUsbdSetThreadPriority2` | OK |
| 0x7663E368 | `cellAudioOutGetDeviceInfo` | HDMI, 1 modo LPCM 2 ch 48 kHz (`CellAudioOutDeviceInfo`, 136 B) |
| 0xC96E89E9 | `cellAudioOutSetCopyControl` | OK |

`cellUsbdRegisterLdd/ExtraLdd/Unregister*` e `cellUsbdEnd` também aceitos: sem dispositivo, a
callback de probe da LDD (volante, etc.) nunca é chamada — como num console sem nada plugado.
