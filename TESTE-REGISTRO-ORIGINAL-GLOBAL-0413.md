# PS4Mount 0.4.13 — teste do registro original GLOBAL

## O que mudou

O botao **Registrar DLCs (GLOBAL)** chama `sceAppInstUtilAppInstallAll(NULL)` exatamente como o projeto upstream/fork, apos preparar no USB todos os DLCs do CUSA selecionado. Esta chamada **nao tem parametro de CUSA** e pode processar qualquer outro jogo/DLC preparado nos diretorios internos, alem do selecionado. Nenhum filtro baseado em registro previo do jogo/DLC, `.removed` ou `addcont.db` impede a tentativa. O que continua obrigatorio: fonte USB acessivel, PKGs validos e preparacao/montagem segura; esses requisitos nao limitam a varredura interna do instalador global.

O botao **Montar** NAO chama AppInstallAll automaticamente. O uso global precisa ser acionado explicitamente e confirmado no painel. O programa conserva metadados e links externos; nao usa InstallByPackage e nao transfere os arquivos via propria rotina. **O servico nativo do PS5 decide o que faz ao processar os links; nao ha garantia de que nao escreva dados internamente.**

## Teste

1. Faça um backup dos dados do console e mantenha o USB conectado.
2. Feche o jogo e outros apps.
3. Monte God of War Ragnarok CUSA34384.
4. Clique em Registrar DLCs (GLOBAL), leia o aviso e confirme.
5. Nao desligue o PS5, reinicie o ELF ou remova o USB durante a chamada global.
6. O painel mantem status de processamento sem percentual (a API nao fornece progresso).
7. Ao finalizar, procure por `[dlc-original]`, `BEFORE`, `AFTER`, `NEW`, `MISSING` e `rc` no log.
8. Use Diagnosticar DB para comparar com Resident Evil 7.

**Risco: ALTO.** Uma versao antiga chamando AppInstallAll causou instabilidade grave. O metodo nao e por jogo; sem filtros de registro ele pode atuar em outros titulos preparados no console. O teste pode travar e a chamada pode ficar pendente indefinidamente. Somente os registros por CUSA escolhido serao comparados, nao todos os efeitos laterais.

## Validacao

Testes locais e compilacao de sintaxe C/JS nao confirmam a funcionalidade no PS5, que exige um console real. Nunca edite os bancos diretamente para simular sucesso.
