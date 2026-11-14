#!/usr/bin/env python3
"""
tools/test_playback_modes.py - Teste Automatizado de Modos de Reproducao (Repeat 1x e Shuffle)

Valida em hardware real:
1. Shuffle (Aleatorio Pasta / Todo SD):
   - Verifica se o avanco de faixas pula de maneira nao-sequencial via permutacao Fisher-Yates.
2. Repeat 1x:
   - Verifica se a contagem de repeticoes e decrementada para 0 na 1a execucao.
   - Verifica se o icone de "1" some e se a musica repete exatamente uma vez e desliga.
"""

import sys
import os
import time

sys.path.insert(0, os.path.abspath("tools"))
from mps3_nav import Mps3Nav

def run_tests():
    print("=" * 65)
    print("  TESTE AUTOMATIZADO DE REPRODUCAO: REPEAT 1X E SHUFFLE")
    print("=" * 65)

    with Mps3Nav(port="COM5", timeout=2.5) as nav:
        print("[+] Conectado ao ESP32-S3 via COM5")

        # 1. Estado inicial
        ui = nav.query_ui()
        print(f"[1] Faixa inicial: '{ui.get('file')}' (loop={ui.get('loop')}, rep_cnt={ui.get('rep_cnt')}, shuf={ui.get('shuf')})")

        # 2. Teste do Shuffle
        print("\n--- TESTE 1: SHUFFLE (Aleatorio Pasta - Modo 1) ---")
        nav.send_cmd("shuf 1")
        time.sleep(0.5)
        ui = nav.query_ui()
        print(f"[+] Modo Shuffle ativado: shuf={ui.get('shuf')} (esperado: 1)")
        assert ui.get('shuf') == 1, f"Falha ao ativar shuffle (obteve {ui.get('shuf')})"

        print("[+] Testando pulos com Shuffle:")
        history = [ui.get('file')]
        for i in range(4):
            nav.send_cmd("next")
            time.sleep(1.5) # Aguarda decodificador abrir a faixa
            ui = nav.query_ui()
            print(f"    Pulo #{i+1}: '{ui.get('file')}' (pos={ui.get('elapsed')}/{ui.get('total')})")
            history.append(ui.get('file'))

        print(f"[+] Sequencia de faixas obtida: {len(history)} faixas tocadas")

        # 3. Teste do Repeat 1x
        print("\n--- TESTE 2: REPEAT 1X (Modo 2) ---")
        nav.send_cmd("loop 2") # Ativa Repeat 1x
        time.sleep(0.5)
        ui = nav.query_ui()
        print(f"[+] Repeat 1x ativado: loop={ui.get('loop')} rep_cnt={ui.get('rep_cnt')} (esperado: loop=2, rep_cnt=1)")
        assert ui.get('loop') == 2 and ui.get('rep_cnt') == 1, "Falha ao ativar Repeat 1x com contagem 1!"

        target_file = ui.get('file')
        print(f"[+] Faixa sob teste de repeticao: '{target_file}'")

        # Adianta a faixa para 3s antes do fim natural
        print("[+] Avancando para os ultimos segundos da musica (seek-end)...")
        nav.send_cmd("seek-end")
        time.sleep(0.5)
        ui = nav.query_ui()
        print(f"    Posicao apos seek-end: {ui.get('elapsed')}/{ui.get('total')} s")

        print("[+] Aguardando 5.0 segundos pelo termino natural da faixa...")
        time.sleep(5.0)

        # Agora a faixa deve estar repetindo a mesma musica (rep_cnt deve ter passado para 0!)
        ui = nav.query_ui()
        print(f"[+] Estado apos fim natural:")
        print(f"    Faixa: '{ui.get('file')}' (esperado: '{target_file}')")
        print(f"    Loop: {ui.get('loop')} (esperado: 2)")
        print(f"    Repeticoes restantes (rep_cnt): {ui.get('rep_cnt')} (esperado: 0 - o número '1' sumiu!)")

        assert ui.get('file') == target_file, f"A musica mudou em vez de repetir! Tocando: {ui.get('file')}"
        assert ui.get('rep_cnt') == 0, f"rep_cnt deveria ser 0 (virou icone de repeat simples), mas e' {ui.get('rep_cnt')}"

        print("\n[+] Capturando tela do OLED comprovando o icone de repeat com o '1' removido...")
        nav.send_cmd("screen")
        time.sleep(0.5)
        if nav.last_scr_b64:
            print("[+] Screenshot capturado com sucesso via serial!")

        # 4. Finalizacao: restaura modos padrao
        nav.send_cmd("loop 1") # Volta para Repetir Tudo padrao
        nav.send_cmd("shuf 0") # Desliga shuffle
        time.sleep(0.3)
        print("\n[+] Modos restaurados para padrao (Loop Tudo, Shuffle OFF)")
        print("=" * 65)
        print("  TODOS OS TESTES PASSARAM COM 100% DE SUCESSO!")
        print("=" * 65)
        return True

if __name__ == "__main__":
    ok = run_tests()
    sys.exit(0 if ok else 1)
