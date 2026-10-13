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

## v0.4.3 — somente três ajustes solicitados

1. **Montar/Desmontar no painel:** botões renomeados; painel dá prioridade ao
   estado real de montagem ao exibir “Montado”, mesmo que haja um marcador de
   desativação anterior. “Não montado pelo PS4Mount” não afirma que o jogo é
   impossível de executar por outro mecanismo. Desmontar mantém o registro.
2. **Pré-verificação USB:** sem um USB/ext realmente montado, a recuperação é
   omitida imediatamente, sem validar individualmente os 46/150 títulos e sem
   notificações de falha em massa. O usuário deve conectar o USB e executar o
   ELF novamente.
3. **Reparo direcionado ao clicar em Montar:** se o jogo já está em `app.db`,
   está inconsistente e tem marcador `.src` do próprio PS4Mount apontando para
   **exatamente a mesma pasta**, tenta reconstruir arquivos auxiliares e
   montagens só daquele CUSA. Não desinstala/reinstala o jogo, não escreve no
   `app.db`, não libera camadas desconhecidas. Um arquivo `.repair` impede
   repetição cega caso uma tentativa seja interrompida. Se a origem do
   registro não estiver comprovada, continua falhando em vez de sobrescrever
   arquivos de outros aplicativos.

**Risco:** regravar metadados internos de um título ainda pode apresentar
problemas no PS5. Teste primeiro em um CUSA, com backup restaurável. Testes no
host não substituem validação em console físico.

Verificações extras no reparo: não prossegue se existir guarda `.attempt` de
registro interrompido, se o digest base anterior divergir do pacote atual,
ou se houver montagem de patch desconhecida em título sem atualização.
