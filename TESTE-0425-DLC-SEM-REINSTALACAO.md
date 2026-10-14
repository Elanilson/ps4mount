# PS4Mount v0.4.25 — teste da montagem dos DLCs

Base: código-fonte **v0.4.21-offload-recovery** enviado pelo usuário.

## Alterações limitadas

- **Desmontar** deixa de desmontar explicitamente `/user/addcont/<CUSA>` e os redirecionamentos `nullfs` nativos dos DLCs. Apenas app/patch são desmontados pelo PS4Mount.
- A chamada original `sceAppInstUtilAppUnInstall()` **continua presente e inalterada**. Essa API é executada pelo PS5 e pode remover o registro dos DLCs ou recusar a desinstalação do jogo; não é possível garantir que o ícone desaparecerá enquanto os DLCs continuam montados.
- **Montar** lê o `addcont.db` por DLC. Se o registro existir e o journal, metadados e link USB estiverem íntegros, valida/restabelece o redirecionamento e **não inicia instalador nativo nem espera um `ac.pkg` de 0 bytes aumentar**.
- DLC registrado mas sem journal e sem PKG nativo completo: **reinstala nativamente para reconstruir os arquivos**, depois confirma o registro e volta a executar offload. DLC com `ac.pkg` nativo completo e metadados válidos usa o offload sem nova instalação.
- DLC sem registro: instala nativamente, depois confirma registro e arquivo, compara SSD/USB e faz o offload, como na v0.4.21.
- Journal corrompido, link USB errado, montagem não identificada ou PKG nativo de tamanho divergente: **falha rapidamente sem apagar arquivos**, para evitar destruição de dados inconsistentes.
- A preparação do instalador só pode liberar a montagem do DLC **individual que precisa de reinstalação**, nunca os DLCs registrados e íntegros de outros jogos ou o conjunto inteiro.

## Teste recomendado (primeiro com o MARVEL, CUSA54308)

1. Feche o jogo e mantenha o disco USB conectado.
2. Substitua o ELF da versão anterior pelo ELF compilado deste código. Não remova arquivos de `/data/ps4mount`, `/system_data/ps4mount` nem do USB.
3. Clique em **Montar** e procure no log por `[oneclick][dlc-reuse]`, `[oneclick][dlc-repair]` ou `[oneclick][dlc-offload]`.
4. Confira dentro do jogo se o DLC funciona.
5. Feche o jogo, clique em **Desmontar** e procure `[bundle][unmount] ... app+patch released; ... DLC title folder(s) kept mounted`.
6. Clique em **Montar** novamente e confira o resultado. Não deve aparecer `STATUS_FALLBACK` nem uma longa espera **se** o journal e o redirecionamento estiverem completos.

## Atenções

- A API original `sceAppInstUtilAppUnInstall()` pode retornar `0x80020010` com DLCs montados (já observado anteriormente). Nesse caso a biblioteca pode manter o jogo. O programa **não** afirma que a desinstalação teve sucesso se a API retornou erro.
- Caso a API do PS5 desregistre um DLC, uma instalação nativa será necessária na próxima montagem. Esta versão **não impede** operações internas do sistema.
- Se o journal sumiu (situação observada no log 18), a primeira montagem pode reinstalar e recriar o redirecionamento. Isso é esperado e diferente de reinstalar em todos os cliques.
- O offload pode remover o PKG nativo pesado do SSD após validações. Mantenha o PKG original no USB e backup dos dados do console.
- Os testes estáticos locais passaram; **o ELF com SDK PS5 e o hardware não foram testados aqui**.
