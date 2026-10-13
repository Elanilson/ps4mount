# PS4Mount 0.4.7 — pacote base, atualização e DLCs

## Implementado

1. Montagem do jogo base, atualização e todas as pastas externas de DLC em uma única operação de montagem, com rollback somente das novas camadas quando algo falhar.
2. Desmontagem explícita inclui as camadas externas de DLC antes de atualização e base; mantém o registro em `app.db` e preserva os PKGs no USB.
3. Compartilhamento: DLC usado por outro jogo montado não é desmontado para evitar quebrar esse outro jogo; a exceção é registrada no log.
4. Não desmonta instalações internas reais ou camadas que não pertencem ao PS4Mount.
5. Identificação de DLCs ao desmontar sem reabrir PKGs, usando diretórios e o cache ativo.
6. Suporte a 64 IDs de pasta de DLC (antes, apenas oito).
7. Verificação somente leitura dos IDs de conteúdo no `addcont.db` ao montar; `unknown` quando não é possível conhecer o registro ou o formato do banco.
8. Progresso por DLC durante a preparação manual, sem porcentagens falsas.

## Não implementado / limite técnico

- **Registro individual automático de DLCs que não existem em `addcont.db`:** não foi possível confirmar uma API segura para registrar conteúdos adicionais externamente sem a chamada global (`AppInstallAll`) ou sem realizar uma instalação convencional para o armazenamento interno. O programa **não** irá fabricar registros de banco nem dizer que os DLCs foram aplicados quando somente montou os arquivos.
- A leitura de `addcont.db` detecta registros, não garante entitlement nem que o jogo aplicará o conteúdo.
- A rotina automática de abertura da tela de exclusão do sistema continua usando o comportamento anterior, separado do botão **Desmontar**.

## Verificação no PS5

- Testar primeiro um jogo com 1–2 DLCs: **Montar → iniciar jogo → Desmontar → Montar novamente**.
- Conferir o log `[bundle][mount]`, `[bundle][unmount]` e `[manual][dlc-db]`.
- Confirmar se o DLC aparece dentro do jogo; isso é mais importante que apenas existir uma montagem no sistema.
- Se `database_registered=0` e `missing>0`, enviar o log. Não repetir instalação global às cegas.

## Compilação e testes

- `PATH=/caminho/para/xxd:$PATH bash test/run.sh` — testes locais.
- `./build.sh` — requer o SDK de payload do PS5 em `PS5_PAYLOAD_SDK`.
