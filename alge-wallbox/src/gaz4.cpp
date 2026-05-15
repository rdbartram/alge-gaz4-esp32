// ============================================================================
//  GAZ4 frame builder + UART transmitter.
//  See gaz4.h for the protocol reference.
// ============================================================================
#include "gaz4.h"
#include "config.h"

#include <Arduino.h>
#include <HardwareSerial.h>
#include <string.h>

namespace gaz4 {

// Build a match frame byte-identical to alge_match.py build_frame().
// Python ref: f"000 {h} {g}    {m1}{m2} {s1}{s2}\r"
void build_match_frame(uint8_t home, uint8_t away, uint16_t total_seconds,
                       char* out_frame) {
    if (total_seconds > 5999) total_seconds = 5999; // clamp to 99:59
    const uint8_t mm = (total_seconds / 60) % 100;
    const uint8_t ss = total_seconds % 60;
    home %= 10;
    away %= 10;

    out_frame[0]  = '0';
    out_frame[1]  = '0';
    out_frame[2]  = '0';
    out_frame[3]  = ' ';
    out_frame[4]  = '0' + home;
    out_frame[5]  = ' ';
    out_frame[6]  = '0' + away;
    out_frame[7]  = ' ';
    out_frame[8]  = ' ';
    out_frame[9]  = ' ';
    out_frame[10] = ' ';
    out_frame[11] = '0' + (mm / 10);
    out_frame[12] = '0' + (mm % 10);
    out_frame[13] = ' ';
    out_frame[14] = '0' + (ss / 10);
    out_frame[15] = '0' + (ss % 10);
    out_frame[16] = '\r';
}

void build_blank_frame(char* out_frame) {
    memset(out_frame, ' ', FRAME_LEN);
    out_frame[0]  = '0';
    out_frame[1]  = '0';
    out_frame[2]  = '0';
    out_frame[16] = '\r';
}

void build_exercise_frame(const uint8_t* idx_list, size_t idx_count,
                          uint8_t value, char* out_frame) {
    // Start with all spaces (digit positions ambiguous in single shots,
    // but in a 10Hz exercise loop they reliably blank). Address + CR fixed.
    memset(out_frame, ' ', FRAME_LEN);
    out_frame[0]  = '0';
    out_frame[1]  = '0';
    out_frame[2]  = '0';
    out_frame[16] = '\r';

    const char digit = '0' + (value % 10);
    for (size_t i = 0; i < idx_count; ++i) {
        const uint8_t pos = idx_list[i];
        // Only digit positions are meaningful.
        if (pos == 4 || pos == 6 || pos == 11 || pos == 12 ||
            pos == 14 || pos == 15) {
            out_frame[pos] = digit;
        }
    }
}

void build_test_pattern_frame(char* out_frame) {
    // "000 8 8    88 88\r" — all 8s for polarity and segment visual check.
    build_match_frame(8, 8, 88 * 60 + 88 /* doesn't matter, overwritten */,
                      out_frame);
    // Force MM=88, SS=88 explicitly to avoid clamp surprises.
    out_frame[11] = '8';
    out_frame[12] = '8';
    out_frame[14] = '8';
    out_frame[15] = '8';
}

void uart_begin() {
    // GAZ4: 2400 baud, 8N1. TX on GPIO 17, RX (unused) on GPIO 18.
    Serial1.begin(GAZ4_BAUD, SERIAL_8N1, PIN_UART_RX, PIN_UART_TX);
}

void transmit(const char* frame, int repeats, int gap_ms) {
    if (repeats < 1) repeats = 1;
    for (int i = 0; i < repeats; ++i) {
        Serial1.write(reinterpret_cast<const uint8_t*>(frame), FRAME_LEN);
        Serial1.flush();
        if (i + 1 < repeats && gap_ms > 0) {
            delay(gap_ms);
        }
    }
}

} // namespace gaz4
