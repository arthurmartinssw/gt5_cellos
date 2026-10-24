# Fase 7.2 — Pivô para Gran Turismo 5 e base do cellFs

## Decisão
O alvo passou de Metal Gear Solid 4 (que ganhou port oficial para PC) para **Gran Turismo 5**.
O runtime já era genérico; a mudança é de nome e de prioridades:
- projeto `mgs4_cellos` → `gt5_cellos`; biblioteca CMake `cellos_core` → `cellos_runtime`;
  executável `gt5_cellos`. Namespace C++ mantido como `cell::` (o runtime não deve carregar o nome
  de um jogo). Variável de testes `MGS4_TEST_ITERATIONS` → `CELLOS_TEST_ITERATIONS`.
- Comentários de regressão que citam endereços do MGS4 (0x10710, 0x2ed878) ficam: são o
  histórico dos bugs que os testes protegem.
- A Fase 7.1 (syscall 14, syscall 30/`sys_process_get_paramsfo`, `_sys_v*printf`, `_sys_strncat`,
  `_sys_spu_printf_*`, cellLibprof, relatório de travamento) segue igual na nova base.

## cellFs / sys_fs (por que agora)
O GT5 lê quase tudo de um arquivo de volume de vários GiB (`GT.VOL`): índice primeiro, depois
leituras aleatórias de blocos comprimidos, de mais de uma thread. Requisitos e como foram atendidos:

| requisito | implementação |
|---|---|
| offsets > 4 GiB | `host::File` com `pread/pwrite` (POSIX) e `ReadFile/WriteFile` + `OVERLAPPED` (Windows); nada de `fseek`/`ftell` de 32 bits |
| leituras concorrentes no mesmo fd | E/S só posicional; a posição do fd fica no `FsHandle` (mutex); `cellFsReadWithOffset` não toca nela |
| sem cópia extra | leitura direto na arena do guest (visão shadow), com invalidação de páginas de código traduzido |
| semântica lv2 | fds 3..255 (`EMFILE`), flags octais do SDK, `CellFsStat` de 52 bytes (campos de 64 bits alinhados a 4), `CellFsDirent` de 258 bytes com `.` e `..`, `EROFS` em `/dev_bdvd` e `/dev_flash`, `EXDEV` entre dispositivos, `ENOTMOUNTED` em dispositivo sem mount |
| duas portas de entrada | HLE `sys_fs::cellFs*` (o que o EBOOT importa) e syscalls 801–834 (o que uma PRX LLE chamaria), ambas pelos mesmos métodos "guest ABI" do `FsService` |
| diagnóstico | todo open no log; os 16 últimos caminhos não encontrados vão para o relatório final e o de travamento (seção `File system`) |
| dumps com caixa diferente | no Linux, se o caminho exato não existe, cada componente é procurado sem distinção de maiúsculas |

Montagem automática: EBOOT em `<raiz>/PS3_GAME/USRDIR/` ⇒ `/dev_bdvd = <raiz>` (somente leitura);
`/app_home` = pasta do EBOOT; `--dev-hdd0` / `--dev-hdd1` para dados graváveis.

## Pendências de arquivo (entram quando o log do GT5 pedir)
- `cellFsAio*` (leitura assíncrona com callback numa thread PPU — `CallGuest` com 4 argumentos);
- `cellFsSt*` (streaming em ring buffer);
- `cellFsSdata*` (arquivos SDATA/EDATA — exigem a chave do usuário, fora do código);
- `sys_fs_fcntl` (817) e `cellFsGetDirectoryEntries`;
- `cellGame*` / `cellSysCacheMount` (caminhos de game data e cache em `/dev_hdd0` e `/dev_hdd1`).
