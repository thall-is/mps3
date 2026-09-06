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

#ifdef __cplusplus
}
void play_web_radio(const char *url);
#endif


