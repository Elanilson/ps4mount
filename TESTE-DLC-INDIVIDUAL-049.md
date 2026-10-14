# PS4Mount 0.4.9 — Instalação individual de DLCs (teste PS5)

Base: **PS4Mount 0.4.7**, não a v0.4.8 experimental global.

## Implementado

- Ação explícita `Instalar DLCs` no card de um jogo **montado** com DLCs.
- API individual `sceAppInstUtilInstallByPackage()` para cada `ac.pkg` do jogo selecionado.
- Ação HTTP: `POST /api/install-dlcs?title_id=CUSA03842` (não aceita GET para instalação).
- Consulta inicial de `addcont.db`; conteúdos já registrados são ignorados.
- Instalações solicitadas sequencialmente; `sceAppInstUtilGetInstallStatus()` monitora a tarefa corrente.
- Depois do estado final, nova verificação somente leitura do `addcont.db`.
- Liberação apenas das montagens externas de DLC pertencentes ao PS4Mount para o instalador nativo não encontrar o diretório encoberto; ao final, tentativa de restabelecer as montagens que não possuem instalação interna.
- Base e atualização preservados durante o teste. Nenhum `AppInstallAll`, nenhuma alteração direta nos DBs do sistema e nenhum `AppUnInstallAddcont`.
- Barra de progresso do DLC atual dentro do card (quando a API retorna tamanho/progresso); caso contrário, indicador indeterminado.
- Mensagens de erro e códigos de retorno no log: `[dlc-install] ...`.

## Como testar com Resident Evil 7

1. Compile a versão com o PS5 Payload SDK usado pelo projeto (`bash build.sh` ou GitHub Actions).
2. Inicie a versão 0.4.9 e abra `http://<IP_PS5>:8770`.
3. Feche o jogo. Encontre **Resident Evil 7 — CUSA03842** e clique **Montar** se necessário.
4. No card, clique **Instalar DLCs** e confirme que compreende que o instalador nativo pode copiar conteúdo para o armazenamento interno.
5. O PS4Mount processará **os DLCs ainda não registrados desse jogo**, um por vez. Não precisa criar arquivo de autorização nem compilar com flag experimental.
6. Inspecione `/data/ps4mount/log.txt`: procure `[dlc-install]` e `[manual][dlc-db]`.
7. Se o registro passar a constar no banco, confirme também a disponibilidade no menu do jogo. Um registro no DB isoladamente não prova que o DLC é funcional.

## Limitações reais

- Esta é uma **instalação nativa individual de PKG**, diferente da montagem `nullfs`. O PS5 pode copiar os conteúdos para o armazenamento interno.
- **Desmontar** o jogo no PS4Mount **não desinstala** DLCs gravados pelo instalador nativo; isso seria uma ação diferente e potencialmente destrutiva.
- A tarefa pode falhar por falta de privilégios, incompatibilidade da API, PKG inválido, espaço insuficiente ou outras condições. O resultado no PS5 não foi validado aqui.
- Se o status de uma instalação ficar inconclusivo, a fila para antes de iniciar outro pacote. As demais ações normais do PS4Mount continuam independentes.
- Diretórios de DLC compartilhados com outro jogo ativo são preservados. O teste não desmonta montagens de outro aplicativo.
- Não faz registro global, não faz escrita manual em `addcont.db` e não presume que `sceAppInstUtilInstallByPackage()` cadastre DLCs externos sem copiar dados.

## Fonte técnica

Estruturas e sequência das chamadas documentadas em `onionHEN-main/docs/pkg-writeup.md`. Também confrontadas com o uso de `sceAppInstUtilInstallByPackage` e `sceAppInstUtilGetInstallStatus` na implementação de instaladores PS5.

## Validação disponível

- Testes existentes de montagem, UI e leitura de `addcont.db`.
- Novo teste do layout ABI nativo (tamanhos e offsets das estruturas usadas para comunicação com a API do PS5).
- Testes de contrato das rotas e da fila individual.
- Sintaxe C verificada com os cabeçalhos simulados do PS5; **compilação ELF com SDK e teste no console não realizados neste ambiente**.
