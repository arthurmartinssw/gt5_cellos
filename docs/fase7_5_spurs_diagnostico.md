# Fase 7.5 — Diagnóstico do ciclo start/exit do SPURS e `cellGcmSetFlipImmediate`

Evidência disponível: os trechos citados no pedido da fase (o log completo de ~9.494 linhas não veio
anexado). Conclusões abaixo separam o que o código prova do que depende do log.

## 1. Start/exit repetido do `sgx-spurs-CellSpursKernelGroup`

**Caminho no nosso código (verificado):**
- `sys_spu_thread_group_start` (173) é o único chamador de `SpuManager::StartGroup`; o runtime
  nunca reinicia um grupo por conta própria.
- `StartGroup` só aceita grupo em `Initialized`; `Initialized` só é restaurado por
  `sys_spu_thread_group_join` (178), depois que todas as threads do host pararam e foram unidas.
  Logo cada `start` do log foi precedido de um `join` feito pelo guest, e um handler não consegue
  reiniciar um grupo ainda rodando ou ainda não unido (`CELL_ESTAT`; testado).
- `sys_spu_thread_group_exit(0)` vem do código SPU (stop 0x101 com status 0 no mailbox).
- Cada start redeposita a imagem no LS e zera registradores/canais, como o lv2 (testado: 50 ciclos,
  mesmo resultado em todos).

**Lado guest (libsre real, LLE):** a instância SPURS criada com `exitIfNoWork`
(último argumento de `_cellSpursAttributeInitialize`) faz o kernel SPURS sair do grupo quando não há
workload pronto (`group_exit(0)`), e a thread `<prefixo>SpursHdlr0` espera um workload ficar pronto,
dá `start`, `join` e repete. Sem `exitIfNoWork` o grupo é iniciado uma vez só. Referência: a
reimplementação HLE da libsre no RPCS3 (`spursHandlerEntry` / `spursHandlerWaitReady` e a seleção de
workload do kernel com `SF1_EXIT_IF_NO_WORK`).

**Conclusão:** o padrão é o ciclo de despacho esperado de uma instância SPURS `exitIfNoWork`, não um
bug de transição de estado do nosso lado. O prefixo `sgx-` é o mesmo da thread `sgx-audio-thr` (mesma
middleware) e o grupo tem 1 SPU; desde a 7.4 essa thread de áudio acorda a cada bloco do cellAudio
(5,333 ms = 187,5/s), o que daria milhares de ciclos por minuto se cada bloco gera um job SPU.
**Isso é hipótese até ver a taxa medida** — o resumo novo mede exatamente isso.

Custos reais (não correção): cada ciclo cria uma thread do host e recopia a imagem SPU. Otimização
futura: thread do host persistente por SPU, estacionada entre execuções.

## 2. `cellGcmSys::0xACEE8542` = `cellGcmSetFlipImmediate`

- **Evidência:** SHA-1 FNID de `"cellGcmSetFlipImmediate"` = 0xACEE8542 (match exato; testado). Foram
  testados 136 nomes da biblioteca; só esse bate.
- **Assinatura (SDK):** `int32_t cellGcmSetFlipImmediate(uint8_t id)` — troca o buffer exibido para `id`
  sem comando no command buffer. `r3 = 1` no log = buffer 1.
- **Retorno:** `CELL_OK`; `CELL_GCM_ERROR_FAILURE` para `id > 7` ou GCM não inicializado (o caso de id
  inválido segue o RPCS3; confiança média nos demais detalhes).
- **Efeito:** estado de exibição (buffer atual, contagem, horário, status DONE) — o mesmo do flip da
  FIFO, reaproveitado (`RsxFifo::DoFlip`) e executado pela thread do RSX, para o back end continuar
  recebendo `OnFlip` só nessa thread.
- **LR = 0x00ce7bdc:** no nosso interpretador o LR é endereço guest direto (sem tradução); os stubs de
  import do PS3 terminam em `bctr`, então LR − 4 = `bl` do chamador dentro do EBOOT (área main). Sem o
  EBOOT aqui não dá para desmontar esse ponto; o aviso de import não implementado agora imprime essa
  instrução desmontada e r3–r6.
- **Era fatal?** Não pela evidência: o stub devolvia CELL_OK e o jogo seguiu. Mas sem atualizar o estado
  de flip, um laço que espera `cellGcmGetFlipStatus` ficaria preso; por isso foi implementado.

## 3. Auditoria do cellAudio (7.4)
- Ciclo de vida da thread do mixer, Init/Quit, validação de ponteiros, filas: sem problema encontrado.
- **Corrigido:** o índice de leitura (u64 BE consultado pelo jogo enquanto o mixer escreve) agora é
  gravado com um store atômico de 8 bytes, igual aos acessos do interpretador PPU (antes: escrita comum
  — corrida formal, valor rasgado possível em teoria).

## 4. Diagnóstico novo
- **SPU:** as 3 primeiras execuções de cada grupo continuam no log; depois uma linha avisa que passam
  a ser contadas, e a cada `--spu-summary` s (padrão 10) sai um resumo por grupo: execuções, taxa/s,
  instruções SPU por execução, duração média, intervalo mínimo/máximo entre starts, causas de saída
  (`GROUP_EXIT(0) xN`). `--log-spu-runs` volta a logar tudo. Falhas, término e avisos nunca são afetados.
  Leitura: taxa estável (~187/s se pareada com o áudio) e trabalho real por execução = ciclo de despacho;
  milhares/s com poucas instruções = laço de reinício (o resumo marca isso como heurística).
- **Relatórios final e de travamento:** totais por grupo; GCM mostra flips imediatos.
- **Monitor de travamento:** além de "nenhuma thread PPU avança", agora dispara quando a **thread
  principal** fica N s parada enquanto outras seguem — o áudio e o handler do SPURS mantinham o contador
  global andando e escondiam um laço principal preso.

## 5. Próximo passo
Rodar de novo e colar: as linhas de resumo `SPU group ...`, o relatório final (seções SPU, RSX,
cellAudio, File system) e, se aparecer, o relatório "primary thread made no progress". A taxa do resumo
decide entre ciclo esperado e laço; o relatório da thread principal mostra onde o boot realmente parou.
