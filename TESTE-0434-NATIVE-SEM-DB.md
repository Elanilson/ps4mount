# PS4Mount 0.4.34 — experimental / native-only changes

**Base:** v0.4.33. **Validação:** `bash test/run.sh`, host C syntax and JavaScript parser passed. **Não compilado com PS5 Payload SDK nem executado num PS5**.

## Comportamento pedido

- **Limpar DLCs:** percorre `/user/addcont` e `/mnt/ext0..7/user/addcont` sem consultar o banco de dados e sem consultar a biblioteca do USB. Trunca para 0 bytes os `ac.pkg` nativos. Quando uma pasta estiver coberta por um overlay do **próprio** PS4Mount, tenta desmontá-lo temporariamente, acessar o arquivo nativo e restaurar o overlay. Não escreve no HD USB.
- **Apontamento individual:** basta haver um DLC PKG legível dentro de `<USB>/ps4/<CUSA>/addcont/.../ac.pkg` para tentar criar a pasta nativa (quando não existe) e montar o redirecionamento. Não exige entrada em `addcont.db`, não compara conteúdo nem tamanhos. As entradas no banco não são criadas por essa operação.
- **Montar jogo:** aceita um CUSA registrado mesmo quando seu `app.pkg`/`patch.pkg` existe como instalação nativa no SSD do PS5 ou em expansão. Abre os arquivos nativos **antes** do nullfs, monta o conteúdo USB e, só depois de confirmar a montagem, usa os descritores nativos para `ftruncate(..., 0)`. Se houver DLCs USB, enfileira **Apontamento**, não uma reinstalação nativa.
- **Desmontar jogo:** remove os overlays `app`/`patch`, preservando registro e DLCs. O jogo nativo que foi zerado continua com arquivos de 0 bytes quando desmontado, até que seja novamente montado a partir do USB ou reinstalado.

## Invariantes para preservar o USB

Os três fluxos acima não chamam `unlink`, `rename`, `ftruncate` ou `open(... O_WRONLY)` sobre um caminho do USB. Toda escrita destrutiva de conteúdo é feita por descritores abertos em `/user` ou `/mnt/extN` e checados com `fstatfs` para excluir `nullfs` e montagens USB. Se houver um overlay desconhecido ou ocupado, a tentativa é registrada como falha em vez de truncar um caminho que possa atingir o USB. Isso é um limite **físico de segurança para a mídia**, não uma exigência de registro/entitlement.

**Operação destrutiva:** limpar DLCs pode apagar permanentemente cópias nativas sem versão correspondente no USB; montar um jogo nativo também elimina seu `app.pkg`/`patch.pkg` nativo em favor do USB. `Desmontar` não restaura automaticamente os bytes. Sem USB/montagem, a instalação pode não abrir. Faça cópia de segurança antes de usar os botões.

## Ponto de atenção ao testar

1. Primeiramente teste `Montar` com um único jogo nativamente instalado e cuja versão base esteja também no USB. Veja o log por `[native-game] NATIVE_0B` e `[manual][mount]`.
2. Teste `Desmontar` e `Montar` de volta; a confirmação é pela abertura real do jogo no PS5, não apenas pelo status da interface.
3. Em uma DLC que não estava registrada, teste `Apontamento`; procure `[dlc-point] ... CREATED native empty ac.pkg` e `DONE`. Isso **não registra** uma licença/entitlement no sistema.
4. Por último, `Limpar DLCs` é global e destrutivo: verifique no log `[dlc-clean] ZEROED_NATIVE`, `ALREADY_0B`, `SKIP` e `TRUNCATE_FAILED`. A existência de falhas impede declarar que todos foram zerados.

Estas alterações não modificam o comportamento separado de **Buscar jogos no USB**, que ainda pode reorganizar arquivos na própria unidade USB quando acionado.
