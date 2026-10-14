# v0.4.17 — Teste sequencial de 17 DLCs no USB

Base: v0.4.16. O resultado 0.4.16 mostrou apenas QUEUED e, depois, uma nova inicializacao do processo. Possivel estouro de pilha na thread; causa nao comprovada.

## O que mudou

- Ao clicar em **Testar 17 DLCs no USB**, a thread registra imediatamente `WORKER START`.
- Cada DLC e processado completamente antes do proximo: `VERIFY START` -> `MATCH` -> `STAGING + MOUNT START` -> `ACTIVE N/17`.
- Buffers de comparacao SSD x USB (128 KiB) ficam no heap, nao na pilha. Dados da thread tambem sao alocados dinamicamente; pilha configurada para 512 KiB.
- Se uma etapa falhar, o programa tenta **rollback em ordem inversa** de tudo o que esta execucao montou. Se a tentativa de desmontagem falhar, NAO desconecte o USB; feche o jogo e use Restaurar 17 DLCs no SSD.
- So executa no RE7 (CUSA03842), com jogo fechado, 17 DLCs nativamente registrados e ambos os discos acessiveis.
- Nao usa AppInstallAll, nao instala nada, nao altera addcont.db nem apaga/substitui os PKGs nativos do M.2.

## Teste

1. Conecte o USB original do RE7 e mantenha o SSD M.2. Feche o RE7.
2. Clique **Testar 17 DLCs no USB** uma vez.
3. Consulte o log: `WORKER START`, `STEP 1/17 ... VERIFY START`, `MATCH`, `ACTIVE 1/17`, `STEP 2/17 ...`, ate `ALL ACTIVE 17/17`.
4. Se aparecer `ALL ACTIVE 17/17`, abra o RE7 e verifique os DLCs. **Isso confirma a montagem, nao a leitura efetiva pelo jogo.**
5. Feche o jogo e clique **Restaurar 17 DLCs no SSD**. Confira `RESTORE FINISH restored=17 failed=0`.
6. Se o ELF reiniciar outra vez antes de `WORKER START`, envie o log. Nao execute multiplas vezes enquanto houver sobreposicoes ativas.

## Observacao

Os DLCs ja precisam estar registrados. Esta versao redireciona arquivos no USB; nao registra novos DLCs de outros jogos e nao libera espaco no SSD.
