# Plano de Implementação — Ordenação de Faixas no `mps3`

**Feature:** Adicionar modo de ordenação "por data de modificação" como alternativa à ordenação alfabética atual, configurável pelo usuário e persistida em NVS.

**Repositório:** `thall-is/mps3` — firmware `1_esp32s3_player`

**Autor do levantamento técnico:** análise direta do código-fonte atual (branch `main`)

---

## 1. Contexto e problema

Hoje, `scan_dir()` em `components/audio_player/fs_browser.cpp` ordena **sempre** por nome de arquivo (`strcasecmp` via `qsort`), sem nenhuma opção alternativa:

```cpp
static int compare_strs(const void *a, const void *b) {
    const char *sa = *(const char **)a;
    const char *sb = *(const char **)b;
    return strcasecmp(sa, sb);
}
...
qsort(out.audio_files, out.audio_count, sizeof(char*), compare_strs);
```

Isso funciona bem quando as faixas têm prefixo numérico (`01 -`, `02 -`), mas quebra a ordem pretendida pelo artista em qualquer pasta sem numeração consistente. Objetivo: dar ao usuário uma opção de ordenar por **data de última modificação** do arquivo (que normalmente reflete a ordem de cópia/download, mais próxima da tracklist original), mantendo o nome como padrão e default seguro.

---

## 2. Escopo da mudança

Toca **4 componentes** do firmware `1_esp32s3_player`:

| Componente | Arquivo(s) | Tipo de mudança |
|---|---|---|
| `audio_player` (fs) | `fs_browser.h`, `fs_browser.cpp` | Nova lógica de coleta de `mtime` + segundo critério de sort |
| `audio_player` (state/NVS) | `audio_player.cpp`, `include/audio_player.h` | Persistência da preferência + getter/setter |
| `touch_input` | `touch_input.h`, `touch_input.c` | Novo `UI_MODE_SORT`, navegação, integração no menu "Conf" |
| `oled_display` | `oled_display.h`, `oled_display.c` | Tela de exibição/alternância do modo de ordenação |
| `src` | `main.c` | Roteamento do novo modo de tela + atualização do array `conf_items` |

Não toca em `2_esp32_bt_companion` nem `3_esp32_bt_audio_sink` (irrelevante — só afeta leitura do SD no S3).

---

## 3. Design técnico

### 3.1. `fs_browser.h` / `fs_browser.cpp` — núcleo da ordenação

**Novo tipo:**

```cpp
enum class SortMode : uint8_t { ByName = 0, ByModifiedTime = 1 };
```

**Nova API pública:**

```cpp
void fs_browser_set_sort_mode(SortMode mode);
SortMode fs_browser_get_sort_mode(void);
```

**Struct interna para carregar nome + mtime juntos** (necessário porque hoje `DirScan.audio_files` é só um array de `char*`; não dá pra ordenar por mtime depois de perder essa informação):

```cpp
struct FileEntry {
    char *name;
    time_t mtime;
};
```

**Mudança em `scan_dir()`:**
- Ao encontrar `DT_REG` com extensão suportada, montar o caminho completo (`path + "/" + ent->d_name`) e chamar `stat()` para obter `st.st_mtime`.
- Guardar em um array temporário de `FileEntry` (pode ser alocado na stack com `MAX_ENTRIES`, já que a struct é pequena — 256 entradas × ~12 bytes = ~3 KB, aceitável na task do display/scan).
- Ordenar esse array temporário com `qsort`, usando o comparador conforme `s_sort_mode`:
  - `ByName`: `strcasecmp(a->name, b->name)` (comportamento atual).
  - `ByModifiedTime`: `a->mtime - b->mtime`, com **desempate por nome** quando os mtimes forem iguais (comum quando um lote de arquivos é copiado na mesma operação e o FS não tem granularidade de tempo suficiente).
- Copiar os ponteiros `name` já ordenados de volta para `out.audio_files[]`, na mesma ordem.
- Subpastas (`out.subdirs`) continuam **sempre** ordenadas por nome — não faz sentido ordenar pastas por data.

**Ponto de atenção de custo:** `stat()` em SDMMC tem custo de I/O por chamada. Para uma pasta com ~20-30 faixas isso é desprezível, mas vale medir tempo de `scan_dir()` antes/depois em uma pasta com o número máximo de entradas (`MAX_ENTRIES = 256`) para garantir que não introduz travamento perceptível na navegação (ver seção de testes).

### 3.2. `audio_player.cpp` — persistência em NVS

Seguir exatamente o padrão já usado para `volume` e `balance`:

```cpp
#define NVS_KEY_SORT_MODE "sort_mode"
```

Novas funções espelhando `audio_player_get_volume()` / `audio_player_set_volume()`:

```cpp
void audio_player_set_sort_mode(SortMode mode); // grava em NVS + chama fs_browser_set_sort_mode()
SortMode audio_player_get_sort_mode(void);       // lê NVS (default ByName se chave não existir)
```

Carregar a preferência salva **no boot**, antes da primeira chamada a `scan_dir()` (mesmo ponto onde hoje volume/balance são restaurados), para que a pasta inicial já apareça ordenada corretamente.

### 3.3. `touch_input.h` / `touch_input.c` — novo modo de UI

**Novo valor de enum** (próximo livre — os atuais vão até 14):

```cpp
UI_MODE_SORT = 15,
```

**Menu "Conf" hoje tem 5 itens** (`Volume`, `Balanco L/R`, `Equalizador`, `LED RGB`, `Tela`), navegados por `s_list_cursor` com wraparound `% 5` em duas posições do código (`JOY_UP` e `JOY_DOWN` dentro de `UI_MODE_CONF_MENU`). Precisa:
1. Trocar os dois wraparounds de `% 5` → `% 6` (e o `4` fixo do "voltar pro topo" para `5`).
2. Adicionar `else if (s_list_cursor == 5) { s_mode = UI_MODE_SORT; }` no bloco `JOY_RIGHT` de `UI_MODE_CONF_MENU`.
3. Dentro de `UI_MODE_SORT`, tratar `JOY_LEFT`/`JOY_RIGHT` (ou `JOY_CENTER`) para alternar entre `ByName` e `ByModifiedTime`, chamando `audio_player_set_sort_mode()` a cada alternância e re-executando `scan_dir()` na pasta atual (`s_browse_dir`) para refletir a mudança imediatamente.
4. `JOY_LEFT` segurado (ou botão de "voltar" já usado nas outras telas de Conf) retorna para `UI_MODE_CONF_MENU`, igual às demais subtelas.

Expor getter de estado para a tela, similar ao padrão de `touch_input_get_tela_cursor()`, ou simplesmente ler direto de `audio_player_get_sort_mode()` — como a preferência já vive centralizada no `audio_player`, não precisa duplicar estado dentro de `touch_input`.

### 3.4. `oled_display.c` — tela nova

Criar `oled_display_show_sort_mode(SortMode mode)`, no mesmo estilo simples de `oled_display_show_led` / `oled_display_show_tela`: título ("Ordenar faixas"), e destaque visual da opção ativa entre as duas ("Nome A-Z" / "Data (recente→antiga)" ou a ordem que fizer mais sentido testando na prática — sugiro mais antiga→mais recente, que tende a acompanhar ordem de cópia/tracklist).

### 3.5. `src/main.c` — roteamento

1. Atualizar o array `conf_items`:
   ```cpp
   const char* conf_items[] = {"Volume", "Balanco L/R", "Equalizador", "LED RGB", "Tela", "Ordenar"};
   oled_display_show_list(conf_items, 6, touch_input_get_list_cursor(), &state);
   ```
2. Adicionar um `else if (touch_input_get_mode() == UI_MODE_SORT) { oled_display_show_sort_mode(audio_player_get_sort_mode()); }` no mesmo bloco condicional onde ficam `UI_MODE_VOLUME`, `UI_MODE_LED`, etc.

---

## 4. Fases de implementação (ordem sugerida)

### Fase 1 — Núcleo de ordenação (sem UI)
- [ ] Implementar `SortMode`, `FileEntry`, comparador por mtime e integração em `scan_dir()` (`fs_browser.h/.cpp`).
- [ ] Implementar `fs_browser_set_sort_mode()` / `fs_browser_get_sort_mode()`.
- [ ] Teste manual temporário: forçar `SortMode::ByModifiedTime` via código fixo (sem UI ainda) e validar em uma pasta de teste que a ordem bate com a data real dos arquivos (`ls -la --time-style=full-iso` no cartão antes de inserir no player).
- **Entregável:** PR isolado, só tocando `fs_browser.*`, com o modo ainda não exposto ao usuário (hardcoded ou via `#ifdef` de debug).

### Fase 2 — Persistência (NVS)
- [ ] Adicionar `NVS_KEY_SORT_MODE`, `audio_player_get_sort_mode()`, `audio_player_set_sort_mode()`.
- [ ] Carregar a preferência no boot, antes do primeiro `scan_dir()`.
- [ ] Teste manual: setar o modo, reiniciar o dispositivo (reset físico), confirmar que a preferência sobrevive.

### Fase 3 — UI (menu "Conf" + tela de ordenação)
- [ ] Novo `UI_MODE_SORT` em `touch_input.h`.
- [ ] Ajustar wraparounds de 5→6 itens no menu Conf (`touch_input.c`).
- [ ] Navegação dentro da nova tela (`JOY_LEFT`/`JOY_RIGHT` alternando modo + re-scan da pasta atual).
- [ ] `oled_display_show_sort_mode()`.
- [ ] Atualizar `conf_items[]` e roteamento em `main.c`.
- **Entregável:** PR que finalmente expõe a feature ponta a ponta no dispositivo físico.

### Fase 4 — Polimento e testes de campo
- [ ] Validar em pelo menos 3 cenários reais de biblioteca (ver seção 6).
- [ ] Medir impacto de performance do `stat()` em pasta com ~256 arquivos.
- [ ] Atualizar `docs/MANUAL.md` com a nova opção de menu.
- [ ] Atualizar o README (seção "O Que Funciona Muito Bem") mencionando a nova opção de ordenação.

---

## 5. Divisão de trabalho sugerida (para a equipe)

| Frente | Arquivos principais | Perfil ideal |
|---|---|---|
| **A — Lógica de ordenação** | `fs_browser.h/.cpp` | Quem já mexeu com FatFS/SDMMC no projeto |
| **B — Persistência e API pública** | `audio_player.cpp/.h` | Quem manja do padrão de NVS já usado (volume/balance) |
| **C — UI/Menu** | `touch_input.c/.h`, `oled_display.c/.h`, `main.c` | Quem já mexeu no menu Conf / telas OLED |
| **D — Testes de campo + docs** | `docs/MANUAL.md`, README | Qualquer pessoa da equipe, mas idealmente com acesso físico ao hardware |

Frentes A e B podem rodar **em paralelo** (não têm dependência direta entre si além da assinatura da API). Frente C depende de A+B estarem mergeadas (precisa de `SortMode` e dos getters/setters prontos). Frente D roda depois de C.

---

## 6. Plano de testes

Como é firmware embarcado, não há testes unitários automatizados fáceis para `scan_dir()` sem mockar `dirent`/`stat` — o plano é essencialmente **teste manual dirigido por cenário**, no hardware real:

1. **Álbum bem numerado** (`01 -`, `02 -`...): ordenar por Nome deve dar a tracklist correta (regressão — comportamento atual não pode quebrar). Ordenar por Data deve, idealmente, também bater (se os arquivos foram copiados em ordem).
2. **Álbum sem numeração**, arquivos copiados manualmente **fora de ordem** para o cartão, mas depois copiados novamente na ordem correta um por um (para gerar mtimes crescentes corretos): ordenar por Data deve corrigir a sequência; ordenar por Nome deve continuar embaralhado (confirma que a feature resolve o problema relatado).
3. **Pasta com muitos arquivos** (próximo de `MAX_ENTRIES = 256`): medir tempo de `scan_dir()` com Nome vs. Data (log via `ESP_LOGI` com timestamp antes/depois) para garantir que o `stat()` extra não introduz atraso perceptível ao entrar na pasta.
4. **Lote copiado de uma vez** (todos os arquivos com o mesmo `mtime` por causa da granularidade do FAT ou de uma cópia em lote/rsync): validar que o desempate por nome evita ordem aleatória/indefinida.
5. **Persistência**: setar "Data", desligar/religar o player, confirmar que abre já ordenado por Data.
6. **Transferência via Wi-Fi (`mps3.local`)**: como esse é outro caminho de escrita no cartão (`wifi_transfer.c`), confirmar que arquivos enviados por essa via também recebem `mtime` coerente e respeitam a ordenação escolhida.
7. **Playback não deve pular/trocar de faixa** ao alternar o modo de ordenação enquanto uma música está tocando em outra pasta — validar que o re-scan do Fase 3 só afeta `s_browse_dir` (navegação), não `s_playback_scan` da faixa atualmente em reprodução.

---

## 7. Riscos e mitigação

| Risco | Mitigação |
|---|---|
| `stat()` por arquivo deixa a navegação de pastas grandes perceptivelmente mais lenta | Medir na Fase 4; se necessário, cachear `mtime` já coletado durante a varredura do diretório (`dirent` no FatFS do ESP-IDF já expõe alguns metadados adicionais em certas configurações — vale investigar se dá pra evitar o `stat()` extra por arquivo antes de assumir que é necessário) |
| Cartões com relógio de sistema divergente / arquivos com mtime "zerado" (1970) por escrita direto de ferramentas sem RTC | Sempre ter "Nome" como default e fallback simples; considerar tratar mtime == 0 como "sem data" e cair para ordenação por nome nesses casos específicos, isoladamente |
| Quebrar o wraparound existente do menu Conf (5→6 itens) e introduzir regressão de navegação nos outros itens | Testar manualmente a navegação completa do menu Conf (ida e volta, incluindo wraparound nas duas pontas) antes de mergear a Fase 3 |
| Usuários que já têm o hábito de nomear arquivos com prefixo numérico não percebem diferença e acham a feature desnecessária | Não é um risco técnico, mas vale deixar claro no README/manual que a opção existe justamente para quem **não** numera os arquivos |

---

## 8. Critérios de aceite (Definition of Done)

- [ ] Existe uma opção "Ordenar" no menu Conf, navegável por joystick, alternando entre "Nome" e "Data".
- [ ] A preferência escolhida sobrevive a reinicializações do dispositivo (persistida em NVS).
- [ ] Trocar o modo de ordenação atualiza a lista da pasta atualmente aberta imediatamente, sem precisar sair e reentrar na pasta.
- [ ] Ordenação por Nome continua idêntica ao comportamento atual (nenhuma regressão).
- [ ] Testado em pelo menos os cenários 1, 2 e 4 da seção 6, em hardware real.
- [ ] `docs/MANUAL.md` e README atualizados.
