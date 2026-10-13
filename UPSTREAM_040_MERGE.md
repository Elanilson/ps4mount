# PS4Mount 0.5.13 — comparação com o original 0.4.0

**Base:** nossa 0.5.12. **Referência:** ZIP do projeto original `gasb07/ps4mount`, cuja fonte declara `PS4Mount 0.4.0`. A integração foi seletiva, não uma substituição integral do daemon. A fonte original de `main.c` tinha SHA-256 `5e93de17d375770ee5aff92f8fd08187c95826faf48c305081a2b9ffb7056421`.

## Incorporado e funcional no código

- **Menu Configurações:** persistência em `/data/ps4mount/config-safe-0.5.13.txt`, validação de pastas, grupos de 1 a 5 **para planejamento**, opção de indexação dos DLCs. Pastas canônicas `PS4` e `ps4` são sempre examinadas; a opção `/` examina toda a unidade como antes. Nada é registrado por abrir a página ou salvar as preferências.
- **Reconhecimento de DLCs (`CATEGORY=ac`) somente leitura:** lista por ID/conteúdo, classificação aproximada *data* (0x1b), *unlock* (0x1c), ou desconhecido, relacionamento inicial por mesmo CUSA, relacionamento alternativo por `SERVICE_ID_ADDCONT_ADD_1...7` no `param.sfo` do jogo/patch como no original, contagem dos DLCs que aguardam pelo jogo e amostras no painel. São metadados lidos, **não validação completa de PKG nem instalação**.
- **Prévia de grupos:** `group_size_preview`/`groups_estimate` na consulta de fila; nada é enviado à API de registro.
- **Diagnóstico de montagem inacessível:** auditoria consulta o registro de montagens do kernel (`getfsstat`) e compara com `statfs`, seguindo a técnica de detecção da 0.4.0. **Não força desmontagem nem interfere em montagens estrangeiras**.
- **Correção de sobreposição de pastas:** o mesmo patch ou DLC não pode ser duplicado por duas raízes de pesquisa sobrepostas.
- **Testes:** preservada a suíte da nossa 0.5.12; acrescentados testes de caminhos/URL, painel sem escrita, DLC com jogo e DLC órfão, pastas sobrepostas.

## Não incorporado nesta versão (decisão de segurança)

- **Registro automático em grupos do original:** invoca `sceAppInstUtilAppInstallAll(NULL)` uma vez por grupo. Essa API em massa já provocou kernel panic e reconstrução de banco de dados no PS5 usado para os testes; portanto não é habilitada. O campo de tamanho de grupo é somente para planejar, não para executar.
- **Instalação/movimentação/montagem de DLCs:** exigiria acrescentar `addcont.db`, licença, montagem por add-on, rollback próprio, limitações de 64 dispositivos e verificações adicionais; nenhuma dessas operações é ativada aqui.
- **Recuperação automática de montagem obsoleta:** esta versão **detecta** na auditoria mas não desmonta; não sabemos ainda se a camada do CUSA06886 pertence a outra instância, PS5 ou dispositivo desconectado.
- **Correções de repouso e remount da 0.4.0:** a arquitetura local de mounts RO/RW é diferente, então transplantar a lógica sem teste no console pode deixar camadas inesperadas. Estão pendentes.
- **Registro contínuo dos 125 títulos:** continua bloqueado. O único teste de registro manual ainda autorizado pela nossa 0.5.12 permanece CUSA06886 com os mesmos tokens/precondições.

## Como validar a versão no PS5

1. Tenha backup e preserve o ELF 0.5.11/0.5.12 como fallback. Compile pelo GitHub Actions (este ZIP contém fontes; não é um ELF).
2. Inicie o novo ELF e aguarde `sync FINISHED 147/147` (a quantidade real depende da biblioteca). Não clique em registro.
3. No painel, abra **DLCs (leitura)**; confira os totais e se os 758 pacotes antes ignorados são indexados como metadados. A contagem pode variar por arquivos não suportados ou limites de profundidade.
4. Abra **Configurações**, teste leitura/salvamento, e então **Sincronizar (somente leitura)**. A opção `/` mantém a busca recursiva original.
5. Abra **Auditar montagem (leitura)** para CUSA06886; observe `stale_app_layers` e `stale_patch_layers`; **não remova montagens estrangeiras**.
6. Envie log para revisão. Ainda não validamos esta versão no hardware nem a integração completa da 0.4.0.

## Execução dos testes locais

```
bash tests/run_host_tests.sh
```

Os testes independentes do original também foram executados separadamente (`test/run.sh` com substituto local de `xxd` porque `xxd` não está instalado no ambiente): **125 verificações, 0 falhas**. Isso valida a fonte original no host, não as funcionalidades não transplantadas no console.
