# PS4Mount 0.4.20 — Montar/Desmontar, DLC registrado e descarregado no USB

## O que muda
- Botões visíveis por jogo: **Montar** e **Desmontar** (jogo fechado).
- Montar: registra o jogo/patch caso necessário; para cada DLC encontrado no USB consulta o `addcont.db`, instala nativamente apenas se ainda não estiver registrado; compara PKG nativo/USB byte a byte; guarda um journal; renomeia temporariamente o ac.pkg original do SSD para uma pasta irmã, cria ac.pkg vazio (0 bytes), ativa nullfs do USB, valida link+mount e **só então** exclui o arquivo temporário grande do SSD. Cada DLC é processado isoladamente.
- Desmontar: remove as camadas controladas e chama `sceAppInstUtilAppUnInstall` para o **título selecionado** em thread dedicada. Pode alterar os registros de DLC desse título; ao Montar novamente serão reavaliados.
- Ao reiniciar o programa, journals de offload são lidos e as camadas são remontadas, se o USB estiver presente. `AppInstallAll` global **não** é chamado.

## IMPORTANTÍSSIMO
- Esta versão **não foi testada em um PS5**. Apenas checagem de sintaxe em host e testes de contrato.
- O arquivo pesado do SSD é **apagado de verdade** após as verificações. Não há restauração local de conteúdo apagado: mantenha o PKG original do USB disponível e íntegro.
- A cópia que pode ser removida é SOMENTE o ac.pkg da instalação nativa; a entrada de 0 bytes fica escondida por um mount nullfs, e o arquivo do USB é lido pelo sistema em seu lugar.
- Se o USB for desconectado ou a montagem não for restaurada depois do reboot, o DLC poderá não funcionar. O journal fica em `/data/ps4mount/offload_<CONTENT_ID>.txt`. Não apague esses arquivos.
- O serviço `AppUnInstall` pode alterar o registro do DLC na base do PS5. Este comportamento exige teste real; não há garantia de que a licença/entitlement persista.
- A versão original 0.5.0 contém outras alterações além do unmount direcionado; aqui foi incorporada a sua estratégia de desmontar, **não** feito um merge integral de todos os commits.

## Teste sugerido com HD de um jogo e um DLC
1. Salve backup dos bancos de sistema e do código/ELF da v0.4.19.
2. Use um DLC descartável de teste, tenha o PKG de origem intacto no USB e feche o jogo.
3. Clique **Montar**. Log esperado: `[oneclick] stage 1/2`, `[dlc-install] ... status=...`, `[offload] ... NATIVE/USB MATCH`, `[offload] DONE ... SSD bytes released`.
4. Leia `addcont.db`/log para assegurar registro e verifique o DLC dentro do jogo.
5. Feche o jogo, clique **Desmontar**, confira que ele saiu da biblioteca, depois clique **Montar** novamente. Envie log completo.

## Diagnóstico
`[offload] DONE` só prova o redirecionamento e exclusão da cópia; **não** garante desbloqueio de DLC protegido pela licença. Caso `addcont.db` não registre o DLC, o PKG nativo **não** é descartado. Se a comparação ou montagem falhar, ocorre tentativa de rollback e o journal permanece para recuperação quando necessário.
