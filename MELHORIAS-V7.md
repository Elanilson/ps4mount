# PS4Mount v7-source — melhorias de USB e montagem manual

## Origem e limitação

Esta é uma **nova base de código-fonte derivada de ps4mount-main_2.zip**, não um patch binário do `ps4mount.elf` presente em `ps4mount_6.zip`. A interface web em português foi recuperada dos próprios bytes do ELF fornecido (v6), pois o código-fonte daquela versão não foi enviado. Portanto, **não é possível provar equivalência completa de comportamento à v6**; compare/teste primeiro em ambiente controlado.

O ProsperoMgr inspirou as verificações de volumes montados, o controle serial de operações e a conferência de estados, mas nenhuma implementação de montagem de jogos foi importada diretamente do ProsperoMgr.

## Melhorias implementadas

1. **Detectar USB real:** `is_mountpoint()` confere `statfs`, `stat` e a tabela de montagens (`getfsstat`), não apenas a existência da pasta `/mnt/usbN` ou `/mnt/extN`.
2. **Não ler PKGs sem USB:** uma busca manual retorna erro sem volumes; a verificação periódica de 10 segundos consulta somente pontos de montagem.
3. **Montagem manual e remontagem:** `/api/mount` põe um único CUSA na fila, abre a origem, prepara arquivos e monta as camadas; só usa a API de registro caso o jogo **não esteja registrado**; confirma o banco antes de gravar `.active`.
4. **Tratar camadas inconsistentes:** antes de montar/desmontar, verifica todas as camadas na tabela. Só remove camadas `nullfs` cuja origem corresponda à pasta de controle prevista do PS4Mount; camadas externas são bloqueadas e registradas no log.
5. **Desmontagem sem desinstalar:** `/api/unmount` **não chama `sceAppInstUtilAppUnInstall()`**, mantém o registro da biblioteca e salva `.disabled`, depois de confirmar a desmontagem controlada.
6. **Operações sem sobreposição:** uma operação por vez, com `g_operation_busy`, fila de montagem de um título e bloqueio de busca/configuração concorrente.
7. **Estados baseados no console:** exibe `ready` somente após confirmar o registro e as camadas de jogo, atualização e add-ons. Exibe `blocked` para camada estranha.
8. **Conexão/desconexão:** registra transições USB, interrompe a varredura se o HD desaparecer e só tenta restaurar após reconexão jogos marcados `.active` com origem `.src` correspondente. Não varre nem registra outros jogos automaticamente.

Adicionais: detecção recursiva de PKGs sob demanda, limite de profundidade de 8 níveis e 20.000 pastas; ignora links simbólicos e pastas de outros dispositivos; progresso em `/api/progress`; prevenção de FD aberto quando `app.pkg` é inválido; gravação atômica dos marcadores de ativação.

## Arquivos alterados

- `src/main.c`: detecção USB, restauração passiva, busca manual, montagem/desmontagem, bloqueios e progresso.
- `src/web.c`, `src/web.h`: validação da busca e nova rota `GET /api/progress`.
- `web/index.html`: interface manual derivada da v6, estado de conflito.
- `src/mount_policy.h`, `test/mount_policy.c`: regra testável de posse de camada.
- `test/run.sh`: teste adicional de política.
- `test/host_include/`: cabeçalhos mínimos **somente para verificação de sintaxe no Linux**; não são o SDK real.

## Compilar o ELF real

Requer SDK oficial de payload do PS5, com `prospero-clang`, e o utilitário `xxd`:

```sh
export PS5_PAYLOAD_SDK=/caminho/para/ps5-payload-sdk
bash build.sh
```

Saída esperada: `build/ps4mount.elf`.

O SDK **não está disponível neste ambiente**, por isso **nenhum novo ELF executável para PS5 foi produzido**. Não substitua seu ELF da v6 pelo código-fonte ou por binário do Linux. Testes locais:

```sh
bash test/run.sh
# Resultado neste ambiente: 150 checks + 6 checks de posse = 156, 0 failed
```

A sintaxe de `src/main.c` e `src/web.c` também foi validada com Clang, `-Wall -Wextra -Werror`, usando cabeçalhos simulados em `test/host_include/`; isso **não equivale a compilação no SDK nem a teste de execução no PS5**.

## Limites importantes e decisões de segurança

- Não removemos montagens de outros aplicativos nem implementamos a remoção forçada de camadas **não identificadas**. Se a v6 usava uma origem de montagem diferente, poderá ser marcada `blocked` até haver uma migração explícita e revisada.
- O fluxo original de mover PKGs de volta foi **desativado** porque dependia de desinstalar o título; mantê-lo depois de mudar `/api/unmount` para apenas desmontar seria perigoso. `/api/restore` e `/api/restored/forget` respondem com erro.
- `/api/rescan` **pode reorganizar PKGs no próprio HD por renomeação**, tal como o mecanismo herdado do original. Não registra nem monta jogos nessa etapa. Faça backup da estrutura e teste em um HD separado.
- `sceAppInstUtilAppInstallAll()` é uma chamada **global** do sistema: quando utilizada para registrar um jogo novo, pode registrar qualquer outro título que já esteja preparado em `/user/app`. O botão é individual, mas a API não garante exclusividade nesse caso. Não declaramos registro individual garantido.
- O novo controle de fila e montagem não protege contra mudanças externas de outro programa entre preflight e execução. Um teste no console continua obrigatório.
- Só jogos que tenham `.active` e `.src` válidos são restaurados automaticamente na reconexão. O comportamento de `.disabled` da v6 é respeitado; partidas instaladas sem `.active` não são ligadas por conta própria.
- Detecção de USB usa leitura leve de estado da montagem. Os metadados dos PKGs são lidos somente com USB montado, ao abrir a lista, restaurar jogo conhecido ou executar ação explícita.

## Testes práticos recomendados, nessa ordem

1. Abrir sem HD; confirmar que `/api/rescan` retorna erro e nenhuma busca acontece.
2. Conectar um HD de teste; confirmar detecção de `usb0`, `usb1` etc., sem mover nada automaticamente.
3. Buscar jogos pelo botão; conferir `/api/progress` e nomes organizados.
4. Montar um jogo **já registrado**; conferir base e atualização com `getfsstat` e biblioteca.
5. Desmontar; confirmar que não foi desinstalado e que `.disabled` foi gravado.
6. Montar novamente sem procurar nem instalar de novo.
7. Retirar o HD, reconectar e verificar restauração **apenas** dos jogos ativos, sem aplicar busca recursiva.
8. Simular camadas de outro programa: recusar montagem/desmontagem e registrar conflito, sem remover nada.
9. Só depois testar um jogo *não registrado* e instalação via API, por envolver o banco do sistema.
10. Verificar atualização e DLC (a instalação de DLC no banco continua sem comprovação independente neste projeto).

## Licença

Código-fonte derivado do projeto original GPL-3.0-or-later, com licença preservada no arquivo `LICENSE`.
