#pragma once

/**
 * @file fs_browser.h
 * @brief Navegador e Varredor de Arquivos do Sistema FatFS
 *
 * Gerencia a exploração de diretórios no cartão MicroSD, ordenação de faixas
 * (alfabética ou cronológica), filtragem de extensões de áudio suportadas e
 * limpeza de arquivos temporários do sistema.
 */

#include <stddef.h>
#include <stdbool.h>
#include "audio_player.h"

#define MAX_ENTRIES 256 /**< Limite máximo de entradas catalogadas por diretório. */

/**
 * @brief Define o critério de ordenação de arquivos e pastas no navegador.
 *
 * @param[in] mode Modo de ordenação desejado (`SORT_MODE_NAME` ou `SORT_MODE_DATE`).
 */
void fs_browser_set_sort_mode(track_sort_mode_t mode);

/**
 * @brief Retorna o critério de ordenação atualmente configurado.
 *
 * @return Modo ativo de ordenação.
 */
track_sort_mode_t fs_browser_get_sort_mode(void);

/**
 * @brief Estrutura que armazena os resultados de uma varredura de diretório.
 */
struct DirScan {
    char *subdirs[MAX_ENTRIES];      /**< Lista de ponteiros com os nomes das subpastas encontradas. */
    int subdir_count;                /**< Quantidade de subpastas catalogadas. */
    char *audio_files[MAX_ENTRIES];  /**< Lista de ponteiros com os nomes dos arquivos de áudio válidos. */
    int audio_count;                 /**< Quantidade de arquivos de áudio catalogados. */

    DirScan() : subdir_count(0), audio_count(0) {}
};

/**
 * @brief Verifica se a extensão do arquivo corresponde a um formato de áudio suportado.
 *
 * Formatos aceitos: .flac, .mp3, .wav, .aac, .m4a, .ogg.
 *
 * @param[in] name Nome do arquivo ou caminho completo.
 *
 * @return true se o arquivo possui formato suportado; false caso contrário.
 */
bool has_supported_extension(const char *name);

/**
 * @brief Identifica se o arquivo ou diretório é lixo de sistema ou arquivo oculto.
 *
 * Filtra arquivos que começam com '.', pastas de sistema como 'System Volume Information',
 * '._*' (arquivos de atributos do macOS) e 'RECYCLED'.
 *
 * @param[in] name Nome do arquivo ou pasta a ser analisado.
 *
 * @return true se for lixo/arquivo oculto; false se for um item legítimo.
 */
bool is_junk_entry(const char *name);

/**
 * @brief Libera toda a memória dinâmica alocada para as strings da estrutura `DirScan`.
 *
 * @param[in,out] scan Referência para a estrutura cujas strings serão liberadas.
 */
void free_dir_scan(DirScan &scan);

/**
 * @brief Realiza a leitura e catalogação completa de um diretório no cartão MicroSD.
 *
 * Lê todas as entradas da pasta, filtra arquivos indesejados, separa subdiretórios
 * de arquivos de áudio e aplica o algoritmo de ordenação selecionado.
 *
 * @param[in]  path Caminho absoluto do diretório no sistema de arquivos FatFS.
 * @param[out] out  Estrutura onde as listas de pastas e arquivos serão preenchidas.
 */
void scan_dir(const char *path, DirScan &out);
