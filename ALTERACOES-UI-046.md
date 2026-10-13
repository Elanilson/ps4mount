# PS4Mount 0.4.6 — interface de operação por jogo

Base: PS4Mount 0.4.5 (`manual-usb-stability`), construída a partir do código-fonte correto da 0.4.4.

- **Montar** inicia sem confirmação. **Desmontar** preserva a confirmação de segurança.
- Barra indeterminada e etapa atual **dentro do card do jogo**, visíveis apenas enquanto há solicitação ou processamento.
- O POST de montagem só coloca o jogo na fila: o card continua mostrando atividade até `/api/progress` informar `complete` ou `error` para o CUSA.
- Progresso também se reconecta ao jogo se o painel for atualizado enquanto o backend está trabalhando; uma conclusão rápida antes do primeiro polling não deixa indicador permanente.
- O painel principal mostra progresso somente durante varredura USB ou restauração. `Operação concluída` deixa de ocupar a página; sucessos de Montar/Desmontar não geram toast no navegador. **Erros continuam visíveis**.
- Lista de jogos em ordem alfabética por nome (`pt-BR`, desconsiderando acentos/caixa, com números naturais); desempate pelo CUSA.
- O `current` do `/api/progress` informa o CUSA ainda na etapa `queued`.

**Sem alterações na instalação ou na lógica de DLCs**. Os logs/avisos do sistema PS5 permanecem como antes.

Testes locais: `bash test/run.sh` (testes originais + verificações de contrato + teste comportamental Node.js quando instalado). O SDK PS5 e um console são necessários para validar o ELF final.
