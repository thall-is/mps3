# MPS3 Mobile Relay & Tailscale Bridge

Este diretório contém as ferramentas para permitir que o player **MPS3 (ESP32-S3)** sincronize seus podcasts a partir do servidor **Radxa Rock 3E (Tailscale: `100.120.93.115:8088`)** quando você estiver fora de casa, utilizando seu smartphone como ponte.

---

## 🚀 Método 1: Ponte pelo Navegador Web (Mais Fácil - Zero Apps)

1. No seu smartphone, ligue a **VPN Tailscale** e conecte o celular à rede Wi-Fi do MPS3 (ou ligue o Hotspot do celular para o MPS3 se conectar).
2. Abra o navegador do celular e acerte:
   - `http://mps3.local` ou `http://192.168.4.1` (ou o IP do MPS3).
3. Na barra superior de arquivos, clique no botão **`🎙️ Podcasts`**.
4. O modal já vem configurado com `http://100.120.93.115:8088`:
   - Clique em **`Testar`** para validar o acesso à Tailscale.
   - Clique em **`🔍 Verificar Novos Episódios`**: o navegador compara o catálogo do servidor com os episódios já gravados no SD Card.
   - Clique em **`Baixar e Gravar no SD`**: o celular baixa os episódios do servidor via 4G/5G pela Tailscale e envia direto para o cartão SD do MPS3 em segundo plano, com barra de progresso em tempo real e animação de transferência no display OLED do player!

---

## ⚙️ Método 2: Relay em Segundo Plano no Celular (Termux)

Se você preferir que o MPS3 sincronize de forma automática diretamente pelo firmware nativo (`podcast_sync.c`), basta rodar este relay no Android:

### Como configurar no Termux (Android):
1. Instale o app **Termux** (disponível no F-Droid ou GitHub).
2. Copie o arquivo `mps3_relay.py` para o Termux:
   ```bash
   pkg update && pkg install python -y
   curl -O https://raw.githubusercontent.com/.../mps3_relay.py
   python mps3_relay.py
   ```
3. O script ficará escutando na porta `8088` do gateway do Hotspot (`192.168.43.1:8088`).
4. Quando o MPS3 se conecta ao Hotspot, ele detecta automaticamente o gateway e busca os episódios através do relay!

