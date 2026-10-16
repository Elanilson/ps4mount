# PS4Mount 0.4.39 TEST — organizar PKGs mesmo quando o jogo está instalado

## Alteração

O botão **Buscar jogos no USB** continua procurando PKGs recursivamente nas
pastas configuradas. A organização **não bloqueia** `app.pkg` ou `patch.pkg` só
porque o mesmo CUSA está instalado no armazenamento do PS5.

Um exemplo é `/mnt/usb0/New folder/app.pkg` do Far Cry 5 (`CUSA05904`):
quando o pacote passa as verificações de PKG fake e não há duplicata já organizada,
ele é movido para `/mnt/usb0/ps4/CUSA05904/app.pkg` — **no mesmo USB**. O
`patch.pkg` vai para a pasta equivalente. Nada é registrado ou montado nessa busca.

Se uma pasta de DLC for visitada antes da pasta do jogo-base, as DLCs pendentes
são reavaliadas **no fim da mesma busca**. Se o jogo-base estiver agora na
biblioteca `/ps4` daquele mesmo armazenamento, os add-ons passam a ser
organizados sem exigir um segundo clique.

## O que permanece inalterado

- As outras validações (tipo de PKG, PKG fake, duplicatas e separação por disco)
  permanecem ativas.
- Uma DLC cujo jogo-base não esteja em `/ps4` **do mesmo dispositivo** ainda fica
  aguardando. O app não move arquivos entre USB e armazenamento expandido.
- O botão de busca não registra jogos, não monta e não mexe nos PKGs internos.
- `installed_internally()` segue sendo aplicado nas operações de montagem e
  registro, separadas da organização.
- O método **Limpar DLCs** é o mesmo da 0.4.38 (sem outras alterações).

## Validação

Testes de código e dos componentes portáveis podem rodar no PC com
`bash test/run.sh`. Compilação com PS5 SDK e teste no console ainda são necessários.
