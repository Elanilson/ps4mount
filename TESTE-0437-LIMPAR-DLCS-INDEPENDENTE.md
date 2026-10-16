# PS4Mount 0.4.37 TEST — Limpar DLCs SEM montar/desmontar

## Escopo exclusivo do botão Limpar DLCs

O botão percorre recursivamente, sem filtro por CUSA/registro/label/tamanho, os diretórios que já existem em:

- `/user/addcont`
- `/mnt/ext0/user/addcont` a `/mnt/ext7/user/addcont`

Para cada `ac.pkg` encontrado em filesystem nativo **acessível e sem overlay**:
1. `lstat()` confere o arquivo; `statfs()`/tabela de montagens apenas **consultam** se o caminho é seguro.
2. `unlink(path)` remove o `ac.pkg` antigo (inclusive se tiver 0 bytes ou for um symlink).
3. `open(path, O_CREAT|O_EXCL|O_NOFOLLOW, ...)` cria um **arquivo regular vazio**.
4. `fsync` e `lstat` confirmam **0 bytes**; somente após isso registra `RECREATED_0B_VERIFIED`.

O método não consulta bancos, não inventaria pacotes USB, não percorre a biblioteca USB, não executa `mount`, `nmount` ou `unmount`, não cria marcador e não faz chamadas de recuperação/registro/montagem.

## Limitação obrigatória

**Não é possível alterar por esse caminho o arquivo nativo que está oculto sob uma montagem `nullfs` SEM desmontar a montagem.** O método **não desmonta**, portanto **não mexe** nessa pasta: registra `SKIPPED_MOUNTED_OR_UNSAFE_DIR` e incrementa `FAILURES`. Isso também protege os pacotes reais no USB. Não há mensagem enganosa de sucesso global se houve falhas.

O PS4Mount mantém suas outras funções de montar/desmontar, mas o botão Limpar DLCs não as chama; uma ação manual ou a restauração automática normal do programa pode montar jogos mais tarde, independentemente desse botão. A antiga alteração global de marcador feita pela limpeza 0.4.36 foi removida; um arquivo marcador da versão anterior não é mais usado.

## Logs

`[dlc-clean] RECREATED_0B_VERIFIED <path> old_bytes=<N> new_bytes=0 NO_MOUNT_NO_UNMOUNT`

`[dlc-clean] SKIPPED_MOUNTED_OR_UNSAFE_DIR <path> (NO_UNMOUNT)`

`[dlc-clean] DONE roots=<N>/<N> ac_pkg_found=<N> RECREATED_0B=<N> FAILURES=<N> NO_MOUNT_NO_UNMOUNT`

`FAILURES=0` afirma que os arquivos nativos **encontrados e acessíveis** foram recriados sem erros. Não afirma que arquivos ocultos sob montagem foram acessados.

## Validação disponível

- `bash test/run.sh`: passou.
- Teste host `test/native_clean_0435_runtime.py`, atualizado para o comportamento 0.4.37: 4 pacotes recriados com 0 bytes, 2 diretórios montados/externos pulados, nenhum pacote USB alterado, `mount_calls=0`, `unmount_calls=0`.
- `cc -fsyntax-only` com headers simulados: passou; aviso já existente em outro fluxo.
- Não foi compilado com o PS5 Payload SDK nem executado em PS5 real.
