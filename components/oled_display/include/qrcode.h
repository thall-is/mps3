/**
 * The MIT License (MIT)
 *
 * This library is written and maintained by Richard Moore.
 * Major parts were derived from Project Nayuki's library.
 *
 * Copyright (c) 2017 Richard Moore     (https://github.com/ricmoo/QRCode)
 * Copyright (c) 2017 Project Nayuki    (https://www.nayuki.io/page/qr-code-generator-library)
 */

#ifndef __QRCODE_H_
#define __QRCODE_H_

/**
 * @file qrcode.h
 * @brief Gerador e Codificador de QR Code Monocromático 2D
 *
 * Biblioteca leve para geração de matrizes de QR Code diretamente na memória,
 * com suporte a níveis de correção de erro (ECC Low, Medium, Quartile, High)
 * e conversão direta para pixels no display OLED SSD1306.
 */

#include <stdbool.h>
#include <stdint.h>

// QR Code Format Encoding
#define MODE_NUMERIC        0 /**< Modo numérico (apenas dígitos 0-9). */
#define MODE_ALPHANUMERIC   1 /**< Modo alfanumérico (A-Z, 0-9, espaço e símbolos). */
#define MODE_BYTE           2 /**< Modo binário/UTF-8 (qualquer caractere de 8 bits). */

// Error Correction Code Levels
#define ECC_LOW            0 /**< Nível de correção L (~7% dos dados recuperáveis). */
#define ECC_MEDIUM         1 /**< Nível de correção M (~15% dos dados recuperáveis). */
#define ECC_QUARTILE       2 /**< Nível de correção Q (~25% dos dados recuperáveis). */
#define ECC_HIGH           3 /**< Nível de correção H (~30% dos dados recuperáveis). */

#ifndef LOCK_VERSION
#define LOCK_VERSION       0
#endif

/**
 * @brief Estrutura de estado de uma matriz de QR Code gerada.
 */
typedef struct QRCode {
    uint8_t version;     /**< Versão do QR Code (1 a 40). */
    uint8_t size;        /**< Dimensão da matriz (size x size módulos). */
    uint8_t ecc;         /**< Nível de correção de erro configurado. */
    uint8_t mode;        /**< Modo de codificação utilizado. */
    uint8_t mask;        /**< Padrão de máscara aplicada. */
    uint8_t *modules;    /**< Ponteiro para o buffer de bytes onde os módulos estão armazenados. */
} QRCode;

#ifdef __cplusplus
extern "C"{
#endif

/**
 * @brief Calcula a quantidade necessária de bytes para o buffer de módulos de uma dada versão.
 *
 * @param[in] version Versão do QR Code (1 a 40).
 *
 * @return Número de bytes necessários para alocação do array `modules`.
 */
uint16_t qrcode_getBufferSize(uint8_t version);

/**
 * @brief Inicializa e codifica uma string de texto/URL em uma matriz QR Code.
 *
 * @param[out] qrcode  Ponteiro para a estrutura `QRCode` a ser preenchida.
 * @param[in]  modules Buffer de bytes previamente alocado com tamanho `qrcode_getBufferSize(version)`.
 * @param[in]  version Versão do código (1 a 40).
 * @param[in]  ecc     Nível de correção de erro (`ECC_LOW`, `ECC_MEDIUM`, `ECC_QUARTILE`, `ECC_HIGH`).
 * @param[in]  data    String null-terminated a ser codificada (ex: "http://mps3.local" ou dados Wi-Fi).
 *
 * @return 0 em caso de sucesso; < 0 se o texto exceder a capacidade da versão especificada.
 */
int8_t qrcode_initText(QRCode *qrcode, uint8_t *modules, uint8_t version, uint8_t ecc, const char *data);

/**
 * @brief Inicializa e codifica um payload binário arbitrário em uma matriz QR Code.
 *
 * @param[out] qrcode  Ponteiro para a estrutura `QRCode` de saída.
 * @param[in]  modules Buffer de bytes alocado para os módulos.
 * @param[in]  version Versão do QR Code (1 a 40).
 * @param[in]  ecc     Nível de correção de erro.
 * @param[in]  data    Ponteiro para os bytes a serem codificados.
 * @param[in]  length  Quantidade de bytes do payload.
 *
 * @return 0 em caso de sucesso; < 0 se o payload exceder a capacidade da versão.
 */
int8_t qrcode_initBytes(QRCode *qrcode, uint8_t *modules, uint8_t version, uint8_t ecc, uint8_t *data, uint16_t length);

/**
 * @brief Consulta o valor de um módulo (pixel) específico na matriz do QR Code.
 *
 * @param[in] qrcode Ponteiro para a estrutura `QRCode` inicializada.
 * @param[in] x      Coordenada horizontal do módulo (0 a size-1).
 * @param[in] y      Coordenada vertical do módulo (0 a size-1).
 *
 * @return true se o pixel/módulo estiver aceso (preto); false se apagado (branco).
 */
bool qrcode_getModule(QRCode *qrcode, uint8_t x, uint8_t y);

#ifdef __cplusplus
}
#endif

#endif  /* __QRCODE_H_ */
