#include "user_metrics.h"
#include "rtc_ds3231.h"
#include "eq.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <ctype.h>

static const char *TAG = "user_metrics";

#define ANALYTICS_DIR          "/sdcard/.mps3_analytics"
#define SUMMARY_FILE_PATH      "/sdcard/.mps3_analytics/tracks_summary.json"
#define SUMMARY_TMP_PATH       "/sdcard/.mps3_analytics/tracks_summary.tmp"
#define MAX_SUMMARY_TRACKS     512

typedef struct {
    uint64_t track_id;
    char     rel_path[192];
    char     title[96];
    char     artist[64];
    char     album[64];
    char     codec[12];
    uint32_t sample_rate;
    uint8_t  bits_per_sample;
    uint32_t total_plays;
    uint32_t total_scrobbles;
    uint32_t total_skips;
    uint32_t total_completed;
    uint32_t total_listen_sec;
    uint32_t first_epoch;
    uint32_t last_epoch;
} track_summary_entry_t;

typedef struct {
    bool     active;
    char     rel_path[256];
    uint64_t total_frames;
    uint32_t last_sample_rate;
    uint64_t accum_listen_ms;
    uint64_t vol_weighted_sum;
    uint32_t vol_samples;
    uint32_t seek_count;
} active_session_t;

static active_session_t s_session = {0};
static track_summary_entry_t *s_summary = NULL;
static int s_summary_count = 0;
static bool s_initialized = false;

// Hash FNV-1a de 64 bits case-insensitive para identificação única de faixas
static uint64_t fnv1a_64_ci(const char *s)
{
    uint64_t hash = 14695981039346656037ULL;
    if (!s) return hash;
    while (*s) {
        unsigned char c = (unsigned char)tolower((unsigned char)*s++);
        hash ^= (uint64_t)c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static uint64_t compute_track_id(const char *artist, const char *title, const char *rel_path)
{
    char key[256];
    if (artist && artist[0] != '\0' && title && title[0] != '\0') {
        snprintf(key, sizeof(key), "%s|%s", artist, title);
    } else if (rel_path && rel_path[0] != '\0') {
        snprintf(key, sizeof(key), "%s", rel_path);
    } else {
        snprintf(key, sizeof(key), "%s", title ? title : "unknown");
    }
    return fnv1a_64_ci(key);
}

// Escapa aspas e barras invertidas para JSON seguro
static void json_escape(const char *src, char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) return;
    size_t j = 0;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    for (size_t i = 0; src[i] != '\0' && j + 2 < dst_size; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            if (j + 3 >= dst_size) break;
            dst[j++] = '\\';
            dst[j++] = (char)c;
        } else if (c >= 0x20) {
            dst[j++] = (char)c;
        }
    }
    dst[j] = '\0';
}

static const char *detect_category(const char *rel_path)
{
    if (!rel_path) return "music";
    const char *p = (rel_path[0] == '/') ? rel_path + 1 : rel_path;
    if (strncasecmp(p, "Podcasts/", 9) == 0 || strncasecmp(p, "Podcast/", 8) == 0) {
        return "podcast";
    }
    if (strncasecmp(p, "Audiobooks/", 11) == 0 || strncasecmp(p, "Audiolivros/", 12) == 0) {
        return "audiobook";
    }
    return "music";
}

static void ensure_analytics_dir(void)
{
    struct stat st;
    if (stat(ANALYTICS_DIR, &st) != 0) {
        mkdir(ANALYTICS_DIR, 0777);
    }
}

static void save_summary_to_sd(void)
{
    if (!s_summary || s_summary_count <= 0) return;
    ensure_analytics_dir();

    FILE *f = fopen(SUMMARY_TMP_PATH, "w");
    if (!f) return;

    uint64_t global_listen_sec = 0;
    uint32_t global_scrobbles = 0;
    uint32_t global_plays = 0;
    for (int i = 0; i < s_summary_count; i++) {
        global_listen_sec += s_summary[i].total_listen_sec;
        global_scrobbles  += s_summary[i].total_scrobbles;
        global_plays      += s_summary[i].total_plays;
    }

    fprintf(f, "{\n  \"version\": 1,\n  \"unique_tracks\": %d,\n  \"total_plays\": %u,\n  \"total_scrobbles\": %u,\n  \"total_listen_sec\": %llu,\n  \"tracks\": [\n",
            s_summary_count, (unsigned)global_plays, (unsigned)global_scrobbles, (unsigned long long)global_listen_sec);

    char esc_path[256], esc_title[160], esc_artist[112], esc_album[112], esc_codec[24];
    for (int i = 0; i < s_summary_count; i++) {
        const track_summary_entry_t *e = &s_summary[i];
        json_escape(e->rel_path, esc_path, sizeof(esc_path));
        json_escape(e->title, esc_title, sizeof(esc_title));
        json_escape(e->artist, esc_artist, sizeof(esc_artist));
        json_escape(e->album, esc_album, sizeof(esc_album));
        json_escape(e->codec, esc_codec, sizeof(esc_codec));

        fprintf(f,
                "    {\"id\":\"%016llx\",\"path\":\"%s\",\"title\":\"%s\",\"artist\":\"%s\",\"album\":\"%s\","
                "\"codec\":\"%s\",\"sr\":%u,\"bits\":%u,\"plays\":%u,\"scrobbles\":%u,\"skips\":%u,"
                "\"completed\":%u,\"listen_s\":%u,\"first_ts\":%u,\"last_ts\":%u}%s\n",
                (unsigned long long)e->track_id, esc_path, esc_title, esc_artist, esc_album,
                esc_codec, (unsigned)e->sample_rate, (unsigned)e->bits_per_sample,
                (unsigned)e->total_plays, (unsigned)e->total_scrobbles, (unsigned)e->total_skips,
                (unsigned)e->total_completed, (unsigned)e->total_listen_sec,
                (unsigned)e->first_epoch, (unsigned)e->last_epoch,
                (i + 1 < s_summary_count) ? "," : "");
    }

    fprintf(f, "  ]\n}\n");
    fclose(f);

    remove(SUMMARY_FILE_PATH);
    rename(SUMMARY_TMP_PATH, SUMMARY_FILE_PATH);
}

void user_metrics_init(void)
{
    if (s_initialized) return;

    s_summary = (track_summary_entry_t *)heap_caps_calloc(
        MAX_SUMMARY_TRACKS, sizeof(track_summary_entry_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_summary) {
        s_summary = (track_summary_entry_t *)calloc(MAX_SUMMARY_TRACKS, sizeof(track_summary_entry_t));
    }

    ensure_analytics_dir();
    s_initialized = true;
    ESP_LOGI(TAG, "Coletor de telemetria MPS3 Wrapped inicializado (%s)", ANALYTICS_DIR);
}

void user_metrics_on_track_begin(const char *rel_path)
{
    if (!s_initialized) user_metrics_init();
    if (!rel_path || rel_path[0] == '\0') return;

    // Se a sessão já está ativa para a mesma faixa (ex: retomada imediata pós-seek),
    // preserva o tempo ouvido e o contador de seeks acumulados.
    if (s_session.active && strcmp(s_session.rel_path, rel_path) == 0) {
        s_session.total_frames = 0;
        return;
    }

    memset(&s_session, 0, sizeof(s_session));
    s_session.active = true;
    snprintf(s_session.rel_path, sizeof(s_session.rel_path), "%.*s", (int)(sizeof(s_session.rel_path) - 1), rel_path);
}

void user_metrics_add_listened_frames(uint32_t frames, uint32_t sample_rate, int volume_pct)
{
    if (!s_session.active || frames == 0 || sample_rate == 0) return;

    s_session.total_frames += frames;
    s_session.last_sample_rate = sample_rate;
    s_session.vol_weighted_sum += (uint64_t)volume_pct;
    s_session.vol_samples++;
}

void user_metrics_on_track_end(const user_metrics_end_info_t *info, bool is_seek_restart)
{
    if (!s_session.active || !info) return;

    if (s_session.last_sample_rate > 0 && s_session.total_frames > 0) {
        s_session.accum_listen_ms += (s_session.total_frames * 1000ULL) / s_session.last_sample_rate;
        s_session.total_frames = 0;
    }

    if (is_seek_restart) {
        s_session.seek_count++;
        return;
    }

    uint32_t listen_sec = (uint32_t)((s_session.accum_listen_ms + 500ULL) / 1000ULL);
    // Ignora transições fantasma (< 1s e sem interação)
    if (listen_sec == 0 && s_session.seek_count == 0) {
        s_session.active = false;
        return;
    }

    uint32_t dur_sec = info->duration_sec;
    uint32_t pct = (dur_sec > 0) ? ((listen_sec * 100U) / dur_sec) : 100U;
    if (pct > 100U && dur_sec > 0) pct = 100U;

    // Regra oficial de Scrobble (Spotify / Last.fm):
    // Ouvido por >= 30s E (>= 50% da duração total OU >= 240s).
    // Para faixas curtas (< 30s de duração total), exige >= 80% ouvido.
    bool scrobbled = false;
    if (dur_sec > 0 && dur_sec < 30) {
        scrobbled = (pct >= 80U);
    } else {
        scrobbled = (listen_sec >= 30U) && (pct >= 50U || listen_sec >= 240U);
    }

    bool completed = (info->end_reason && strcmp(info->end_reason, "eof") == 0) || (pct >= 90U);
    bool is_skip   = (!scrobbled && listen_sec < 30U && info->end_reason &&
                      (strncmp(info->end_reason, "skip", 4) == 0 || strcmp(info->end_reason, "select") == 0));

    int avg_vol = (s_session.vol_samples > 0)
                  ? (int)(s_session.vol_weighted_sum / s_session.vol_samples)
                  : 50;

    // Obtém data/hora real do RTC DS3231
    rtc_clock_info_t clk = {0};
    int year = 2026, month = 10, day = 15, hour = 0, min = 0, sec = 0, wday = 0;
    if (rtc_ds3231_get_clock_info(&clk) == ESP_OK && clk.rtc_ok && clk.year >= 2024) {
        year  = clk.year;
        month = clk.month;
        day   = clk.day;
        hour  = clk.hour;
        min   = clk.min;
        sec   = clk.sec;
        wday  = clk.wday;
    } else {
        time_t now_t = time(NULL);
        struct tm tm_now = {0};
        localtime_r(&now_t, &tm_now);
        if (tm_now.tm_year + 1900 >= 2024) {
            year  = tm_now.tm_year + 1900;
            month = tm_now.tm_mon + 1;
            day   = tm_now.tm_mday;
            hour  = tm_now.tm_hour;
            min   = tm_now.tm_min;
            sec   = tm_now.tm_sec;
            wday  = tm_now.tm_wday;
        }
    }

    struct tm t_epoch = {0};
    t_epoch.tm_year = year - 1900;
    t_epoch.tm_mon  = month - 1;
    t_epoch.tm_mday = day;
    t_epoch.tm_hour = hour;
    t_epoch.tm_min  = min;
    t_epoch.tm_sec  = sec;
    uint32_t epoch_ts = (uint32_t)mktime(&t_epoch);

    // Estado atual do Equalizador
    eq_config_t eq_cfg = {0};
    eq_get_config(&eq_cfg);
    int p_idx = (eq_cfg.active_preset_idx >= 0 && eq_cfg.active_preset_idx < EQ_MAX_PRESETS)
                ? eq_cfg.active_preset_idx : 0;
    const char *eq_preset_name = eq_cfg.presets[p_idx].name;

    const char *rel_path = info->rel_path ? info->rel_path : s_session.rel_path;
    uint64_t track_id = compute_track_id(info->artist, info->title, rel_path);
    const char *cat = detect_category(rel_path);

    char esc_path[256], esc_title[160], esc_artist[112], esc_album[112], esc_codec[24], esc_preset[32];
    json_escape(rel_path, esc_path, sizeof(esc_path));
    json_escape(info->title ? info->title : "", esc_title, sizeof(esc_title));
    json_escape(info->artist ? info->artist : "", esc_artist, sizeof(esc_artist));
    json_escape(info->album ? info->album : "", esc_album, sizeof(esc_album));
    json_escape(info->codec ? info->codec : "PCM", esc_codec, sizeof(esc_codec));
    json_escape(eq_preset_name, esc_preset, sizeof(esc_preset));

    ensure_analytics_dir();
    char events_file[64];
    snprintf(events_file, sizeof(events_file), "%s/events_%04d.jsonl", ANALYTICS_DIR, year);

    FILE *fe = fopen(events_file, "a");
    if (fe) {
        fprintf(fe,
                "{\"ts\":\"%04d-%02d-%02dT%02d:%02d:%02d\",\"epoch\":%u,\"wday\":%d,\"hour\":%d,"
                "\"id\":\"%016llx\",\"path\":\"%s\",\"cat\":\"%s\",\"title\":\"%s\",\"artist\":\"%s\","
                "\"album\":\"%s\",\"codec\":\"%s\",\"sr\":%u,\"bits\":%u,\"kbps\":%u,"
                "\"dur_s\":%u,\"listen_s\":%u,\"pct\":%u,\"scrobble\":%s,\"completed\":%s,"
                "\"end\":\"%s\",\"seeks\":%u,\"vol\":%d,\"eq_on\":%s,\"eq_preset\":\"%s\"}\n",
                year, month, day, hour, min, sec, (unsigned)epoch_ts, wday, hour,
                (unsigned long long)track_id, esc_path, cat, esc_title, esc_artist,
                esc_album, esc_codec, (unsigned)info->sample_rate, (unsigned)info->bits_per_sample,
                (unsigned)(info->bitrate_bps / 1000U), (unsigned)dur_sec, (unsigned)listen_sec,
                (unsigned)pct, scrobbled ? "true" : "false", completed ? "true" : "false",
                info->end_reason ? info->end_reason : "unknown",
                (unsigned)s_session.seek_count, avg_vol,
                eq_cfg.enabled ? "true" : "false", esc_preset);
        fclose(fe);
    }

    // Atualiza o catálogo acumulado em PSRAM e salva tracks_summary.json
    if (s_summary) {
        int idx = -1;
        for (int i = 0; i < s_summary_count; i++) {
            if (s_summary[i].track_id == track_id) {
                idx = i;
                break;
            }
        }
        if (idx < 0 && s_summary_count < MAX_SUMMARY_TRACKS) {
            idx = s_summary_count++;
            memset(&s_summary[idx], 0, sizeof(track_summary_entry_t));
            s_summary[idx].track_id = track_id;
            s_summary[idx].first_epoch = epoch_ts;
        }
        if (idx >= 0) {
            track_summary_entry_t *e = &s_summary[idx];
            snprintf(e->rel_path, sizeof(e->rel_path), "%.*s", (int)(sizeof(e->rel_path) - 1), rel_path);
            if (info->title && info->title[0])   snprintf(e->title, sizeof(e->title), "%.*s", (int)(sizeof(e->title) - 1), info->title);
            if (info->artist && info->artist[0]) snprintf(e->artist, sizeof(e->artist), "%.*s", (int)(sizeof(e->artist) - 1), info->artist);
            if (info->album && info->album[0])   snprintf(e->album, sizeof(e->album), "%.*s", (int)(sizeof(e->album) - 1), info->album);
            if (info->codec && info->codec[0])   snprintf(e->codec, sizeof(e->codec), "%.*s", (int)(sizeof(e->codec) - 1), info->codec);
            e->sample_rate = info->sample_rate;
            e->bits_per_sample = (uint8_t)info->bits_per_sample;
            e->total_plays++;
            if (scrobbled) e->total_scrobbles++;
            if (is_skip)   e->total_skips++;
            if (completed) e->total_completed++;
            e->total_listen_sec += listen_sec;
            e->last_epoch = epoch_ts;
            save_summary_to_sd();
        }
    }

    ESP_LOGI(TAG, "[WRAPPED] %s '%s' (%u s / %u s, %u%%, scrobble=%d, end=%s)",
             scrobbled ? "SCROBBLED" : "LOGGED",
             (info->title && info->title[0]) ? info->title : rel_path,
             (unsigned)listen_sec, (unsigned)dur_sec, (unsigned)pct,
             scrobbled ? 1 : 0, info->end_reason ? info->end_reason : "?");

    s_session.active = false;
}

