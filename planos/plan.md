# Plano de Correção — Explorador de Arquivos Web (mps3)

Escopo: `1_esp32s3_player/components/wifi_transfer/` (back-end `wifi_transfer.c` + front-end `web/index.html`).

Objetivo: corrigir o bug que impede mover pastas para níveis inferiores, fechar uma falha de segurança encontrada durante a revisão, e aplicar otimizações de performance/robustez identificadas no código.

---

## 🔴 Prioridade 1 — Bug relatado: "só deixa mover para níveis superiores"

### Causa raiz confirmada

O back-end (`api_list_get_handler` em `wifi_transfer.c`) sempre responde no formato:

```json
{"entries": [ {"name": "...", "dir": true/false, "size": 0}, ... ]}
```

O explorador principal (`load()` em `index.html`) lê isso corretamente:

```js
let files = (await r.json()).entries || [];
```

Mas a função que lista subpastas dentro do modal **"Mover para..."** lê a chave errada:

```js
// renderMoveFolderList(), em index.html
const dirs = (data.files || []).filter(f => f.dir && ...);
//            ^^^^^^^^^^^ deveria ser data.entries
```

Como `data.files` nunca existe na resposta, o array fica sempre vazio — nenhuma subpasta aparece, exceto o item fixo de "**.. (Subir uma pasta)**", que é adicionado manualmente fora do fetch. É por isso que o modal só permite subir de nível.

### Correção

**1.1 — Trocar a chave lida no JS**

```diff
- const dirs = (data.files || []).filter(f => f.dir && (!moveTargetItem || !moveTargetItem.isDir || f.name !== moveTargetItem.name));
+ const dirs = (data.entries || []).filter(f => f.dir && (!moveTargetItem || !moveTargetItem.isDir || f.name !== moveTargetItem.name));
```

**1.2 — Bloquear mover uma pasta para dentro dela mesma (ou de uma subpasta dela)**

O filtro atual só impede mover para o mesmo nível imediato. Nada impede navegar para dentro de uma subpasta da própria pasta sendo movida e confirmar ali, criando uma referência circular que corrompe a árvore no FatFS.

```diff
document.getElementById('btnConfirmMove').onclick = async () => {
  if (!moveTargetItem) return;
+ if (moveTargetItem.isDir &&
+     (moveCurrentDir === moveTargetItem.path || moveCurrentDir.startsWith(moveTargetItem.path + '/'))) {
+   alert('Não é possível mover uma pasta para dentro dela mesma.');
+   return;
+ }
  let newPath = joinPath(moveCurrentDir, moveTargetItem.name);
  ...
```

**1.3 — Revisão do handler `/api/move` no back-end (CONFIRMADA)**

Código real encontrado em `wifi_transfer.c`:

```c
static esp_err_t api_rename_post_handler(httpd_req_t *req) {
    ...
    get_json_string(buf, "old_path", old_path, sizeof(old_path));
    get_json_string(buf, "new_path", new_path, sizeof(new_path));

    if (!build_abs_path(old_path, old_abs, sizeof(old_abs)) ||
        !build_abs_path(new_path, new_abs, sizeof(new_abs))) { ... }

    mkdir_p_for_file(new_abs);
    if (rename(old_abs, new_abs) != 0) { ... }
    ...
}

static esp_err_t api_move_post_handler(httpd_req_t *req) {
    return api_rename_post_handler(req);   // /api/move É /api/rename por baixo
}
```

Checklist do que foi pedido para confirmar:

- [x] **Usa `rename()`** — confirmado, operação atômica no mesmo filesystem, sem copiar+apagar. Correto.
- [x] **Valida os dois caminhos contra `..`/path traversal** — confirmado, ambos passam por `build_abs_path()`, que já rejeita qualquer `..` na string. Correto.
- [ ] **Rejeita mover um caminho para dentro de si mesmo** — **NÃO existe essa checagem**. O handler aceita qualquer `old_path`/`new_path` que passem em `build_abs_path()` e chama `rename()` direto. Se `new_abs` for um descendente de `old_abs` (ex: mover `/Rock` para `/Rock/Sub`), o comportamento fica por conta do driver FatFS do ESP-IDF — pode falhar silenciosamente, corromper a estrutura de diretórios, ou (dependendo da implementação) entrar em um estado inconsistente. **Confirma a necessidade do item 1.2, e mostra que a proteção não pode viver só no front-end.**
- [ ] **Retorna erro claro se o destino já existir** — **NÃO existe essa checagem** antes do `rename()`. O comportamento de `rename()` sobre um destino já existente depende da implementação do FatFS (pode sobrescrever silenciosamente ou falhar) — vale testar esse caso especificamente e, se sobrescrever sem aviso, adicionar uma checagem `stat(new_abs)` antes do rename para pedir confirmação.

**Ação adicionada ao plano**: incluir a mesma checagem de "destino não pode ser o próprio caminho ou descendente dele" dentro de `api_rename_post_handler()` (compartilhada por `/api/rename` e `/api/move`, já que um chama o outro):

```c
// logo após validar old_abs e new_abs, antes do mkdir_p_for_file:
size_t old_len = strlen(old_abs);
if (strncmp(old_abs, new_abs, old_len) == 0 &&
    (new_abs[old_len] == '/' || new_abs[old_len] == '\0')) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, "{\"error\":\"destino nao pode ser o proprio item ou uma subpasta dele\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
}
```

**1.4 — Testes manuais após aplicar 1.1–1.3**

- [ ] Mover arquivo entre pastas irmãs
- [ ] Mover arquivo/pasta para uma pasta-neta (2+ níveis abaixo)
- [ ] Tentar mover uma pasta para dentro dela mesma → deve bloquear com mensagem clara
- [ ] Tentar mover para um caminho com `../` manipulado via DevTools → deve ser rejeitado pelo back-end mesmo que o front-end não bloqueie

---

## 🟠 Prioridade 2 — Falha de segurança encontrada na revisão

Não é o bug pedido, mas apareceu durante a leitura do JS e é grave o suficiente para entrar no plano.

```js
function submitLogin() {
  let pwd = document.getElementById('djPass').value;
  if (pwd === "1234") { // Temporary hardcoded password for Phase 1
    localStorage.setItem('mps3_admin', 'true');
```

A senha está em texto puro no JavaScript do cliente, e a "autenticação" é apenas uma flag no `localStorage` do navegador. Qualquer pessoa na mesma rede Wi-Fi do mps3 pode abrir o DevTools e rodar `localStorage.setItem('mps3_admin','true')` — sem precisar da senha — e ganhar acesso total (mover, apagar, sobrescrever arquivos).

### Correção recomendada

- [ ] Mover a checagem de senha para o back-end: endpoint `/api/login` que valida a senha no ESP32-S3 e devolve um token/cookie de sessão
- [ ] Todo handler sensível (`/api/upload`, `/api/delete`, `/api/move`, `/api/rename`, `/api/mkdir`, `/api/copy`) passa a exigir esse token
- [ ] Tornar a senha configurável via NVS (mesmo padrão já usado para as redes Wi-Fi salvas), em vez de fixa no firmware

**Confirmado na leitura completa do back-end**: nenhum dos handlers de mutação (`api_rename_post_handler`, `api_move_post_handler`, `api_mkdir_post_handler`, `api_copy_post_handler`, `api_delete_handler`, `api_upload_put_handler`) faz qualquer checagem de sessão/token — todos processam a requisição assim que ela chega. Ou seja, o "admin" do front-end é puramente decorativo hoje: mesmo sem a flag de `localStorage`, uma requisição HTTP direta (via `curl`, por exemplo) para qualquer um desses endpoints já funciona sem senha nenhuma. Isso eleva a prioridade da correção 2 — não é só "burlar o botão de login", é "não existe proteção nenhuma no servidor".

---

## 🟡 Prioridade 3 — Otimizações identificadas

| # | Otimização | Justificativa | Arquivo |
|---|---|---|---|
| 3.1 | Reaproveitar o `dirCache` já existente dentro do modal de mover, em vez de sempre buscar via `fetch` | Se a pasta já foi visitada no explorador principal, evita requisição HTTP/leitura de SD redundante a cada clique de navegação dentro do modal | `index.html` |
| 3.2 | Invalidar/atualizar o cache das pastas de **origem e destino** após mover, apagar, renomear ou criar pasta | Hoje só `curDir` é re-buscada (via `load()`). Se o usuário mover algo para uma pasta B que já estava em `dirCache` de uma visita anterior, essa entrada fica desatualizada até ele clicar nela de novo | `index.html` |
| 3.3 | Remover o bloco `#if 0 // EXPERIMENTAL_RADIO_SERVER` (~150 linhas mortas) | Código desabilitado ocupa espaço de flash e binário, e atrapalha qualquer revisão futura. Se não vai ser usado, remover; se vai, terminar e testar | `wifi_transfer.c` |
| 3.4 | Paginação/carregamento incremental em `/api/list` para pastas muito grandes | A resposta atual monta o JSON inteiro da pasta numa passada (mesmo em chunks HTTP). Pastas com milhares de arquivos podem fragmentar heap ou deixar a resposta lenta. Prioridade baixa se as pastas normalmente têm dezenas/centenas de faixas | `wifi_transfer.c` |
| 3.5 | Centralizar/documentar o contrato de nomes de campo do JSON (`entries`, `dir`, `name`, `size`) | O bug da Prioridade 1 existe porque esse contrato só existe "de cabeça" — nenhuma checagem impede um endpoint novo de usar um nome de campo diferente. Um comentário centralizado no topo do arquivo (ou uma lib mínima tipo cJSON) reduz a chance de repetição | `wifi_transfer.c` + `index.html` |
| 3.6 | `mkdir_p_for_file()` roda em toda chamada de upload, mesmo quando a pasta já existe | `mkdir()` numa pasta existente falha silenciosamente (sem quebrar nada), mas é uma syscall desnecessária por segmento de caminho a cada upload. Otimização pequena: cachear quais pastas já foram criadas na sessão de transferência atual | `wifi_transfer.c` |
| 3.7 | **Duas funções de parsing JSON manual duplicadas** (`json_extract_field()` na linha 953 e `get_json_string()` na linha 1282, fazendo essencialmente a mesma coisa) | Confirmado na leitura completa. Isso é exatamente o tipo de duplicação que gera bugs como o da Prioridade 1 — se um dia alguém mexer numa das duas achando que corrige as duas, ou usar a errada num handler novo, o comportamento diverge silenciosamente. Unificar numa só função (ou trocar por cJSON) | `wifi_transfer.c` |
| 3.8 | `/api/copy` está registrado no servidor (`api_copy_post_handler`) mas não é chamado por nenhum lugar do `index.html` revisado | Não é um bug, mas é código morto do ponto de vista da UI atual — vale decidir se vira uma função real (duplicar arquivo/pasta) exposta no menu de contexto, ou se é removido para reduzir superfície de manutenção | `wifi_transfer.c` + `index.html` |

---

## 🟢 Prioridade 4 — Checklist de validação final

- [ ] Fluxo completo: abrir "Mover para...", navegar 2-3 níveis, confirmar — deve funcionar (Prioridade 1)
- [ ] Tentar mover pasta para dentro dela mesma — deve bloquear (1.2)
- [ ] Tentar burlar login via `localStorage` no DevTools antes/depois da correção (Prioridade 2)
- [ ] Upload e movimentação de arquivos simultâneos com música tocando — garantir estabilidade da concorrência de acesso ao SD
- [ ] Testar em pasta com muitos arquivos (50+) para avaliar necessidade real do item 3.4

---

## Status da revisão

Revisão completa de `wifi_transfer.c` (1975 linhas) e das ~1000 primeiras linhas de `index.html` concluída. Todos os itens de back-end levantados na Prioridade 1 e 2 foram confirmados diretamente no código-fonte. Only pendência real: revisar o restante de `index.html` (linha 1000 em diante — upload em lote, drag-and-drop, extração de metadata) para garantir que nenhuma outra função do front-end tem o mesmo tipo de erro de nome de campo do item 1.1.
