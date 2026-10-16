# PS4Mount v0.4.31 — Apontamento global de DLCs (TESTE)

Base: v0.4.30-manual-pointing-TEST. Esta versão adiciona somente a execução em lote do **Apontamento** existente, sem alterar os algoritmos de instalação nativa, redirecionamento `nullfs`, journal, montagem ou desmontagem.

## Como usar

1. Espere a instalação **nativa** dos DLCs terminar no PS5 (notificação concluída) e feche todos os jogos.
2. Abra o painel `http://IP-DO-PS5:8770`.
3. Ao lado de **Buscar jogos no USB**, clique em **Apontar todos os DLCs**.
4. Leia a confirmação e confirme somente se as instalações terminaram. O processo usa a biblioteca USB organizada (`ps4/CUSAxxxxx`), um jogo por vez.
5. Veja o progresso geral no topo. No fim, consulte as mensagens `[dlc-point-all]` e `[dlc-point]` no log.

## O que o botão faz

- Não chama instalador nativo nem `sceAppInstUtilAppInstallAll`.
- Reutiliza o fluxo individual **Apontamento** da v0.4.30.
- Só aplica offload ao DLC com registro em `addcont.db` e Content ID compatível com a pasta do jogo; DLC sem registro/CUSA diferente é pulado.
- Se um jogo estiver aberto, sem registro base, no armazenamento interno ou com USB indisponível, pula esse jogo e passa ao próximo.
- Um DLC já apontado com journal e overlay válidos é reconfirmado/reutilizado, sem nova exclusão.
- Em erros de redirecionamento do DLC, a rotina original tenta preservar/restaurar a cópia nativa quando possível e segue para o próximo.
- O status geral mostra jogos visitados, DLCs apontados e DLCs que não puderam ser apontados, com contagem separada de itens sem registro/CUSA diferente.
- Não compara bytes nem espera estabilidade: **essa é a condição experimental já existente no apontamento manual da v0.4.30**.

## Logs esperados

```
[dlc-point-all] START games=N...
[dlc-point-all] TITLE 1/N: CUSAxxxxx
[dlc-point] ... DONE
[dlc-point-all] SKIP CUSAxxxxx: ...
[dlc-point-all] DONE games=N/N ok_dlcs=X not_pointed_dlcs=Y skipped_unregistered_or_mismatched_dlcs=Z skipped_games=W
```

## O que NÃO resolve

Este botão **não corrige falta de registro de DLCs**. No log 9(3), God of War Ragnarök `CUSA34384` tinha 3 DLCs com DB=0 e instalação de estado incerto; esses DLCs precisam ser registrados nativamente antes de serem apontados. No mesmo log, algumas pastas de jogos têm DLCs com CUSA distinto (por exemplo CUSA00243 contendo Content IDs CUSA00242). O lote recusa esse tipo de inconsistência em vez de apagar arquivos.

## Precauções

- Guarde a v0.4.30 para reversão.
- Comece com um HD de testes ou com DLCs pequenos, pois o botão percorre toda a biblioteca USB organizada.
- **Não desconecte o HD, desligue o PS5 nem abra os jogos durante o processamento.**
- A confirmação humana da conclusão nativa não substitui uma validação independente. Como a comparação foi retirada a pedido do usuário na v0.4.30, existe risco de um DLC aparentemente registrado ainda estar sendo gravado pelo instalador. O lote mantém os mesmos riscos do apontamento individual.
- O resultado de sintaxe é local, com substitutos para tipos do SDK. O ELF não foi compilado no SDK do PS5 nem testado em console.
