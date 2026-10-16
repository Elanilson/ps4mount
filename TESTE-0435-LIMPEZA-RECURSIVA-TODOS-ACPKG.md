# PS4Mount 0.4.35 experimental — Limpar DLCs: varredura recursiva total

**Base**: v0.4.34. Nenhum outro botão ou fluxo de apontamento/montagem foi modificado nesta versão.

## Limpeza

O botão **Limpar DLCs** percorre todas as árvores de DLC nativas disponíveis:

- `/user/addcont` (SSD interno)
- `/mnt/ext0/user/addcont` até `/mnt/ext7/user/addcont` (SSD de expansão)

Em cada raiz, percorre **todas as subpastas, em qualquer profundidade prática**, independentemente de nome de jogo, CUSA, label, registro, tamanho, versão, presença no banco de dados ou cópia no USB. Qualquer arquivo regular com o nome literal **`ac.pkg`** é truncado para **0 bytes**. As pastas que não contêm `ac.pkg` são percorridas, mas **arquivos que não existem não podem ser zerados**; criar DLCs ausentes continua sendo tarefa distinta do botão **Apontamento**.

O processo **não consulta o banco nem o disco USB**. Para evitar truncar o HD USB acidentalmente, não segue symlinks nem `nullfs`, e rejeita um alvo que na inspeção final por `fstatfs` pertença ao USB/`nullfs`. Quando o overlay for comprovadamente do próprio PS4Mount, tenta desmontá-lo temporariamente e restaurá-lo após varrer o conteúdo nativo. Camadas desconhecidas/ocupadas não são forçadas: são **relatadas como falha**.

## Novos contadores e exemplo de log

```
[dlc-clean] START native_storage_roots=2; recursive any ac.pkg; no DB, CUSA, label or USB check
[dlc-clean] SCAN_ROOT /mnt/ext1/user/addcont
[dlc-clean] ZEROED_NATIVE .../ac.pkg old_bytes=1048576 new_bytes=0 USB_UNTOUCHED
[dlc-clean] ALREADY_0B .../ac.pkg
[dlc-clean] ROOT_DONE /mnt/ext1/user/addcont zeroed=1 already_0B=1 errors=0
[dlc-clean] DONE roots=2/2 ac_pkg_found=2 ZEROED_NOW=1 ALREADY_0B=1 FAILURES=0 USB_UNTOUCHED
```

O painel mostra quantos **`ac.pkg` foram encontrados**, quantos **zerados nesta execução**, quantos **já estavam em 0 B**, e **falhas**. `ac_pkg_found` refere-se aos arquivos efetivamente identificados, não aos que podem estar escondidos atrás de overlays indisponíveis.

## Limites e risco

**Destrutivo para SSD interno e expansão:** zerar `ac.pkg` pode inutilizar DLCs instalados que não tenham outra cópia. Não há desfazer. **O USB não é alterado por esse botão.** Recomenda-se backup e encerrar jogos/instalações antes.

O código-fonte passou nos testes `bash test/run.sh`, incluindo um **teste funcional host** que usa um falso sistema de arquivos com pastas fora do padrão CUSA, profundidade adicional, dois `ac.pkg` preenchidos, um vazio, symlinks e uma visão simulada de USB. A sintaxe C foi validada com Clang e stubs host. **Não foi compilado com o SDK do PS5 nem executado no console.**
