// ---------- double tap ----------
const float    TAP_G        = 0.6;   // jolt size that counts as a tap (raise if swimming triggers it)
const uint32_t TAP_SETTLE   = 80;    // ignore ringing for this long after a tap
const uint32_t DT_MIN       = 120;   // second tap must come at least this long after the first...
const uint32_t DT_MAX       = 500;   // ...and no later than this
float    gBase = 1.0;                // slow average of |acceleration| (gravity)
uint32_t lastTap = 0, firstTap = 0;
bool     waitingSecond = false;
uint32_t doubleTaps = 0;

void onDoubleTap() {
  doubleTaps++;
  Serial.println("DOUBLE TAP");
  pumpOff();      // example action: cancel a false alarm. Change to whatever you want it to do.
  buzz();         // confirmation buzz
}

void tapCheck(float x, float y, float z) {
  uint32_t now = millis();
  float m = sqrtf(x * x + y * y + z * z);
  float jolt = fabsf(m - gBase);
  gBase += 0.02f * (m - gBase);                      // slow follow, so motion doesn't count as taps

  if (waitingSecond && now - firstTap > DT_MAX) waitingSecond = false;   // too slow, start over
  if (jolt < TAP_G || now - lastTap < TAP_SETTLE) return;                // not a tap, or still ringing
  lastTap = now;

  if (!waitingSecond) { firstTap = now; waitingSecond = true; return; }  // first tap
  if (now - firstTap >= DT_MIN) { waitingSecond = false; onDoubleTap(); } // second tap in window
}



// ------------Stagger Start------------ //

const int PUMP = 13;                        // DRV8871 IN1
const uint32_t PWM_HZ = 20000, RAMP_MS = 2000;
const uint8_t RES_BITS = 10, PUMP_CH = 0;
const uint32_t MAX_DUTY = (1u << RES_BITS) - 1;   // 1023

enum PumpState { P_OFF, P_RAMP, P_ON };
PumpState pumpState = P_OFF;
uint32_t rampStart = 0;

void pumpOn() {
  if (pumpState != P_OFF) return;
  ledcAttachChannel(PUMP, PWM_HZ, RES_BITS, PUMP_CH);  // 20 kHz PWM, 10-bit
  ledcWrite(PUMP, 0);                                  // start at 0 %
  rampStart = millis();
  pumpState = P_RAMP;
}

void pumpOff() {
  if (pumpState == P_OFF) return;
  if (pumpState == P_RAMP) ledcDetach(PUMP);
  pinMode(PUMP, OUTPUT);
  digitalWrite(PUMP, LOW);
  pumpState = P_OFF;
}

void setup() {
  pinMode(PUMP, OUTPUT);
  digitalWrite(PUMP, LOW);                             // pump OFF at power-up
}

void loop() {
  if (pumpState == P_RAMP) {
    uint32_t el = millis() - rampStart;
    if (el >= RAMP_MS) {                               // ramp finished
      ledcDetach(PUMP);
      pinMode(PUMP, OUTPUT);
      digitalWrite(PUMP, HIGH);                        // true 100 %
      pumpState = P_ON;
    } else {
      ledcWrite(PUMP, (uint64_t)MAX_DUTY * el / RAMP_MS);  // duty rises linearly 0 → 100 %
    }
  }
}
