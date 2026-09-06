#include "bitrate_abr.h"
#include <stdio.h>
#include <assert.h>

// --- teste 1: comeca no nivel indicado, sem eventos ainda -------------
static void test_init(void)
{
    bitrate_abr_state_t st;
    bitrate_abr_init(&st, BITRATE_ABR_TIER_HIGH);
    assert(st.tier == BITRATE_ABR_TIER_HIGH);
    assert(st.consecutive_fail == 0);
    assert(st.consecutive_ok == 0);

    printf("test_init: OK\n");
}

// --- teste 2: sucessos continuos no nivel maximo NUNCA mudam nada -----
// (nao ha' pra' onde subir - ja' esta' no melhor nivel)
static void test_success_at_max_tier_never_changes(void)
{
    bitrate_abr_state_t st;
    bitrate_abr_init(&st, BITRATE_ABR_TIER_HIGH);

    for (int i = 0; i < 10000; i++) {
        bool changed = bitrate_abr_report(&st, true);
        assert(!changed);
        assert(st.tier == BITRATE_ABR_TIER_HIGH);
    }

    printf("test_success_at_max_tier_never_changes: OK (10000 sucessos, sem mudanca)\n");
}

// --- teste 3: threshold EXATO de queda - nao deve cair antes da hora --
static void test_downgrade_exact_threshold(void)
{
    bitrate_abr_state_t st;
    bitrate_abr_init(&st, BITRATE_ABR_TIER_HIGH);

    // N-1 falhas: ainda NAO deve ter caido
    for (int i = 0; i < BITRATE_ABR_DOWNGRADE_AFTER_FAILS - 1; i++) {
        bool changed = bitrate_abr_report(&st, false);
        assert(!changed);
        assert(st.tier == BITRATE_ABR_TIER_HIGH);
    }

    // a N-esima falha (a que fecha o threshold) deve cair exatamente agora
    bool changed = bitrate_abr_report(&st, false);
    assert(changed);
    assert(st.tier == BITRATE_ABR_TIER_STANDARD);
    assert(st.consecutive_fail == 0); // reseta apos a queda

    printf("test_downgrade_exact_threshold: OK (caiu exatamente na falha #%d, nao antes)\n",
           BITRATE_ABR_DOWNGRADE_AFTER_FAILS);
}

// --- teste 4: uma unica falha isolada, entre sucessos, NAO derruba o nivel
// (e' exatamente o que a histerese existe pra' evitar)
static void test_isolated_failure_does_not_downgrade(void)
{
    bitrate_abr_state_t st;
    bitrate_abr_init(&st, BITRATE_ABR_TIER_HIGH);

    for (int round = 0; round < 100; round++) {
        // uma falha isolada
        bool changed = bitrate_abr_report(&st, false);
        assert(!changed);
        assert(st.consecutive_fail == 1);

        // seguida de sucesso - reseta o contador de falha antes de
        // chegar perto do threshold
        changed = bitrate_abr_report(&st, true);
        assert(!changed);
        assert(st.consecutive_fail == 0);
        assert(st.tier == BITRATE_ABR_TIER_HIGH); // nunca caiu
    }

    printf("test_isolated_failure_does_not_downgrade: OK (100 rodadas de falha isolada, nivel nunca caiu)\n");
}

// --- teste 5: queda em cascata ate' o piso (MOBILE), e trava la' ------
static void test_cascading_downgrade_to_floor(void)
{
    bitrate_abr_state_t st;
    bitrate_abr_init(&st, BITRATE_ABR_TIER_HIGH);

    // Falhas suficientes pra' cair 2 niveis (HIGH->STANDARD->MOBILE)
    int total_fails_needed = BITRATE_ABR_DOWNGRADE_AFTER_FAILS * 2;
    int changes_seen = 0;
    for (int i = 0; i < total_fails_needed; i++) {
        if (bitrate_abr_report(&st, false)) changes_seen++;
    }
    assert(changes_seen == 2);
    assert(st.tier == BITRATE_ABR_TIER_MOBILE);

    // Mais falhas alem disso NAO devem mudar nada - ja' esta' no piso,
    // nao ha' pra' onde cair mais.
    for (int i = 0; i < 10000; i++) {
        bool changed = bitrate_abr_report(&st, false);
        assert(!changed);
        assert(st.tier == BITRATE_ABR_TIER_MOBILE);
    }

    printf("test_cascading_downgrade_to_floor: OK (caiu HIGH->STANDARD->MOBILE, trava no piso)\n");
}

// --- teste 6: recuperacao completa (MOBILE de volta pra HIGH) ---------
static void test_full_recovery_to_ceiling(void)
{
    bitrate_abr_state_t st;
    bitrate_abr_init(&st, BITRATE_ABR_TIER_MOBILE);

    // Sucessos suficientes pra' subir 2 niveis (MOBILE->STANDARD->HIGH)
    int total_ok_needed = BITRATE_ABR_UPGRADE_AFTER_OK * 2;
    int changes_seen = 0;
    for (int i = 0; i < total_ok_needed; i++) {
        if (bitrate_abr_report(&st, true)) changes_seen++;
    }
    assert(changes_seen == 2);
    assert(st.tier == BITRATE_ABR_TIER_HIGH);

    printf("test_full_recovery_to_ceiling: OK (subiu MOBILE->STANDARD->HIGH)\n");
}

// --- teste 7: threshold de subida EXATO --------------------------------
static void test_upgrade_exact_threshold(void)
{
    bitrate_abr_state_t st;
    bitrate_abr_init(&st, BITRATE_ABR_TIER_MOBILE);

    for (int i = 0; i < BITRATE_ABR_UPGRADE_AFTER_OK - 1; i++) {
        bool changed = bitrate_abr_report(&st, true);
        assert(!changed);
        assert(st.tier == BITRATE_ABR_TIER_MOBILE);
    }

    bool changed = bitrate_abr_report(&st, true);
    assert(changed);
    assert(st.tier == BITRATE_ABR_TIER_STANDARD);
    assert(st.consecutive_ok == 0);

    printf("test_upgrade_exact_threshold: OK\n");
}

// --- teste 8: uma falha isolada durante uma sequencia de recuperacao
// reresta o contador de sucessos (nao "perdoa" parcialmente)
static void test_failure_resets_recovery_progress(void)
{
    bitrate_abr_state_t st;
    bitrate_abr_init(&st, BITRATE_ABR_TIER_MOBILE);

    // Quase la' (so' falta 1 pra' completar o threshold de subida)
    for (int i = 0; i < BITRATE_ABR_UPGRADE_AFTER_OK - 1; i++) {
        bitrate_abr_report(&st, true);
    }
    assert(st.consecutive_ok == BITRATE_ABR_UPGRADE_AFTER_OK - 1);

    // uma falha bem na hora H
    bool changed = bitrate_abr_report(&st, false);
    assert(st.consecutive_ok == 0); // progresso perdido, nao parcial

    // MOBILE e' o piso, entao essa falha isolada tambem nao derruba nada
    // (ja' esta' no minimo) - so' confirma que nao mudou de nivel
    assert(!changed);
    assert(st.tier == BITRATE_ABR_TIER_MOBILE);

    // precisa do threshold INTEIRO de novo, do zero
    for (int i = 0; i < BITRATE_ABR_UPGRADE_AFTER_OK - 1; i++) {
        changed = bitrate_abr_report(&st, true);
        assert(!changed);
    }
    changed = bitrate_abr_report(&st, true);
    assert(changed);
    assert(st.tier == BITRATE_ABR_TIER_STANDARD);

    printf("test_failure_resets_recovery_progress: OK (uma falha zera o progresso de recuperacao, sem perdao parcial)\n");
}

// --- teste 9: nao oscila indefinidamente num padrao alternado plausivel
// (simula um link "na fronteira": maioria de sucessos com falhas
// esporadicas, nunca deveria alternar de nivel toda hora)
static void test_no_flapping_on_borderline_link(void)
{
    bitrate_abr_state_t st;
    bitrate_abr_init(&st, BITRATE_ABR_TIER_HIGH);

    int changes = 0;
    // 5000 eventos: 1 falha a cada 15 (bem mais espaçado que o
    // threshold de queda de 20 consecutivas) - um link real "só um
    // pouco instável", nao um link ruim de verdade.
    for (int i = 0; i < 5000; i++) {
        bool ok = (i % 15 != 0);
        if (bitrate_abr_report(&st, ok)) changes++;
    }

    // Nunca deveria cair, porque a falha isolada sempre reseta o
    // contador antes de chegar no threshold de 20 seguidas.
    assert(changes == 0);
    assert(st.tier == BITRATE_ABR_TIER_HIGH);

    printf("test_no_flapping_on_borderline_link: OK (link so' um pouco instavel nao derruba a qualidade)\n");
}

// --- teste 10: valores dos tiers casam com ldac_enc_quality_t ---------
// (documentado em bitrate_abr.h - confere que o contrato nao regrediu)
static void test_tier_values_match_ldac_enum(void)
{
    assert(BITRATE_ABR_TIER_HIGH == 0);
    assert(BITRATE_ABR_TIER_STANDARD == 1);
    assert(BITRATE_ABR_TIER_MOBILE == 2);
    printf("test_tier_values_match_ldac_enum: OK\n");
}

int main(void)
{
    test_init();
    test_success_at_max_tier_never_changes();
    test_downgrade_exact_threshold();
    test_isolated_failure_does_not_downgrade();
    test_cascading_downgrade_to_floor();
    test_full_recovery_to_ceiling();
    test_upgrade_exact_threshold();
    test_failure_resets_recovery_progress();
    test_no_flapping_on_borderline_link();
    test_tier_values_match_ldac_enum();
    printf("\nTODOS OS TESTES PASSARAM\n");
    return 0;
}
