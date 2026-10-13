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
