# PS4Mount 0.4.11 — TESTE de revalidação direcionada dos DLCs via USB

Objetivo: testar se revalidar o jogo via `sceAppInstUtilAppInstallTitleDir(title_id, "/user/app/", NULL)` após montar o bundle, como alternativa **não comprovada** para registrar conteúdos adicionais.

**Limite conhecido**: essa API é voltada ao registro de jogos/títulos. O original registra add-ons via `sceAppInstUtilAppInstallAll(NULL)`, que é **global** e não é usado aqui. Não encontramos API comprovada de registro individual de DLC sem copiar; a chamada desta versão PODE não registrar nenhum DLC novo. NÃO usar a mensagem de sucesso da API como prova de DLC; verificar o `addcont.db` com o relatório `AFTER`.

Esta versão **remove** o botão/API da instalação nativa e nunca usa `sceAppInstUtilInstallByPackage()` ou `sceAppInstUtilAppInstallAll()`. Não copia PKGs do USB para o interno. Metadados reduzidos continuam em armazenamento interno, como no projeto original. DLCs previamente instalados pelo método nativo continuam presentes internamente, e a versão **não os remove**. Isso significa que um DLC já registrado pelo instalador nativo NÃO comprova execução pelo USB.

## Teste

1. Com o jogo fechado, clique em Montar. O PS4Mount tentará a revalidação individual do título depois de preparar/montar o bundle.
2. Para repetir no jogo já montado, clique em **Registrar DLCs (USB)**.
3. Analise `[dlc-usb] ... db_before=...`, `AppInstallTitleDir ... rc=...`, `AFTER: ... newly_registered=... missing=...`.
4. Como o Resident Evil 7 já teve **17 de 17** DLCs instalados nativamente, prefira para esse teste *não destrutivo* um título que ainda tenha DLCs sem registro, como **CUSA34384** (3 DLCs, 0 no banco no log enviado). Não desinstale conteúdo apenas para fazer o teste.
5. Se `newly_registered=0 missing>0`, o teste confirma que `AppInstallTitleDir` NÃO resolveu o registro de DLCs nesta instalação; não marque como funcional. Não há fallback global.

Não foram realizados testes em hardware PS5 e não podemos garantir o comportamento do firmware.
