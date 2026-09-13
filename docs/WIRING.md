# mps3 — Guia de Fiação e Esquema Elétrico

Este documento detalha todas as conexões físicas, níveis de tensão e recomendações elétricas para montar o reprodutor **mps3**.

---

## ⚡ Alimentação e Consumo

* **Tensão de Operação dos Microcontroladores**: **3.3V DC** regulados.
* **Bateria Recomendada**: Célula Li-Ion / Li-Po de 3.7V nominal (4.2V em carga máxima).
* **Consumo de Corrente Típico**:
  * ESP32-S3 em reprodução FLAC com display OLED ligado: ~110 mA.
  * ESP32 Companion em transmissão LDAC 990 kbps: ~140 mA.
  * DAC PCM5102A: ~25 mA.
  * **Consumo Total Médio**: ~275 mA (uma célula de 2000 mAh oferece cerca de 7 horas de reprodução contínua).

---

## 📐 Diagrama de Interligação dos Módulos

```text
                  +-----------------------------------+
                  |        BATERIA LI-ION (3.7V)      |
                  +-----------------+-----------------+
                                    |
                                    v
                  +-----------------------------------+
                  |      REGULADOR LDO 3.3V (1A)      |
                  +-----------------+-----------------+
                                    |
          +-------------------------+-------------------------+
          | (3.3V)                  | (3.3V)                  | (3.3V)
          v                         v                         v
+-------------------+     +-------------------+     +-------------------+
|     ESP32-S3      |     |  ESP32 Companion  |     |   DAC PCM5102A    |
| (Placa Principal) |     | (Transmissor BT)  |     | (Saída P2 Fones)  |
+---------+---------+     +---------+---------+     +---------+---------+
          |                         |                         |
          |  [BARRAMENTO I2S]       |                         |
          |  BCLK (GPIO 48) --------+------------------------>| BCK
          |  LRCK (GPIO 21) --------+------------------------>| LCK
          |  DOUT (GPIO 47) --------+------------------------>| DIN
          |                         |                         | SCK --> GND
          |  [ENLACE UART]          |                         |
          |  TX (GPIO 14) --------->| RX (GPIO 16)            |
          |  RX (GPIO 13) <---------| TX (GPIO 17)            |
          |  EN (GPIO 12) --------->| EN / CHIP_PU            |
          |                         |                         |
          |                         +-------------------------+
          |                                      |
          |  [PERIFÉRICOS S3]                   GND COMUM DE TODAS AS PLACAS
          |
          +----> OLED I2C: SDA (GPIO 10) | SCL (GPIO 9)
          +----> SDMMC 4-Bit: CLK (5), CMD (6), D0 (4), D1 (17), D2 (16), D3 (7)
          +----> Joystick 5-way: UP (2), LEFT (39), DOWN (41), RIGHT (42), CENTER (40)
          +----> Sensor Bateria: Divisor Resistivo 100k/100k ligado ao GPIO 1 (ADC)
          +----> Status TP4056: Pino CHRG ligado ao GPIO 11
          +----> LED RGB: Sinal de dados WS2812 no GPIO 38
```

---

## 📋 Tabelas Oficiais de Conexão de Pinos

### 1. Barramento de Áudio Digital I2S
| Sinal | ESP32-S3 | ESP32 Companion | DAC PCM5102A | Papel Elétrico |
|---|:---:|:---:|:---:|---|
| **BCLK** | **48** | **26** | **BCK** | Saída 3.3V (Master) / Entradas nos Slaves |
| **LRCK** | **21** | **25** | **LCK** | Saída 3.3V (Master) / Entradas nos Slaves |
| **DOUT** | **47** | **22 (DIN)** | **DIN** | Saída 3.3V (Master) / Entradas nos Slaves |
| **GND**  | GND | GND | GND / SCK | Terra comum de referência |

> **Atenção**: No módulo DAC PCM5102A, conecte o pino **SCK ao GND** para que ele utilize o gerador de clock interno síncrono aos sinais BCK/LCK.

### 2. Barramento de Controle UART
| Linha | ESP32-S3 | ESP32 Companion | Nível Elétrico |
|---|:---:|:---:|---|
| **TX → RX** | **GPIO 14** | **GPIO 16** | Nível lógico direto 3.3V TTL |
| **RX ← TX** | **GPIO 13** | **GPIO 17** | Nível lógico direto 3.3V TTL |
| **RESET / EN** | **GPIO 12** | **EN** | Nível lógico 3.3V (Reset ativo baixo) |

### 3. Conexão do Cartão Micro SD (SDMMC 4-Bit a 40 MHz)
| Linha SDMMC | GPIO ESP32-S3 | Recomendação de Hardware |
|---|:---:|---|
| **CLK** | **GPIO 5** | Linha direta de clock de 40 MHz |
| **CMD** | **GPIO 6** | **Resistor de Pull-Up de 10 kΩ ligado ao 3.3V** |
| **D0**  | **GPIO 4** | **Resistor de Pull-Up de 10 kΩ ligado ao 3.3V** |
| **D1**  | **GPIO 17** | **Resistor de Pull-Up de 10 kΩ ligado ao 3.3V** |
| **D2**  | **GPIO 16** | **Resistor de Pull-Up de 10 kΩ ligado ao 3.3V** |
| **D3**  | **GPIO 7** | **Resistor de Pull-Up de 10 kΩ ligado ao 3.3V** |

### 4. Entradas do Usuário e Sensores (ESP32-S3)
| Periférico | GPIO ESP32-S3 | Conexão Elétrica |
|---|:---:|---|
| **OLED SDA** | **GPIO 10** | Linha de dados I2C (módulo já possui pull-up) |
| **OLED SCL** | **GPIO 9** | Linha de clock I2C (módulo já possui pull-up) |
| **Joystick UP** | **GPIO 2** | Chave normalmente aberta conectada ao GND |
| **Joystick LEFT** | **GPIO 39** | Chave normalmente aberta conectada ao GND |
| **Joystick DOWN** | **GPIO 41** | Chave normalmente aberta conectada ao GND |
| **Joystick RIGHT** | **GPIO 42** | Chave normalmente aberta conectada ao GND |
| **Joystick CENTER** | **GPIO 40** | Botão de seleção conectado ao GND |
| **Sensor de Bateria** | **GPIO 1** | Ponto médio de divisor 100 kΩ / 100 kΩ entre Bateria (+) e GND |
| **Status Carregador TP4056** | **GPIO 11** | Conectado ao pino CHRG do TP4056 (ativo baixo) |
| **LED RGB WS2812** | **GPIO 38** | Sinal de dados DIN do LED NeoPixel on-board |
