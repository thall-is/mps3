#pragma once

#include "audio_player.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PLAYER_CMD_NONE,
    PLAYER_CMD_NEXT,
    PLAYER_CMD_PREV,
    PLAYER_CMD_RESTART,
    PLAYER_CMD_SEEK_FWD,
    PLAYER_CMD_SEEK_BWD,
    PLAYER_CMD_PLAY_INDEX,
    PLAYER_CMD_USB_TAKEOVER,
    PLAYER_CMD_USB_RESTORE,
    PLAYER_CMD_PLAY_URL,
    PLAYER_CMD_STOP_URL
} player_cmd_t;

extern SemaphoreHandle_t s_state_mutex;
extern playback_state_t s_state;
extern volatile int s_volume_percent;
extern volatile player_cmd_t s_pending_cmd;
extern volatile bool s_paused;
extern volatile bool s_usb_takeover_active;

void state_lock(void);
void state_unlock(void);

// Dual-Core Audio DSP API (Core 0: DSP / Volume / EQ / I2S DMA)
esp_err_t audio_dsp_send_pcm(const int32_t *samples, size_t count, uint32_t rate);
void audio_dsp_flush(void);
void audio_dsp_drain(void);
void audio_dsp_set_rate(uint32_t rate);
void audio_dsp_trigger_fade_in(uint32_t sample_rate);
int32_t *ensure_stereo_scratch(size_t needed_samples);

#ifdef __cplusplus
}
void play_web_radio(const char *url);
#endif


