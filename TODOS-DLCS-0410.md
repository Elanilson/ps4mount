# PS4Mount 0.4.10 — Todos os DLCs encontrados no USB

- **Montar**: sempre faz `dlc_open()` para PKGs organizados e validos, independentemente da configuracao `dlc=0` antiga, arquivos `.removed` e estado no `addcont.db`. Jogo, update e pastas externas dos DLCs sao montados como um conjunto. A etapa normal de montagem prepara os metadados; ela nao executa instalacao convencional em disco interno nem garante registro dos DLCs no banco.
- **Desmontar**: continua retirando somente as montagens criadas pelo PS4Mount de DLCs, update e base. Instalacoes nativas internas nao sao removidas.
- **Instalar DLCs**: aciona `sceAppInstUtilInstallByPackage()` separadamente para **todos** os DLCs validos do titulo, inclusive aqueles marcados como ja registrados no banco. O retorno da API e o estado final sao registrados separadamente; uma falha com um pacote nao oculta os seguintes, salvo estado incerto de tarefa em execucao.
- **Marcadores**: `.removed` de DLC legados nao impedem mais sua descoberta. Eventos `[PlayGoCore][Uninstall] end` so sao tratados como confirmados se o codigo de retorno for `0x00000000`. DLCs desinstalados pelo PS5 nao apagam a copia preparada pelo PS4Mount nem geram novos marcadores `.removed`.
- **Configuracao**: antiga opcao de desligar DLC removida do painel. O campo `dlc=0` continua sendo aceito por compatibilidade, mas os DLCs sempre serao incluidos.
- **Protecoes preservadas**: nenhum `AppInstallAll`; validacao basica de pkg, conexao USB, base nao instalada internamente, concorrencia, tarefas nativas com estado incerto, e montagem compartilhada (nao sobrepoe add-ons instalados internamente). Essas protecoes evitam perda de dados.

## Teste no Resident Evil 7

1. Substitua o codigo por v0.4.10, compile ELF e execute. Abra o painel em `http://<IP do PS5>:8770`.
2. Clique **Montar** CUSA03842 e confirme no log `[manual][dlc] CUSA03842 discovered=17 ...` e `activated ... 17 DLC source(s) staged`.
3. Confirme que o card mostra `17 DLC(s)` e **Instalar DLCs**.
4. Feche o jogo e clique **Instalar DLCs**; a instalacao nativa tentara cada pacote individualmente. Observe `[dlc-install] CUSA03842 found=17 ... attempting_all=17`, retornos por pacote, e o diagnostico DB ao fim.

**Alerta:** instalar DLC por PKG e uma operacao separada de Montar. Pode consumir espaco interno e persistir apos Desmontar. Se a API retornar erro de DLC ja instalado, o log mostrara a falha sem esconder o pacote. A instalacao no console nao foi validada aqui.
