# PS4Mount 0.4.21 — recuperação do offload após `0x80a30003`

## O que foi corrigido

Na 0.4.20, o instalador nativo criou o registro do DLC do MARVEL Cosmic Invasion (`CUSA54308`), mas `sceAppInstUtilGetInstallStatus()` retornou `0x80a30003`. O código encerrava o procedimento antes de tentar o offload. O log real mostrou `database_registered=1`, sem nenhuma linha `[offload]`.

Na 0.4.21, a aplicação faz até cinco consultas de status antes de tratá-lo como indisponível. Em seguida, **sem presumir sucesso**, aguarda até 180 segundos pela presença do registro no `addcont.db`, pelo PKG nativo com o mesmo tamanho do USB, pelos metadados `ac.json`/`ac.pbm`/`ac.pbm.backup` e por 15 leituras estáveis do arquivo, mais um período de tranquilidade do banco. Depois faz a comparação integral de bytes, prepara um journal, cria o stub de 0 bytes, valida a montagem `nullfs` e **somente então** exclui o arquivo nativo. Registros e caminhos antigos são preservados.

`0x80a30003` **não é prova de término da instalação**. Estabilidade de arquivos também não fornece uma garantia absoluta de que o instalador encerrou; por isso o teste permanece experimental. Se qualquer confirmação falhar, a cópia do SSD é preservada.

## Teste com MARVEL

1. Faça backup dos dados importantes e mantenha o PKG original no USB. Feche o jogo.
2. Inicie a **0.4.21** e clique em **Montar**. Se o jogo já estiver montado, feche-o e use **Desmontar**, depois **Montar**. Desmontar pode apagar o registro do DLC, e Montar pode reinstalá-lo.
3. Veja as linhas `status unavailable 0x80a30003`, `[dlc-verify] ... READY`, `[offload] ... NATIVE/USB MATCH` e principalmente `[offload] DONE ... logical_bytes=1048576 ... native_stub=0`. Se não aparecer `DONE`, **não assuma** que houve economia no SSD.
4. Confirme dentro do jogo se Ciclope e O Coisa continuam selecionáveis e abra novamente após uma segunda montagem.
5. Envie o log completo. Se houver `TIMEOUT`, `offload deferred` ou erros de metadados, pare e preserve os arquivos no SSD. Não apague `ac.pkg` ou o journal manualmente.

O arquivo físico subjacente fica em `/mnt/ext1/user/addcont/CUSA54308/0833301153272321/ac.pkg` ou `/user/addcont/CUSA54308/0833301153272321/ac.pkg`. **Quando a montagem `nullfs` estiver ativa, o gerenciador pode mostrar o tamanho do USB (1 MiB), mesmo que o stub físico subjacente tenha 0 bytes.** Compare os logs e o espaço livre do SSD antes/depois, não só o tamanho exibido no caminho montado.

O journal é mantido em `/data/ps4mount/offload_<CONTENT_ID>.txt` para recuperação após reiniciar. Desconectar o USB impede o DLC de funcionar até remontá-lo; uma desinstalação pelo PS5 pode remover o registro do DLC.

## O que NÃO mudou

- Apenas dois botões: **Montar** e **Desmontar**.
- Sem `AppInstallAll` global, sem alterações manuais no SQLite e sem os cinco experimentos M1–M5.
- Mantém a preparação do jogo/patch, a rotina `nullfs`, o método sequencial experimental RE7 e a desinstalação direcionada do título.
- Os testes automatizados e a verificação de sintaxe rodam em host; **ELF não foi compilado com SDK PS5 nem testado em console nesta versão**.
