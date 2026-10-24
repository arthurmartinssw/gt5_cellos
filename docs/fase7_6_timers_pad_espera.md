# Fase 7.6 — Thread principal parada: timers lv2, cellPad e diagnóstico de espera

Evidência: o relatório de travamento e os avisos citados no pedido da fase (o log completo não
foi anexado). O que o código prova está separado do que depende do próximo teste.

## Syscalls 70 e 73 (verificadas)
Tabela do RPCS3 (`rpcs3/Emu/Cell/lv2/lv2.cpp`): 70 `sys_timer_create`, 71 `sys_timer_destroy`,
72 `sys_timer_get_information`, 73 `_sys_timer_start`, 74 `sys_timer_stop`, 75
`sys_timer_connect_event_queue`, 76 `sys_timer_disconnect_event_queue`; semântica de
`sys_timer.cpp`/`sys_timer.h` do RPCS3 (ordem dos erros, fonte do evento = nome ou `pid<<32|id`,
`data3` = horário de expiração do disparo, alinhamento do período com `base_time`).
- 70 foi chamada pela thread principal (`0x00c1d3ec`) e recebia `ENOSYS`: o timer nunca existiu.
- 73 foi chamada por `9qstY` (`0x008cfc84`) — que também está parada num `sys_lwcond_wait`.
- Um timer só faz algo conectado a uma fila de eventos: ele acorda quem está em
  `sys_event_queue_receive`. Há threads bloqueadas assim (ex.: `MEfYS` na fila `0x8d00000a`).

**Hipótese principal (não confirmada):** o timer que falhou é o relógio que move uma cadeia
`timer → fila → thread → lwcond → ... → thread principal`. Confirma se, no próximo teste, o
timer aparecer conectado (75) a uma das filas com waiter e a thread principal avançar;
refuta se o timer conectar a uma fila que ninguém espera, ou se nenhum 75 acontecer.
O aviso de syscall não implementada não imprimia argumentos; agora imprime r3–r6 e LR.

## A espera da thread principal
- `sys_lwcond_wait(sys_lwcond_t* lwcond, u64 timeout)` tem **2** argumentos (wrapper HLE em
  `sys_prx_for_user.cpp`). `0xd001f938` é o endereço guest do `sys_lwcond_t`, na área de pilha
  (0xD0000000+): um objeto local da thread principal; timeout 0 = infinito. `0xd001f7f0` e
  `0x59` eram r5/r6 sobrando de chamadas anteriores, não argumentos — o relatório agora mostra só
  os argumentos reais.
- `sys_lwcond_t` = `{be32 lwmutex*, be32 id do lv2 cond}`; o wait exige que a thread seja dona do
  lwmutex (senão `EPERM`), libera o lwmutex e entra na fila de waiters atomicamente sob o mesmo lock,
  e volta com o lwmutex de novo (também em timeout). Signal acorda exatamente um, signal_all todos,
  signal sem waiter não fica guardado (semântica de condvar). Não achei defeito; agora há testes
  de regressão para isso (vários waiters, timeout, EPERM, sinal sem waiter).
- `LR = 0x00c03d24` é endereço guest direto; LR−4 é a instrução que chamou o stub de import (os
  stubs terminam em `bctr`). "thunk/sc at" é o endereço do thunk HLE (código nosso), não do jogo.
  Sem o EBOOT aqui não dá para desmontar; o relatório agora desmonta o chamador e, para a thread
  principal, as 8 instruções anteriores.
- **Quem deveria acordar:** não determinável sem o próximo relatório. Ele agora mostra, para o
  lwcond: id do cond, waiters, esperas, sinais (quantos sem waiter), último sinal e quem sinalizou;
  dono do lwmutex; para filas: eventos recebidos no total, último, e os produtores conectados
  (portas, timers, threads/grupos SPU).

Como ler: `never signaled` + nenhum produtor = produtor nunca rodou (caso 2/7); sinais "found no
waiter" logo antes da espera = sinal chegou antes do waiter (problema de ordem no guest, caso 3);
fila "0 received" com timer conectado parado = timer não iniciado (caso 4).

## cellPadInit e o resto do cellPad
`sys_io::cellPadInit(7)` era stub. O próprio Init "dava certo", mas `cellPadGetData` /
`cellPadGetInfo2` também eram stubs com `CELL_OK` e buffer não escrito — o jogo veria um controle
conectado com botões aleatórios. Implementado com o contrato do RPCS3 e **nenhum controle**:
Init/End, GetInfo (647 B), GetInfo2 (124 B), GetData/ClearBuf/GetCapabilityInfo → `NO_DEVICE`
sem escrever o buffer, SetPortSetting/SetPressMode/SetSensorMode guardados. Não há evidência de
que o pad causa a parada (o Init já devolvia OK).

## NID 0x9FB6228E (sysPrxForUser, importado pela libsre)
Não identificado. Testados os 131 nomes que o RPCS3 registra em sysPrxForUser (todos os
`sys_*_.cpp`) e os 8 NIDs sem nome que ele registra — nenhum bate; o NID também não está na tabela
`PPUFunction.cpp`. Continua stub que loga; se for chamado, o aviso "unimplemented
sysPrxForUser::0x9FB6228E called" aparece com argumentos e chamador. Nenhum indício de que é
chamado no caminho da parada.

## SPURS
Inalterado. 187,6 execuções/s com ~104 mil instruções SPU cada é trabalho real a cada bloco de
áudio (5,333 ms): ciclo de despacho esperado.

## Aviso -Warray-bounds (GCC no MSYS2)
Não reproduzido com GCC 13.3 nem Clang 18 no Linux. Para localizar é preciso o texto completo
("In function ... inlined from ... at src/...:linha"); GCC 12–15 tem falsos positivos conhecidos
desse aviso em `std::vector` com `-O2`. Nada foi alterado por ele.
