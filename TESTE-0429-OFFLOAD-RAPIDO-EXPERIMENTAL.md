# PS4Mount 0.4.29 TEST — offload rápido de DLCs

## Atenção: experimento, não substitui a v0.4.28 estável

Esta versão remove **apenas após uma instalação/reinstalação nativa solicitada**:
- A espera de 15 amostras estáveis de 1 segundo;
- A comparação byte a byte dos PKGs do SSD e do USB.

Mantém a verificação mínima do **Content ID associado**, registro em `addcont.db`, existência do `ac.pkg` nativo com **mesmo tamanho do PKG USB**, presença dos metadados nativos, ausência de sobreposições desconhecidas e montagem `nullfs` confirmada **antes de excluir a cópia do SSD**. O offload segue reversível até a etapa final de exclusão, e o arquivo original no USB permanece intacto.

### IMPORTANTE — firmware com status 0x80a30003

Quando `sceAppInstUtilGetInstallStatus` informa explicitamente `playable`/`completed`, há confirmação direta da tarefa. Quando retorna `0x80a30003`, **não existe confirmação autoritativa**: a alternativa experimental exige duas observações consecutivas do registro, tamanho e metadados, com 1 segundo de intervalo. **Isso pode confundir uma instalação em andamento com instalação concluída**, caso o instalador atualize o arquivo no mesmo tamanho antes de finalizar; portanto há risco de danificar a instalação nativa se a cópia for removida cedo demais. Não é equivalente à notificação nativa do PS5.

### O que NÃO mudou

- Montar: DLC com registro não é reinstalado automaticamente.
- Desmontar: sem remoção da biblioteca, preserva montagens dos DLCs.
- Botão Reinstalar DLCs continua explícito.
- Journals e recuperação dinâmica de USB iguais à v0.4.28.
- Redirecionamento permanece baseado em cópia nativa para staging, criação de stub de 0 bytes, `nullfs`, confirmação e exclusão tardia do arquivo retido.
- DLC **já registrado** com arquivo nativo ainda passa pelo fluxo original (comparação e estabilidade); modo rápido só ocorre após NOVA instalação nativa.

### Como testar

1. Guarde o ZIP/ELF funcional da v0.4.28 e um backup do DLC/HD.
2. Use um DLC pequeno e feche completamente o jogo.
3. Clique **Reinstalar DLCs**.
4. Observe `[dlc-fast]` no log: `system status terminal` ou aviso `status=0x80a30003`.
5. Procure `full byte comparison SKIPPED by test mode` e, após a montagem, `native_stub=0`.
6. Desmonte e monte novamente; confira `[offload] ACTIVE` e o funcionamento do DLC dentro do jogo.
7. Se aparecer `completion could not be inferred safely`, o original no SSD foi preservado. Não tente apagar manualmente.

**Limites dos testes:** testes de contrato e sintaxe C no host passaram; ELF/SDK e console PS5 não foram testados aqui.
