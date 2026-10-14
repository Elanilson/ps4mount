# PS4Mount v0.4.28 — reconhecer DLC quando o USB muda de porta

Base: o código-fonte completo da v0.4.27 (`ps4mount-0.4.27-panel-refresh-manual-dlcs-source.zip`).

## Journal: o que é?

É um arquivo-texto de recuperação do PS4Mount, em `/data/ps4mount/offload_<CONTENT_ID>.txt`. Guarda quatro linhas: Content ID, pasta nativa do DLC, pasta intermediária do redirecionamento (`stage`) e caminho do PKG no USB. **Não é o PKG e não é o registro no addcont.db**. É preservado após o offload para que o marcador físico de 0 bytes seja reconectado ao USB depois de uma reinicialização.

## Única melhoria

Se um DLC tem journal válido, mas o PKG não está mais no caminho salvo (`/mnt/usb0/...`), a recuperação consulta **somente os outros pontos `/mnt/usb0` a `/mnt/usb7`**, mantendo exatamente o caminho relativo `/ps4/.../ac.pkg`.

Só atualiza o `stage/ac.pkg` (link simbólico) e a quarta linha do journal se localizar **um único candidato** de uma unidade USB realmente montada cujo PKG válido tem o **mesmo Content ID**. Se o caminho original continua válido, nenhum arquivo é regravado.

Não há reinstalação automática, mudança no banco SQLite, alteração de Mount/Desmontar/Reinstalar DLCs, mudança na UI, desmontagem de DLC ou exclusão do PKG original. Se houver dois candidatos, PKG incorreto, DLC em execução, montagem estranha/empilhada ou arquivo nativo físico diferente de 0 bytes, nenhuma alteração é feita.

Quando o journal e o link ficarem diferentes por uma interrupção durante a troca, o programa pode reconciliá-los na próxima execução, apenas se confirmados os mesmos caminhos relativos e a identidade do DLC.

## Teste recomendado com MARVEL Cosmic Invasion (CUSA54308)

1. **Com o jogo fechado**, confirme que o DLC está em estado `ACTIVE` e que o journal foi criado pela versão anterior. Não é necessário usar **Reinstalar DLCs** novamente.
2. Desligue completamente o PS5 **antes de mover o HD**. Não desconecte uma unidade montada durante execução do jogo.
3. Conecte o HD em outra porta e inicie o PS5. Confirme qual caminho ele recebeu, por exemplo `/mnt/usb1/ps4/CUSA54308/addcont/CUSA54308/0833301153272321/ac.pkg`.
4. Inicie o `.elf` da v0.4.28. Se o título estiver ativo para restauração, o journal será verificado no boot. Também é possível clicar **Montar** se o título estiver desativado.
5. Confira o log. O sucesso na migração deve mostrar `[offload][usb-port] ... moved /mnt/usb0/... -> /mnt/usb1/...` e depois `[offload] ACTIVE ... USB=/mnt/usb1/...`.
6. Abra o jogo e confirme que o DLC é reconhecido. O arquivo físico `ac.pkg` do SSD deverá continuar **com 0 bytes** e o PKG original do USB deverá permanecer íntegro.
7. Desmonte e monte novamente: deve reutilizar o DLC, sem instalação nativa nem espera pelo stub.

## Limitações e validação

- O arquivo de journal **deve existir**; este ajuste não recria journals perdidos nem faz varredura recursiva por DLC.
- O layout de pastas deve ser o mesmo; a busca troca apenas a parte `/mnt/usbN` do caminho. `/mnt/extN` não é abrangido por esta melhoria.
- Verifica a identidade pelo Content ID dentro do PKG. Sem a origem anterior acessível, não há comparação byte a byte entre PKGs em portas distintas; use o mesmo HD com os mesmos arquivos.
- Os testes de regressão do repositório, teste de sintaxe C e simulações das funções de recuperação de porta passaram. A compilação do ELF com SDK e a validação no PS5 não foram realizadas neste ambiente.
- Se não encontrar um candidato inequívoco, o programa preserva o journal original. Recolocar o HD na porta antiga permite voltar ao caminho anterior; para um estado inconsistente, consulte o log antes de usar a reinstalação manual.
