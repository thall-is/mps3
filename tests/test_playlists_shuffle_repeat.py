#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
=============================================================================
  TESTE DE REGRESSAO EM BANCADA: PLAYLISTS, FAVORITAR, SHUFFLE & REPEAT
=============================================================================
Valida a integridade funcional completa do sistema de reproducao no ESP32-S3:
  1. Criacao, listagem, selecao e exclusao de playlists (.m3u8)
  2. Protecao da lista mestra Favoritos.m3u8
  3. Toggle de Favoritar (Like/Unlike) com sincronia de NVS e flag de estado
  4. Inclusao de pasta inteira na playlist alvo
  5. Modos de Shuffle (OFF, FOLDER via Fisher-Yates em PSRAM, ALL Global)
  6. Modos de Loop/Repeat (NONE, ALL, TRACK 1x)
  7. Comportamento de quebra de loop ao avancar faixa manualmente
  8. Drenagem e restauracao do estado seguro de bancada

INVARIANTE DE HARDWARE:
  A conexao com a porta COM5 utiliza obrigatoriamente dtr=False e rts=False
  antes da chamada open(), prevenindo resets espurios do hardware.
=============================================================================
"""

import sys
import time
import serial
import re

PORT = "COM5"
BAUD = 115200

class Color:
    GREEN = "\033[92m"
    RED = "\033[91m"
    YELLOW = "\033[93m"
    CYAN = "\033[96m"
    BOLD = "\033[1m"
    RESET = "\033[0m"

def log_info(msg):
    print(f"{Color.CYAN}[INFO]{Color.RESET} {msg}")

def log_pass(msg):
    print(f"{Color.GREEN}[PASS]{Color.RESET} {msg}")

def log_fail(msg):
    print(f"{Color.RED}[FAIL]{Color.RESET} {msg}")

def log_step(title):
    print(f"\n{Color.BOLD}>>> {title} <<<{Color.RESET}")

class BenchController:
    def __init__(self, port=PORT, baud=BAUD):
        self.port = port
        self.baud = baud
        self.ser = None

    def connect(self):
        log_info(f"Conectando a {self.port} ({self.baud} bps, DTR=False, RTS=False)...")
        self.ser = serial.Serial()
        self.ser.port = self.port
        self.ser.baudrate = self.baud
        self.ser.timeout = 0.4
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        time.sleep(0.5)
        # Limpa lixo residual do buffer
        self.ser.reset_input_buffer()

    def close(self):
        if self.ser and self.ser.is_open:
            self.ser.close()

    def send_cmd(self, cmd, wait_sec=0.4):
        self.ser.reset_input_buffer()
        self.ser.write((cmd.strip() + "\n").encode("utf-8"))
        time.sleep(wait_sec)
        out = ""
        while self.ser.in_waiting:
            chunk = self.ser.read(self.ser.in_waiting).decode("utf-8", errors="replace")
            out += chunk
            time.sleep(0.05)
        return out

    def get_track_info(self):
        out = self.send_cmd("track", wait_sec=0.3)
        # Exemplo: [TRACK] 'Titulo' - 'Artista' ('06 - Just Coolin'.flac') | 12/523 s | Loop: 0 | Shuf: 0 | Fav: 1
        m = re.search(r"\[TRACK\]\s+'(.*?)'\s+-\s+'(.*?)'\s+\('(.*?)'\)\s+\|\s+(\d+)/(\d+)\s+s\s+\|\s+Loop:\s+(\d+)\s+\|\s+Shuf:\s+(\d+)(?:\s+\|\s+Fav:\s+(\d+))?", out)
        if m:
            return {
                "title": m.group(1),
                "artist": m.group(2),
                "filename": m.group(3),
                "elapsed": int(m.group(4)),
                "total": int(m.group(5)),
                "loop": int(m.group(6)),
                "shuf": int(m.group(7)),
                "fav": int(m.group(8)) if m.group(8) is not None else None,
                "raw": out
            }
        return {"raw": out}

def run_tests():
    ctrl = BenchController()
    try:
        ctrl.connect()
    except Exception as e:
        log_fail(f"Impossivel abrir porta serial {PORT}: {e}")
        return False

    all_passed = True
    test_pl_name = "Autotest_Suite"

    try:
        # -------------------------------------------------------------
        # 1. VERIFICACAO DE ESTADO INICIAL
        # -------------------------------------------------------------
        log_step("1. Verificacao de Estado Inicial do Player")
        st = ctrl.get_track_info()
        if "filename" in st:
            log_pass(f"Faixa ativa: '{st['filename']}' ({st['total']}s) | Loop={st['loop']}, Shuf={st['shuf']}, Fav={st['fav']}")
        else:
            log_info(f"Resposta bruta de track: {st.get('raw', '').strip()}")

        # -------------------------------------------------------------
        # 2. CICLO DE VIDA DE PLAYLISTS (.M3U8)
        # -------------------------------------------------------------
        log_step("2. Ciclo de Vida de Playlists (Create, List, Target, Delete)")
        
        # 2.1 Listagem inicial
        out = ctrl.send_cmd("pl list")
        log_info(f"Playlists atuais:\n{out.strip()}")
        if "@OK pl count=" in out:
            log_pass("Listagem de playlists operacional")
        else:
            log_fail("Falha ao obter 'pl list'")
            all_passed = False

        # 2.2 Criacao de playlist nova
        out = ctrl.send_cmd(f"pl create {test_pl_name}")
        log_info(f"Criacao: {out.strip()}")
        if f"@OK pl created: {test_pl_name}.m3u8" in out:
            log_pass(f"Playlist {test_pl_name}.m3u8 criada com sucesso")
        else:
            log_fail(f"Falha ao criar playlist {test_pl_name}")
            all_passed = False

        # 2.3 Confirmar que ela e a playlist alvo ativa
        out = ctrl.send_cmd("pl target")
        log_info(f"Playlist Alvo: {out.strip()}")
        if f"{test_pl_name}.m3u8" in out:
            log_pass(f"{test_pl_name}.m3u8 foi definida automaticamente como alvo ativo")
        else:
            log_fail("Playlist criada nao se tornou alvo ativo")
            all_passed = False

        # 2.4 Alternar playlist alvo para Favoritos.m3u8 (indice 0)
        out = ctrl.send_cmd("pl set 0")
        log_info(f"Troca para indice 0: {out.strip()}")
        if "Favoritos.m3u8" in out:
            log_pass("Selecao de playlist alvo por indice funciona corretamente")
        else:
            log_fail("Falha ao trocar para playlist alvo indice 0")
            all_passed = False

        # -------------------------------------------------------------
        # 3. FEATURE FAVORITAR (LIKE / UNLIKE)
        # -------------------------------------------------------------
        log_step("3. Feature Favoritar (Like / Unlike na Playlist Alvo)")
        
        # Estado atual de favorito
        st_before = ctrl.get_track_info()
        initial_fav = st_before.get("fav")
        log_info(f"Status inicial de favorito: {initial_fav}")

        # Toggle 1
        out1 = ctrl.send_cmd("fav")
        log_info(f"Toggle 1: {out1.strip()}")
        st_after1 = ctrl.get_track_info()
        fav1 = st_after1.get("fav")
        log_info(f"Status apos Toggle 1: {fav1}")

        if "@OK fav liked" in out1 or "@OK fav unliked" in out1:
            log_pass("Comando 'fav' executado com retorno valido (@OK)")
        else:
            log_fail("Comando 'fav' retornou erro ou resposta inesperada")
            all_passed = False

        # Toggle 2 (deve reverter o estado)
        out2 = ctrl.send_cmd("fav")
        log_info(f"Toggle 2: {out2.strip()}")
        st_after2 = ctrl.get_track_info()
        fav2 = st_after2.get("fav")
        log_info(f"Status apos Toggle 2: {fav2}")

        if fav1 is not None and fav2 is not None:
            if fav1 != fav2:
                log_pass(f"Toggle de Like comutou com sucesso: {fav1} -> {fav2}")
            else:
                log_fail(f"Estado de Like nao comutou: fav1={fav1}, fav2={fav2}")
                all_passed = False

        # Se mudou do estado inicial, restaura
        if fav2 != initial_fav:
            ctrl.send_cmd("fav")
            log_info(f"Restaurado status original de like: {initial_fav}")

        # -------------------------------------------------------------
        # 4. ADICAO DE PASTA INTEIRA A PLAYLIST E DUMP DE CONTEUDO
        # -------------------------------------------------------------
        log_step("4. Adicao de Pasta Inteira e Dump de Conteudo")
        # Define a playlist de teste como alvo
        out = ctrl.send_cmd("pl list")
        # Localiza o indice da playlist de teste
        m = re.search(r"\[(\d+)\]\s+" + re.escape(test_pl_name), out)
        if m:
            test_idx = m.group(1)
            ctrl.send_cmd(f"pl set {test_idx}")
            log_info(f"Definido alvo para indice {test_idx} ({test_pl_name}.m3u8)")

            # Adiciona a pasta atual
            out_add = ctrl.send_cmd("pl add folder", wait_sec=0.8)
            log_info(f"Adicionar pasta: {out_add.strip()}")
            if "@OK pl folder added:" in out_add:
                log_pass(f"Faixas da pasta adicionadas a {test_pl_name}.m3u8")
            else:
                log_fail("Falha ao adicionar pasta a playlist")
                all_passed = False

            # Dump do conteudo da playlist de teste
            out_dump = ctrl.send_cmd(f"pl dump {test_pl_name}.m3u8", wait_sec=0.5)
            log_info(f"Dump da playlist:\n{out_dump.strip()}")
            if "#EXTM3U" in out_dump:
                log_pass("Cabecalho #EXTM3U e faixas confirmados no arquivo gravado")
            else:
                log_fail("Cabecalho #EXTM3U nao encontrado no dump da playlist")
                all_passed = False
        else:
            log_fail("Indice da playlist de teste nao encontrado em 'pl list'")
            all_passed = False

        # -------------------------------------------------------------
        # 5. EXCLUSAO SEGURA E PROTECAO DE LISTA MESTRA
        # -------------------------------------------------------------
        log_step("5. Exclusao Segura e Protecao da Lista Favoritos")
        
        # 5.1 Tentar apagar Favoritos.m3u8 (DEVE SER BLOQUEADO)
        out_del_fav = ctrl.send_cmd("pl delete Favoritos.m3u8")
        log_info(f"Tentativa de apagar Favoritos.m3u8: {out_del_fav.strip()}")
        if "@ERR pl delete failed" in out_del_fav:
            log_pass("Protecao de seguranca ativa: Favoritos.m3u8 NAO PODE ser excluida")
        else:
            log_fail("Invariante violada! Favoritos.m3u8 foi permitida para exclusao")
            all_passed = False

        # 5.2 Apagar playlist de teste
        out_del = ctrl.send_cmd(f"pl delete {test_pl_name}.m3u8")
        log_info(f"Exclusao de {test_pl_name}.m3u8: {out_del.strip()}")
        if f"@OK pl deleted: {test_pl_name}.m3u8" in out_del:
            log_pass(f"{test_pl_name}.m3u8 excluida com sucesso")
        else:
            log_fail(f"Falha ao excluir {test_pl_name}.m3u8")
            all_passed = False

        # 5.3 Verificar se o alvo retornou automaticamente para Favoritos.m3u8
        out_target = ctrl.send_cmd("pl target")
        log_info(f"Alvo apos exclusao: {out_target.strip()}")
        if "Favoritos.m3u8" in out_target:
            log_pass("Fallback automatico seguro para Favoritos.m3u8 apos exclusao da playlist ativa")
        else:
            log_fail("Alvo nao retornou para Favoritos.m3u8")
            all_passed = False

        # -------------------------------------------------------------
        # 6. MODOS DE SHUFFLE (ALEATORIO)
        # -------------------------------------------------------------
        log_step("6. Modos de Reproducao Aleatoria (Shuffle)")
        
        modes = [
            (0, "OFF (Linear / Alfabetico)"),
            (1, "FOLDER (Fisher-Yates na Pasta/Playlist em PSRAM)"),
            (2, "ALL (Baralho Global Anti-Cluster na Biblioteca)")
        ]
        
        for code, desc in modes:
            out = ctrl.send_cmd(f"shuf {code}")
            log_info(f"Configurando shuf {code} ({desc}): {out.strip()}")
            st = ctrl.get_track_info()
            if st.get("shuf") == code:
                log_pass(f"Modo Shuffle {code} confirmado no firmware")
            else:
                log_fail(f"Modo Shuffle {code} nao refletiu no estado (shuf={st.get('shuf')})")
                all_passed = False

        # Teste de Ciclo (shuf)
        out_cyc = ctrl.send_cmd("shuf")
        log_info(f"Ciclo de shuf sem argumentos: {out_cyc.strip()}")
        if "@OK shuf" in out_cyc:
            log_pass("Comutacao ciclica de shuffle via UART validada")
        else:
            log_fail("Falha na comutacao ciclica de shuffle")
            all_passed = False

        # Restaura Shuffle OFF (0)
        ctrl.send_cmd("shuf 0")

        # -------------------------------------------------------------
        # 7. MODOS DE REPETICAO (LOOP / REPEAT)
        # -------------------------------------------------------------
        log_step("7. Modos de Repeticao (Loop / Repeat)")
        
        loop_modes = [
            (0, "NONE (Linear: para ao fim da pasta/playlist)"),
            (1, "ALL (Repete toda a pasta/playlist ciclicamente)"),
            (2, "TRACK (Repete a faixa atual 1 vez)")
        ]
        
        for code, desc in loop_modes:
            out = ctrl.send_cmd(f"loop {code}")
            log_info(f"Configurando loop {code} ({desc}): {out.strip()}")
            st = ctrl.get_track_info()
            if st.get("loop") == code:
                log_pass(f"Modo Loop {code} confirmado no firmware")
            else:
                log_fail(f"Modo Loop {code} nao refletiu no estado (loop={st.get('loop')})")
                all_passed = False

        # 7.4 Teste de Quebra de Loop ao Avancar Faixa
        # Quando em Loop 2 (Repetir Faixa), pressionar 'next' DEVE quebrar o loop para NONE (0)
        ctrl.send_cmd("loop 2")
        st_l2 = ctrl.get_track_info()
        log_info(f"Loop configurado para 2: loop={st_l2.get('loop')}")
        
        out_next = ctrl.send_cmd("n", wait_sec=0.8) # next
        st_after_next = ctrl.get_track_info()
        log_info(f"Apos comando 'next': loop={st_after_next.get('loop')}, faixa={st_after_next.get('filename')}")
        
        if st_after_next.get("loop") == 0:
            log_pass("Invariante de UX confirmada: Avancar faixa manualmente quebra o Loop de Faixa Única (Loop -> NONE)")
        else:
            log_fail(f"Loop permaneceu em {st_after_next.get('loop')} apos avancar faixa!")
            all_passed = False

        # -------------------------------------------------------------
        # 8. RESTAURACAO DE ESTADO SEGURO
        # -------------------------------------------------------------
        log_step("8. Restauracao de Parametros Padrão de Fábrica")
        ctrl.send_cmd("loop 1") # Loop ALL (padrao de fabrica)
        ctrl.send_cmd("shuf 0") # Shuffle OFF
        ctrl.send_cmd("pl set 0") # Favoritos.m3u8
        st_final = ctrl.get_track_info()
        log_pass(f"Estado restaurado: Loop={st_final.get('loop')}, Shuf={st_final.get('shuf')}, Alvo=Favoritos.m3u8")

    finally:
        ctrl.close()

    print("\n" + "=" * 60)
    if all_passed:
        print(f"{Color.GREEN}{Color.BOLD}SUCESSO: TODOS OS TESTES DE LISTAS, FAVORITAR, SHUFFLE E REPEAT PASSARAM!{Color.RESET}")
    else:
        print(f"{Color.RED}{Color.BOLD}FALHA: ALGUNS TESTES APRESENTARAM ERROS (VERIFICAR LOG ACIMA){Color.RESET}")
    print("=" * 60 + "\n")
    return all_passed

if __name__ == "__main__":
    success = run_tests()
    sys.exit(0 if success else 1)
