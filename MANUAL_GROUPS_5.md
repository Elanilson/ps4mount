# PS4Mount 0.5.14 — grupos manuais de 5 (Phase 4M)

**ALTERAÇÕES DE METADADOS NO PS5: alto risco.** Apenas para testes em um console com backup restaurável. Não validado no hardware nesta versão. Um registro `AppInstallTitleDir` com retorno zero e presença no `app.db` não garantem que o jogo inicia; teste o tile no PS5.

## Fluxo implementado

1. Ao executar o ELF: catalogação e verificação **somente leitura**. Não registra nada. Abrir e reabrir `http://IP_DO_PS5:8770` também **não registra**.
2. Clique em **Iniciar registro em grupos de 5**: o painel solicita a **prévia completa** das entradas ainda ausentes do `app.db`, excluindo títulos em conflito, com atualização suspeita, duplicata inconclusiva, drive não suportado, instalação interna ou journal USB pendente. Todo resultado desconhecido do DB bloqueia a prévia. A prévia usa token aleatório com validade máxima de 120 segundos.
3. Leia a relação de jogos, confirme backup e ausência de jogo em execução e clique em **CONFIRMAR e iniciar registro dos grupos** **uma única vez**. O backend recebe **um POST** e salva o conjunto completo de CUSAs em `/data/ps4mount/manual-groups-v1.state` **antes de iniciar**.
4. O daemon separa a fila em grupos **fixos de cinco** (o último pode ter 1 a 4). Para cada título usa o fluxo existente: `app_plan_json`/organização USB com journal, rescan se preciso, preflight, montagem RO temporária, liberação, `sceAppInstUtilAppInstallTitleDir` individual, verificação de `app.db`, marcador de origem e retorno a montagem RO. Não utiliza `AppInstallAll`, SQL WRITE ou instalação de DLC.
5. Ao concluir os cinco, exige que **todos** estejam registrados no `app.db`, com marcador de origem e montagem própria RO. Se todos passarem, grava o checkpoint e aguarda **10 segundos**; **ao final da pausa faz a mesma conferência novamente**, antes de iniciar o próximo grupo. Falha de um único jogo ou de uma conferência de grupo causa **parada total**. Nenhum pulo ou retry oculto.
6. A barra do painel usa o status do ELF, com `registrados/N`, `processados/N`, `grupo atual/grupos totais`, título atual, etapa e segundos restantes. O ELF exibe notificações no PS5 com andamento geral e conclusão ou erro. Não há POST no polling.

## Interrupções e bloqueios

- **Nenhuma retomada automática:** reiniciar o ELF durante um grupo ou a pausa marca o job como `interrupted` no journal; mesmo um grupo recém-concluído não permite avançar sem investigação.
- **Falha bloqueia novos lotes:** `failed`/`interrupted` no journal antigo `/data/ps4mount/batch-v1.state` OU no novo journal impede iniciar. **A falha real anterior do CUSA06886 da 0.5.10 pode continuar guardada**. Nesse caso, a prévia será recusada corretamente. **NÃO apague os arquivos `.state`, não force desmontagens e não repita registros** sem investigar logs, `app.db` e metadados/montagens residuais.
- Conflitos de atualização e DLCs suspeitas continuam excluídos; DLCs `CATEGORY=ac` são apenas catalogados, nunca instalados.
- Antes de começar, nenhuma ação de registro ou organização é executada automaticamente. **Após o clique de confirmação, a fila inteira pode modificar dezenas de entradas do sistema**. Pode sair da página; não desligue o console/ELF ou desconecte o USB.
- Segurança adicional: montar ou acessar um caminho estrangeiro é proibido; o caso sem patch usa apenas `/user/app/<CUSA>`. O fluxo não remove montagens desconhecidas.

## Código e APIs

- `GET /api/batch-preview?limit=5`: analisa e apresenta todos os candidatos pendentes; revoga a prévia anterior.
- `POST /api/batch-start?limit=5` + header `X-PS4Mount-Token` recebido na prévia: inicia **uma única execução da fila**.
- `GET /api/batch-status`: fases `idle`, `running`, `waiting`, `completed`, `failed`, `interrupted`; inclui `group_phase`, `group_index`, `group_total`, `count`, `processed`, `registered`, `current`, `wait_seconds`, `message`, `previous_batch_unresolved`, `legacy_batch_message`.
- `GET /api/sync-status` e botão **Sincronizar** permanecem somente leitura.
- O botão manual trabalha com grupos de 5 fixos; o controle `group_size` da página Settings ainda configura apenas **prévia de grupos**, não muda a execução manual desta versão.

## Testes locais

Execute `bash tests/run_host_tests.sh` na raiz. Inclui casos de grupo `5+5+2`, estado persistido RUNNING/WAITING transformado em INTERRUPTED ao reiniciar, autorização exata de CUSA em fase de registro, parada quando faltam confirmações por jogo ou por grupo, ausência de POST automático ao carregar página, e não repetição após reload. Os testes e a sintaxe não substituem compilação pelo SDK e validação no PS5.
