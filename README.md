# gt5_cellos — runtime CellOS (loader EBOOT/PRX, memória, PPU, kernel lv2, SPU, mmapper, GCM/RSX, cellFs)

Alvo atual: **Gran Turismo 5**. O projeto começou como `mgs4_cellos` (as Fases 1–7 foram
validadas com o EBOOT do MGS4; a 7.1 ainda não rodou com um jogo) e foi renomeado quando o alvo mudou; nada no runtime é específico
de um jogo — o executável, as bibliotecas HLE e o kernel lv2 servem a qualquer título de PS3.
A biblioteca CMake do runtime chama-se `cellos_runtime`; o namespace C++ continua `cell::`.

Carrega o `EBOOT.BIN` **descriptografado** (ELF PPC64) do jogo num espaço de endereços PS3
emulado de 4 GiB, liga os imports a thunks HLE e executa o processo no interpretador PPU de
referência, com threads PPU reais (uma thread do host cada) e os objetos de sincronização do lv2
(mutex, cond, semáforo, event flag, event queue/port, lwmutex/lwcond). Roda até o processo sair,
pedir `exitspawn` de outro executável ou uma thread parar numa instrução/função não implementada
(com relatório detalhado da thread culpada).

Fase 4: SPU thread groups do lv2 (syscalls 165–194), local store de 256 KiB por SPU thread na
arena (0xF0000000+), MFC (DMA, listas, GETLLAR/PUTLLC, sinais), canais, mailboxes e o protocolo
de eventos SPU↔PPU, com cada SPU thread numa thread do host. **Ainda não há decodificador de
instruções SPU**: ao iniciar um grupo, o processo termina com o relatório
`no SPU executor is installed`. `spu_tests` exercita tudo isso com programas SPU em C++ que usam a
mesma interface (`ISpuExecutor`) que o interpretador/JIT SPU vai usar.

Fase 5: `sys_mmapper` (áreas de endereço + objetos de memória), `cellGcmSys` HLE
(`_cellGcmInitBody`, IO mapping, labels, flips...) e o front end do RSX: uma thread do host que
consome o buffer de comandos (jumps, call/return, referência, semáforos, flips). Os métodos
gráficos ainda não desenham nada: são contados e aparecem no relatório final (seção `RSX`).

Fase 6: interpretador do ISA SPU completo (`spu/spu_interpreter`), instalado como executor das
SPU thread groups do lv2. O SPURS continua parando no guarda HLE com relatório; o plano (LLE da
`libsre.sprx`) está em `docs/fase6_spu_spurs_plano.md`.

Fase 7: loader de PRX (relocações SCE, exports/imports), linker LLE/HLE, `sys_prx_*`, VFS e eventos
de grupo SPU. Para rodar a libsre real do seu console (descriptografada):
```sh
gt5_cellos EBOOT.elf --dev-flash /caminho/dev_flash --lle libsre
# ou, com o arquivo solto:
gt5_cellos EBOOT.elf --lle /caminho/libsre.prx
```

Fase 7.1: o que a libsre real pediu — syscall 14 (`sys_process_is_spu_lock_line_reservation_address`),
syscall 30 / `sys_process_get_paramsfo` (lê o `PARAM.SFO`), `_sys_vprintf`/`_sys_vsnprintf`/`_sys_vsprintf`/
`_sys_strncat`, `_sys_spu_printf_*` com os callbacks de verdade, `cellLibprof` — e o relatório de
travamento (`--stall-report`), que mostra em que chamada cada thread PPU/SPU está parada.
```sh
gt5_cellos USRDIR/EBOOT.elf --dev-flash /caminho/dev_flash --lle libsre --max-insns 0 --stall-report 10
```

Sistema de arquivos (pivô para o GT5): `cellFs` completo para E/S síncrona — open/read/write/lseek,
`cellFsReadWithOffset` (leitura posicional, várias threads no mesmo fd), stat/fstat, diretórios,
mkdir/rename/unlink/truncate — sobre o serviço lv2 `sys_fs` (syscalls 801–834). Offsets de 64 bits
de ponta a ponta (arquivos maiores que 4 GiB, como o `GT.VOL`), leitura direto para a memória do
jogo, `/dev_bdvd` e `/dev_flash` somente leitura (`EROFS`), e queda para busca sem distinção de
maiúsculas no Linux quando o dump tem outra caixa. Ainda não: `cellFsAio*`, `cellFsSt*`
(streaming), `cellFsSdata*`.
```sh
# layout de disco: <raiz>/PS3_GAME/USRDIR/EBOOT.elf -> /dev_bdvd = <raiz> automaticamente
gt5_cellos /jogos/GT5/PS3_GAME/USRDIR/EBOOT.elf --dev-hdd0 /jogos/dev_hdd0 \
           --dev-flash /caminho/dev_flash --lle libsre --max-insns 0
```

Fase 7.3: paginação sob demanda do `sys_mmapper` (syscall 327 + `sys_ppu/spu_thread_recover_page_fault`):
uma thread que toca uma área notificada sem memória fica suspensa, a fila do paginador do jogo recebe o
evento, e o acesso é refeito depois do mapeamento — o padrão de boot do GT5. HLE de boot que preenche
estruturas: `cellVideoOut*`/`cellAudioOut*`, callbacks e parâmetros do `cellSysutil`, `cellSysCache*`,
`cellGame*`, e rede offline (`cellNetCtl`, `sceNp`, `sys_net`). Detalhes em `docs/fase7_3_paginacao_boot.md`.

Fase 7.7: os handlers do libgcm (vblank 60 Hz, flip, queue, user) passam a ser chamados numa thread PPU
de interrupção do próprio jogo (`_gcm_intr_thread`, criada no `_cellGcmInitBody` como no console);
contadores do RSX (submissões, draws, clears, flips, quadros apresentados — sempre 0, não há renderizador);
`sys_prx_get_module_list`, `sys_mempool_*`, `cellKb*`/`cellMouse*` sem dispositivo e `cellKey2CharOpen`;
o relatório de travamento mostra quem já sinalizou cada condição e as últimas chamadas antes da espera.
`--no-gcm-interrupts` volta ao comportamento antigo (comparação A/B). Detalhes em
`docs/fase7_7_handlers_gcm_rsx.md`.

## Requisitos
- Windows 10 versão 1803 ou superior (APIs de placeholder `VirtualAlloc2` / `MapViewOfFile3`), ou Linux.
- CMake 3.20+, compilador C++20 (GCC 12+, Clang 15+ ou MSVC 2022).
- zlib (única dependência externa) e threads do sistema (já incluídas no compilador).

## Windows — MSYS2 (GCC ou Clang)
No terminal **MSYS2 UCRT64**:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake \
                   mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-zlib
cd /c/dev/gt5_cellos
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXE_LINKER_FLAGS="-static"
cmake --build build
ctest --test-dir build --output-on-failure
```
Sanitizers (opcional): `-DCMAKE_CXX_FLAGS=-fsanitize=thread` ou `address,undefined`; nesses builds
use `CELLOS_TEST_ITERATIONS=5000` para encurtar o teste de contenção de `kernel_tests`.

Clang: instale `mingw-w64-ucrt-x86_64-clang` e acrescente
`-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++` à linha do `cmake -S`.

`-static` embute libstdc++ e zlib no `.exe`, que então roda também fora do MSYS2 (PowerShell, cmd).

## Windows — Visual Studio 2022 + vcpkg (alternativa)
No "x64 Native Tools Command Prompt":

```bat
vcpkg install zlib:x64-windows-static
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake ^
      -DVCPKG_TARGET_TRIPLET=x64-windows-static -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```
Executáveis em `build\Release\`.

## Linux
```sh
sudo apt install build-essential cmake zlib1g-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Uso
```sh
gt5_cellos <EBOOT.elf> [--no-run] [--max-insns N] [--trace-ppu] [--trace] [--argv0 CAMINHO] [-- args]
```
- `--no-run`: só carrega (comportamento da Fase 1).
- `--max-insns N`: limite de instruções da thread principal (padrão 200 milhões; `0` = sem limite).
- `--dev-bdvd DIR`: raiz do disco (contém `PS3_GAME/`); automático quando o EBOOT está em
  `<raiz>/PS3_GAME/USRDIR/`. `--dev-hdd0 DIR` / `--dev-hdd1 DIR`: dados do jogo/saves e cache do
  sistema (graváveis). O relatório final tem a seção `File system` (fds abertos, bytes lidos e os
  últimos caminhos que o jogo procurou e não encontrou).
- `--resolution 720|1080`: melhor modo de vídeo que a "TV" virtual oferece ao jogo (padrão 720).
- `/dev_hdd1` (cache do sistema) usa uma pasta temporária quando `--dev-hdd1` não é passado.
- `--title-id ID` / `--param-sfo ARQUIVO`: título para `sys_process_get_paramsfo` (por padrão lido do
  `PARAM.SFO` em `PS3_GAME/` acima do `USRDIR`, na pasta do EBOOT ou em `/dev_bdvd/PS3_GAME/`).
- `--stall-report N`: após N s sem progresso das threads PPU, imprime o estado de todas as threads
  PPU/SPU, objetos lv2 com waiters e o RSX (padrão 10; `0` desliga). `--status-every N` imprime o
  mesmo relatório a cada N s (útil quando uma thread fica em laço de polling).
- `--trace-ppu`: imprime cada instrução executada (`pc: palavra  mnemônico`). Muito verboso;
  combine com `--max-insns`.
- `EBOOT.elf`: o `EBOOT.BIN` descriptografado. Um `EBOOT.BIN` retail (criptografado) é recusado
  com uma mensagem explicando isso.
- `--trace`: logs detalhados.
- `loader_tests --dump PASTA` grava EBOOTs sintéticos (`fixture_EBOOT.elf`, `..._debug.self`,
  `..._retail.self`) para testar o executável sem o jogo.

Código de saída: `0` o PPU terminou (saída ou exitspawn), `1` erro de carregamento, `2` uso incorreto,
`3` uma thread parou por falha/instrução não implementada ou uma função HLE/SPU abortou com motivo
(veja `== process ended ==`, linha `reason:` e a seção `SPU thread groups`).
