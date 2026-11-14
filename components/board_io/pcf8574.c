#include "pcf8574.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "pcf8574";

static i2c_master_bus_handle_t s_bus_handle = NULL;
static i2c_master_dev_handle_t s_pcf_dev = NULL;
static uint8_t s_detected_addr = 0;
static bool s_available = false;
static uint8_t s_last_read_val = 0xFF;

// Faixas de endereços válidos para PCF8574 (0x20..0x27) e PCF8574A (0x38..0x3F)
static const uint8_t SCAN_ADDRS[] = {
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, // PCF8574 Standard
    0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F  // PCF8574A Variant
};

esp_err_t pcf8574_init(i2c_master_bus_handle_t bus_handle)
{
    if (!bus_handle) {
        ESP_LOGE(TAG, "Bus handle I2C nulo fornecido ao PCF8574");
        s_available = false;
        return ESP_ERR_INVALID_ARG;
    }
    s_bus_handle = bus_handle;
    s_detected_addr = 0;
    s_available = false;

    // Se já havia dispositivo registrado, remova para reconfiguração segura
    if (s_pcf_dev) {
        i2c_master_bus_rm_device(s_pcf_dev);
        s_pcf_dev = NULL;
    }

    // Auto-detecção: sondar cada endereço possível
    for (size_t i = 0; i < sizeof(SCAN_ADDRS) / sizeof(SCAN_ADDRS[0]); i++) {
        uint8_t addr = SCAN_ADDRS[i];
        
        // Evita testar 0x3C/0x3D se o OLED SSD1306 já estiver lá
        // Mas se responder, verifica se é realmente o expansor
        esp_err_t probe_ret = i2c_master_probe(bus_handle, addr, 20);
        if (probe_ret == ESP_OK) {
            // Se for 0x3C ou 0x3D, o OLED usa esse endereço, então só consideramos PCF8574A se não for OLED
            if (addr == 0x3C || addr == 0x3D) {
                ESP_LOGD(TAG, "Endereco 0x%02X respondeu, mas reservado para OLED SSD1306", addr);
                continue;
            }

            s_detected_addr = addr;
            ESP_LOGI(TAG, "PCF8574 detectado no endereco I2C 0x%02X (Tipo: %s)",
                     addr, (addr >= 0x38) ? "PCF8574A" : "PCF8574");
            break;
        }
    }

    if (s_detected_addr == 0) {
        ESP_LOGW(TAG, "Nenhum PCF8574 / PCF8574A detectado no barramento I2C");
        return ESP_ERR_NOT_FOUND;
    }

    // Registra o dispositivo no barramento I2C mestre do ESP-IDF
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = s_detected_addr,
        .scl_speed_hz = 100000, // 100 kHz (padrão robusto para PCF8574)
    };

    esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &s_pcf_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar dispositivo PCF8574: %s", esp_err_to_name(ret));
        s_available = false;
        return ret;
    }

    // Configura todas as 8 portas como entradas (escreve 0xFF para habilitar o pull-up interno)
    uint8_t init_val = 0xFF;
    ret = i2c_master_transmit(s_pcf_dev, &init_val, 1, 50);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao enviar byte de configuracao inicial (0xFF): %s", esp_err_to_name(ret));
        s_available = false;
        return ret;
    }

    s_available = true;
    ESP_LOGI(TAG, "PCF8574 inicializado com sucesso em 0x%02X. P0..P7 configurados como entradas com pull-up.", s_detected_addr);
    
    // Leitura inicial de teste
    pcf8574_read_raw(&s_last_read_val);
    ESP_LOGI(TAG, "Estado inicial das portas PCF8574: 0x%02X", s_last_read_val);

    return ESP_OK;
}

bool pcf8574_is_available(void)
{
    return s_available;
}

uint8_t pcf8574_get_address(void)
{
    return s_detected_addr;
}

esp_err_t pcf8574_read_raw(uint8_t *val)
{
    if (!s_available || !s_pcf_dev) {
        if (val) *val = 0xFF;
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t buf = 0xFF;
    esp_err_t ret = i2c_master_receive(s_pcf_dev, &buf, 1, 50);
    if (ret == ESP_OK) {
        s_last_read_val = buf;
        if (val) *val = buf;
    } else {
        ESP_LOGD(TAG, "Falha na leitura I2C: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t pcf8574_write_raw(uint8_t val)
{
    if (!s_available || !s_pcf_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit(s_pcf_dev, &val, 1, 50);
}

bool pcf8574_get_pin_pressed(uint8_t pin)
{
    if (pin > 7) return false;
    uint8_t raw = 0xFF;
    if (pcf8574_read_raw(&raw) != ESP_OK) {
        return false;
    }
    // No PCF8574, bit em 0 indica que o botão fechou para GND (pressionado)
    return ((raw & (1u << pin)) == 0);
}

