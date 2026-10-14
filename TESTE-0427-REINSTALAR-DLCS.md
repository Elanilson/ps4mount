# PS4Mount v0.4.27 — atualização visual + reinstalação manual de DLCs

Base: v0.4.26. O fluxo normal de **Montar** e **Desmontar** foi preservado.

## Alterações

1. **Conclusão no painel:** o cartão é atualizado imediatamente quando `/api/progress` retorna `dlc_complete` ou `dlc_error`, mesmo quando o navegador não viu a fase intermediária `installing_dlcs`. Um erro não é exibido como sucesso; não é necessário reabrir a página para liberar o botão.
2. **Reinstalar DLCs:** novo botão por jogo organizado que tenha DLCs, disponível quando o jogo estiver montado ou inativo no painel. Usa `POST /api/reinstall-dlcs?title_id=CUSAxxxxx` para pedir explicitamente a **instalação nativa de todos os DLCs do jogo**, inclusive se já constarem no `addcont.db`. **Montar não chama esta ação.**

## Caminho manual

- Feche o jogo, conecte o USB e confirme que há espaço livre no SSD suficiente **temporariamente** para a instalação nativa. Para DLCs grandes, é necessária capacidade temporária antes de removê-la.
- Clique **Reinstalar DLCs** e confirme. O painel passa a mostrar progresso e conclui com sucesso ou erro.
- Antes de reinstalar, o programa tenta liberar somente montagens dos DLCs identificadas como próprias do PS4Mount (inclusive uma montagem válida sem journal); montagens desconhecidas bloqueiam a operação.
- Cada DLC é instalado a partir de seu PKG original **no USB** com `sceAppInstUtilInstallByPackage()`. A operação pode sobrescrever o registro existente pelo instalador nativo; não há SQL de escrita manual e não há `AppInstallAll` global.
- Após a instalação, exige o `ac.pkg` nativo completo e estável, compara seus bytes com o USB e valida o redirecionamento `nullfs` **antes** de apagar a cópia nativa e manter o marcador de 0 bytes. A cópia do USB não é apagada.
- Se a verificação falhar, nenhuma exclusão de cópia nativa incompleta deve ser executada. Um estado desconhecido pode necessitar recuperação/manual após o teste.

## Teste MARVEL Cosmic Invasion (CUSA54308)

1. Inicie o PS4Mount e monte o jogo com **Montar**. Com DLC já registrado, o programa **não deve reinstalá-lo automaticamente**.
2. Observe o painel: assim que terminar, o botão deve voltar a ser clicável, mesmo se o trabalho terminar entre duas consultas de progresso; se falhar, aparece uma notificação de erro.
3. Com o jogo fechado, clique em **Reinstalar DLCs**. Acompanhe no log `[dlc-force]`, `[dlc-install]`, `[dlc-verify]`, `[offload]`.
4. Resultado esperado quando a reinstalação funcionar: `[dlc-install] ... force=1`, `NATIVE/USB MATCH` e `[offload] DONE ... native_stub=0`.
5. Abra o jogo e confirme o DLC funcionando. **Não considere apenas `database_registered=1` uma confirmação do conteúdo em uso no USB.**

## Limites dos testes atuais

Validamos apenas sintaxe C e testes unitários/contratos locais. **Ainda não foi compilado com PS5 Payload SDK nem testado no console.** Ao forçar uma reinstalação, o instalador do PS5 pode responder com erros específicos da versão de firmware; em especial, caso o status seja indisponível, o programa exige o arquivo nativo realmente completo e não interpreta o antigo marcador de 0 bytes como sucesso. Teste primeiro com o DLC pequeno do MARVEL.
