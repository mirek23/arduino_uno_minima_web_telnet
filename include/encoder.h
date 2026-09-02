#pragma once
#include <Arduino.h>
#include "config.h"

// ─── Incremental rotary encoder + push button ────────────────────────────────
//
// Channels A (ENC_A_PIN) and B (ENC_B_PIN) are decoded with a full quadrature
// state table on both edges of both channels, so no motion is lost and
// direction reversals inside a detent cannot produce a phantom count. Both
// pins use interrupts; the raw edge count is divided by ENC_EDGES_PER_DETENT
// to give one count per mechanical click.
//
// The push button (ENC_BTN_PIN) is polled from update() rather than wired to a
// third interrupt: a 50 ms debounce is far slower than the loop period, and it
// keeps an RA4M1 ICU channel free. Internal pull-up, so LOW = pressed.
//
// Thread safety: the ISRs own _edgeAccum/_count; readers use count(), which
// performs a naturally-aligned 32-bit load (atomic on Cortex-M4).

class Encoder {
public:
    Encoder();

    // Configure pins and attach the channel interrupts. Call from setup().
    void begin();

    // Poll the button debounce state machine. Call every loop() iteration.
    void update();

    // Current detent count (signed, wraps at int32 limits).
    int32_t count() const;

    // Set the count back to zero, discarding any partial detent.
    void resetCount();

    // True (once) if the count changed since the previous call.
    bool countChanged();

    // True (once) per button press. update() clears the underlying flag.
    bool buttonPressed();

    // Current debounced button state (true = pressed).
    bool buttonDown() const;

private:
    static void isrChannelA();
    static void isrChannelB();
    void        onEdge();

    // Encoder state, written from interrupt context.
    volatile uint8_t _prevState;    // (A << 1) | B
    volatile int8_t  _edgeAccum;    // partial detent, |value| < EDGES_PER_DETENT
    volatile int32_t _count;
    volatile bool    _countChanged;

    // Button debounce state, polled from update().
    bool     _btnState;             // true = pressed (debounced)
    bool     _btnRaw;               // last raw sample
    uint32_t _btnLastChange;
    bool     _btnPressed;           // latched press event

    static Encoder* _instance;
};
