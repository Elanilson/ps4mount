# PS4Mount v0.4.19 — matriz de testes individuais de registro USB

## Objetivo

Testar **um DLC selecionado** por clique, mantendo seu arquivo `ac.pkg` no HD USB, sem instalar o PKG nativamente. O jogo já precisa estar montado no painel. **Nenhum método novo está comprovado como registrador de DLCs**. São variações experimentais da **mesma função** `sceAppInstUtilAppInstallTitleDir`, e retornos `0x00000000` não comprovam registro.

## Passos

1. Faça backup dos dados importantes do console antes de testar chamadas experimentais do serviço de instalação.
2. Conecte o HD com o jogo e o DLC. No painel, clique **Montar** no jogo se ele ainda não estiver montado. O teste usa somente um DLC escolhido, mesmo que o HD tenha outros.
3. Expanda **Testes de registro USB (5 métodos)** no cartão do jogo.
4. Clique **M1** (controle, já testado em versões antigas). Selecione o DLC. Veja as mensagens `[dlc-probe] M1`.
5. Se `NEW_DB_RECORD=0`, tente **M2**, depois **M3**, **M4** e **M5**, **um de cada vez**. Pare se aparecer `NEW_DB_RECORD=1` e então valide no jogo.
6. Envie o log completo contendo `[dlc-probe]`. Não execute todos os botões ao mesmo tempo.

| Método | ID passado a `AppInstallTitleDir` | Diretório passado como raiz | Estado |
|---|---|---|---|
| M1 | CUSA do jogo | `/user/app/` | Controle; já falhou em registrar DLC novo nos testes anteriores |
| M2 | CUSA do jogo | `/user/addcont/` | Hipótese não validada |
| M3 | CUSA extraído do PKG do DLC | `/user/addcont/` | Hipótese não validada |
| M4 | CUSA extraído do PKG do DLC | `/user/addcont/<DLC_TITLE_ID>/` | Hipótese não validada |
| M5 | CUSA extraído do PKG do DLC | `/user/addcont/<DLC_TITLE_ID>/<LABEL>/` | Hipótese não validada |

### Como ler o log

- `QUEUED`: painel aceitou o pedido.
- `BEFORE=0`: DLC não estava no `addcont.db`.
- `RETURN rc=...`: retorno da função; não equivale ao registro do DLC.
- `AFTER=1 NEW_DB_RECORD=1`: evidência de registro novo no banco durante o teste; ainda é preciso verificar reconhecimento no jogo e a origem do arquivo.
- `STILL_MISSING=1`: função não criou o registro.
- `PREPARE_FAILED` / `BASE_NOT_READY`: nenhum método de registro foi chamado; consulte motivo.

Os métodos **não pulam DLCs por `.removed` nem por já constarem no banco**. Os controles de serialização e existência do PKG evitam mexer no jogo errado ou executar operações concorrentes.

### O que foi preservado

- Scanner e montagem USB; jogo base, patch e DLCs já existentes.
- Consulta `addcont.db`, o botão de revalidação USB anterior e instalador nativo **separado**.
- Redirecionamento sequencial dos 17 DLCs do Resident Evil 7, restauração e piloto reversível do God of War.
- Nenhum `AppInstallAll` (já provocou eventos de desinstalação de outros jogos).
- Nenhuma escrita direta em `addcont.db`, nem exclusão dos arquivos do SSD.

### Melhorias da v0.5.0 incorporadas

- Limite de leitura/alocação para entradas PKG.
- Verificação rigorosa de offsets/tamanhos do `param.sfo`.
- Validação de título antes de ler o ícone pela interface.
- Proteção do arquivo de log caso sua abertura falhe.
- Desconsiderar eventos de remoção de DLC fora da tela de exclusão.

Outras funcionalidades da v0.5.0 (devolver PKGs à pasta original, re-registrar automaticamente jogos com erro de atualização, desinstalação nativa em thread) **não foram importadas**: implicariam trocar fluxos que já funcionam ou desinstalações não solicitadas.

### Compatibilidade e limites

Validado com compilação de sintaxe C e testes locais em computador, **não executado no firmware real**. Mesmo um HD com só um jogo não isola estado global persistido no PS5. Os métodos M2–M5 podem receber erro por um diretório não ser aceito pela API; essa é justamente a informação que o log deve revelar. Não desligue ou desconecte o HD durante uma chamada.
