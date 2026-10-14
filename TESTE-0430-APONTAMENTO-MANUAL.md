# PS4Mount v0.4.30 — TESTE de apontamento manual (experimental)

## Mudança de fluxo

- **Montar**: continua registrando DLCs ausentes, e reutilizando DLCs já apontados. Se encontrar um DLC registrado com `ac.pkg` nativo presente, **não remove o arquivo**. Não chama instalador se já registrado.
- **Reinstalar DLCs**: solicita instalação nativa a partir dos PKGs no USB, com registro em `addcont.db`. **Não faz offload nem coloca 0 bytes**; a cópia nativa fica no SSD. Em firmwares que retornam `0x80a30003`, executa apenas verificações curtas de registro/arquivo/metadados **para coordenar instalações sequenciais**, sem comparação byte a byte, e pode sinalizar estado incerto se não confirmar.
- **Apontamento**: ação separada. **Não instala**. Somente para DLC registrado, jogo fechado, USB conectado e usuário que já conferiu a conclusão da instalação pelo PS5. Reutiliza journal existente ou redireciona PKG físico nativo para USB. Faz verificações baratas de identidade, tipo de arquivo, tamanho e ausência de camadas desconhecidas. **Não faz espera de estabilidade nem comparação byte a byte**. Preserva o original no USB e cria stub físico de 0 bytes apenas após montar e validar `nullfs`, removendo a cópia nativa retida depois.
- **Desmontar**: inalterado; desmonta somente app e patch; DLCs e redirecionamentos ficam montados. Não remove título da biblioteca.

## ⚠️ Risco da versão experimental

**O registro no `addcont.db` por si só não prova que a instalação terminou ou que o arquivo nativo está íntegro.** A notificação de conclusão do PS5 não é consumida diretamente pelo PS4Mount. É RESPONSABILIDADE DO USUÁRIO esperar todas as notificações de DLC concluído ANTES de clicar em **Apontamento**; não execute enquanto o PS5 estiver copiando/instalando. Os arquivos precisam permanecer acessíveis no USB. Um arquivo truncado cujo tamanho coincida com o original, ou corrupção de igual tamanho, não será detectado pela versão sem comparação.

**O botão Apontamento ainda verifica existência/tamanho/Content ID** para não mover arquivos incorretos e recusa estado desconhecido. Se algum DLC estiver não registrado, incompleto, tiver camada estrangeira ou não encontrar o PKG nativo, não o apaga; confira o log. A ação aplica DLCs um a um; um DLC pode concluir mesmo que outro falhe. Teste primeiro com um DLC pequeno e backup.

## Teste sugerido

1. Salve a v0.4.28, última versão validada no seu console, e mantenha uma cópia da v0.4.29 para referência. Não substitua seu ELF funcional até testar.
2. Feche o jogo. Clique **Reinstalar DLCs** e aguarde todas as instalações nativas terminarem na tela de notificações do PS5.
3. Confirme que `/mnt/ext1/user/addcont/<CUSA>/<label>/ac.pkg` contém os bytes originais (não 0 bytes).
4. Clique em **Apontamento**, confirme o aviso de risco.
5. No log, busque `[dlc-point]`, `full byte comparison SKIPPED`, `[offload] DONE` e `native_stub=0`.
6. Desmonte/monte o jogo e confirme que o DLC funciona dentro do jogo. A próxima montagem deve registrar `[offload] ACTIVE` sem nova instalação.
7. Em caso de erro ou status incerto, não apague o PKG nativo à mão. Preserve o log e o journal para diagnóstico.

## Limites da validação

Testes unitários e de contrato no host, teste do painel em Node.js e sintaxe C no compilador host foram aprovados. **Não houve compilação com o SDK PS5 nem teste no console.**
