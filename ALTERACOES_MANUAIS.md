# PS4Mount 0.4.2 experimental — alterações em relação à 0.4.1 recursiva

- **Sem busca/registro automático:** o ELF só restaura montagens anteriormente ativas.
- **Recuperação:** lê os arquivos `.src` existentes (compatibilidade com 0.4.1),
  considera `.active` e respeita `.disabled`. Valida metadados, presença do jogo e
  `app.db` antes de remontar. Sem nova instalação ou busca recursiva.
- **Painel web:** Buscar jogos, Ativar jogo, Desativar jogo e Encerrar ELF;
  `/api/progress` mostra estado, andamento, falhas e CUSA atual. Uma notificação no
  começo e outra no fim da recuperação, mais avisos essenciais em buscas/ativações.
- **Ativação individual:** se o CUSA já está registrado, somente remonta;
  se não está, tenta `AppInstallTitleDir` (NID `Wudg3Xe3heE`), sem `AppInstallAll`.
- **DLCs:** a organização original está preservada; o preparo dos arquivos de DLC
  ocorre em grupos configuráveis. Registro *efetivo* de DLC por API individual
  ainda não foi comprovado no PS5, portanto não se afirma que elas foram instaladas.
- **Desativar:** remove `.active` e cria `.disabled`, preserva o registro no PS5;
  não desmonta camadas desconhecidas nem força desmontagem de DLC compartilhada.
- **Proteção:** um arquivo `.attempt` bloqueia repetição de um registro que falhou
  após iniciar a preparação interna. Nunca apagar esse arquivo cegamente.
- **Atenção:** o atalho do painel na Mídia não é reinstalado automaticamente;
  acessar `http://IP_DO_PS5:8770`. Buscar jogos pode **mover PKGs no próprio USB**.

## Limites da validação

- 125 testes originais e busca recursiva: passaram no host.
- 17 verificações adicionais do fluxo manual: passaram.
- Sintaxe C (`-fsyntax-only`, stub de cabeçalhos PS5) e JavaScript (Node): passou.
- **Sem compilação real com o SDK do PS5 e sem execução física no PS5.**
- Fazer backup e testar ativação de **um jogo por vez**. Consultar
  `/data/ps4mount/log.txt` diante de qualquer estado incerto.
