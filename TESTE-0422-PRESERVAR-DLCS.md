# Teste PS4Mount v0.4.22 — conservar registros e montagens dos DLCs

Alvo de teste: MARVEL Cosmic Invasion `CUSA54308`, DLC `EP1470-CUSA54308_00-0833301153272321`.

1. Antes de tudo, guarde um backup restaurável dos dados relevantes. Confirme que a 0.4.21 já concluiu `offload DONE` e que o DLC funciona.
2. Encerre o jogo. Na 0.4.22, clique **Desmontar**.
3. Esperado: `[oneclick][keep-dlcs] CUSA54308 base+patch unmounted; add-on mounts and DB registrations preserved`; NÃO deve aparecer `[PlayGoCore][Uninstall]` causado pelo botão. A base continua registrada na biblioteca, e o ícone pode permanecer.
4. **Não inicie o jogo pelo ícone enquanto desmontado.** No painel, clique **Montar**.
5. Esperado: `already_in_db=1`, `all selected DLCs registered; no installer and no add-on unmount`, `already registered, skipping installer`, `offload ACTIVE`, `1/1 registered`. NÃO devem aparecer novas chamadas `IpcFacade::appInstallByPackage` nem `[offload] NATIVE/USB MATCH` para este DLC já redirecionado.
6. Abra o jogo e valide o conteúdo DLC. Feche o jogo e reinicie o PS4Mount com o USB conectado, verificando `[offload] restore USB DLC overlay ...` e `ACTIVE`.

**Importante:** a desinstalação manual pelo menu do PS5 continua podendo eliminar registros de DLC, pois isso é feito pelo sistema e foge do PS4Mount. Se o USB não estiver conectado, a entrada física nativa continua com 0 byte: NÃO abra o título antes de reativar o redirecionamento.

A sintaxe C e os testes locais não substituem o teste do PS5.
