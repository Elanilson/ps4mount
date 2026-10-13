# PS4Mount 0.4.5 — base no código-fonte real da versão 0.4.4

## Preservado da v6
- Registro individual com AppInstallTitleDir, sem `AppInstallAll` global, inclusive na inicialização.
- `.attempt` quando uma instalação falha; sem repetição automática perigosa.
- Desmontagem sem desinstalar; DLC compartilhada não é desmontada.
- Busca recursiva de PKGs somente ao solicitar; progresso por pastas, PKGs e arquivos organizados.
- Sem `MNT_FORCE` para camadas desconectadas ou desconhecidas.
- Compatibilidade com `.src`, `.active`, `.disabled` da v6.

## Melhorias incrementais
- Verificação de HD conectado por `statfs` + `stat` + tabela real de montagens.
- Botão Buscar só aceita iniciar com um HD externo realmente montado.
- Busca pode ser interrompida no próximo diretório se HD desaparecer.
- Identificação de camadas montadas empilhadas e bloqueio de camadas externas.
- Desmontagem de app/patch próprios até confirmar ausência das camadas, nunca forçada.
- Verificação do estado das camadas após uma montagem e antes de exibir sucesso.
- Polling leve a cada 10 s, restauração apenas de registros conhecidos após reconexão USB.
- Não adicionado sistema de filas separado: o daemon v6 já serializa as ações, e os botões impedem operações concorrentes.

## Estado dos testes
- Testes originais de dados, PKG, metadados: **125 verificações, 0 falhas**.
- Testes estruturais do contrato manual da v6: **25 verificações aprovadas**.
- Teste de travessia recursiva, exclusões e cancelamento: **aprovado**.
- Sintaxe C para `src/main.c`, `src/web.c`, `src/scan_walk.c`: **aprovada** com Clang e cabeçalhos simulados.
- Teste em PS5 e compilação com PS5 Payload SDK: **não realizados**.
- Os cabeçalhos em `test/host_include` são **somente stubs**, não substituem SDK do PS5.

## Importante
Ao clicar em Buscar, a rotina herdada pode **mover PKGs** no mesmo HD para a pasta ps4/CUSA. Faça backup.
A rotina de restauração na reconexão não examina PKGs novos nem faz registro no banco. Caso haja camada inacessível, não a desmonta forçadamente.

## Limitações
- Detecção por polling a cada 10 segundos pode perder desconexões/reconexões muito rápidas.
- Uma chamada bloqueada pelo dispositivo físico desconectado não é interrompível a meio de leitura; a busca cancela entre arquivos/diretórios.
- DLCs são verificados quanto à montagem, mas não há confirmação de registro individual no banco.
- Camadas mortas que o kernel não deixa remover sem força continuam bloqueadas; não se usa `MNT_FORCE`.
- Nenhuma tentativa de registro é repetida automaticamente após arquivo `.attempt` de falha.
