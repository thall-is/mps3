#include <stdio.h>
#include "components/audio_player/audio_decoder.h"

int main() {
    esp_audio_simple_dec_type_t type;
    const char *label;
    bool res_alac = mps3::audio_format_to_simple_dec(mps3::AudioFormat::Alac, &type, &label);
    bool res_webm = mps3::audio_format_to_simple_dec(mps3::AudioFormat::Webm, &type, &label);
    bool res_ogg  = mps3::audio_format_to_simple_dec(mps3::AudioFormat::Ogg, &type, &label);
    printf("ALAC: %d\n", res_alac);
    printf("WEBM: %d\n", res_webm);
    printf("OGG: %d\n", res_ogg);
    return 0;
}
