# PS4Mount 0.4.18 — Piloto de 1 DLC do God of War Ragnarök

Objetivo: testar **um DLC por vez** (CUSA34384), usando a instalacao nativa APENAS para criar registro e depois retirando temporariamente o ac.pkg do caminho esperado no SSD, com overlay nullfs para o USB. **Nao ha liberacao de espaco nesta etapa.** O arquivo e apenas renomeado no mesmo SSD (`ac.pkg.ps4mount-held`).

## Como testar

1. Verifique se o jogo base CUSA34384 esta registrado/montado e fechado. Conecte o USB com os tres DLCs.
2. Clique `Instalar 1 DLC (NATIVO)` e escolha um numero na lista, confirme. Isso **copia o PKG inteiro para o SSD**; aguarde `result=REGISTERED` e consulte o `addcont.db` antes de prosseguir.
3. Clique `Testar 1 DLC (USB)` e selecione **o mesmo Content ID**. Aguarde `[dlc-pilot] ACTIVE ONE ...`. Se aparecer erro, consulte o log antes de jogar.
4. Com o USB conectado, execute o jogo e verifique se esse DLC e reconhecido. Durante o piloto, o arquivo nativo fica como `ac.pkg.ps4mount-held` no SSD e nao e apagado.
5. Feche completamente o jogo e clique `Restaurar 1 DLC (SSD)` (restaura automaticamente o unico DLC em teste; nao requer selecao nem USB conectado). Confira `[dlc-pilot] RESTORED ...`.

## Observacoes
- Nenhum `AppInstallAll`, nenhuma escrita direta em `addcont.db`, nenhum unlink do PKG nativo.
- Se o processo reiniciar com o journal em `/system_data/ps4mount/pilot-gow-one.txt`, tentara restaurar automaticamente o nome original ao iniciar, **se o jogo estiver fechado e o ponto de montagem puder ser desmontado**.
- Se aparecer `RESTORE BLOCKED`/`MANUAL RECOVERY REQUIRED`, **mantenha o USB conectado e nao execute outro teste**. Envie o log. Nao altere ac.pkg manualmente sem verificar o estado da montagem.
- Uma montagem `ACTIVE ONE` confirma sobreposicao e link, mas **nao comprova leitura fisica do USB durante a partida**. Ainda precisamos validar isso no PS5.
- A copia nativa ocupa espaco ate eventual experimento futuro, separado, com recuperacao persistente validada. Nao delete manualmente `ac.pkg.ps4mount-held`.
- Apenas para God of War Ragnarök; o teste anterior RE7 17/17 permanece disponivel e inalterado.
