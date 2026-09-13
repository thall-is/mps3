#include "fs_browser.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "audio_decoder.h"

static const char *TAG = "fs_browser";

struct FileEntry {
    char *name;
    time_t mtime;
};

static track_sort_mode_t s_sort_mode = SORT_MODE_NAME;

void fs_browser_set_sort_mode(track_sort_mode_t mode)
{
    s_sort_mode = mode;
}

track_sort_mode_t fs_browser_get_sort_mode(void)
{
    return s_sort_mode;
}

static int compare_strs(const void *a, const void *b)
{
    const char *sa = *(const char **)a;
    const char *sb = *(const char **)b;
    return strcasecmp(sa, sb);
}

static int compare_entries(const void *a, const void *b)
{
    const FileEntry *ea = (const FileEntry *)a;
    const FileEntry *eb = (const FileEntry *)b;

    if (s_sort_mode == SORT_MODE_DATE) {
        if (ea->mtime != eb->mtime) {
            // Mais antigo primeiro (ordem natural de tracklist / gravacao / download)
            return (ea->mtime < eb->mtime) ? -1 : 1;
        }
    }
    // Desempate por nome alfabetico (ou modo SORT_MODE_NAME)
    return strcasecmp(ea->name, eb->name);
}

// BUG ENCONTRADO: esta funcao mantinha sua PROPRIA lista de extensoes
// (so' .mp3/.flac/.wav/.aac/.m4a), separada e desatualizada em relacao a
// mps3::detect_audio_format() (audio_decoder.h) - que e' quem de fato
// decide o que o decoder consegue tocar. Resultado: qualquer formato
// suportado pelo decoder mas ausente desta lista a parte (.m4b, .amr,
// .awb, .3ga, .3gp, .ogg, .opus, .weba, .webm, .ts, .alac) NUNCA aparecia
// no navegador de pastas - o arquivo existia no cartao, o decoder ate'
// saberia toca-lo, mas o usuario nunca conseguia SELECIONAR ele pra
// come,car, porque scan_dir() (mais abaixo) simplesmente pulava esses
// arquivos na varredura. Corrigido delegando pra' detect_audio_format(),
// que passa a ser a UNICA fonte de verdade sobre "isso e' um arquivo de
// audio suportado?" - qualquer formato novo adicionado la' passa a
// aparecer aqui automaticamente, sem precisar lembrar de atualizar 2
// lugares.
bool has_supported_extension(const char *name)
{
    return mps3::detect_audio_format(name) != mps3::AudioFormat::Unknown;
}

bool is_junk_entry(const char *name)
{
    if (name[0] == '.') return true;
    if (strcasecmp(name, "System Volume Information") == 0) return true;
    if (strcasecmp(name, "Android") == 0) return true;
    if (strcasecmp(name, "LOST.DIR") == 0) return true;
    return false;
}

void free_dir_scan(DirScan &scan)
{
    for (int i = 0; i < scan.subdir_count; i++) free(scan.subdirs[i]);
    for (int i = 0; i < scan.audio_count; i++) free(scan.audio_files[i]);
    scan.subdir_count = 0;
    scan.audio_count = 0;
}

void scan_dir(const char *path, DirScan &out)
{
    free_dir_scan(out);
    DIR *d = opendir(path);
    if (!d) {
        ESP_LOGE(TAG, "Falha ao abrir diretorio %s", path);
        return;
    }

    FileEntry *entries = (FileEntry *)malloc(MAX_ENTRIES * sizeof(FileEntry));
    int entry_count = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (is_junk_entry(ent->d_name)) continue;

        if (ent->d_type == DT_DIR) {
            if (out.subdir_count < MAX_ENTRIES) {
                char *s = strdup(ent->d_name);
                if (s) out.subdirs[out.subdir_count++] = s;
            }
        } else if (ent->d_type == DT_REG) {
            if (has_supported_extension(ent->d_name)) {
                if (entry_count < MAX_ENTRIES) {
                    char *s = strdup(ent->d_name);
                    if (s) {
                        time_t mt = 0;
                        if (s_sort_mode == SORT_MODE_DATE) {
                            char full_path[512];
                            snprintf(full_path, sizeof(full_path), "%s/%s", path, ent->d_name);
                            struct stat st;
                            if (stat(full_path, &st) == 0) {
                                mt = st.st_mtime;
                            }
                        }
                        if (entries) {
                            entries[entry_count].name = s;
                            entries[entry_count].mtime = mt;
                            entry_count++;
                        } else {
                            if (out.audio_count < MAX_ENTRIES) {
                                out.audio_files[out.audio_count++] = s;
                            }
                        }
                    }
                }
            }
        }
    }
    closedir(d);

    if (out.subdir_count > 0)
        qsort(out.subdirs, out.subdir_count, sizeof(char*), compare_strs);

    if (entries) {
        if (entry_count > 0) {
            qsort(entries, entry_count, sizeof(FileEntry), compare_entries);
            out.audio_count = entry_count;
            for (int i = 0; i < entry_count; i++) {
                out.audio_files[i] = entries[i].name;
            }
        }
        free(entries);
    } else if (out.audio_count > 0) {
        qsort(out.audio_files, out.audio_count, sizeof(char*), compare_strs);
    }
}
