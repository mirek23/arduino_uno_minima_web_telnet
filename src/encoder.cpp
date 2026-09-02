#include "encoder.h"

Encoder* Encoder::_instance = nullptr;

// Quadrature transition table, indexed by (previous << 2) | current where each
// state is (A << 1) | B. Legal single-step transitions yield +1/-1; no-ops and
// illegal double transitions (a missed edge) yield 0 rather than guessing.
static const int8_t QUAD_TABLE[16] = {
     0, -1, +1,  0,
    +1,  0,  0, -1,
    -1,  0,  0, +1,
     0, +1, -1,  0
};

Encoder::Encoder()
    : _prevState(0)
    , _edgeAccum(0)
    , _count(0)
    , _countChanged(false)
    , _btnState(false)
    , _btnRaw(false)
    , _btnLastChange(0)
    , _btnPressed(false)
{}

void Encoder::begin() {
    _instance = this;

    pinMode(ENC_A_PIN,   INPUT_PULLUP);
    pinMode(ENC_B_PIN,   INPUT_PULLUP);
    pinMode(ENC_BTN_PIN, INPUT_PULLUP);

    // Seed the decoder with the resting position so the first edge after boot
    // is measured against reality instead of against state 0.
    _prevState = (uint8_t)((digitalRead(ENC_A_PIN) << 1) | digitalRead(ENC_B_PIN));
    _btnRaw    = (digitalRead(ENC_BTN_PIN) == LOW);
    _btnState  = _btnRaw;

    attachInterrupt(digitalPinToInterrupt(ENC_A_PIN), isrChannelA, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ENC_B_PIN), isrChannelB, CHANGE);

    Serial.println("[Encoder] quadrature interrupts attached, button polled");
}

// ─── Interrupt path ──────────────────────────────────────────────────────────

void Encoder::onEdge() {
    uint8_t state = (uint8_t)((digitalRead(ENC_A_PIN) << 1) | digitalRead(ENC_B_PIN));
    int8_t  step  = QUAD_TABLE[(_prevState << 2) | state];
    _prevState    = state;

    if (step == 0) return;              // no movement, or a missed edge

    _edgeAccum += step;

    // Emit a detent once enough edges have accumulated in one direction.
    if (_edgeAccum >= ENC_EDGES_PER_DETENT) {
        _edgeAccum   -= ENC_EDGES_PER_DETENT;
        _count++;
        _countChanged = true;
    } else if (_edgeAccum <= -ENC_EDGES_PER_DETENT) {
        _edgeAccum   += ENC_EDGES_PER_DETENT;
        _count--;
        _countChanged = true;
    }
}

void Encoder::isrChannelA() { if (_instance) _instance->onEdge(); }
void Encoder::isrChannelB() { if (_instance) _instance->onEdge(); }

// ─── Polled path ─────────────────────────────────────────────────────────────

void Encoder::update() {
    // Debounce the button: a new raw level must hold for DEBOUNCE_MS before it
    // is accepted as the settled state.
    bool     raw = (digitalRead(ENC_BTN_PIN) == LOW);
    uint32_t now = millis();

    if (raw != _btnRaw) {
        _btnRaw        = raw;
        _btnLastChange = now;
    } else if (raw != _btnState && (now - _btnLastChange) >= DEBOUNCE_MS) {
        _btnState = raw;
        if (raw) _btnPressed = true;    // latch on press, not on release
    }
}

// ─── Accessors ───────────────────────────────────────────────────────────────

int32_t Encoder::count() const {
    return _count;
}

void Encoder::resetCount() {
    // The ISR touches all three fields, so update them as one unit.
    noInterrupts();
    _count        = 0;
    _edgeAccum    = 0;
    _countChanged = true;
    interrupts();
}

bool Encoder::countChanged() {
    if (_countChanged) { _countChanged = false; return true; }
    return false;
}

bool Encoder::buttonPressed() {
    if (_btnPressed) { _btnPressed = false; return true; }
    return false;
}

bool Encoder::buttonDown() const {
    return _btnState;
}
