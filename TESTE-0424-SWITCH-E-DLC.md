# PS4Mount 0.4.24 — desmontagem opcional da biblioteca e DLC persistente

Base: código-fonte **0.4.21** enviado pelo usuário.

- Switch **Remover da biblioteca ao desmontar**: **desligado por padrão**, persistido em `/data/ps4mount/config.txt` como `remove_from_library=0`.
- Desligado: Desmontar libera somente `/user/app/<CUSA>` e `/user/patch/<CUSA>` controlados pelo PS4Mount; não chama `sceAppInstUtilAppUnInstall()`, não libera `/user/addcont/` nem nullfs do offload. O ícone do jogo continuará na biblioteca.
- Ligado: Desmontar usa a ordem de liberação da v0.4.21 e chama a desinstalação direcionada. O PS5 **pode apagar** os registros no `addcont.db`; na próxima Montar, se o DLC estiver ausente, a instalação nativa volta a ocorrer.
- Montar com DLC já registrado e journal validado: verifica USB e remonta diretamente o nullfs do DLC, **sem aguardar o PKG de 0 bytes crescer**.
- Montar com DLC ausente do DB: libera somente as sobreposições necessárias e usa a instalação nativa da 0.4.21, seguida por verificação, comparação de PKGs e offload.
- Não apaga PKGs no USB nem edita o SQLite.

## Teste sugerido

1. Com o switch desligado, feche o MARVEL. Clique em Desmontar: ele continuará na biblioteca.
2. Monte de novo. Confira `[oneclick][offload-reuse]` e `[offload] ACTIVE`; **não** deve ocorrer espera `[dlc-verify]` de 180 segundos nem nova chamada `InstallByPackage`.
3. Desmonte, ative o switch e clique em Desmontar se quiser removê-lo da biblioteca. Confirme o resultado real; se houver erro da API, não trate como sucesso.
4. Monte novamente e veja se o registro foi removido ou preservado pelo PS5.
5. Reinicie o PS4Mount com o jogo desmontado e USB conectado; a recuperação dos DLCs deve ocorrer mesmo sem um jogo base ativo.

> Testes locais não substituem validação real no PS5. Se um journal estiver ausente/corrompido, não tente excluir um `ac.pkg` de 0 bytes sem origem confirmada. Faça backup.
