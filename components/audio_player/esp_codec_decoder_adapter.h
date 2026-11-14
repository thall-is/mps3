#ifndef ESP_CODEC_DECODER_ADAPTER_H
#define ESP_CODEC_DECODER_ADAPTER_H

#include "audio_decoder.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_log.h"
#include <cstdio>
#include <cstring>

namespace mps3 {

// Adaptador UNICO pra todos os formatos suportados pelo ESP Audio Simple
// Decoder (biblioteca oficial "esp_audio_codec" da Espressif, componente
// espressif/esp_audio_codec): FLAC, MP3, WAV, AAC (ADTS cru) e M4A (AAC
// dentro de container MP4). Um unico wrapper porque a API do Simple
// Decoder ja' e' identica pra todos os containers - so' muda o
// "esp_audio_simple_dec_type_t" passado no open() (ver ESP_CODEC_TYPE_*
// em audio_decoder.h).
// IMPORTANTE: esp_audio_simple_dec_process() pode revelar sample_rate,
// canais e bits E ja' entregar amostras decodificadas numa unica chamada.
// O pipeline do audio_player aloca os buffers de saida ao receber HeaderReady.
// Por isso este adaptador decodifica para um scratch buffer proprio e, quando
// a primeira leva de amostras chega com as informacoes do stream, devolve
// HeaderReady com 0 amostras e preserva o PCM em "pending_" para entregar na
// chamada seguinte (Success), garantindo integridade absoluta de tempo.
class EspCodecDecoderAdapter : public AudioDecoderBase {
public:
    EspCodecDecoderAdapter(esp_audio_simple_dec_type_t type, const char *label)
        : type_(type), label_(label) {}

    ~EspCodecDecoderAdapter() override
    {
        if (handle_) esp_audio_simple_dec_close(handle_);
        if (scratch_) heap_caps_free(scratch_);
    }

    DecodeStatus decode(const uint8_t *input, size_t input_len,
                         int32_t *output, size_t output_capacity_samples,
                         size_t &bytes_consumed, size_t &samples_decoded) override
    {
        bytes_consumed = 0;
        samples_decoded = 0;

        // Se sobrou PCM decodificado da chamada anterior (ver nota da
        // classe acima), entrega ele primeiro - sem tocar em `input`.
        if (pending_bytes_ > pending_offset_) {
            samples_decoded = drain_pending(output, output_capacity_samples);
            return DecodeStatus::Success;
        }

        if (!handle_) {
            esp_audio_simple_dec_cfg_t cfg = {};
            cfg.dec_type = type_;
            esp_audio_err_t open_ret = esp_audio_simple_dec_open(&cfg, &handle_);
            if (open_ret != ESP_AUDIO_ERR_OK || !handle_) {
                ESP_LOGE(kTag, "Falha ao abrir decodificador (%s), codigo %d", label_, (int)open_ret);
                return DecodeStatus::Error;
            }
            // DIAGNOSTICO TEMPORARIO (ver AUDIO_FORMATS_PLAN.md)
            ESP_LOGI(kTag, "Decoder aberto com sucesso: label=%s dec_type=%d", label_, (int)type_);
        }

        if (!scratch_) {
            scratch_capacity_ = INITIAL_SCRATCH_BYTES;
            scratch_ = (uint8_t *)heap_caps_malloc(scratch_capacity_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!scratch_) {
                scratch_ = (uint8_t *)heap_caps_malloc(scratch_capacity_, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            }
            if (!scratch_) return DecodeStatus::Error;
            ESP_LOGI(kTag, "Scratch buffer alocado: %u bytes (%s)",
                     (unsigned)scratch_capacity_,
                     esp_ptr_internal(scratch_) ? "DRAM interno" : "PSRAM");
        }

        esp_audio_simple_dec_raw_t raw = {};
        raw.buffer = const_cast<uint8_t *>(input);
        raw.len = (uint32_t)input_len;
        raw.eos = false;

        esp_audio_simple_dec_out_t out = {};
        out.buffer = scratch_;
        out.len = (uint32_t)scratch_capacity_;

        esp_audio_err_t ret = esp_audio_simple_dec_process(handle_, &raw, &out);

        if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            // Nosso scratch nao coube o frame decodificado - cresce e
            // pede pro chamador tentar de novo (nao avancamos bytes_consumed
            // nem samples_decoded, entao a proxima chamada repete com o
            // mesmo `input`).
            size_t needed = out.needed_size > scratch_capacity_ ? out.needed_size : scratch_capacity_ * 2;
            uint8_t *bigger = (uint8_t *)heap_caps_malloc(needed, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!bigger) return DecodeStatus::Error;
            heap_caps_free(scratch_);
            scratch_ = bigger;
            scratch_capacity_ = needed;
            return DecodeStatus::NeedMoreData;
        }
        if (ret != ESP_AUDIO_ERR_OK) {
            // DIAGNOSTICO TEMPORARIO (ver AUDIO_FORMATS_PLAN.md, investigacao
            // do .opus): este caminho antes falhava EM SILENCIO TOTAL - sem
            // nenhum log, so' "pede mais dados" pra sempre, mesmo se o erro
            // for definitivo (formato genuinamente nao suportado por esse
            // dec_type) e nunca fosse se resolver sozinho. Log limitado as
            // primeiras N ocorrencias por instancia do decoder pra nao
            // inundar o serial monitor num arquivo que fique preso nesse
            // estado por muito tempo.
            if (error_log_count_ < 5) {
                ESP_LOGE(kTag, "esp_audio_simple_dec_process falhou (%s): codigo=%d input_len=%u raw.consumed=%u",
                         label_, (int)ret, (unsigned)input_len, (unsigned)raw.consumed);
                error_log_count_++;
            }
            bytes_consumed = (raw.consumed > 0) ? raw.consumed : 1;
            consecutive_errors_++;
            if (consecutive_errors_ > 30) {
                return DecodeStatus::Error;
            }
            return DecodeStatus::NeedMoreData;
        }

        consecutive_errors_ = 0;
        bytes_consumed = raw.consumed;

        if (out.decoded_size == 0) {
            // Sem amostras ainda (so' juntou cabecalho/parte de um frame) -
            // pede mais dados (bytes_consumed pode ser 0 ou nao, tanto faz,
            // o loop externo so' avanca se for > 0).
            return DecodeStatus::NeedMoreData;
        }

        if (!info_ready_) {
            esp_audio_simple_dec_info_t info = {};
            if (esp_audio_simple_dec_get_info(handle_, &info) == ESP_AUDIO_ERR_OK && info.sample_rate > 0) {
                bool dec_unsupported = false;
                const char *limit_reason = "192 kHz / 24 bits";
                if (type_ == ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC) {
                    if (info.bits_per_sample > 24 || info.sample_rate > 192000) {
                        dec_unsupported = true;
                        limit_reason = "FLAC max 24b/192k";
                    }
                } else if (type_ == ESP_AUDIO_SIMPLE_DEC_TYPE_WAV) {
                    if (info.bits_per_sample > 32 || info.sample_rate > 384000) {
                        dec_unsupported = true;
                        limit_reason = "WAV max 32b/384k";
                    }
                } else {
                    if (info.bits_per_sample > 24 || info.sample_rate > 192000) {
                        dec_unsupported = true;
                        limit_reason = "Max 192kHz/24b";
                    }
                }
                if (dec_unsupported) {
                    ESP_LOGE(kTag, "Taxa ou bits do stream (%s) excedem limites suportados: %u Hz / %u bits (%s)",
                             label_, (unsigned)info.sample_rate, (unsigned)info.bits_per_sample, limit_reason);
                    return DecodeStatus::Error;
                }
                sample_rate_ = info.sample_rate;
                channels_ = info.channel;
                bits_per_sample_ = info.bits_per_sample;
                info_ready_ = true;
                // DIAGNOSTICO TEMPORARIO (ver AUDIO_FORMATS_PLAN.md)
                ESP_LOGI(kTag, "Cabecalho detectado (%s): sample_rate=%u channels=%u bits=%u",
                         label_, (unsigned)sample_rate_, (unsigned)channels_, (unsigned)bits_per_sample_);
            } else {
                // Ainda nao temos as infos do stream - descarta essa leva
                // (nao deveria acontecer na pratica, decoded_size>0 normalmente
                // implica infos disponiveis) e pede mais dados.
                return DecodeStatus::NeedMoreData;
            }
        }

        if (!header_signaled_) {
            header_signaled_ = true;
            pending_bytes_ = out.decoded_size;
            pending_offset_ = 0;
            return DecodeStatus::HeaderReady; // 0 amostras - PCM fica em pending_
        }

        // Ja' sinalizamos HeaderReady antes - entrega direto.
        pending_bytes_ = out.decoded_size;
        pending_offset_ = 0;
        samples_decoded = drain_pending(output, output_capacity_samples);
        return DecodeStatus::Success;
    }

    uint32_t sample_rate() const override     { return sample_rate_; }
    uint32_t channels() const override        { return channels_; }
    uint32_t bits_per_sample() const override { return bits_per_sample_ ? bits_per_sample_ : 16; }

    size_t recommended_output_capacity_samples() const override
    {
        // O buffer do chamador precisa caber o MAIOR "pending_" que
        // formos gerar numa unica leva - garantimos isso convertendo
        // scratch_capacity_ (em bytes) pra amostras usando o tamanho de
        // amostra REAL do stream (ver bytes_per_source_sample()).
        size_t cap = scratch_capacity_ ? scratch_capacity_ : INITIAL_SCRATCH_BYTES;
        size_t ch = channels_ ? channels_ : 2;
        size_t smp = cap / bytes_per_source_sample();
        return smp - (smp % ch);
    }

    void format_label(char *out, size_t out_len) const override
    {
        snprintf(out, out_len, "%s", label_);
    }

private:
    // O ESP Audio Simple Decoder entrega PCM na largura NATIVA do stream.
    // Amostras de ate' 16 bits vem em 2 bytes (int16_t) e de 32 bits vem
    // em 4 bytes (int32_t) - mas 24 bits vem EMPACOTADO em 3 bytes (sem
    // preenchimento para 4), no formato padrão packed Little-Endian.
    // Isso so' e' valido depois de info_ready_ (bits_per_sample_ setado).
    size_t bytes_per_source_sample() const
    {
        if (bits_per_sample_ <= 16) return 2;
        if (bits_per_sample_ == 24) return 3;
        return 4; // 32 bits
    }

    // Converte o quanto couber de pending_ (PCM cru vindo do scratch_, na
    // largura nativa do stream) pra int32 left-justified no buffer do
    // chamador - mesma convencao usada em todo o resto do pipeline (I2S
    // configurado pra 32 bits/slot). Um deslocamento de (32 -
    // bits_per_sample) alinha amostras de 16, 24 ou 32 bits corretamente
    // pro topo da palavra de 32 bits (24 bits desloca 8 posições).
    // Devolve quantas amostras (intercaladas) foram escritas.
    size_t drain_pending(int32_t *output, size_t output_capacity_samples)
    {
        size_t bytes_per_sample = bytes_per_source_sample();
        size_t bytes_avail = pending_bytes_ - pending_offset_;
        size_t samples_avail = bytes_avail / bytes_per_sample;
        size_t ch = channels_ ? channels_ : 2;
        if (samples_avail < ch) {
            // Menos de 1 frame completo disponivel (ou bytes fracionarios residuais < bytes_per_sample):
            // descarta e encerra pending_ para evitar loop infinito ou desalinhamento.
            pending_bytes_ = 0;
            pending_offset_ = 0;
            return 0;
        }

        size_t n = samples_avail < output_capacity_samples ? samples_avail : output_capacity_samples;
        n -= (n % ch);

        uint32_t bps = bits_per_sample_ ? bits_per_sample_ : 16;
        int shift = (bps < 32) ? (int)(32 - bps) : 0;

        const uint8_t *src = scratch_ + pending_offset_;
        switch (bytes_per_sample) {
            case 2: {
                // Tiling em stack interna (SRAM DRAM) para leitura e escrita em rajada via memcpy
                constexpr size_t TILE_SAMPLES = 512;
                int16_t tile_in[TILE_SAMPLES];
                int32_t tile_out[TILE_SAMPLES];
                size_t remaining = n;
                size_t offset_in = 0;
                size_t offset_out = 0;
                while (remaining > 0) {
                    size_t chunk = (remaining > TILE_SAMPLES) ? TILE_SAMPLES : remaining;
                    memcpy(tile_in, src + offset_in, chunk * sizeof(int16_t));
                    for (size_t i = 0; i < chunk; i++) {
                        tile_out[i] = ((int32_t)tile_in[i]) << shift;
                    }
                    memcpy(output + offset_out, tile_out, chunk * sizeof(int32_t));
                    offset_in += chunk * sizeof(int16_t);
                    offset_out += chunk;
                    remaining -= chunk;
                }
                break;
            }
            case 3: {
                // Little-endian 24 bits packed (3 bytes por amostra).
                // Tiling em stack interna (SRAM DRAM) com leitura e escrita em rajada via memcpy.
                // Isso elimina transacoes lentas e desalinhadas na PSRAM.
                // Converte cada amostra diretamente a partir de bytes individuais (sem chamadas memcpy),
                // reduzindo a zero a sobrecarga de funcao por amostra e blindando contra desalinhamento.
                constexpr size_t TILE_SAMPLES = 512;
                uint8_t tile_in[TILE_SAMPLES * 3];
                int32_t tile_out[TILE_SAMPLES];
                size_t remaining = n;
                size_t offset_in = 0;
                size_t offset_out = 0;
                while (remaining > 0) {
                    size_t chunk = (remaining > TILE_SAMPLES) ? TILE_SAMPLES : remaining;
                    memcpy(tile_in, src + offset_in, chunk * 3);
                    for (size_t i = 0; i < chunk; i++) {
                        const uint8_t *s = tile_in + i * 3;
                        tile_out[i] = (int32_t)(((uint32_t)s[0] << 8) | ((uint32_t)s[1] << 16) | ((uint32_t)s[2] << 24));
                    }
                    memcpy(output + offset_out, tile_out, chunk * sizeof(int32_t));
                    offset_in += chunk * 3;
                    offset_out += chunk;
                    remaining -= chunk;
                }
                break;
            }
            default: { // 4 bytes (32 bits)
                if (shift > 0) {
                    constexpr size_t TILE_SAMPLES = 512;
                    int32_t tile_in[TILE_SAMPLES];
                    int32_t tile_out[TILE_SAMPLES];
                    size_t remaining = n;
                    size_t offset_in = 0;
                    size_t offset_out = 0;
                    while (remaining > 0) {
                        size_t chunk = (remaining > TILE_SAMPLES) ? TILE_SAMPLES : remaining;
                        memcpy(tile_in, src + offset_in, chunk * sizeof(int32_t));
                        for (size_t i = 0; i < chunk; i++) {
                            tile_out[i] = tile_in[i] << shift;
                        }
                        memcpy(output + offset_out, tile_out, chunk * sizeof(int32_t));
                        offset_in += chunk * sizeof(int32_t);
                        offset_out += chunk;
                        remaining -= chunk;
                    }
                } else {
                    memcpy(output, src, n * sizeof(int32_t));
                }
                break;
            }
        }

        pending_offset_ += n * bytes_per_sample;
        if (pending_offset_ >= pending_bytes_) {
            pending_bytes_ = 0;
            pending_offset_ = 0;
        }
        return n;
    }

    static constexpr size_t INITIAL_SCRATCH_BYTES = 64 * 1024;
    static constexpr const char *kTag = "esp_codec_decoder";

    esp_audio_simple_dec_type_t type_;
    const char *label_;
    esp_audio_simple_dec_handle_t handle_ = nullptr;

    uint8_t *scratch_ = nullptr;
    size_t scratch_capacity_ = 0;

    size_t pending_bytes_ = 0;
    size_t pending_offset_ = 0;

    bool info_ready_ = false;
    bool header_signaled_ = false;
    uint32_t sample_rate_ = 0;
    uint32_t channels_ = 0;
    uint32_t bits_per_sample_ = 0;
    int error_log_count_ = 0;
    int consecutive_errors_ = 0;
};

} // namespace mps3

#endif // ESP_CODEC_DECODER_ADAPTER_H
