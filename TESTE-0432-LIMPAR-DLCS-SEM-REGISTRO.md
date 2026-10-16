# PS4Mount v0.4.32 — Limpar DLCs / Apontamento sem registro (EXPERIMENTAL)

Base: **v0.4.31**. Preservar o arquivo v0.4.30/v0.4.31 como referência até testar no PS5.

## Distinção crucial: 0 bytes não é DLC instalada

O `ac.pkg` nativo com **0 bytes** é um arquivo físico vazio. Ele não contém a DLC. O conteúdo continua no `ac.pkg` completo no HD USB; o PS4Mount mantém um symlink, a montagem `nullfs` e um journal para a recuperação após reiniciar. Zero bytes **sem** um redirecionamento funcional pode inutilizar a DLC.

O registro no `addcont.db` e o arquivo nativo são coisas distintas: apontar **não cria registro** nem garante reconhecimento da DLC pelo PS5.

## Alterações

- Botão global ao lado de Buscar jogos no USB: **Limpar DLCs**. Endpoint: `POST /api/clean-dlcs`. Duas confirmações explícitas. Percorre os jogos da biblioteca organizada no USB.
- Individual **Apontamento**: sem bloqueio por `db=0` ou estado desconhecido. Também não exige que o jogo base esteja registrado no PS5.
- Individual e global não comparam bytes, não exigem tamanhos nativo e USB idênticos e não fazem instalação nativa.
- Uma DLC com arquivo nativo em SSD e PKG correspondente válido no USB: grava journal, prepara link, renomeia o nativo, cria stub físico 0B, monta e verifica o `nullfs`; **só então** exclui a cópia nativa temporariamente preservada.
- Reuso/reconexão de journals de apontamento manual sem registro agora é permitido com verificação de USB e do overlay.
- **Sem truncamento cego:** se faltar o `ac.pkg` original do USB, o nativo, metadados indispensáveis, correspondência de Content ID com a pasta, ou se houver camadas de montagem incompatíveis, o item é pulado/falha sem deletar o SSD. Isso é intencional; limpar indiscriminadamente destruiria conteúdos.
- Se já existe journal funcional, o apontamento reutiliza o redirecionamento anterior.

## Cuidados importantes

1. Deixe **todas as instalações nativas terminarem** no PS5; mantenha jogos fechados.
2. **Não desconecte o HD USB** durante limpeza e mantenha o `ac.pkg` original no mesmo HD. O nome/caminho dos DLCs no USB deve permanecer válido.
3. Teste primeiro em um jogo com DLC de 1 MiB; depois tente Limpar DLCs.
4. Ocorre remoção real da cópia nativa somente após o redirecionamento validado. **O procedimento pode causar perda de DLC se realizado durante uma instalação em andamento**. Não há comparação de conteúdo.
5. Este build é experimental. Os testes no host não simulam a montagem `nullfs` do PS5.

## Log esperado

```
[dlc-clean] START games=...
[dlc-clean] TITLE 1/N: CUSAxxxxx
[dlc-point] ... manual override: db=0 ignored; USB PKG stays intact
[dlc-point] ... full byte comparison SKIPPED by manual request
[offload] DONE ... native_stub=0 usb=/mnt/usb0/ps4/...
[dlc-clean] DONE ...
```

Arquivos **originais do USB nunca são zerados** pelo Limpar DLCs. O código não chama o instalador nativo para apontamento manual ou limpeza global.
