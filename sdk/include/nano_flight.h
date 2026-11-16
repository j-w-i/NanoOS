#ifndef NANOOS_SDK_FLIGHT_H
#define NANOOS_SDK_FLIGHT_H
/* Flight recorder: state that survives a freeze and a forced reboot, read back over
 * USB with hwbridge/pull_flight.py (no disk writes on the iPod).
 *
 * Two RAM windows outside the SDK heap and the game image, the same ones the bench
 * reports survive in (0x0ba00000, 0x0be60000), each holding:
 *   +0x0000 report header (report.h rpt_hdr, mirror format) and +0x1000 text (<=16 KiB)
 *   +0x5000 live status: magic, frame, phase, usec, user words (written every frame)
 *   +0x6000 screenshot: header + 120x216 RGB565 (the last presented frame, 2x down)
 * Cost: status ~1 us per frame; nano_flight_commit ~0.1 ms; a screenshot ~5 ms
 * (uncached frame-buffer reads), so take one every few seconds. */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void nano_flight_status(uint32_t frame, uint32_t phase, const uint32_t user[4]);  /* every frame */
void nano_flight_log(const char *fmt, ...);        /* appends to the text ring (last ~12 KiB kept) */
void nano_flight_commit(void);                     /* publish the text ring as a checksummed report */
void nano_flight_screenshot(void);                 /* copy the last presented 240x432 frame */
#ifdef __cplusplus
}
#endif
#endif
