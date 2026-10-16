# PS4Mount v0.4.33 — Limpar DLCs do PS5, USB intocado (EXPERIMENTAL)

Base: **v0.4.32**. Os botoes **Limpar DLCs** e **Apontamento** possuem funcoes diferentes.

## Limpar DLCs — SOMENTE console

- Percorre exclusivamente ` /user/addcont/CUSAxxxxx/<label>/ac.pkg ` (SSD interno) e ` /mnt/ext0..7/user/addcont/CUSAxxxxx/<label>/ac.pkg ` (expansao).
- Faz `ftruncate(fd, 0)` nos arquivos *fisicos nativos* encontrados, inclusive DLCs nao registradas. Nao consulta `addcont.db`, nao compara PKGs, nao consulta nem necessita de HD USB. Nao instala DLCs nem faz redirecionamento.
- Arquivos nativos ja em 0B permanecem como estao.
- **Nunca escreve, renomeia, move, trunca ou remove um arquivo no USB**; nesta operacao o USB sequer e consultado.
- Por protecao obrigatoria ao USB, ignora diretorios ou arquivos apontados por `nullfs`, links simbolicos ou montagens inseguras. Tambem ignora jogos em execucao. O total pode ter itens pulados.
- **DESTRUTIVO**: DLCs nativamente instaladas e sem uma copia funcional em outro lugar podem deixar de funcionar. Os bytes nativos apagados NAO podem ser recuperados pelo PS4Mount. As duas confirmacoes explicam isso.

## Apontamento individual — separado, sem banco

- Parte da pasta de DLC na biblioteca USB de um jogo, usando o pacote do USB como **origem somente de leitura**.
- Nao exige registro em `addcont.db`, nao compara o conteudo byte a byte nem tamanho entre nativo/USB.
- Procura o diretorio nativo correspondente ao Content ID. Aceita `ac.pkg` nativo tanto **inteiro** quanto **ja em 0B** (por exemplo, apos Limpar DLCs). Se estiver inteiro, o apontamento cria stub e journal, monta nullfs e so entao remove a copia do SSD. Se ja estiver 0B, so configura o redirecionamento; nao ha conteudo a remover.
- **Protecao indispensavel**: recusa pastas redirecionadas que possam esconder o USB, a falta de `ac.pkg` nativo ou a falta do arquivo original USB. Isso nao e validacao de registro ou de igualdade de arquivo: impede perda de dados no USB e a criacao de um apontamento sem origem.
- Se o Content ID usar um CUSA de outra regiao, procura a pasta nativa pelo CUSA contido no proprio Content ID, nao pelo nome da pasta da biblioteca.
- **Apontar nao instala nem registra DLC**. Mesmo com nullfs ativo, jogos podem nao reconhecer uma DLC sem registro.

## Como testar sem afetar o USB

1. Guarde a v0.4.30 ou v0.4.32 para retorno e nao deixe jogos nem instalacoes ativas.
2. Antes de Limpar DLCs, decida se aceita perder definitivamente todos os bytes de `ac.pkg` nativos instalados que nao tenham origem restauravel.
3. Confirme o processo duas vezes no painel. Consulte `[dlc-clean] ZEROED_NATIVE` para os arquivos zerados e `[dlc-clean] SKIP` para itens que nao foram alterados.
4. Para utilizar algum DLC que ja tenha `ac.pkg` nativo em 0B, conecte o HD USB original e use o botao **Apontamento** do jogo correspondente. Confira `[dlc-point] ... native ac.pkg already 0B` e `[offload] DONE/ACTIVE`.
5. Se nao houver PKG original USB ou pasta nativa, Apontamento falha **sem alterar o USB**.

Validacao: `bash test/run.sh` e `clang -fsyntax-only` em ambiente host com cabecalhos simulados. **Nao executado no hardware PS5 e nao compilado com o SDK PS5 nesta entrega.**
