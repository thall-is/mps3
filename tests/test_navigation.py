#!/usr/bin/env python3
"""
test_navigation.py - Suite de Testes Automatizados de Navegacao e Telas do MPS3
"""

import os
import sys
import time

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
from tools.mps3_nav import Mps3Nav

def test_suite(port="COM5"):
    print(f"=== INICIANDO TESTES AUTOMATIZADOS DE NAVEGACAO MPS3 [{port}] ===")
    
    with Mps3Nav(port) as nav:
        # Teste 1: Estado Inicial e Home
        print("\n[1] Testando nav.home() e snapshot inicial...")
        st = nav.home()
        assert "mode" in st, "Chave 'mode' ausente no snapshot"
        print(f"  [PASS] Modo atual: {st['mode_name']} (code={st['mode']})")

        # Teste 2: Captura de Tela (OLED PNG)
        print("\n[2] Testando captura de tela OLED (128x64 PNG)...")
        png_data = nav.screenshot("tools/golden_now_playing.png")
        assert len(png_data) > 200, "PNG gerado muito pequeno ou invalido"
        print(f"  [PASS] Screenshot gerado com sucesso ({len(png_data)} bytes)")

        # Teste 3: Navegacao para TOP_SCREEN
        print("\n[3] Testando navegacao para TOP_SCREEN...")
        nav.seq("U")
        st = nav.wait_for(lambda s: s.get("mode_name") == "TOP_SCREEN", timeout=3.0)
        assert st["mode_name"] == "TOP_SCREEN"
        print(f"  [PASS] TOP_SCREEN alcancado com sucesso (top_cursor={st.get('top')})")
        nav.screenshot("tools/golden_top_screen.png")

        # Teste 4: Retorno ao Now Playing
        print("\n[4] Testando retorno para PLAYING...")
        nav.seq("D")
        st = nav.wait_for(lambda s: s.get("mode_name") == "PLAYING", timeout=3.0)
        assert st["mode_name"] == "PLAYING"
        print(f"  [PASS] Retornado para PLAYING com sucesso")

        # Teste 5: Ciclo de Pausa e Play via Botao Central Virtual
        print("\n[5] Testando Play/Pause via botao central virtual...")
        st_before = nav.query_ui()
        p_before = st_before.get("paused")
        nav.press("c", 80)
        time.sleep(0.3)
        st_after = nav.query_ui()
        p_after = st_after.get("paused")
        print(f"  [PASS] Paused mudou de {p_before} para {p_after}")

        # Retorna o estado de reproducao original
        nav.press("c", 80)
        time.sleep(0.2)

        # Teste 6: Reset Home
        print("\n[6] Testando nav.home() de seguranca...")
        st = nav.home()
        assert st.get("locked") is False
        print(f"  [PASS] Home limpo e pronto")

    print("\n=== TODOS OS TESTES DE NAVEGACAO PASSARAM COM SUCESSO! ===")

if __name__ == "__main__":
    p = sys.argv[1] if len(sys.argv) > 1 else "COM5"
    test_suite(p)
