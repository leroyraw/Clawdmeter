#include "../../hal/sound_hal.h"
#include "board.h"

#if BOARD_HAS_SOUND

#include <Arduino.h>
#include "../../chime.h"
#include "io_expander.h"
#include "board_rev.h"

// AMOLED-1.8: ES8311 codec + speaker. Codec/I2S/playback live in the shared
// chime engine (../../chime.cpp); this file supplies the board's pins and the
// amp-enable hook. The amp enable differs by 1.8 hardware revision:
//   - V1 (SH8601/FT3168): amp enable is XCA9554 EXIO2.
//   - V2 (CO5300/CST820):  amp enable is direct GPIO 46, and EXIO2 instead gates
//                          the TOUCH controller's power rail (verified on
//                          hardware). Driving EXIO2 low on V2 kills touch, so we
//                          must NOT touch EXIO2 on that revision.
// io_expander is already up (board_init() ran it).

static void amp_enable(bool on) {
    digitalWrite(SND_PA_PIN, on ? HIGH : LOW);   // V2: direct GPIO 46
    if (board_rev() != REV_CO5300_CST816)
        io_expander_set(IOX_PIN_PA_EN, on);      // V1 only: XCA9554 EXIO2 (= touch power on V2)
}

void sound_hal_init(void) {
    pinMode(SND_PA_PIN, OUTPUT);
    const ChimeConfig cfg = {
        SND_I2S_MCLK, SND_I2S_BCLK, SND_I2S_WS, SND_I2S_DOUT, SND_I2S_DIN,
        SND_SAMPLE_RATE, SND_ES8311_ADDR, 65, amp_enable
    };
    chime_init(cfg);
}

void sound_hal_play_reset(void) { chime_play(); }
void sound_hal_tick(void)       { chime_tick(); }

#endif  // BOARD_HAS_SOUND
