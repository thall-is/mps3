#include "audio_player_internal.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "i2s_output.h"
#include "eq.h"
#include "audio_decoder.h"
#include "esp_codec_decoder_adapter.h"
#include "esp_audio_simple_dec_default.h"
#include <math.h>
#include <string.h>

static const char *TAG = "webradio";

#define INBUF_SIZE (64 * 1024)

static float volume_to_gain(int percent)
{
    if (percent <= 0) return 0.0f;
    float db = -40.0f * (1.0f - (percent / 100.0f));
    return powf(10.0f, db / 20.0f);
}

void play_web_radio(const char *url)
{
    ESP_LOGI(TAG, "Iniciando Radio Web: %s", url);
    state_lock();
    s_state.playing = true;
    s_state.track_loaded = false;
    s_state.duration_is_estimate = true;
    s_state.elapsed_sec = 0;
    s_state.total_sec = 0;
    s_state.last_error[0] = '\0';
    snprintf(s_state.title, sizeof(s_state.title), "Radio Online");
    snprintf(s_state.artist, sizeof(s_state.artist), "%s", url);
    snprintf(s_state.album, sizeof(s_state.album), "Web Stream");
    strncpy(s_state.format_name, "Conectando...", sizeof(s_state.format_name) - 1);
    s_state.format_name[sizeof(s_state.format_name) - 1] = '\0';
    state_unlock();

    esp_http_client_config_t config = {};
    config.url = url;
    config.event_handler = NULL;
    config.buffer_size = 8192;
    config.timeout_ms = 5000;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        state_lock();
        snprintf(s_state.last_error, sizeof(s_state.last_error), "Erro HTTP Init");
        s_state.playing = false;
        state_unlock();
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        state_lock();
        snprintf(s_state.last_error, sizeof(s_state.last_error), "Falha ao conectar");
        s_state.playing = false;
        state_unlock();
        return;
    }

    uint8_t *inbuf = (uint8_t*)heap_caps_malloc(INBUF_SIZE, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (!inbuf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        state_lock();
        s_state.playing = false;
        s_state.is_web_radio = false;
        state_unlock();
        return;
    }

    esp_audio_simple_dec_type_t t; 
    const char* lbl; 
    mps3::audio_format_to_simple_dec(mps3::AudioFormat::Mp3, &t, &lbl); 
    mps3::AudioDecoderBase* decoder = new mps3::EspCodecDecoderAdapter(t, lbl);

    if (!decoder) {
        heap_caps_free(inbuf);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        state_lock();
        s_state.playing = false;
        s_state.is_web_radio = false;
        state_unlock();
        return;
    }

    int32_t *outbuf = NULL;
    size_t out_cap = 0;
    uint32_t sample_rate = 0;
    int channels = 0;
    size_t valid_end = 0;

    while (1) {
        player_cmd_t cmd = s_pending_cmd;
        if (cmd == PLAYER_CMD_STOP_URL) break;
        if (cmd == PLAYER_CMD_USB_TAKEOVER) { s_usb_takeover_active = true; break; }

        if (s_paused) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

        if (valid_end < INBUF_SIZE) {
            int read_len = esp_http_client_read(client, (char*)(inbuf + valid_end), INBUF_SIZE - valid_end);
            if (read_len < 0) break;
            if (read_len == 0 && esp_http_client_is_complete_data_received(client)) break;
            valid_end += read_len;
        }

        if (valid_end == 0) { vTaskDelay(10); continue; }

        size_t bytes_consumed = 0;
        size_t samples_decoded = 0;

        mps3::DecodeStatus status = decoder->decode(inbuf, valid_end, outbuf, out_cap, bytes_consumed, samples_decoded);

        if (status == mps3::DecodeStatus::Error) {
            break;
        }

        if (status == mps3::DecodeStatus::HeaderReady) {
            out_cap = decoder->channels() * 2048; 
            if (outbuf) heap_caps_free(outbuf);
            outbuf = (int32_t*)heap_caps_malloc(out_cap * sizeof(int32_t), MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
            sample_rate = decoder->sample_rate();
            channels = decoder->channels();
            i2s_output_set_rate(sample_rate);
            eq_set_sample_rate(sample_rate);
            eq_reset_state();

            state_lock();
            s_state.track_loaded = true;
            strncpy(s_state.format_name, "RADIO", sizeof(s_state.format_name) - 1);
            s_state.format_name[sizeof(s_state.format_name) - 1] = '\0';
            s_state.sample_rate = sample_rate;
            s_state.bits_per_sample = 16;
            s_state.bitrate = 128000;
            state_unlock();
        }

        if (bytes_consumed > 0 && bytes_consumed <= valid_end) {
            memmove(inbuf, inbuf + bytes_consumed, valid_end - bytes_consumed);
            valid_end -= bytes_consumed;
        }

        if (samples_decoded > 0 && outbuf && channels > 0) {
            static int s_cached_radio_vol = -1;
            static int s_cached_radio_bal = 999;
            static int32_t s_cached_radio_final_L = 32768;
            static int32_t s_cached_radio_final_R = 32768;

            int vol = s_volume_percent;
            int bal = audio_player_get_balance();
            if (vol != s_cached_radio_vol || bal != s_cached_radio_bal) {
                s_cached_radio_vol = vol;
                s_cached_radio_bal = bal;

                float vol_gain = volume_to_gain(vol);
                int32_t vol_mult = (int32_t)(vol_gain * 32768.0f + 0.5f);
                if (vol_mult > 32768) vol_mult = 32768;
                if (vol_mult < 0) vol_mult = 0;

                int pct_L = 100;
                int pct_R = 100;
                if (bal < 0) {
                    pct_R = 100 + bal;
                } else if (bal > 0) {
                    pct_L = 100 - bal;
                }
                if (pct_L < 0) pct_L = 0;
                if (pct_R < 0) pct_R = 0;
                if (pct_L > 100) pct_L = 100;
                if (pct_R > 100) pct_R = 100;

                int32_t bal_mult_L = (pct_L <= 0) ? 0 : ((pct_L >= 100) ? 32768 : (int32_t)(((int64_t)pct_L * pct_L * 32768) / 10000));
                int32_t bal_mult_R = (pct_R <= 0) ? 0 : ((pct_R >= 100) ? 32768 : (int32_t)(((int64_t)pct_R * pct_R * 32768) / 10000));

                s_cached_radio_final_L = (int32_t)(((int64_t)vol_mult * bal_mult_L) >> 15);
                s_cached_radio_final_R = (int32_t)(((int64_t)vol_mult * bal_mult_R) >> 15);
            }

            int32_t final_L = s_cached_radio_final_L;
            int32_t final_R = s_cached_radio_final_R;

            if (final_L < 32768 || final_R < 32768) {
                for (size_t i = 0; i < samples_decoded; i += 2) {
                    outbuf[i] = (int32_t)(((int64_t)outbuf[i] * final_L) >> 15);
                    if (i + 1 < samples_decoded) {
                        outbuf[i + 1] = (int32_t)(((int64_t)outbuf[i + 1] * final_R) >> 15);
                    }
                }
            }
            eq_process(outbuf, samples_decoded);
            size_t bw = 0;
            i2s_output_write(outbuf, samples_decoded, &bw);
        }
    }

    if (outbuf) heap_caps_free(outbuf);
    delete decoder;
    heap_caps_free(inbuf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    state_lock();
    s_state.playing = false;
    s_state.is_web_radio = false;
    state_unlock();
}


