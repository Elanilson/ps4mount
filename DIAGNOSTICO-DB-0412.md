# PS4Mount v0.4.12 — Diagnóstico somente leitura do addcont.db

No painel, localize **Resident Evil 7 — CUSA03842** e clique **Diagnosticar DB**. Faça o mesmo para **God of War Ragnarök — CUSA34384**. O programa inspeciona `/system_data/priv/mms/addcont.db`, incluindo nomes de tabelas e colunas, e apresenta somente as linhas relacionadas ao CUSA selecionado, com valores de `contentId`, `state`, `status`, `contentType` etc., quando presentes. O jogo não precisa estar aberto nem montado para consultar o banco.

O sistema registra `[db-inspect]` no log do PS4Mount. O botão abre automaticamente a janela **Log** após a consulta. Envie o log após executar os dois diagnósticos.

O diagnóstico usa SQLite `SQLITE_OPEN_READONLY` e `mode=ro&nolock=1`, sem modificações em `addcont.db`, sem cópias de PKG, sem invocar AppInstallAll ou APIs de instalação. Dados binários e valores de conta/licença não são exibidos. A existência de linhas no banco não prova licença válida nem que os dados estejam no USB (RE7 foi instalado pelo instalador nativo no teste anterior).

Importante: o painel de Log mostra os últimos 16 KB; se houver muita atividade, use o arquivo completo `/data/ps4mount/log.txt` para preservar o diagnóstico. Não publique um dump bruto de `addcont.db` ou `app.db`.
