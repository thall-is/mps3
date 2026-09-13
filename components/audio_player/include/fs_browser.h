#pragma once

#include <stddef.h>
#include <stdbool.h>

#define MAX_ENTRIES 256

struct DirScan {
    char *subdirs[MAX_ENTRIES];
    int subdir_count;
    char *audio_files[MAX_ENTRIES];
    int audio_count;

    DirScan() : subdir_count(0), audio_count(0) {}
};

bool has_supported_extension(const char *name);
bool is_junk_entry(const char *name);
void free_dir_scan(DirScan &scan);
void scan_dir(const char *path, DirScan &out);
