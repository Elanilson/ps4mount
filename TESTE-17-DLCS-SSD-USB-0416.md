# v0.4.16 — Redirecionamento reversível dos 17 DLCs nativos do Resident Evil 7

Objetivo: verificar se os DLCs já instalados nativamente e registrados no addcont.db podem ser lidos via pacotes idênticos no HD USB. Não registra DLCs novos, não libera espaço no SSD e não altera bancos.

1. Conecte o HD original com os 17 ac.pkg de CUSA03842; mantenha o M.2 conectado e com os 17 DLCs nativos intactos. Faça backup dos dados importantes. Feche o jogo.
2. Inicie PS4Mount v0.4.16; no cartão Resident Evil 7 clique **Testar 17 DLCs no USB**.
3. O programa primeiro compara **todos os bytes de cada um dos 17 PKGs** contra o ac.pkg nativo em `/mnt/ext1/user/addcont/CUSA03842/<label>/ac.pkg`. Isso envolve vários gigabytes e pode demorar. O progresso é exibido no painel e no log. Se QUALQUER PKG/registro faltar ou diferir, NÃO monta nenhum overlay.
4. Depois de validar os 17, copia apenas `ac.json`, `ac.pbm` e `ac.pbm.backup` para staging em `/system_data/ps4mount/redirect-native/CUSA03842/<label>`, cria links USB e monta por nullfs as 17 pastas em `/mnt/ext1/user/addcont/CUSA03842/`.
5. Confira `[dlc-redirect] ALL ACTIVE 17/17`. Só então abra o jogo para verificar se seus DLCs continuam disponíveis. Isso NÃO prova sozinho a leitura dos dados pelo USB; é possível haver cache/outros caminhos.
6. **Feche o jogo**, clique **Restaurar 17 DLCs no SSD**, confirme `[dlc-redirect] RESTORE FINISH restored=17 failed=0`. Se houver falha, NÃO desconecte o USB nem apague os arquivos. Envie o log.

## Segurança

- Restrito ao CUSA03842 e aos 17 IDs conhecidos; exige registros prévios em addcont.db, pacote nativo real, arquivos no USB idênticos byte a byte e ausência de montagens estranhas.
- Não usa `AppInstallAll`, não instala PKGs, não renomeia nem remove ac.pkg do SSD, não escreve SQLite.
- Se falhar durante as montagens, tenta rollback em ordem inversa, e mantém os arquivos nativos preservados por baixo do nullfs.
- Botão restaurar procura montagens efetivas (funciona mesmo após reiniciar o ELF); não faz unmount forçado e recusa camadas estranhas.
- Encerrar o programa ou perder o USB tenta restaurar apenas após fechar o jogo. Não desligue ou troque o HD durante a verificação ou enquanto jogar.
- Verificações feitas em thread separada para preservar o painel; o botão Montar/Desmontar/Instalar/Buscar permanece bloqueado enquanto o teste está em andamento ou overlays estão ativos.
- É um experimento não testado no hardware, sem garantia de que o jogo lerá realmente pelo USB.
