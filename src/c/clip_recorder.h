#pragma once

#ifdef TUNER_DEBUG
// Debug tool: records a labelled clip of raw mic audio and sends it to PKJS,
// which logs it for scripts/clip_logs_to_wav.py
void clip_recorder_show(void);
#endif
