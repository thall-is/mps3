#include "a2dp_media_payload.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(void)
{
    uint8_t buf[64];
    a2dp_media_stream_state_t state = {0};

    // Pacote 1: 4 frames LDAC, 128*4=512 amostras
    bool ok = a2dp_media_write_header(buf, sizeof(buf), &state, 4, 512);
    assert(ok);

    // V=2,P=0,X=0,CC=0 -> 0x80
    assert(buf[0] == 0x80);
    // M=0,PT=96 -> 0x60
    assert(buf[1] == 0x60);
    // seq_num comecava em 0, foi escrito ANTES de incrementar
    assert(buf[2] == 0x00 && buf[3] == 0x00);
    // timestamp comecava em 0
    assert(buf[4] == 0 && buf[5] == 0 && buf[6] == 0 && buf[7] == 0);
    // ssrc = 1
    assert(buf[8] == 0 && buf[9] == 0 && buf[10] == 0 && buf[11] == 1);
    // frame_count=4 nos 4 bits altos -> 0x40
    assert(buf[12] == 0x40);

    // estado avancou
    assert(state.seq_num == 1);
    assert(state.timestamp == 512);

    // Pacote 2: confere que sequence_number e timestamp acumulam
    ok = a2dp_media_write_header(buf, sizeof(buf), &state, 3, 384);
    assert(ok);
    assert(buf[2] == 0x00 && buf[3] == 0x01); // seq_num=1 (big-endian)
    assert(buf[7] == (512 & 0xFF));            // timestamp=512, coube num byte
    assert(buf[12] == 0x30);                   // frame_count=3 -> 0011 0000
    assert(state.seq_num == 2);
    assert(state.timestamp == 512 + 384);

    // Rejeita frame_count fora de 4 bits
    assert(a2dp_media_write_header(buf, sizeof(buf), &state, 16, 100) == false);
    assert(a2dp_media_write_header(buf, sizeof(buf), &state, 0, 100) == false);

    // Rejeita buffer pequeno demais
    uint8_t small[5];
    assert(a2dp_media_write_header(small, sizeof(small), &state, 1, 128) == false);

    // Confere overflow de sequence_number (deve dar wraparound de uint16 sem crashar)
    state.seq_num = 65535;
    ok = a2dp_media_write_header(buf, sizeof(buf), &state, 1, 128);
    assert(ok);
    assert(buf[2] == 0xFF && buf[3] == 0xFF); // escreveu 65535 antes de incrementar
    assert(state.seq_num == 0);               // deu a volta certinho

    printf("OK - todos os asserts passaram\n");

    // --- variante RTP-only (usada pelo aptX HD) --------------------
    {
        uint8_t rtp_buf[64];
        a2dp_media_stream_state_t rtp_state = {0};

        bool rtp_ok = a2dp_media_write_rtp_header_only(rtp_buf, sizeof(rtp_buf), &rtp_state, 4);
        assert(rtp_ok);
        assert(rtp_buf[0] == 0x80);
        assert(rtp_buf[1] == 0x60);
        assert(rtp_buf[2] == 0x00 && rtp_buf[3] == 0x00); // seq_num=0
        assert(rtp_state.seq_num == 1);
        assert(rtp_state.timestamp == 4);

        // So' 12 bytes exigidos (nao 13) - diferenca chave em relacao a
        // a2dp_media_write_header()
        uint8_t exact[12];
        rtp_ok = a2dp_media_write_rtp_header_only(exact, sizeof(exact), &rtp_state, 4);
        assert(rtp_ok);

        uint8_t too_small[11];
        rtp_ok = a2dp_media_write_rtp_header_only(too_small, sizeof(too_small), &rtp_state, 4);
        assert(!rtp_ok);

        printf("teste RTP-only (aptX HD): OK\n");
    }

    return 0;
}
