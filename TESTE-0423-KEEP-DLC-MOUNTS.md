# v0.4.23 — Desmontar sem liberar as montagens dos DLCs

Base: v0.4.21 que funcionou no MARVEL Cosmic Invasion.

A única mudança de comportamento solicitada foi no botão **Desmontar**:
- Libera o `nullfs` do jogo base e da atualização, mantendo as verificações de propriedade das camadas.
- **Não libera** os `nullfs` de `/user/addcont/` nem os redirecionamentos `nullfs` nativos (SSD -> USB), e não apaga o journal dos DLCs.
- **Continua chamando** `sceAppInstUtilAppUnInstall(CUSA...)`, removendo o jogo da biblioteca conforme o comportamento da v0.4.21.

## Atenção

Nos testes anteriores, `sceAppInstUtilAppUnInstall()` também removeu os registros dos DLCs em `addcont.db`. Deixar os arquivos montados NÃO garante impedir isso. Esta versão é um teste direcionado para comprovar se manter as montagens afeta esse resultado. Se o registro desaparecer, o processo nativo continuará sendo necessário na próxima montagem. Não altere manualmente o banco de dados.

## Teste no PS5

1. Mantenha backup dos dados importantes e os PKGs no USB; inicie o PS4Mount v0.4.23.
2. No MARVEL (`CUSA54308`), confirme que o DLC está registrado e funciona, feche o jogo e clique em **Desmontar**.
3. Confira se o jogo desaparece da biblioteca e procure no log `[bundle][unmount] ... add-on folder(s) and USB DLC offloads kept mounted`.
4. Clique em **Montar**, espere o processo finalizar e abra o jogo. Confira `already_in_db`, `database_registered` e se aparece `START ... ac.pkg` em `[dlc-install]`.
5. Envie o novo log para analisarmos se o desinstalador do PS5 removeu o registro dos DLCs mesmo com as montagens preservadas.

**Ainda não compilado/testado no PS5.**
