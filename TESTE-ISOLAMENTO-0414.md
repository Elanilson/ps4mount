# PS4Mount v0.4.14 — Isolamento manual de PKGs preparados

Teste opcional sobre a base v0.4.13.

- No card do jogo alvo (ex.: CUSA34384), clique **Isolar outros**. A ação executa imediatamente, **não** chama `AppInstallAll`.
- Apenas links simbólicos `app.pkg`, `patch.pkg` e `ac.pkg` do staging pertencente ao PS4Mount, em `/system_data/ps4mount/`, têm o prefixo `__ps4mount_hidden__` acrescentado ao nome. O jogo selecionado é preservado.
- Nenhuma pasta montada em `/user` ou PKG original do USB é renomeado. Não remove registros de `app.db`/`addcont.db` ou mexe em licenças e outros metadados.
- Clique **Registrar DLCs (GLOBAL)** se desejar realizar o teste **POR SUA CONTA E RISCO**. A chamada continua global, e outros jogos **podem** ser afetados independentemente do prefixo.
- Após o término do registro global, clique **Restaurar outros**. É bloqueado até a operação global terminar.
- O painel exibe contagem de links ocultos; veja `[isolation]` no log para caminhos e resultado.
- Na inicialização, o PS4Mount tenta restaurar automaticamente links que tenham ficado ocultos após reinício inesperado, antes de restaurar os jogos.
- Não execute enquanto algum jogo estiver aberto; o painel recusa o isolamento. Enquanto houver links ocultos, o painel também impede montagem, desmontagem e busca USB.

## Limitação crítica

Ocultar PKG symlinks de staging **não prova** que o serviço `AppInstallAll` deixará de considerar o jogo. Ele também pode reconhecer registros ou metadados em outros locais. **Nunca** trate essa alteração como isolamento completo ou como modo por título.

## Reversão

Clique Restaurar outros. Em caso de conflito ou falha, o log informa o nome do arquivo; o prefixo `__ps4mount_hidden__` identifica exatamente os links para desfazer a operação. Não force `rename()` de diretórios montados.
