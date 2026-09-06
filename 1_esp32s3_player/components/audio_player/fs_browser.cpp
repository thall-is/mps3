#include "fs_browser.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "audio_decoder.h"

static const char *TAG = "fs_browser";

static int compare_strs(const void *a, const void *b)
{
    const char *sa = *(const char **)a;
    const char *sb = *(const char **)b;
    return strcasecmp(sa, sb);
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
                if (out.audio_count < MAX_ENTRIES) {
                    char *s = strdup(ent->d_name);
                    if (s) out.audio_files[out.audio_count++] = s;
                }
            }
        }
    }
    closedir(d);

    if (out.subdir_count > 0)
        qsort(out.subdirs, out.subdir_count, sizeof(char*), compare_strs);
    if (out.audio_count > 0)
        qsort(out.audio_files, out.audio_count, sizeof(char*), compare_strs);
}
