# PS4Mount v0.4.26 — apenas duas alterações funcionais sobre a v0.4.21

## 1. Desmontar não remove da biblioteca nem desmonta DLCs

`Desmontar` libera somente as camadas de app e patch controladas pelo PS4Mount.
Não executa `sceAppInstUtilAppUnInstall`, não chama `offload_unmount_title` e
não libera `/user/addcont/`. O título **continuará na biblioteca do PS5**;
não abra o jogo pelo ícone enquanto estiver desmontado, primeiro clique em
`Montar`.

## 2. Instalação nativa apenas se registro da DLC estiver ausente

- `addcont.db = 0`: instala a DLC, confirma registro/arquivo e executa o offload original.
- `addcont.db = 1` + journal USB válido: reconecta ou reaproveita a DLC sem instalação.
- `addcont.db = 1` + sem journal, mas PKG nativo completo: valida a cópia e faz offload,
  sem reinstalação (a checagem de estabilidade ainda pode levar ~20 segundos).
- `addcont.db = 1` + sem journal nem PKG nativo íntegro: **não instala**;
  registra aviso de estado inconsistente e encerra a verificação desse DLC prontamente.
- Estado desconhecido do banco: não instala nem exclui arquivos.

O painel e o procedimento de montagem do jogo/patch da v0.4.21 foram preservados.
Nenhuma edição direta nos bancos é feita. A rotina antiga de desinstalação ficou
no fonte, sem chamada no botão Desmontar.

## Como verificar

1. Feche o jogo antes de desmontar.
2. Clique em `Desmontar`; o título continuará na biblioteca do console.
3. Clique em `Montar`; confira `[oneclick][dlc-reuse] ... DB=1; no native installer`
   e `[offload] ... ACTIVE` se houver journal válido.
4. Se a DLC for nova, confira `[dlc-install] START`, depois registro no banco e `[offload] DONE`.
5. Confirme em jogo que a DLC realmente funciona. Somente a presença no banco
   não comprova montagem, licença ou integridade dos arquivos.

Validação local: testes host + checagem de sintaxe Clang. Não testado em PS5 real.
