# Diretrizes de Contribuição — mps3

Agradecemos o interesse em contribuir com o **mps3**! O projeto é focado em engenharia de firmware de alto desempenho, áudio digital em tempo real e conectividade sem fio em sistemas embarcados.

---

## 🛠️ Como Reportar Problemas (Issues)

1. **Verifique se o problema já não foi relatado** nas Issues ou documentado no [`README.md`](README.md) na seção de limitações conhecidas.
2. Ao relatar um bug, inclua:
   - Modelo exato da placa ESP32 / ESP32-S3 (versão de Flash e PSRAM).
   - Versão do ESP-IDF utilizada (`idf.py --version`).
   - Log completo da porta serial em nível `DEBUG` ou `INFO`.
   - Fone de ouvido Bluetooth utilizado (para problemas com A2DP/LDAC) ou modelo do cartão microSD.

---

## 💻 Como Submeter Mudanças (Pull Requests)

1. **Fork e Branch**: Crie uma branch específica para sua alteração:
   ```bash
   git checkout -b feature/minha-melhoria
   ```
2. **Estilo de Código**:
   - Respeite o padrão de indentação de 4 espaços.
   - Siga a convenção de nomenclatura do ESP-IDF (`snake_case` para funções/variáveis, `UPPER_CASE` para constantes/macros).
   - Mantenha comentários claros em blocos matemáticos ou de temporização de hardware.
3. **Validação de Testes**:
   - Se alterar componentes do `2_esp32_bt_companion`, execute a suíte de testes de host:
     ```bash
     cd 2_esp32_bt_companion/tests
     python run_tests.py
     ```
   - Certifique-se de que todos os 12 alvos de teste passem com 100% de sucesso.
4. **Segurança e Chaves**:
   - **NUNCA** commite chaves de criptografia privadas, senhas de redes Wi-Fi ou certificados confidenciais.
   - Não inclua binários pré-compilados (`.bin`, `.elf`, `.enc`) no commit.

---

## 📜 Licenciamento de Contribuições

Ao submeter código ao repositório **mps3**, você concorda que suas contribuições serão disponibilizadas sob os termos da **Licença MIT** do projeto.
