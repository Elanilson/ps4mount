# PS4Mount 0.4.38 TEST — Limpar DLCs forçado em caminhos visíveis

Base: PS4Mount 0.4.37 enviado pelo usuário. Alteração restrita ao botão **Limpar DLCs**; funções comuns de montar/desmontar e instalação permanecem intactas.

## O que foi alterado

- `/user/addcont` e `/mnt/ext0...ext7/user/addcont` são percorridos recursivamente, sem filtro por CUSA, tamanho ou registro.
- **Não são puladas pastas montadas**: retiradas as checagens `layers_on`, `dead_layer` e `native_clean_safe_fs` que causaram 232 falhas no log 0.4.37.
- Para cada `ac.pkg` visível do tipo arquivo regular ou symlink: `unlink()` seguido de `open(O_CREAT|O_EXCL|O_NOFOLLOW)`, verificando arquivo regular com tamanho 0 bytes no mesmo caminho.
- Não chama `mount`, `nmount` nem `unmount`, não consulta DB e não realiza apontamento.
- Registra `RECREATED_VISIBLE_0B_VERIFIED` e `WARNING_VISIBLE_MOUNT` (diagnóstico, não um bloqueio); falhas reais também entram no resumo.
- Não segue links simbólicos de **diretórios** para evitar percorrer caminhos fora das raízes selecionadas. Links `ac.pkg` são substituídos diretamente, sem seguir o alvo.

## ALERTA IMPORTANTE: NÃO É LIMPEZA FÍSICA GARANTIDA

**Um caminho visível por `nullfs` não aponta necessariamente para o arquivo nativo físico.** `unlink()` naquele caminho altera a pasta de origem da montagem: pode apagar o `ac.pkg` verdadeiro no USB (se for montagem direta do USB) ou substituir apenas o `ac.pkg` no diretório intermediário `HOLD_DIR`. O `ac.pkg` nativo que ficou escondido sob a montagem **pode permanecer com seu tamanho original**. Não existe garantia de liberar espaço do SSD com a montagem sobre o caminho e a proibição de desmontá-la.

O botão exige duas confirmações explícitas sobre esse risco. Faça backup antes. Jogos devem estar fechados. Esta versão **NÃO garante USB intacto** e pode inutilizar as DLCs.

## Saída de log esperada

```text
[main] PS4Mount 0.4.38-force-visible-acpkg-0B-TEST ...
[dlc-clean] START roots=2; FORCED_VISIBLE_PATHS; ... WARNING_USB_MAY_BE_AFFECTED
[dlc-clean] WARNING_VISIBLE_MOUNT ... mount_source=... (may affect USB; underlying native hidden)
[dlc-clean] RECREATED_VISIBLE_0B_VERIFIED ... old_bytes=... new_bytes=0 view=nullfs NO_MOUNT_NO_UNMOUNT
[dlc-clean] DONE roots=2/2 ac_pkg_found=... RECREATED_VISIBLE_0B=... FAILURES=... NO_MOUNT_NO_UNMOUNT
```

Validação efetuada em Linux host: seis arquivos, inclusive subpastas simuladas como `nullfs`, recriados com 0 bytes; zero chamadas `nmount/unmount`. Bateria `bash test/run.sh` passou; compilação final e teste em PS5 não executados.
