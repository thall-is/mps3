#ifndef AUDIO_DECODER_H
#define AUDIO_DECODER_H

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <strings.h> // strcasecmp
#include "esp_audio_simple_dec.h"

// Interface comum entre decodificadores de audio, pra que o loop de
// reproducao em audio_player.cpp nao precise conhecer detalhes
// especificos de cada formato. Desde a migracao pro ESP Audio Simple
// Decoder (componente oficial espressif/esp_audio_codec), TODOS os
// formatos passam por um unico adaptador concreto (ver
// esp_codec_decoder_adapter.h) - a interface continua existindo pra
// manter o loop de audio_player.cpp desacoplado da biblioteca de
// decodificacao especifica, igual antes.
namespace mps3 {

enum class DecodeStatus {
    NeedMoreData, // dados insuficientes; leia mais do arquivo e chame de novo
    HeaderReady,  // formato do stream acabou de ser identificado (sample
                  // rate/canais ja' disponiveis); ainda sem PCM
    Success,      // amostras decodificadas (samples_decoded pode ser 0)
    EndOfStream,  // fim do arquivo/stream
    Error,        // frame corrompido ou erro fatal
};

class AudioDecoderBase {
public:
    virtual ~AudioDecoderBase() = default;

    // Decodifica o quanto for possivel a partir de `input` (ate' input_len
    // bytes), escrevendo amostras PCM 32-bit inteiras (left-justified, uma
    // por canal, intercaladas: L,R,L,R,...) em `output` (capacidade
    // output_capacity_samples amostras). Avanca bytes_consumed (bytes lidos
    // de `input`) e samples_decoded (amostras intercaladas escritas).
    virtual DecodeStatus decode(const uint8_t *input, size_t input_len,
                                 int32_t *output, size_t output_capacity_samples,
                                 size_t &bytes_consumed, size_t &samples_decoded) = 0;

    virtual uint32_t sample_rate() const = 0;
    virtual uint32_t channels() const = 0;
    virtual uint32_t bits_per_sample() const = 0;

    // Capacidade (em amostras int32 intercaladas) que o buffer de saida
    // precisa ter. Disponivel a partir de HeaderReady.
    virtual size_t recommended_output_capacity_samples() const = 0;

    // Rotulo curto pra exibir na tela (ex: "FLAC", "MP3", "AAC"). out_len
    // >= 8 e' suficiente pra qualquer formato suportado.
    virtual void format_label(char *out, size_t out_len) const = 0;
};

// NOTA sobre duracao: o ESP Audio Simple Decoder e' um decodificador de
// STREAMING - nao le' o arquivo inteiro nem expoe contagem total de
// amostras/duracao. Por isso a duracao (exata quando da' - FLAC via
// STREAMINFO, MP3 via Xing/VBRI - ou estimada por tamanho/bitrate quando
// nao da') e' calculada a parte, direto do arquivo, em metadata_extract.c
// (mesma chamada que ja' le' as tags de titulo/artista/album em
// audio_player.cpp) - nao e' mais responsabilidade do decoder.

enum class AudioFormat { Unknown, Flac, Mp3, Wav, Aac, M4a, AmrNb, AmrWb, Ogg, Webm, Ts, Vorbis };

inline AudioFormat detect_audio_format(const char *filename)
{
    size_t len = strlen(filename);
    if (len >= 5 && strcasecmp(filename + len - 5, ".flac") == 0) return AudioFormat::Flac;
    if (len >= 4 && strcasecmp(filename + len - 4, ".mp3") == 0)  return AudioFormat::Mp3;
    if (len >= 4 && strcasecmp(filename + len - 4, ".wav") == 0)  return AudioFormat::Wav;
    if (len >= 4 && strcasecmp(filename + len - 4, ".aac") == 0)  return AudioFormat::Aac;
    if (len >= 4 && strcasecmp(filename + len - 4, ".m4a") == 0)  return AudioFormat::M4a;
    // .m4b (audiobook/podcast "capitulado") usa o MESMO container MP4
    // que .m4a - so' uma convencao de extensao diferente pra sinalizar
    // "livro/podcast" em vez de "musica" pros players. Mesmo caminho de
    // codigo, inclusive o suporte a MP4 fragmentado (M4aFragDemuxer) -
    // relevante aqui, ja' que audiobooks/podcasts longos sao exatamente
    // o tipo de arquivo mais provavel de vir fragmentado (baixado em
    // pedacos/streaming).
    if (len >= 4 && strcasecmp(filename + len - 4, ".m4b") == 0)  return AudioFormat::M4a;
    // .alac - extensao rara, mas quando aparece o arquivo real quase
    // sempre e' um container MP4/M4A de verdade por dentro (confirmado:
    // um arquivo de teste real ".alac" comecava com "ftyp" + brand "M4A ",
    // isto e', byte a byte identico a um .m4a comum) - so' um apelido de
    // extensao pra sinalizar "sei que o codec e' ALAC" sem mudar o
    // container. Mesmo caminho de codigo do .m4a/.m4b: o decoder M4A da
    // lib ja' suporta ALAC internamente (ver audio_format_to_simple_dec
    // mais abaixo) e o codigo de container (scan_m4a_top_level_boxes,
    // prepare_m4a_prefix) nao faz nenhuma suposicao de codec, so' de
    // estrutura de caixas - funciona igual pra' AAC ou ALAC por dentro.
    if (len >= 5 && strcasecmp(filename + len - 5, ".alac") == 0) return AudioFormat::M4a;
    // AMR nao tem como distinguir NB de WB pelo conteudo sem abrir o
    // arquivo (create_decoder() so' recebe o nome) - usamos a convencao
    // de extensao mais comum: ".amr" = narrowband (o formato "AMR" original,
    // de longe o mais comum em gravacoes de voz antigas), ".awb" =
    // wideband. Um arquivo com a extensao "errada" pro seu conteudo real
    // falha ao abrir - raro na pratica, mas didatico deixar registrado.
    if (len >= 4 && strcasecmp(filename + len - 4, ".amr") == 0)  return AudioFormat::AmrNb;
    if (len >= 4 && strcasecmp(filename + len - 4, ".awb") == 0)  return AudioFormat::AmrWb;
    // .3gp/.3ga - contêiner 3GPP (celulares antigos), quase sempre AMR-NB
    // por dentro quando e' audio puro (gravador de voz de telefone) -
    // mesma convencao de "assume pela extensao" que .amr/.awb ja' usam.
    if (len >= 4 && strcasecmp(filename + len - 4, ".3ga") == 0)  return AudioFormat::AmrNb;
    if (len >= 4 && strcasecmp(filename + len - 4, ".3gp") == 0)  return AudioFormat::AmrNb;
    // .ogg (Vorbis) / .opus (Opus em container Ogg de verdade). Container
    // resolvido no nivel de EXTENSAO aqui e' so' "Ogg" generico - qual
    // codec tem por dentro (Vorbis ou Opus) so' da' pra saber abrindo o
    // arquivo e olhando o primeiro pacote (ver OggPacketDemuxer em
    // audio_player.cpp, que faz exatamente isso e reclassifica pra
    // AudioFormat::Vorbis internamente quando for o caso - Opus reusa o
    // mesmo caminho de AudioFormat::Webm, ja' que os dois usam o mesmo
    // decodificador Opus "cru", so' com demuxers de container diferentes
    // por fora).
    if (len >= 4 && strcasecmp(filename + len - 4, ".ogg") == 0)  return AudioFormat::Ogg;
    if (len >= 5 && strcasecmp(filename + len - 5, ".opus") == 0) return AudioFormat::Ogg;
    // .weba/.webm = Opus (ou, mais raramente, Vorbis - nao suportado)
    // dentro de um container WebM (EBML/Matroska) - formato DIFERENTE do
    // Ogg por baixo, tipico de audio baixado direto de segmentos DASH do
    // YouTube (itag 251) sem remux completo. O Simple Decoder da
    // Espressif nao entende EBML/Matroska - ver WebmOpusDemuxer em
    // audio_player.cpp, que extrai os pacotes Opus brutos manualmente.
    if (len >= 5 && strcasecmp(filename + len - 5, ".weba") == 0) return AudioFormat::Webm;
    if (len >= 5 && strcasecmp(filename + len - 5, ".webm") == 0) return AudioFormat::Webm;
    // MPEG-TS (.ts) - container de transporte usado em streaming/broadcast
    // (o mesmo formato de segmentos HLS), carregando MP3 ou AAC por
    // dentro. Suportado pelo Simple Decoder como container proprio desde
    // a v2.0.0 (nao precisa de demuxer nosso, igual AMR).
    if (len >= 3 && strcasecmp(filename + len - 3, ".ts") == 0)   return AudioFormat::Ts;
    return AudioFormat::Unknown;
}

// Mapeia pro tipo que o ESP Audio Simple Decoder usa internamente, e da'
// o rotulo de tela correspondente - usado por create_decoder() em
// audio_player.cpp pra montar o EspCodecDecoderAdapter certo.
//
// Versao exigida: espressif/esp_audio_codec == 2.6.1 (fixado, sem faixa
// de versoes - ver idf_component.yml).
inline bool audio_format_to_simple_dec(AudioFormat fmt, esp_audio_simple_dec_type_t *out_type, const char **out_label)
{
    switch (fmt) {
        case AudioFormat::Flac:  *out_type = ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;  *out_label = "FLAC";  return true;
        case AudioFormat::Mp3:   *out_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;   *out_label = "MP3";   return true;
        case AudioFormat::Wav:   *out_type = ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;   *out_label = "WAV";   return true;
        case AudioFormat::Aac:   *out_type = ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;   *out_label = "AAC";   return true;
        case AudioFormat::M4a:   *out_type = ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;   *out_label = "M4A";   return true;

        case AudioFormat::AmrNb: *out_type = ESP_AUDIO_SIMPLE_DEC_TYPE_AMRNB; *out_label = "AMR";    return true;
        case AudioFormat::AmrWb: *out_type = ESP_AUDIO_SIMPLE_DEC_TYPE_AMRWB; *out_label = "AMR-WB"; return true;
        case AudioFormat::Ts:    *out_type = ESP_AUDIO_SIMPLE_DEC_TYPE_TS;    *out_label = "TS";     return true;

        // CORRECAO DE ROTA (ver AUDIO_FORMATS_PLAN.md): .ogg/.opus agora
        // usam o tipo de container "OGG" dedicado do Simple Decoder
        // (confirmado na doc oficial da propria v2.6.1: "OGG | Supports
        // VORBIS, OPUS" - aceita o arquivo Ogg genuino, com o framing de
        // pagina intacto, e resolve Vorbis-vs-Opus por dentro sozinho).
        // Isso substitui a abordagem anterior (OggPacketDemuxer proprio,
        // removendo o framing na mao e entregando pacotes crus pro tipo
        // "raw VORBIS"/"raw OPUS") - aquele caminho exigia fornecer
        // "informacao de cabecalho comum" que a doc menciona pro tipo
        // "raw VORBIS" especificamente, e mesmo depois de um bug real ser
        // achado e corrigido ali (pacote 1 descartado - ver historico no
        // AUDIO_FORMATS_PLAN.md), Vorbis/Opus ainda nao tocavam. Usar o
        // tipo "OGG" evita esse problema inteiro: mesmo padrao simples
        // (so' entregar o arquivo cru) ja' usado com sucesso pra
        // M4A/FLAC/MP3/WAV/TS/AMR.
        case AudioFormat::Ogg:   *out_type = ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;   *out_label = "OGG";    return true;

        // WebM (.weba/.webm) nao e' um container que o Simple Decoder
        // entenda (nao existe tipo "WEBM") - continua usando o decoder
        // "OPUS cru" (frame a frame), que e' exatamente o que
        // WebmOpusDemuxer entrega: pacotes Opus ja' extraidos do
        // EBML/Matroska, sem contexto de container nenhum.
        case AudioFormat::Webm:  *out_type = ESP_AUDIO_SIMPLE_DEC_TYPE_RAW_OPUS;  *out_label = "OPUS";   return true;

        default: return false;
    }
}

} // namespace mps3

#endif // AUDIO_DECODER_H
