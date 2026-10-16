# PS4Mount 0.4.36 TEST — Limpar DLCs: excluir e recriar `ac.pkg` nativo

Base: código-fonte experimental 0.4.35 recebido do usuário. Alterações específicas no botão **Limpar DLCs** e na recuperação automática dos overlays após reinício/reconexão.

## O problema da versão 0.4.35

A limpeza usava `ftruncate(fd,0)`. Quando havia `nullfs` administrado pelo PS4Mount, desmontava temporariamente o overlay, zerava o arquivo físico e **remontava** o overlay. O arquivo visível voltava a apontar para o `ac.pkg` original no USB, exibindo o tamanho original. Isso explicava o cenário de log `ALREADY_0B`, embora o explorador mostrasse o arquivo grande.

## Novo comportamento

- Varre recursivamente **somente** `/user/addcont` e `/mnt/ext0/user/addcont` a `/mnt/ext7/user/addcont`. Não depende de banco, CUSA, tamanho ou presença do arquivo no USB.
- Cada `ac.pkg` **nativo** (inclusive arquivo já vazio ou symlink) é removido com `unlink`, recriado com `O_CREAT|O_EXCL|O_NOFOLLOW` e verificado como arquivo regular de **0 bytes**. Uma falha de exclusão, criação, fsync ou validação é registrada como **falha**, não como sucesso.
- Quando o arquivo parecia existir só no overlay de propriedade do PS4Mount, preserva o inventário de nomes da pasta controlada pelo programa, desmonta a camada e cria as pastas e arquivos `ac.pkg` vazios necessários no **armazenamento físico nativo**. Não reativa esses overlays no final da limpeza.
- Não segue symlinks de diretórios e não modifica os PKGs originais `/mnt/usbN` ou `nullfs` de terceiros. Overlay desconhecido, empilhado, ocupado ou não desmontável gera falha sem forçar alteração.
- O marcador `/data/ps4mount/native-acpkg-cleaned.flag` impede que a **recuperação automática** do PS4Mount no próximo início/reconexão restaure os overlays de DLC, escondendo os stubs vazios. **Montagem/apontamento manual permanece disponível**, podendo fazer aparecer novamente o pacote do USB. Esse marcador não altera o banco de dados.
- O painel passou a apresentar **Recriados com 0 B** e **Falhas** (sem o antigo contador “Já estavam 0 B”).

## O que verificar no PS5

Depois de encerrar jogos e instalações, execute **Limpar DLCs** e abra o log. Procure estas linhas:

```text
[dlc-clean] SCAN_ROOT /mnt/ext1/user/addcont
[dlc-clean] OWN_OVERLAY_RELEASED /mnt/ext1/user/addcont/CUSA00363 inventory=...
[dlc-clean] RECREATED_0B_VERIFIED /mnt/ext1/user/addcont/CUSA00363/.../ac.pkg old_bytes=... new_bytes=0 USB_UNTOUCHED
[dlc-clean] CREATED_MISSING_NATIVE_0B_VERIFIED /mnt/ext1/user/addcont/CUSA00363/.../ac.pkg USB_UNTOUCHED
[dlc-clean] DONE roots=... ac_pkg_found=... RECREATED_0B=... FAILURES=... USB_UNTOUCHED
```

Para afirmar que a limpeza terminou sem falhas: `FAILURES=0` e o contador `RECREATED_0B` deve refletir o número de arquivos nativos efetivamente recriados. Arquivos não presentes e não expostos por um overlay próprio não são inventados. Se houver `UNMOUNT_FAILED`, `FOREIGN_OR_STACKED_OVERLAY`, `CRITICAL_RECREATE_FAILED` ou `VERIFY_FAILED`, **o processo não foi totalmente concluído**; examine os caminhos no log.

**Aviso:** esta ação remove os bytes de DLCs instaladas no armazenamento nativo. Não há rollback do conteúdo excluído. O banco de dados de DLCs não é limpo; os arquivos de USB não são tocados.

## Validação disponível

- `bash test/run.sh`: passou no ambiente de desenvolvimento.
- Teste funcional com filesystem falso: substituição de arquivos preenchidos/0B/symlinks, DLC criada a partir de inventário de overlay próprio, overlay deixado desmontado e USB não alterado.
- `cc -fsyntax-only` com os stubs host: passou; um warning preexistente fora do novo trecho.
- **Não compilado com o SDK do PS5 e não executado no console real.** A compatibilidade real da desmontagem e das permissões deve ser verificada no hardware.
