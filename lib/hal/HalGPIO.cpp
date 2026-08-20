#include <HalGPIO.h>
#include <Logging.h>
#include <Preferences.h>
#include <SPI.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <esp_sleep.h>

#include <cstring>

// Global HalGPIO instance
HalGPIO gpio;

namespace X3GPIO {

struct X3ProbeResult {
  bool bq27220 = false;
  bool ds3231 = false;
  bool qmi8658 = false;

  uint8_t score() const {
    return static_cast<uint8_t>(bq27220) + static_cast<uint8_t>(ds3231) + static_cast<uint8_t>(qmi8658);
  }
};

bool readI2CReg8(uint8_t addr, uint8_t reg, uint8_t* outValue) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(addr, static_cast<uint8_t>(1), static_cast<uint8_t>(true)) < 1) {
    return false;
  }
  *outValue = Wire.read();
  return true;
}

bool readI2CReg16LE(uint8_t addr, uint8_t reg, uint16_t* outValue) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(addr, static_cast<uint8_t>(2), static_cast<uint8_t>(true)) < 2) {
    while (Wire.available()) {
      Wire.read();
    }
    return false;
  }
  const uint8_t lo = Wire.read();
  const uint8_t hi = Wire.read();
  *outValue = (static_cast<uint16_t>(hi) << 8) | lo;
  return true;
}

bool readBQ27220CurrentMA(int16_t* outCurrent) {
  uint16_t raw = 0;
  if (!readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_CUR_REG, &raw)) {
    return false;
  }
  *outCurrent = static_cast<int16_t>(raw);
  return true;
}

bool probeBQ27220Signature() {
  uint16_t soc = 0;
  uint16_t voltageMv = 0;
  if (!readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_SOC_REG, &soc)) {
    return false;
  }
  if (soc > 100) {
    return false;
  }
  if (!readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_VOLT_REG, &voltageMv)) {
    return false;
  }
  return voltageMv >= 2500 && voltageMv <= 5000;
}

bool probeDS3231Signature() {
  uint8_t sec = 0;
  if (!readI2CReg8(I2C_ADDR_DS3231, DS3231_SEC_REG, &sec)) {
    return false;
  }
  const uint8_t tensDigit = (sec >> 4) & 0x07;
  const uint8_t onesDigit = sec & 0x0F;

  return tensDigit <= 5 && onesDigit <= 9;
}

bool probeQMI8658Signature() {
  uint8_t whoami = 0;
  if (readI2CReg8(I2C_ADDR_QMI8658, QMI8658_WHO_AM_I_REG, &whoami) && whoami == QMI8658_WHO_AM_I_VALUE) {
    return true;
  }
  if (readI2CReg8(I2C_ADDR_QMI8658_ALT, QMI8658_WHO_AM_I_REG, &whoami) && whoami == QMI8658_WHO_AM_I_VALUE) {
    return true;
  }
  return false;
}

X3ProbeResult runX3ProbePass() {
  X3ProbeResult result;
  Wire.begin(X3_I2C_SDA, X3_I2C_SCL, X3_I2C_FREQ);
  Wire.setTimeOut(6);

  result.bq27220 = probeBQ27220Signature();
  result.ds3231 = probeDS3231Signature();
  result.qmi8658 = probeQMI8658Signature();

  Wire.end();
  pinMode(20, INPUT);
  pinMode(0, INPUT);
  return result;
}

}  // namespace X3GPIO

namespace {
constexpr char HW_NAMESPACE[] = "cphw";
constexpr char NVS_KEY_DEV_OVERRIDE[] = "dev_ovr";  // 0=auto, 1=x4, 2=x3
constexpr char NVS_KEY_DEV_CACHED[] = "dev_det";    // 0=unknown, 1=x4, 2=x3

enum class NvsDeviceValue : uint8_t { Unknown = 0, X4 = 1, X3 = 2 };

NvsDeviceValue readNvsDeviceValue(const char* key, NvsDeviceValue defaultValue) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, true)) {
    return defaultValue;
  }
  const uint8_t raw = prefs.getUChar(key, static_cast<uint8_t>(defaultValue));
  prefs.end();
  if (raw > static_cast<uint8_t>(NvsDeviceValue::X3)) {
    return defaultValue;
  }
  return static_cast<NvsDeviceValue>(raw);
}

void writeNvsDeviceValue(const char* key, NvsDeviceValue value) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, false)) {
    return;
  }
  prefs.putUChar(key, static_cast<uint8_t>(value));
  prefs.end();
}

HalGPIO::DeviceType nvsToDeviceType(NvsDeviceValue value) {
  return value == NvsDeviceValue::X3 ? HalGPIO::DeviceType::X3 : HalGPIO::DeviceType::X4;
}

HalGPIO::DeviceType detectDeviceTypeWithFingerprint() {
  // Explicit override for recovery/support:
  // 0 = auto, 1 = force X4, 2 = force X3
  const NvsDeviceValue overrideValue = readNvsDeviceValue(NVS_KEY_DEV_OVERRIDE, NvsDeviceValue::Unknown);
  if (overrideValue == NvsDeviceValue::X3 || overrideValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Device override active: %s", overrideValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(overrideValue);
  }

  const NvsDeviceValue cachedValue = readNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::Unknown);
  if (cachedValue == NvsDeviceValue::X3 || cachedValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Using cached device type: %s", cachedValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(cachedValue);
  }

  // No cache yet: run active X3 fingerprint probe and persist result.
  const X3GPIO::X3ProbeResult pass1 = X3GPIO::runX3ProbePass();
  delay(2);
  const X3GPIO::X3ProbeResult pass2 = X3GPIO::runX3ProbePass();

  const uint8_t score1 = pass1.score();
  const uint8_t score2 = pass2.score();
  LOG_INF("HW", "X3 probe scores: pass1=%u(bq=%d rtc=%d imu=%d) pass2=%u(bq=%d rtc=%d imu=%d)", score1, pass1.bq27220,
          pass1.ds3231, pass1.qmi8658, score2, pass2.bq27220, pass2.ds3231, pass2.qmi8658);
  const bool x3Confirmed = (score1 >= 2) && (score2 >= 2);
  const bool x4Confirmed = (score1 == 0) && (score2 == 0);

  if (x3Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X3);
    return HalGPIO::DeviceType::X3;
  }

  if (x4Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X4);
    return HalGPIO::DeviceType::X4;
  }

  // Conservative fallback for first boot with inconclusive probes.
  return HalGPIO::DeviceType::X4;
}

// --- X3 panel-controller fingerprint (UC8253 vs UC8279d) --------------------
// Newer X3 production units ship a UC8279d panel controller on the same
// board, glass and pins (Issue #109). Ported from freeink-sdk XteinkDetect:
// a half-duplex 4-wire SPI probe bit-banged on the EPD pins reads the
// UC8279's VER (0x70) and FLG (0x71) registers after a reset pulse. The
// UC8253 answers neither, so its bus floats to a uniform level. MUST run
// before SPI.begin() attaches the pins to the SPI matrix.
//
// Result caching reuses NvsDeviceValue: 1 (X4) = UC8253, 2 (X3) = UC8279 —
// the same value mapping upstream CrossPoint uses for these keys.
constexpr char NVS_KEY_EPD_OVERRIDE[] = "epd_ovr";  // 0=auto, 1=uc8253, 2=uc8279
constexpr char NVS_KEY_EPD_CACHED[] = "epd_det";    // 0=unknown, 1=uc8253, 2=uc8279

constexpr size_t EPD_MTP_DUMP_LEN = 48;

// Snapshot of the last controller decision, kept so main() can replay the
// diagnostics AFTER Serial.begin() — the probe itself runs in gpio.begin(),
// before serial is up, so anything logged there never reaches the monitor.
struct ProbeDiag {
  enum class Source : uint8_t { None = 0, Override, Cached, Probe };
  Source source = Source::None;
  bool uc8279 = false;
  uint8_t verdict = 0;  // X3EpdProbe::Verdict when source == Probe
  uint8_t ver[5] = {0};
  uint8_t flg = 0;
  uint8_t mtp[EPD_MTP_DUMP_LEN] = {0};
  bool mtpValid = false;
  bool oemScreenTypeSet = false;
  uint8_t oemScreenType = 0;
};
ProbeDiag g_epdProbeDiag;

namespace X3EpdProbe {

// UC81xx read-capable registers (UC8179 / UC8279d datasheets, identical
// layout). The UC8253 has none of them.
constexpr uint8_t UC81XX_CMD_VER = 0x70;   // reserved 0x00, CHIP_VER, LUT_VER[23:0]
constexpr uint8_t UC81XX_CMD_FLG = 0x71;   // status; BUSY_N (D0) = 1 when idle
constexpr uint8_t UC81XX_CMD_RMTP = 0xA2;  // bulk MTP read: 1 dummy byte, then MTP[0..n]

constexpr size_t MTP_DUMP_LEN = EPD_MTP_DUMP_LEN;

inline void clockDelay() { delayMicroseconds(1); }  // <= ~500 kHz (upper bound), timing-safe

void writeByte(uint8_t b) {
  for (uint8_t i = 0; i < 8; i++) {
    digitalWrite(EPD_MOSI, (b & 0x80) ? HIGH : LOW);
    clockDelay();
    digitalWrite(EPD_SCLK, HIGH);
    clockDelay();
    digitalWrite(EPD_SCLK, LOW);
    b <<= 1;
  }
}

uint8_t readByte() {
  uint8_t b = 0;
  for (uint8_t i = 0; i < 8; i++) {
    // The controller shifts the next bit out on the SCL falling edge; sample
    // while the clock is low, then pulse.
    clockDelay();
    b = static_cast<uint8_t>((b << 1) | (digitalRead(EPD_MOSI) == HIGH ? 1 : 0));
    digitalWrite(EPD_SCLK, HIGH);
    clockDelay();
    digitalWrite(EPD_SCLK, LOW);
  }
  return b;
}

// One command + N-byte half-duplex read: command with DC low, then SDA (our
// MOSI) released to input with DC high while the controller drives the reads.
void cmdRead(uint8_t cmd, uint8_t* out, uint8_t len) {
  pinMode(EPD_MOSI, OUTPUT);
  digitalWrite(EPD_DC, LOW);
  digitalWrite(EPD_CS, LOW);
  clockDelay();
  writeByte(cmd);
  // Release SDA BEFORE raising DC (deliberate swap vs the freeink reference,
  // which raises DC first): once DC is high the controller may start driving
  // the line, and pinMode() is slow enough to leave a contention window.
  pinMode(EPD_MOSI, INPUT_PULLUP);
  digitalWrite(EPD_DC, HIGH);
  clockDelay();
  for (uint8_t i = 0; i < len; i++) {
    out[i] = readByte();
  }
  digitalWrite(EPD_CS, HIGH);
  pinMode(EPD_MOSI, OUTPUT);
}

// A released SDA reads back all-0x00 or all-0xFF through the pull-up; any
// variation across the five VER bytes means a real, driven response.
bool verIsFloating(const uint8_t ver[5]) {
  for (int i = 1; i < 5; i++) {
    if (ver[i] != ver[0]) {
      return false;
    }
  }
  return true;
}

bool matchUc81xx(const uint8_t ver[5], uint8_t flg) {
  // FLG must be a real, non-floating status with BUSY_N (bit0) asserted
  // (idle), and VER an actually-driven (non-uniform) pattern.
  if (flg == 0x00 || flg == 0xFF) {
    return false;
  }
  if ((flg & 0x01) != 0x01) {
    return false;
  }
  return !verIsFloating(ver);
}

bool runProbePass(uint8_t ver[5], uint8_t* flg, uint8_t rstLowMs) {
  pinMode(EPD_CS, OUTPUT);
  digitalWrite(EPD_CS, HIGH);
  pinMode(EPD_SCLK, OUTPUT);
  digitalWrite(EPD_SCLK, LOW);
  pinMode(EPD_DC, OUTPUT);
  digitalWrite(EPD_DC, LOW);
  pinMode(EPD_MOSI, OUTPUT);
  pinMode(EPD_BUSY, INPUT);

  // Hardware reset pulse. We can't trust BUSY polarity here — the controller
  // (and therefore its idle level) is exactly what we're trying to identify —
  // so a flat settle delay covers every UC81xx power-up. EInkDisplay::begin()
  // resets again afterwards, so this leaves no state behind.
  pinMode(EPD_RST, OUTPUT);
  digitalWrite(EPD_RST, HIGH);
  delay(2);
  digitalWrite(EPD_RST, LOW);
  delay(rstLowMs);
  digitalWrite(EPD_RST, HIGH);
  delay(30);

  uint8_t flgByte = 0;
  cmdRead(UC81XX_CMD_FLG, &flgByte, 1);
  cmdRead(UC81XX_CMD_VER, ver, 5);
  if (flg != nullptr) {
    *flg = flgByte;
  }
  return matchUc81xx(ver, flgByte);
}

void releasePins() {
  // Leave the data pins released; EInkDisplay::begin() reconfigures them.
  pinMode(EPD_SCLK, INPUT);
  pinMode(EPD_MOSI, INPUT);
  pinMode(EPD_CS, INPUT_PULLUP);  // don't leave the panel selected
  pinMode(EPD_DC, INPUT);
  // Keep RST actively driven HIGH (deliberate divergence from the freeink
  // reference, which floats it): EInkDisplay::begin() may be seconds away
  // (SD init, font scan, button settle), and a floating reset input burns
  // current in the input buffer and relies on an assumed internal pull-up.
  pinMode(EPD_RST, OUTPUT);
  digitalWrite(EPD_RST, HIGH);
}

enum class Verdict : uint8_t { Uc8253Assumed, Uc8279Confirmed, Inconclusive };

// Two-pass probe with agreement. Confirmed only when both passes match the
// UC81xx signature AND agree on the VER bytes — a floating bus can't produce
// the same stable non-trivial pattern twice. Reset budget: pass 1 screens
// with a short 1 ms reset, escalating once to the vendor-identification
// timing (RST low 50 ms) on failure; pass 2 confirms at 50 ms only when
// pass 1 matched.
Verdict probeController() {
  uint8_t ver1[5] = {0};
  uint8_t ver2[5] = {0};
  uint8_t flg1 = 0;
  bool pass1 = runProbePass(ver1, &flg1, /*rstLowMs=*/1);
  if (!pass1) {
    delay(2);
    pass1 = runProbePass(ver1, &flg1, /*rstLowMs=*/50);
  }
  delay(2);
  const bool pass2 = runProbePass(ver2, nullptr, /*rstLowMs=*/pass1 ? 50 : 1);

  const bool verAgree = memcmp(ver1, ver2, 5) == 0;
  bool confirmed = pass1 && pass2 && verAgree;
  const bool flgDriven = flg1 != 0x00 && flg1 != 0xFF && (flg1 & 0x01) == 0x01;

  // Ground-truth dump of the module's factory configuration: RMTP (0xA2)
  // returns a dummy byte then MTP[0..n]. On a confirmed part it's
  // diagnostics; on the fallback path below it is the discriminator itself.
  // A part without RMTP (UC8253) floats the line and reads uniform garbage.
  uint8_t* mtp = g_epdProbeDiag.mtp;
  bool mtpValid = false;
  if (confirmed || flgDriven) {
    uint8_t raw[MTP_DUMP_LEN + 1] = {0};
    cmdRead(UC81XX_CMD_RMTP, raw, sizeof(raw));
    memcpy(mtp, raw + 1, MTP_DUMP_LEN);
    mtpValid = true;
  }

  // Fallback match — field-observed UC8279d signature: some units return
  // VER = FF FF FF FF FF (blank/unreadable LUT_VER area) with a driven FLG,
  // which the uniform-VER floating-bus test wrongly rejects. A pulled-up
  // floating bus also reads FF — so require POSITIVE evidence: the RMTP dump
  // must start with the 0xA5 MTP key, which only a real UC81xx with a
  // programmed MTP can produce. The UC8253 has no RMTP command.
  if (!confirmed && flgDriven && verAgree && verIsFloating(ver1) && ver1[0] == 0xFF && mtpValid && mtp[0] == 0xA5) {
    confirmed = true;
  }
  releasePins();

  Verdict verdict = Verdict::Inconclusive;
  if (confirmed) {
    verdict = Verdict::Uc8279Confirmed;
  } else if (!pass1 && !pass2) {
    verdict = Verdict::Uc8253Assumed;
  }

  // Persist the raw probe evidence for the post-Serial.begin() replay (the
  // probe runs before serial is up, so anything logged here only reaches the
  // RTC ring buffer). Keeping the hex formatting out of this frame also keeps
  // its stack usage within the 256-byte local-variable guideline.
  memcpy(g_epdProbeDiag.ver, (pass1 && pass2) ? ver2 : ver1, 5);
  g_epdProbeDiag.flg = flg1;
  g_epdProbeDiag.mtpValid = mtpValid;
  g_epdProbeDiag.verdict = static_cast<uint8_t>(verdict);
  LOG_INF("XTDET", "bus probe verdict=%u", static_cast<unsigned>(verdict));
  return verdict;
}

const char* verdictName(uint8_t v) {
  switch (static_cast<Verdict>(v)) {
    case Verdict::Uc8279Confirmed:
      return "UltraChip";
    case Verdict::Uc8253Assumed:
      return "default controller";
    default:
      return "inconclusive (default)";
  }
}

}  // namespace X3EpdProbe

bool detectX3DisplayIsUc8279() {
  const NvsDeviceValue overrideValue = readNvsDeviceValue(NVS_KEY_EPD_OVERRIDE, NvsDeviceValue::Unknown);
  if (overrideValue != NvsDeviceValue::Unknown) {
    g_epdProbeDiag.source = ProbeDiag::Source::Override;
    g_epdProbeDiag.uc8279 = overrideValue == NvsDeviceValue::X3;
    return g_epdProbeDiag.uc8279;
  }

  const NvsDeviceValue cachedValue = readNvsDeviceValue(NVS_KEY_EPD_CACHED, NvsDeviceValue::Unknown);
  if (cachedValue != NvsDeviceValue::Unknown) {
    g_epdProbeDiag.source = ProbeDiag::Source::Cached;
    g_epdProbeDiag.uc8279 = cachedValue == NvsDeviceValue::X3;
    return g_epdProbeDiag.uc8279;
  }

  // The OEM records the panel controller in NVS hw_calib/screenType, but a
  // full-flash from another unit can overwrite it — capture it for
  // diagnostics, never decide on it. The live bus probe is the ground truth.
  {
    Preferences prefs;
    if (prefs.begin("hw_calib", true)) {
      if (prefs.isKey("screenType")) {
        g_epdProbeDiag.oemScreenTypeSet = true;
        g_epdProbeDiag.oemScreenType = prefs.getUChar("screenType", 0);
      }
      prefs.end();
    }
  }

  g_epdProbeDiag.source = ProbeDiag::Source::Probe;
  const X3EpdProbe::Verdict verdict = X3EpdProbe::probeController();
  if (verdict == X3EpdProbe::Verdict::Uc8279Confirmed) {
    LOG_INF("XTDET", "promoted UC8253 -> UC8279");
    writeNvsDeviceValue(NVS_KEY_EPD_CACHED, NvsDeviceValue::X3);
    g_epdProbeDiag.uc8279 = true;
    return true;
  }
  if (verdict == X3EpdProbe::Verdict::Uc8253Assumed) {
    // Cache the negative too, or every cold boot on a UC8253 unit pays the
    // full ~150 ms escalated probe.
    writeNvsDeviceValue(NVS_KEY_EPD_CACHED, NvsDeviceValue::X4);
  }
  // Inconclusive: run as UC8253 (the shipping controller) but don't persist,
  // so a flaky first boot gets re-probed.
  return false;
}

}  // namespace

void HalGPIO::begin() {
  inputMgr.begin();

  // The deep-sleep path (esp_sleep_config_gpio_isolate + gpio_deep_sleep_hold_en)
  // may leave per-pin holds on the EPD pins; whether they survive the wake
  // reset on the C3 is disputed (the IDF note says digital pads reset, the
  // freeink bench found the RST hold sticking), so release them here
  // unconditionally — the calls are free and this keeps every boot identical
  // regardless of whether the probe below runs or hits its NVS cache.
  gpio_hold_dis(static_cast<gpio_num_t>(EPD_RST));
  gpio_hold_dis(static_cast<gpio_num_t>(EPD_DC));
  gpio_hold_dis(static_cast<gpio_num_t>(EPD_CS));

  // Device fingerprint is I2C-only, so it can run before SPI comes up. The
  // panel-controller probe bit-bangs the EPD pins, so it MUST run before
  // SPI.begin() attaches them to the SPI matrix.
  _deviceType = detectDeviceTypeWithFingerprint();
  _x3DisplayIsUc8279 = deviceIsX3() && detectX3DisplayIsUc8279();

  SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS);

  if (deviceIsX4()) {
    pinMode(BAT_GPIO0, INPUT);
    pinMode(UART0_RXD, INPUT);
  }
}

HalGPIO::EpdOverride HalGPIO::toggleX3DisplayControllerOverride() {
  // Three-state cycle: auto -> force UC8253 -> force UC8279 -> auto. Both
  // forced states must be reachable by buttons alone: a UC8253 unit
  // misdetected as UC8279 needs force-UC8253, and a UC8279 unit whose probe
  // stays Inconclusive (e.g. an FLG variant the matcher rejects) needs
  // force-UC8279 — neither can navigate a settings UI with a dead screen.
  // The forced states take effect in-RAM immediately (display init runs
  // after this); returning to auto only takes effect next boot, because SPI
  // already owns the EPD pins and re-probing now is unsafe.
  const NvsDeviceValue current = readNvsDeviceValue(NVS_KEY_EPD_OVERRIDE, NvsDeviceValue::Unknown);
  if (current == NvsDeviceValue::Unknown) {
    writeNvsDeviceValue(NVS_KEY_EPD_OVERRIDE, NvsDeviceValue::X4);
    _x3DisplayIsUc8279 = false;
    LOG_INF("XTDET", "EPD override -> force UC8253");
    return EpdOverride::ForceUc8253;
  }
  if (current == NvsDeviceValue::X4) {
    writeNvsDeviceValue(NVS_KEY_EPD_OVERRIDE, NvsDeviceValue::X3);
    _x3DisplayIsUc8279 = true;
    LOG_INF("XTDET", "EPD override -> force UC8279");
    return EpdOverride::ForceUc8279;
  }
  // Force-UC8279 active: back to auto, drop the cache so the next boot
  // re-probes from scratch.
  writeNvsDeviceValue(NVS_KEY_EPD_OVERRIDE, NvsDeviceValue::Unknown);
  writeNvsDeviceValue(NVS_KEY_EPD_CACHED, NvsDeviceValue::Unknown);
  LOG_INF("XTDET", "EPD override cleared -> auto-detect on next boot");
  return EpdOverride::Auto;
}

void HalGPIO::logX3DisplayProbeDiag() const {
  // Replays the panel-controller decision for the serial monitor; the
  // decision itself was made in begin(), before Serial.begin().
  if (!deviceIsX3() || g_epdProbeDiag.source == ProbeDiag::Source::None) {
    return;
  }
  switch (g_epdProbeDiag.source) {
    case ProbeDiag::Source::Override:
      LOG_INF("XTDET", "EPD controller override active: %s", g_epdProbeDiag.uc8279 ? "UC8279" : "UC8253");
      return;
    case ProbeDiag::Source::Cached:
      LOG_INF("XTDET", "Using cached EPD controller: %s", g_epdProbeDiag.uc8279 ? "UC8279" : "UC8253");
      return;
    default:
      break;
  }
  if (g_epdProbeDiag.oemScreenTypeSet) {
    LOG_INF("XTDET", "NVS hw_calib/screenType=%u [info only]", g_epdProbeDiag.oemScreenType);
  } else {
    LOG_INF("XTDET", "NVS hw_calib/screenType: not set [info only]");
  }
  const uint8_t* ver = g_epdProbeDiag.ver;
  LOG_INF("XTDET", "bus probe VER=%02X %02X %02X %02X %02X FLG=%02X -> %s", ver[0], ver[1], ver[2], ver[3], ver[4],
          g_epdProbeDiag.flg, X3EpdProbe::verdictName(g_epdProbeDiag.verdict));
  if (g_epdProbeDiag.mtpValid) {
    char mtpHex[EPD_MTP_DUMP_LEN * 3 + 1];
    for (size_t i = 0; i < EPD_MTP_DUMP_LEN; i++) {
      snprintf(&mtpHex[i * 3], 4, " %02X", g_epdProbeDiag.mtp[i]);
    }
    LOG_INF("XTDET", "MTP[0x000..0x02F]:%s", mtpHex);
  }
  if (g_epdProbeDiag.uc8279) {
    LOG_INF("XTDET", "promoted UC8253 -> UC8279");
  }
}

void HalGPIO::update() {
  inputMgr.update();
  const bool connected = isUsbConnected();
  usbStateChanged = (connected != lastUsbConnected);
  lastUsbConnected = connected;
}

bool HalGPIO::wasUsbStateChanged() const { return usbStateChanged; }

bool HalGPIO::isPressed(uint8_t buttonIndex) const { return inputMgr.isPressed(buttonIndex); }

bool HalGPIO::wasPressed(uint8_t buttonIndex) const { return inputMgr.wasPressed(buttonIndex); }

bool HalGPIO::wasAnyPressed() const { return inputMgr.wasAnyPressed(); }

bool HalGPIO::wasReleased(uint8_t buttonIndex) const { return inputMgr.wasReleased(buttonIndex); }

bool HalGPIO::wasAnyReleased() const { return inputMgr.wasAnyReleased(); }

unsigned long HalGPIO::getHeldTime() const { return inputMgr.getHeldTime(); }

void HalGPIO::startDeepSleep() {
  // Ensure that the power button has been released to avoid immediately turning back on if you're holding it
  while (inputMgr.isPressed(BTN_POWER)) {
    delay(50);
    inputMgr.update();
  }
  // Arm the wakeup trigger *after* the button is released
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  // Enter Deep Sleep
  esp_deep_sleep_start();
}

void HalGPIO::verifyPowerButtonWakeup(uint16_t requiredDurationMs, bool shortPressAllowed) {
  if (shortPressAllowed) {
    // Fast path - no duration check needed
    return;
  }
  // TODO: Intermittent edge case remains: a single tap followed by another single tap
  // can still power on the device. Tighten wake debounce/state handling here.

  // Calibrate: subtract boot time already elapsed, assuming button held since boot
  const uint16_t calibration = millis();
  const uint16_t calibratedDuration = (calibration < requiredDurationMs) ? (requiredDurationMs - calibration) : 1;

  const auto start = millis();
  inputMgr.update();
  // inputMgr.isPressed() may take up to ~500ms to return correct state
  while (!inputMgr.isPressed(BTN_POWER) && millis() - start < 1000) {
    delay(10);
    inputMgr.update();
  }
  if (inputMgr.isPressed(BTN_POWER)) {
    do {
      delay(10);
      inputMgr.update();
    } while (inputMgr.isPressed(BTN_POWER) && inputMgr.getHeldTime() < calibratedDuration);
    if (inputMgr.getHeldTime() < calibratedDuration) {
      startDeepSleep();
    }
  } else {
    startDeepSleep();
  }
}

bool HalGPIO::isUsbConnected() const {
  if (deviceIsX3()) {
    // X3: infer USB/charging via BQ27220 Current() register (0x0C, signed mA).
    // Positive current means charging.
    for (uint8_t attempt = 0; attempt < 2; ++attempt) {
      int16_t currentMa = 0;
      if (X3GPIO::readBQ27220CurrentMA(&currentMa)) {
        return currentMa > 0;
      }
      delay(2);
    }
    return false;
  }
  // U0RXD/GPIO20 reads HIGH when USB is connected
  return digitalRead(UART0_RXD) == HIGH;
}

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
  const auto wakeupCause = esp_sleep_get_wakeup_cause();
  const auto resetReason = esp_reset_reason();

  const bool usbConnected = isUsbConnected();

  if ((wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && !usbConnected) ||
      (wakeupCause == ESP_SLEEP_WAKEUP_GPIO && resetReason == ESP_RST_DEEPSLEEP && usbConnected)) {
    return WakeupReason::PowerButton;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_UNKNOWN && usbConnected) {
    return WakeupReason::AfterFlash;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && usbConnected) {
    return WakeupReason::AfterUSBPower;
  }
  return WakeupReason::Other;
}
