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

## Histórico: v0.4.3 — três ajustes (reparo antigo substituído na 0.4.4)

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


## PS4Mount 0.4.4 — Montagem manual e progresso da busca

Apenas **duas alterações solicitadas** em relação à 0.4.3:

1. **Montar sem exigência do marcador de propriedade antigo:** ao clicar em **Montar** para um CUSA organizado no USB, se esse título já estiver registrado no banco do PS5, a aplicação usa o registro existente. Se os links/metadados auxiliares do próprio PS4Mount estiverem ausentes ou desatualizados, recria-os a partir do PKG encontrado, sem chamar o instalador do PS5 novamente. Isso permite remontar títulos desmontados e títulos anteriormente gerenciados pela 0.4.1. **Desmontar** mantém o registro no sistema. Para títulos ainda não registrados, continua sendo utilizada a API individual anterior, sem `AppInstallAll` global.

2. **Progresso visível durante a busca USB:** ao clicar em **Buscar jogos no USB**, a barra fica animada (total desconhecido) e o painel atualiza a contagem de pastas percorridas, PKGs examinados, arquivos organizados e a pasta atual. A interface consulta `/api/progress` durante a varredura. A busca continua exclusivamente manual, sem registrar jogos nesta etapa.

**Limites preservados:** não desmonta à força camadas ocupadas por outro aplicativo, não altera diretamente os bancos de dados do console e não instala títulos internos sobre PKGs do USB. A recriação de metadados do jogo selecionado pode sobrescrever arquivos auxiliares antigos do próprio título; faça backup restaurável antes de testar. Não testado no PS5 real; testes de compilação de sintaxe local não equivalem à build com SDK PS5 nem à execução no console.
