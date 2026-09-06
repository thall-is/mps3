#include "bitrate_abr.h"

void bitrate_abr_init(bitrate_abr_state_t *st, bitrate_abr_tier_t start_tier)
{
    st->tier = start_tier;
    st->consecutive_fail = 0;
    st->consecutive_ok = 0;
}

bool bitrate_abr_report(bitrate_abr_state_t *st, bool ok)
{
    if (ok) {
        st->consecutive_ok++;
        if (st->consecutive_fail > 0) st->consecutive_fail--;

        if (st->consecutive_ok >= BITRATE_ABR_UPGRADE_AFTER_OK &&
            st->tier > BITRATE_ABR_TIER_HIGH) {
            st->tier = (bitrate_abr_tier_t)(st->tier - 1);
            st->consecutive_ok = 0;
            return true;
        }
        return false;
    }

    st->consecutive_fail += 2;
    st->consecutive_ok = 0;

    if (st->consecutive_fail >= BITRATE_ABR_DOWNGRADE_AFTER_FAILS &&
        st->tier < BITRATE_ABR_TIER_MOBILE) {
        st->tier = (bitrate_abr_tier_t)(st->tier + 1);
        st->consecutive_fail = 0;
        return true;
    }
    return false;
}
