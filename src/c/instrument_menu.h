#pragma once

#include "tuner_types.h"

typedef void (*InstrumentMenuCallback)(InstrumentId id);

// Pushes a menu listing the instruments with the current one selected; calls
// back with the chosen one and closes. Back closes without a choice.
void instrument_menu_show(InstrumentId current, InstrumentMenuCallback callback);
