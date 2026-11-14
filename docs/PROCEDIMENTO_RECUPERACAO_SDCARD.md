# Procedimento Operacional Padrão: Recuperação de MicroSD Corrompido (FAT32) — MPS3

Este documento estabelece o procedimento técnico oficial de diagnóstico, reparo e resgate integral de dados quando o cartão MicroSD do **MPS3** apresenta corrupção de sistema de arquivos FAT32.

---

## 1. Sintomas e Diagnóstico

### Sintomas Visuais no Hardware
1. **OLED com Nomes Aleatórios**: A tela de navegação de arquivos (`UI_MODE_LIST`) exibe linhas com caracteres binários aleatórios (ex: `/¿\**...`, `/◊¿**...`, `/†¿¿*...`).
2. **Telemetria Serial (`ui`)**:
   ```json
   @UI {"mode":0,"mode_name":"LIST","cursor":4,"browse_root":true,"file":"","playing":false}
   ```
3. **Varredura Serial (`ls`)**:
   ```text
   SD_SCAN:   /sdcard/\xac\xe9\xf8\xca (1168.6 MB)
   SD_SCAN:   /sdcard/\xden\xde\xa9\x02\x83i\x8b.i\x99\x82 (1136.3 MB)
   SD_SCAN: [DIR] /sdcard/\xb8\xdf\xe4\x9b\xc2T\xe5\xe8.\xc8r\xba
   ```

### Diagnóstico no Host (Windows)
Ao conectar o MicroSD ao computador via adaptador USB ou modo Pendrive USB MSC:
1. `Get-Volume -DriveLetter D`:
   - `FriendlyName`: Corrompido (ex: `y)yI7à┌úK9`)
   - `HealthStatus`: `Warning`
   - `OperationalStatus`: **`Full Repair Needed`**
2. `Get-ChildItem D:\`: Erro de argumento (`Caracteres inválidos no caminho`).

---

## 2. Causas Raiz

* **Desconexão a Quente sem "Ejetar com Segurança"**: O Windows mantém tabelas e descritores FAT32 em cache de escrita em memória RAM do PC. Se o cabo USB for retirado sem o comando de ejeção do sistema operacional, o diretório raiz perde sincronia e é marcado como corrompido.
* **Queda de Energia Durante Escrita**: Desligamento do MPS3 enquanto gravava playlists, logs analíticos ou podcasts.
* **Incompatibilidade exFAT**: O firmware ESP-IDF do MPS3 opera em **FAT32** (`CONFIG_FATFS_SECTOR_4096=y`, `CONFIG_FATFS_MAX_LFN=255`). Cartões de 128 GB não devem ser formatados em exFAT ou NTFS.

---

## 3. Passo a Passo de Recuperação

### Passo 1: Reparo da Tabela FAT32 no Windows
Com o cartão montado (exemplo: unidade `D:`):
Abra o PowerShell ou Prompt de Comando e execute:
```powershell
Repair-Volume -DriveLetter D -OfflineScanAndFix
```
*Ou, alternativamente em Prompt de Administrador:*
```cmd
chkdsk D: /f
```
**Efeito**: O Windows normaliza o cabeçalho do volume, limpa o diretório raiz e move todos os clusters órfãos e arquivos desconectados para a pasta oculta `D:\FOUND.000` (como `FILE0000.CHK`, `FILE0001.CHK`, etc.).

---

### Passo 2: Execução da Ferramenta Oficial de Recuperação MPS3
O repositório dispõe da ferramenta automatizada [`tools/recover_mps3_sd.py`](../tools/recover_mps3_sd.py).

#### 1. Simulação (Dry-Run):
```bash
python tools/recover_mps3_sd.py --drive D:
```
*O script valida assinaturas binárias, extrai Vorbis Comments de FLAC, ID3v1/ID3v2 de MP3 e exibe o preview das músicas e arquivos de sistema.*

#### 2. Execução Real e Reorganização:
```bash
python tools/recover_mps3_sd.py --drive D: --execute --clean-found
```
*A movimentação ocorre no mesmo volume `D:`, sendo instantânea e com consumo de **zero bytes adicionais** de armazenamento.*

---

## 4. O que a Ferramenta Reconstrói

### 1. Biblioteca Musical (`/Musicas`)
* Varre os arquivos `.CHK` e identifica o formato real:
  - **FLAC**: Cabeçalho `fLaC`, bloco de metadados `VORBIS_COMMENT`. Extrai `ARTIST`, `ALBUM`, `TITLE`, `TRACKNUMBER` e embedded lyrics.
  - **MP3**: Cabeçalho `ID3` ou frames MPEG sync `0xFF 0xFB/F3/F2` com ID3v1 ao final.
  - **M4A / AAC**: Átomos `ftyp` e caixas `moov/udta`.
  - **OGG**: Páginas `OggS` e comentários Vorbis/Opus.
* Organiza em pastas padronizadas:
  `/sdcard/Musicas/<Artista>/<Album>/<Faixa> - <Título>.<ext>`

### 2. Podcasts e Episódios (`/Podcasts`)
* Isola episódios de canais cadastrados (NerdCast, Vortex, Hora Boa Podcast, Não Ficção, etc.).
* Aloca em `/sdcard/Podcasts/<Programa>/<Episódio>.<ext>`.

### 3. Arquivos de Sistema e Telemetria do Firmware
* **`/.mps3_analytics/tracks_summary.json`**: Reconstrói todas as linhas de telemetria histórica de scrobbles, plays, skips e tempo ouvido até o último segundo gravado.
* **`/.mps3_analytics/sessions_recovered.jsonl`**: Recompõe as sessões cronológicas de escuta.
* **`/.mps3_bookmarks.dat`**: Restaura os marcadores binários de retomada (`MPBM` / `0x4D50424D`).
* **`/Playlists/*.m3u8`**: Reconstrói as listas de reprodução em formato estendido `#EXTM3U`.
* **`/Podcasts/.subscriptions.json`**: Restaura os feeds RSS e canais de streaming.
* **`/Podcasts/.played_history`**: Restaura o índice `caminho|timestamp|duracao` dos episódios já reproduzidos.

---

## 5. Regra de Ouro para Prevenção
1. **Sempre ejetar com segurança** no Windows antes de desconectar o leitor USB ou sair do modo `usb-msc`.
2. Não formatar cartões de 64 GB / 128 GB em exFAT. Utilizar utilitários FAT32 dedicados (**GUIFormat** ou **Rufus**) com unidades de alocação de 32 KB ou 64 KB.

