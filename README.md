# PS4Mount 0.5.13 — Integração segura de funcionalidades do original 0.4.0

Esta versão é baseada na nossa **0.5.12**, com leitura de DLCs, configurações de busca e prévia de grupos inspiradas na versão original de gasb07. **Não** instala DLCs, **não** executa grupos no sistema e **não** reativa `AppInstallAll`. Abrir/reabrir a página é uma operação de consulta. Para uma descrição exata das melhorias e das partes ainda pendentes, leia [`UPSTREAM_040_MERGE.md`](UPSTREAM_040_MERGE.md).

**Teste inicial recomendado:** sincronize a biblioteca e inspecione a aba **DLCs (leitura)**; não inicie registros até validarmos a nova leitura no PS5. A única operação de registro individual ainda permitida no código continua limitada ao CUSA06886, como na 0.5.12.

---

# PS4Mount 0.5.12 — Phase 4K: base-only mount fix and isolated CUSA06886 audit

**Based on the full 0.5.11 source ZIP, not a rewrite.** This release addresses
what the real 0.5.10 PS5 log reported: `CUSA06886` has NO `patch.pkg`, but the
original code still attempted to inspect/unmount `/user/patch/CUSA06886` in
several paths; an unrelated/foreign mount made the writable transition fail.
The log confirms that failure, **not** that every foreign patch layer is safe.

### Changes

- `mount_paths()`: when `has_patch==0`, mount/transition ONLY `/user/app/<CUSA>`;
  do not inspect, mount or unmount `/user/patch/<CUSA>`.
- Probe preview, probe mount, probe release and individual registration's
  pre-write mountpoint checks now check the patch path ONLY if the catalog has
  an approved update. Probe state stores whether a patch was expected.
- `unmount_title()` respects the `has_patch` ownership flag. Existing real
  patch games still refuse to touch any foreign mount layer (fail closed).
- `GET /api/mount-audit?title_id=CUSA06886` reports the app layer, patch layer,
  whether patch is needed, and mount-probe readiness, **without modifications**.
  For a base-only game a foreign patch is labeled `UNUSED_UNTOUCHED`.
- `GET /api/registration-plan` lists the catalog snapshot's pending eligible
  CUSAs and counts, in read-only mode. **This does not authorize or start a
  queue**; a title labeled eligible still needs all per-title preflight checks.
- Only **CUSA06886** is allowed to reach the already-existing explicit
  single-title register preview/POST. Even that is blocked if the prior batch
  state is FAILED/INTERRUPTED/running. No other title can be registered by
  this edition, and any 3/5 batch start request is explicitly refused.
- Auto-start remains OFF (`AUTO_BOOT_BATCH_LIMIT=0`), `AppInstallAll` stays
  unavailable, no direct SQLite writes, no automatic recovery or unmount of
  foreign layers. Opening or refreshing the web panel makes GET requests only.

### First console test (read only, recommended)

1. Build the source via GitHub Actions, load the new ELF and connect the USB.
   The scanner should report 147 indexed titles (the exact count depends on USB).
2. Open `http://<PS5 IP>:8770`. The existing sync is read-only. Use
   **Consultar fila (somente leitura)** to see pending CUSAs; opening the page
   or queue plan does not register anything.
3. Find **What Remains of Edith Finch (CUSA06886)** in the panel. Click
   **Auditar montagem (leitura)**. Record `app_layer`, `patch_layer`,
   `patch_action`, `safe_to_probe`, and the PS4Mount log.
4. **STOP if `app_layer` is `owned_rw`, `foreign_or_unknown`, or the previous
   batch is FAILED/INTERRUPTED.** Do not manually delete `/user/patch` mounts,
   remove `.state` files, or repeatedly re-register. The previous failed
   attempt may have staged system files or left a mount on the console.
5. If it is clear, the read-only **Temporary read-only mount** preview can be
   reviewed for the one title. This mounts temporarily: release it through
   the same panel before stopping the ELF. Do not proceed to registration
   before reviewing the new log and having a restorable console backup.

The source retains the explicit single-title registration flow for CUSA06886,
with the previous failed-batch lock preserved. Therefore the 0.5.12 test **may
intentionally stop before registration**. This is the safe outcome when the
previous 0.5.10 batch has unresolved partial filesystem/metadata state; do not
bypass that lock just to get a success message. Registering all 125 pending
eligible games remains **disabled** until the mount issue is diagnosed on-console
and several isolated tests have succeeded with rollback planning.

### Local validation

`bash tests/run_host_tests.sh`: covers a simulated base-only app mount with a
foreign patch layer, unchanged foreign mounts, update-bearing failure cases,
failed-batch recovery, web queue preview and read-only page load. Static host
checks do **not** demonstrate behavior on PS5 firmware. Requires the PS5 Payload
SDK and GitHub Actions to compile a deployable ELF. Back up console/user data
and your USB PKGs outside the console before any experimental registration.

---

# PS4Mount 0.5.11 — Sincronizacao segura com progresso (Phase 4J)

**Diagnostico comprovado no log da 0.5.10:** somente 2 jogos foram verificados
(CUSA00074, CUSA13285). Em CUSA06886 houve `foreign layer at
/user/patch/CUSA06886; untouched`, seguida por `FAILED at 3/5`.
NAO registrar todos automaticamente enquanto essa camada nao for investigada.

Esta versao faz uma leitura automatica do USB ao iniciar o ELF, depois valida
os jogos do catalogo, mostrando notificacoes de progresso (`1/N`, `25/N`, etc.)
e notificacao de conclusao. `N` inclui jogos com base PKG catalogada;
`elegiveis` e um subconjunto separado, nao significa jogos comprovadamente
executaveis. A API `/api/sync-status` retorna o progresso real em passos;
a pagina atualiza a barra e os numeros usando SOMENTE GET.

O botao **Sincronizar (somente leitura)** solicita reindexacao da lista sem
organizar PKGs, montar ou registrar. Abrir/fechar/recarregar a pagina nao inicia
escrita nem sincronizacao nova. O registro automatico da 0.5.10 foi desligado
(`AUTO_BOOT_BATCH_LIMIT 0`) para evitar repeticoes apos o erro documentado.
O estado de falha existente em `/data/ps4mount/batch-v1.state` e preservado,
sem limpar ou contornar as protecoes.

Os metodos originais de registro individual permanecem no codigo mas nao sao
automaticamente chamados. Nunca usa `AppInstallAll`, nem SQLite WRITE. A
varredura USB ainda e sincrona, portanto a barra de progresso acompanha a
VALIDACAO dos jogos depois da descoberta, nao o progresso de leitura byte a byte
de cada PKG. Ela pode demorar para mostrar 1/N durante um scan USB longo.

**Uso:** compilar via GitHub Actions, executar ELF, observar notificacoes de
leitura e conclusao, abrir painel 8770 para consultar. Para atualizar arquivos
posteriormente, clique Sincronizar; isso nao repete registros.

**Importante:** para liberar um modo de registro de toda a biblioteca sera
necessario diagnosticar o mountpoint estrangeiro do CUSA06886, e testar
recuperacao e enumeracao de titulos em um console de teste com backup. Esta
versao NAO registra 147 jogos.

---

## 0.5.10 Phase 4I — auto-boot (ate 5 jogos)

**Esta distribuicao e a variante com auto-boot ATIVO.** Assim que o ELF for
executado e terminar a primeira varredura de um USB com jogos, ele inicia
**automaticamente UM lote de ate CINCO jogos elegiveis**. Nao e necessario abrir
o navegador, conferir uma previa ou clicar em Iniciar. A pagina /api/batch-status
mostra o andamento em modo somente leitura. **Somente um lote automatico por
execucao do ELF**, sem encadear os 147 jogos.

Requisitos antes de executar: backup verificavel do banco de dados do PS5 e dos
PKGs em outro dispositivo, nenhum jogo em execucao, USB estavel, e aceitacao
consciente de que a API individual escreve metadados internos. **Nao execute
esta variante para apenas navegar ou examinar arquivos**: iniciar o ELF e
autorizar implicitamente um lote com escritas. Para desativar em um build
personalizado, mude `#define AUTO_BOOT_BATCH_LIMIT 5` para `0` em `src/main.c`.

Criterios: apenas CUSAs nao registrados e sem conflitos; por CUSA, preserva as
mesmas fases da 0.5.9 (organizacao, re-scan, preflight, montagem RO, liberacao,
registro individual, verificacao). Nenhum AppInstallAll, fallback SQLite,
registro de DLCs ou retomada automatica apos falha/interrupcao.
Ao fim de ate cinco, PARA; para outro lote sera necessario iniciar o ELF
novamente em nova sessao, depois de conferir que os jogos aparecem/abrem.
Se houve falha/interrupcao na sessao anterior, bloqueia auto-boot ate a
inspecao e recuperacao manual. Se iniciar sem USB, espera catalogacao, mas
nao executa duas vezes na mesma sessao.

# PS4Mount 0.5.9 — Fila automatica no ELF (Phase 4H)

**Base:** PS4Mount 0.5.8, mantendo o mesmo `app_plan_json` / `app_organize` / `app_probe_mount` / `app_probe_release` / `app_register_one`, com registro INDIVIDUAL por `sceAppInstUtilAppInstallTitleDir`. Nenhuma API global foi reativada.

## Objetivo

Na 0.5.8, fechar o navegador interrompia o `for` do JavaScript no meio do lote. A partir da 0.5.9 o painel apenas envia **UMA** solicitacao com nonce para iniciar; uma maquina de estados no `src/main.c` processa o restante dentro do ELF. O painel pode ser fechado e reaberto para consultar `/api/batch-status`; isso NAO fecha/para o ELF. **Se o ELF ou PS5 parar, o processamento para e NAO reinicia automaticamente.** O ultimo lote incompleto e marcado `interrupted` e novas escritas automaticas sao bloqueadas ate inspecao manual.

- Escolha `3` (recomendado para primeiro teste) ou `5` (somente depois de validar tres jogos). Nao ha opcoes de 10/147.
- Server seleciona no maximo 3 ou 5 CUSAs elegiveis, ordenados por tamanho e CUSA. Jogos registrados e conflitos/suspeitas sao ignorados. A pre-visualizacao informa cada nome e CUSA; confirme com backup.
- Para cada jogo: plano+nonce -> USB organizar apenas se necessario -> reindexar -> preflight -> RO mount+nonce -> release+nonce -> registro individual+nonce -> conferir app.db/RO. So entao passa ao seguinte.
- Um erro causa parada definitiva do lote. Sem retry, sem AppInstallAll, sem SQLite WRITE e sem executar automaticamente jogos.
- Progresso e fases persistidos atomicamente em `/data/ps4mount/batch-v1.state` (NAO inclui tokens); um reboot nunca refaz mutacoes incertas.
- Enquanto roda, o servidor rejeita outras chamadas POST no painel, inclusive `stop`. Nao desconecte USB, nao encerre ELF, nao inicie outro jogo ate terminar. A fila nao executa novas fases se detectar jogo gerenciado rodando ou montado RW.

**Procedimento:** compile pelo GitHub Actions, inicie o ELF, abra painel na porta 8770, clique `Automatizar jogos (ELF)`, selecione 3, leia a lista, confirme backup, clique `Iniciar lote no ELF` UMA VEZ. Aguarde `Estado: completed`, com `Registrados neste lote: 3 / 3`. Pode sair da pagina e retornar, mas nao desligar PS5 ou encerrar ELF. Se `failed` ou `interrupted`, NAO tente novo lote; envie log e inspecione app.db, USB journal e mountpoints. Depois de validar um lote inteiro de 3 e jogos no home screen, pode escolher 5 com novo preview.

**Validacao:** `bash tests/run_host_tests.sh` passou no ambiente de host (testes C de estados e tokens, recuperacao apos interrupcao simulada, e navegador simulado incluindo fechamento/reabertura). Compilacao e API reais do PS5 **nao foram executadas aqui**; o GitHub Actions continua responsavel por gerar o ELF. Escritas em metadados internos e mounts experimentais podem danificar o console; faça backups restauraveis. A interface web **nao tem login**: use somente rede local confiavel.

---

# PS4Mount 0.5.8 — Automacao limitada a 3 jogos por vez (Phase 4G)

Registro REAL foi comprovado no log do PS5 0.5.7 para `CUSA03323` usando `sceAppInstUtilAppInstallTitleDir`; o PS5 lancou `eboot.bin` e o usuario confirmou que jogou. **Essa evidencia nao valida automaticamente outros titulos nem justifica registrar 147 de uma vez.**

NOVO: no painel, clique **Automatizar proximos 3 (experimental)**, confira os ate tres nomes mostrados, declare backup de console e PKGs e inicie. O browser executa sequencialmente, sem novas confirmacoes por jogo: preflight, plano, se preciso rename via journal, rescan, novo preflight, montagem read-only individual, liberacao, registro com API individual e verificacao de app.db + mount RO pela rotina existente. Nao inclui AppInstallAll, SQL WRITE, registro no scan nem execucao automatica de jogos. Se houver falha, timeout, mount nao liberado ou resultado desconhecido, PARA e nao tenta novamente. Um novo lote so e iniciado manualmente apos conferir a biblioteca e logs.

O backend da 0.5.7 permitia somente uma montagem experimental por sessao. Na 0.5.8 o bloqueio e resetado **somente apos o registro individual completar sem falha, com app.db confirmada, marcador escrito e montagem verificada como RO**. Caso contrario continua bloqueado.

**Nao tente 147 jogos em um lote**: ha 6 conflitos e 3 atualizacoes suspeitas no catalogo conhecido, outros titulos podem falhar na API. Nao ha rollback transacional dos metadados internos; mesmo tres titulos podem causar travamento, degradacao ou recuperacao da base de dados. Producao desaconselhada. Executar games somente pelo PS5 home. Recomendo testar o primeiro lote e enviar log antes de seguir.

Se uma solicitacao HTTP falhar, o status real do console pode ser desconhecido. Nao repita. Console e disco devem ter backups testados. O painel :8770 nao tem login: use somente em rede local confiavel e desative acesso durante os testes.

Testes locais validam apenas codigo de host e JavaScript simulado, **nao** garantem funcionamento de varios jogos no PS5. ELF via GitHub Actions.

---

# PS4Mount 0.5.7 — Registro individual experimental (Fase 4F)

**Limites:** esta compilacao NAO garante execucao do jogo. O log 0.5.6 nao mostrou montagem efetiva, somente PREVIEW. A nova versao corrige os `prompt()/confirm()` escondidos pelo `<dialog>`, para que o botao realmente envie a tentativa de montagem. Adiciona suporte *condicional* a `sceAppInstUtilAppInstallTitleDir` (NID `Wudg3Xe3heE`), SEM fallback para `sceAppInstUtilAppInstallAll(NULL)`. Firmwares 12.00+ podem nao oferecer essa API: neste caso, o painel bloqueia registro ANTES de escrever metadados.

**Procedimento:** Backup dos PKGs e dados internos. Selecione CUSA03323 > `Temporary read-only mount` > `Montar SOMENTE este jogo (RO)`; espere resposta. Reabra a janela e clique `Liberar montagem deste jogo`; espere sucesso. So se ambas ocorrerem, clique `Tentar registro individual (apos montagem)`, confira se `Individual API available: true` e siga a confirmacao de backup. O app vai tentar escrever os metadados do titulo e registrar so esse CUSA, verificando `app.db` somente por leitura. Se confirmado, abra pelo home screen do PS5; nenhum injetor ShellUI foi incluido. Caso ocorram falhas, nao repita, pois pode existir estado parcial interno. Nao desligue nem desconecte USB durante as operacoes.

**Risco serio:** o registro grava metadados internos e pode travar o console. Nao ha rollback transacional da preparacao interna nem mecanismo de desinstalacao nesta versao. Nenhuma chamada em lote nem escrita SQL direta foi adicionada. Use somente em console que possa ser restaurado a partir de backup e nunca em console de producao.

Build ELF somente via GitHub Actions/PS5 SDK. `bash tests/run_host_tests.sh` testa as partes portaveis, nao as APIs/montagens do PS5. **Este ZIP e codigo-fonte, nao ELF.**

---

# PS4Mount 0.5.6 — Corrige prévia após organizar no USB (Fase 4E)

**Confirmado no log real da 0.5.4:** `CUSA03323 DONE`, e no rescan os dois PKGs aparecem em
`/mnt/usb0/PS4/CUSA03323 - Uncanny Valley/`. A prévia de montagem foi bloqueada porque o
plano gerava `/mnt/usb0/ps4/...` (pasta com letra minúscula), e a comparação literal de
caminhos não reconhecia os PKGs **já movidos** no exFAT. Isso é um bloqueio lógico,
não uma falha de movimentação dos arquivos.

Esta versão mantém a preparação automática **por UM jogo** da 0.5.5 e corrige a
normalização limitada ao nome da pasta `PS4/ps4` **quando a origem já está na
localização esperada**. Não ignora verificações de origem, proteção contra
symlinks, sobregravação ou conflitos. O diagnóstico `mount-probe` passa a registrar
`plan_detail` para tornar o próximo bloqueio visível.

**Próximo teste:** NÃO organize o Uncanny Valley outra vez. Ele já foi organizado.
Abra a 0.5.6, selecione CUSA03323 e use apenas `Check launch prerequisites`.
Se `PREFLIGHT-PASS`, a preparação foi validada; o teste temporário `nullfs` é opcional,
separado e envolve riscos de montagem do PS5. **Nenhum jogo é instalado, registrado
ou executado por esta versão.** Não reativa AppInstallAll ou SQLite WRITE.

---

# PS4Mount 0.5.5 — Preparação automática de UM jogo (Fase 4D)

**Novidade:** botão **Preparar automaticamente (1 jogo)** em cada cartão. Um único clique, no CUSA selecionado, executa as etapas anteriores que exigiam várias janelas: GET /api/plan, POST /api/organize com token individual descartável, POST /api/rescan, conferência dos caminhos atualizados e GET /api/launch-check. A preparação altera somente os arquivos do CUSA selecionado no USB (rename transacional); não registra, monta ou inicia o jogo.

- **Nunca organiza a biblioteca toda ao iniciar.** O processo só roda por clique no cartão do jogo escolhido. Nada é feito automaticamente durante o scan dos 147 jogos.
- A proteção no backend permanece: nenhum conflito, nenhum journal pendente, arquivos estáveis, sem sobrescrever destinos e token de uso único com validade curta. Falhas **nunca** iniciam tentativa automática de novo POST.
- O painel mostra se a organização foi concluída e o resultado real do `launch-check`. Um `PREFLIGHT-PASS` **não significa que o jogo inicia**; registro continua desabilitado e sem chamadas AppInstallAll ou SQLite WRITE.
- O teste do `nullfs` continua sendo uma ação separada e experimental, por CUSA, com desbloqueio somente se passar no preflight.
- Faça backup dos PKGs em outro disco antes de usar; nenhuma promessa de ausência de risco em exFAT ou montagens experimentais. A porta 8770 não tem autenticação e deve ficar apenas em uma LAN confiável.

**Teste rápido:** no PS5 escolha `Uncanny Valley / CUSA03323`, clique `Preparar automaticamente (1 jogo)` UMA VEZ, aguarde resultado na janela e envie `/data/ps4mount/log.txt`. Se houver erro ou perda de conexão, **não repita**. Use o painel `USB recovery` para inspecionar o journal.

```bash
bash tests/run_host_tests.sh
PS5_PAYLOAD_SDK="$HOME/ps5-payload-sdk" bash build.sh
```

---

# PS4Mount 0.5.4 — Phase 4C: organizer confirmation UI fix


This build adds an **explicit, single-title, temporary read-only nullfs mount** on top
of version 0.5.2. It does **not** register, install, or launch PS4 titles. The
legacy `sceAppInstUtilAppInstallAll(NULL)` call remains **disabled**, and all
SQLite operations are still read-only.

## Console test (EXPERIMENTAL)

1. Back up the chosen title's PKGs and any important PS5 data. Use **Uncanny
   Valley / CUSA03323** only. Do not batch other titles.
2. Finish USB organization and verify **Check launch prerequisites** says
   `PREFLIGHT-PASS`. If it is `BLOCKED`, stop: the probe refuses to mount.
3. Choose **Temporary read-only mount (Phase 4B)**, review the preview, type
   `MOUNT CUSA03323` and confirm. This only mounts directories: it does not
   create a library entry or launch a game.
4. Open the same probe dialog again. Type `RELEASE CUSA03323` to unmount the
   layers. **Do not reboot / disconnect the USB while a probe is active.**
5. Export `/data/ps4mount/log.txt` and send it for analysis. If cleanup fails,
   **do not attempt another mount**. Do not treat a successful mount as proof
   that title registration/launch will work.

Only one mount attempt is allowed per daemon session. A different/unknown/foreign
mount is never forcibly unmounted. If the daemon stops gracefully it attempts
owned-layer cleanup. An unexpected shutdown can leave temporary mounts until
reboot (but this build never writes the game to PS5's app databases).

**Security:** the web panel binds to all interfaces and has no full login or
HTTPS. Use only on a trusted isolated LAN. The confirmation nonce is short-lived
and single-use, but it is not a substitute for authentication.

## Build

```bash
PS5_PAYLOAD_SDK="$HOME/ps5-payload-sdk" bash build.sh
bash tests/run_host_tests.sh
```

The host tests simulate mounts; only the PS5 SDK/real console can validate the
actual nullfs behavior. This release makes no changes to system SQLite or
AppInstallAll and gives no guarantee that a game can launch.

---

# PS4Mount 0.5.2-phase4a-preflight

## Fase 4A — auditoria por jogo, **somente leitura**

Esta versão adiciona um preflight de montagem/registro **sem montar e sem registrar nada**. O objetivo é descobrir se o **CUSA escolhido** está realmente organizado na pasta aprovada na Fase 3B, sem conflito de PKG/update, com metadados estáveis, sem journal de movimentação pendente, com o USB montado e sem colidir com uma instalação existente. Consulta o SQLite do PS5 **apenas em modo leitura** para identificar se a biblioteca já registra esse CUSA.

### Como usar

1. Compile o ELF pelo GitHub Actions (`build.sh` inclui `src/organize.c`). Execute `PS4Mount 0.5.2-phase4a-preflight` no PS5.
2. Entre em `http://IP_DO_PS5:8770`. Selecione **um** jogo, de preferência `Uncanny Valley / CUSA03323`.
3. Clique **Check launch prerequisites (READ-ONLY)**. O painel mostrará cada verificação como OK ou bloqueada/não verificada, o caminho atual, o esperado e os possíveis destinos de montagem (somente informativos).
4. Se aparecer `PKGs are not yet at their Phase 3B destination`, a Fase 3B ainda não foi testada/completada: faça backup externo e realize o teste com **um jogo apenas**, conforme as instruções da versão 0.5.1. Depois use `Scan for games` e refaça a verificação.
5. Envie o resultado `launch-check` (texto do painel) e `/data/ps4mount/log.txt` antes de seguir para montagem real.

A rota `GET /api/launch-check?title_id=CUSA03323` fornece JSON de leitura (`read_only: true`, `launchable: false`, `registration_enabled: false`). `preflight_passed: true` **NÃO** significa que o registro ou execução funcionam: só confirma que os requisitos locais contemplados foram verificados.

**Nenhuma nova rota POST de registro/montagem foi criada.** Registro experimental continua `ENABLE_EXPERIMENTAL_REGISTRATION 0` e as escritas diretas em SQLite continuam bloqueadas. Os seis títulos com conflitos permanecem bloqueados. O panel de Fase 3B e o rollback continuam disponíveis.

### Riscos ainda não resolvidos

- O código legado de registro usa `sceAppInstUtilAppInstallAll(NULL)` (operação global) e escreve metadados em caminhos internos do PS5; **não** foi ativado. Um teste seguro do registro exige substituir/auditar a estratégia global, elaborar transação de metadados com recuperação e validar montagem `nullfs`/desmontagem em console isolado.
- Os metadados dos PKGs são verificados pelo parser, não por leitura/hash integral de dezenas de GB. Um resultado positivo não comprova integridade criptográfica nem compatibilidade do jogo.
- O banco da biblioteca é consultado com `SQLITE_OPEN_READONLY`; se a leitura falhar, o preflight é bloqueado. Não trate falha de consulta como CUSA ausente.
- O painel local não tem autenticação de usuário; nunca exponha a porta 8770 na internet/rede não confiável.

### Testes

```bash
bash tests/run_host_tests.sh
PS5_PAYLOAD_SDK="$HOME/ps5-payload-sdk" bash build.sh
```

Os testes no host simulam disco/PKGs no `/tmp`, mas **não** validam montagem/registro nem funcionamento no PS5. A compilação real do ELF precisa rodar no GitHub Actions.

---

# PS4Mount 0.5.1-phase3b-single — Fase 3B (um jogo por vez, USB)

> **Esta versão MOVE arquivos PKG de verdade, mas SOMENTE após confirmação explícita no painel. NÃO registra, NÃO monta e NÃO executa jogos.** A validação local usa arquivos temporários, não um PS5. Faça backup dos PKGs de teste antes de usar a operação real. Não desconecte o HD durante movimentação/recuperação.

## Objetivo

Continuar a `0.5.0-phase3-plan`: preservar a descoberta recursiva, escolha da versão mais recente e bloqueio dos seis títulos conflitantes, acrescentando a **organização individual, confirmada e com registro de recuperação** dentro de um mesmo volume USB (`/mnt/usb0` a `/mnt/usb7`).

Exemplo: `<usb>/God of War Base/app.pkg` e `<usb>/God of War Update/...pkg` passam para `<usb>/ps4/CUSA07408 - God of War/app.pkg` e `patch.pkg` (o nome é gerado pelo SFO). Um `app.pkg` já localizado em `/ps4/CUSAxxxxx/app.pkg` permanece na pasta original, como na versão 0.5.0. Não movimenta DLCs nem updates não selecionados.

## Como usar — teste de UM título com backup

1. Instalar a versão compilada `PS4Mount 0.5.1-phase3b-single` pelo ELF construído no GitHub Actions. Fazer um backup dos dois PKGs do título escolhido **fora do próprio HD que será modificado**. Preferir um jogo pequeno, sem conflito, como `CUSA03323` (Uncanny Valley, conforme o log anterior).
2. Abrir `http://IP_DO_PS5:8770`, escolher o título sem conflito, clicar `Preview folder plan` e conferir **todos os caminhos**, o tamanho e a escolha de update.
3. Só após conferir, usar `Organize THIS title on USB`. A tela pede o CUSA exato e uma segunda confirmação. O token dessa prévia é de uso único e vence em 120 segundos.
4. Esperar o resultado; sem reiniciar, sem tirar o HD da tomada ou remover durante a operação. Conferir no PC os arquivos `app.pkg` e `patch.pkg` no destino e o sumiço dos arquivos de **origem escolhidos**. DLCs/updates antigos devem permanecer nas pastas originais.
5. Reabrir o painel e fazer `Scan for games`: o mesmo CUSA deve ser reconhecido com os novos caminhos. Enviar `/data/ps4mount/log.txt` antes de mover outro jogo.

**Importante:** se houver erro, NÃO tentar novamente imediatamente. Usar `USB recovery` no painel, informar a porta (geralmente `0`) e seguir a confirmação de rollback. Se disser `invalid journal`, `rollback incomplete` ou `manual inspection`, NÃO apagar o journal nem renomear nada sem examinar o HD e o log; recuperar a partir do backup pode ser necessário.

## Operação e proteção

- `GET /api/plan?title_id=CUSAxxxxx` continua somente leitura, verificando caminhos, USB montado, conflitos, metadados do PKG, symlinks e destinos existentes; oferece um nonce aleatório de uso único na resposta.
- `POST /api/organize?title_id=CUSAxxxxx` requer `X-PS4Mount-Token` da prévia e apenas realiza **rename dentro do mesmo USB**, sem cópia e sem chamadas de montagem/registro. Revalida metadados e identidade/tamanho/data dos arquivos antes do movimento. Nunca opera automaticamente ao iniciar nem processa fila.
- Journal persistente: `<usb>/ps4/.ps4mount-transaction` criado e sincronizado **antes** do primeiro rename. Seu conteúdo armazena os dois pares de caminhos e assinaturas rápidas do começo e do fim dos PKGs. Se um movimento falhar, tenta restaurar os caminhos originais; se a restauração não puder ser verificada, mantém o journal e bloqueia novas gravações.
- `GET /api/recovery?usb=0` examina o journal sem alterações; `POST /api/rollback?usb=0`, também com token e confirmação, é a recuperação **manual**, nunca acionada automaticamente no boot. Verifica arquivos antes de renomear de volta, sem sobrescrever arquivos existentes.
- Não segue links simbólicos em caminhos, recusa destino ocupado, incompatibilidades, arquivos especiais, mudanças inesperadas e arquivos fora da mesma montagem USB.
- O registro continua **desativado**: `ENABLE_EXPERIMENTAL_REGISTRATION 0`; rotas antigas `/api/mount` e `/api/unmount` não ativam instalação ou remoção. Também não escreve no SQLite do sistema.

## Limites importantes (não confundir com segurança garantida)

- A verificação de destino antes do `rename` **não equivale** a um `rename-no-replace` atômico: outro processo poderia modificar o USB no intervalo. Não usar outros programas gravando no mesmo HD durante a operação.
- Os hashes de início/fim usados para recuperação são verificação parcial, **não** garantem integridade completa ou autenticidade criptográfica de PKGs de dezenas de GB. Eles não substituem backup nem validação independente.
- `fsync` e `sync()` reduzem a janela de perda de dados mas não garantem que o firmware/driver exFAT/controlador USB persistiu tudo numa falha de energia. Um journal inválido/ausente após queda abrupta deve ser resolvido manualmente.
- O scanner, as operações de filesystem e o painel foram testados **fora** de um console PS5. A compilação ELF ainda precisa ser verificada pelo SDK do GitHub Actions e a movimentação real deve ser testada em **um** título com backup. Não são testes de registro, montagem ou execução.
- A interface HTTP na porta 8770 deve ser usada apenas em **rede local confiável**. O token evita acionamento acidental/CSRF simples, não autentica usuários contra atacantes com acesso direto à rede.
- O journal usa um formato próprio desta versão; fazer a recuperação usando a **mesma versão do PS4Mount**, não atualizar a ferramenta enquanto houver um journal pendente.

## Testes de host e build

```
bash tests/run_host_tests.sh
PS5_PAYLOAD_SDK="$HOME/ps5-payload-sdk" bash build.sh
```

Os testes de host verificam seleção, DLCs, conflitos, parser, controles de registro, organização real em diretórios temporários, recusa de sobrescrita, symlinks, journal truncado, interrupções simuladas e rollback individual. O SDK PS5 **não está instalado aqui**; a segunda linha deve ser executada pelo GitHub Actions.

## Ainda faltam para executar jogos

**Fase 4:** revisão isolada da integração `nullfs`, montagem, metadados e registro da biblioteca do PS5, sem SQLite direto, registro em massa ou desinstalação forçada. **Fase 5:** testes no console real de lançamento do jogo, reinicialização, desligamento e desconexão do disco. Não habilitar registro apenas porque os testes locais passaram.

Projeto derivado de [gasb07/ps4mount](https://github.com/gasb07/ps4mount), GPL-3.0-or-later.

## Phase 4C — confirmation UI fix (0.5.4)

In the 0.5.3 PS5 logs, the organizer generated a READY plan but never received
`POST /api/organize` when the user clicked the button. Previous UI depended
on native `prompt()` + `confirm()` dialogs, which may be suppressed by browsers
while an HTML `<dialog>` is open. This update replaces both popups with an
in-panel, two-stage, per-CUSA confirmation. The user first clicks Organize,
then types the CUSA, confirms an **external** backup, and explicitly clicks the
second confirmation button. The UI displays success/error and never retries
a move on uncertain HTTP responses.

The server now logs `[safe][organize-http] POST CUSAxxxxx` when a request
actually arrives, without logging confirmation tokens. If that line is missing,
the browser never sent the request. Registration, database writes, game launch
and bulk moves remain disabled. Mount probe behavior is unchanged.

For hardware testing: select **Uncanny Valley CUSA03323**; preview the folder
plan; click Organize; type CUSA03323; confirm the backup checkbox; click the
second Confirm button. Do not disconnect USB. On uncertain outcome, inspect
USB recovery and do not retry.
