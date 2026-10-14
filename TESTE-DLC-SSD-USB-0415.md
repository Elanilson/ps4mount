# Teste controlado: DLC nativo do Resident Evil 7 via USB — v0.4.15

## Objetivo

Verificar, de forma reversível e sem alterar `addcont.db`, se o PS5 continua reconhecendo o DLC nativo `BH70000000DLC001` quando sua pasta instalada no M.2 é temporariamente coberta por uma pasta `nullfs` cujo `ac.pkg` aponta ao PKG **idêntico** no HD USB.

Este teste **não** prova que uma DLC *nova* possa ser registrada sem instalação, **não** libera o espaço nativo e **não garante** que o jogo abra a montagem testada (ele pode ler outro caminho ou cache).

## Pré-requisitos

1. Console PS5 com ambiente homebrew e permissões adequadas. Não temos SDK nem console para validar a montagem real.
2. Backup antes do experimento. Feche o Resident Evil 7.
3. Instalação nativa original intacta em `/mnt/ext1/user/addcont/CUSA03842/BH70000000DLC001/`, incluindo `ac.json`, `ac.pbm`, `ac.pbm.backup` e um `ac.pkg` **arquivo normal** de 14.876.672 bytes.
4. O HD USB que contém Resident Evil 7 e seus DLCs em `/mnt/usbN/ps4/CUSA03842/addcont/CUSA03842/BH70000000DLC001/ac.pkg`; o jogo precisa constar no catálogo do painel.
5. Registro `EP0102-CUSA03842_00-BH70000000DLC001` presente em `addcont.db`; o código o verifica em modo leitura.

## Procedimento

1. Compile via GitHub Actions com o SDK configurado no repositório (`build.sh`). Inicie o PS4Mount v0.4.15 e abra `http://IP_PS5:8770`.
2. Com o HD do Resident Evil conectado e o jogo **fechado**, localize `CUSA03842` e clique **Testar 1 DLC no USB**.
3. O programa compara o PKG do USB com o PKG nativo **byte a byte** (até 32 MiB). Caso sejam diferentes ou falte algum arquivo, a operação é recusada **antes** da montagem.
4. Se o log mostrar `[dlc-redirect] ACTIVE 1/17`, confira que `ac.pkg` está acessível no alvo e que o conteúdo do link aponta ao USB. A operação mascara o diretório nativo via `nullfs`, sem apagar o PKG no M.2.
5. Só então inicie Resident Evil 7 e teste o conteúdo desbloqueado por `BH70000000DLC001`. Pode haver conteúdo de desbloqueio pouco visível; abrir o jogo ou ter o registro no DB **não prova** que leu o USB. O teste de I/O ainda pode exigir instrumentação posterior.
6. Feche o jogo completamente. Clique **Restaurar DLC no SSD** e confira `[dlc-redirect] RESTORED native SSD ac.pkg` no log.
7. Envie o log completo `/data/ps4mount/log.txt`, principalmente os registros `[dlc-redirect]`, incluindo falha de montagem caso apareça.

## Proteções e limitações

- O teste afeta apenas **um** DLC fixo de RE7. Os outros 16 DLCs não são cobertos.
- Não usa `AppInstallAll`, nem altera registros SQLite, nem remove o arquivo do SSD, nem renomeia diretórios nativos.
- A restauração somente desmonta uma camada `nullfs` do próprio PS4Mount e recusa desmontar montagens desconhecidas. Não usa desmontagem forçada.
- Ao encerrar normalmente o ELF, o programa tenta restaurar a camada se o jogo estiver fechado. Se o HD USB desaparecer, também tenta restaurar com o jogo fechado. Se a operação falhar, use o botão **Restaurar DLC no SSD** após fechar o jogo. Um desligamento inesperado pode exigir verificar a tabela de montagens.
- Enquanto o teste estiver montado, busca, montagem/desmontagem de outros jogos, registro USB e instalação nativa são bloqueados para evitar operações simultâneas.
- **Instalar DLCs (NATIVO / SSD)** é um recurso diferente e realiza instalação real; não deve ser usado para o teste do DLC que já está registrado. A opção nativa foi recuperada exclusivamente para outros casos necessários.
- A montagem sobre `/mnt/ext1/...` ainda não foi demonstrada como suportada pelo firmware nem como caminho efetivamente usado pelo jogo. Mesmo que o `nullfs` retorne sucesso, o PS5 pode usar outro namespace, caminho ou cache.
- Não remova o USB enquanto o jogo estiver em execução.
