#include <BoardConfig.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <PowerManager.h>
#include <Preferences.h>
#include <SPI.h>
#include <Wire.h>
#include <XteinkDetect.h>
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

}  // namespace

void HalGPIO::begin() {
  // The deep-sleep path (esp_sleep_config_gpio_isolate + gpio_deep_sleep_hold_en)
  // may leave per-pin holds on the EPD pins; whether they survive the wake
  // reset on the C3 is disputed (the IDF note says digital pads reset, the
  // freeink bench found the RST hold sticking), so release them here
  // unconditionally — the calls are free and this keeps every boot identical
  // regardless of whether the probe below runs.
  gpio_hold_dis(static_cast<gpio_num_t>(EPD_RST));
  gpio_hold_dis(static_cast<gpio_num_t>(EPD_DC));
  gpio_hold_dis(static_cast<gpio_num_t>(EPD_CS));

  // Device fingerprint is I2C-only, so it can run before SPI comes up.
  _deviceType = detectDeviceTypeWithFingerprint();

  // The SDK's InputManager / SDCardManager / FreeInkDisplay all read
  // BoardConfig::ACTIVE, so the profile must be locked in before any of
  // them initialize.
  BoardConfig::selectDevice(deviceIsX3() ? BoardConfig::Board::XteinkX3 : BoardConfig::Board::XteinkX4);

  if (deviceIsX3()) {
    // 前回スリープでSDレール(GPIO13)がLOWホールドされたまま復帰した場合、
    // 無給電のSDカードが共有SPIバス(SCLK/MOSI)をクランプして下のパネル
    // プローブを妨害しうる。ホールド解除+給電+CSデアサートを先に行う
    // （SDCardManager::begin()も同処理を持つが、走るのはプローブより後）。
    BoardConfig::releaseSdRail();

    // The OEM records the panel controller in NVS hw_calib/screenType, but a
    // full-flash from another unit can overwrite it — capture it for the
    // diagnostics replay, never decide on it. The live bus probe below is the
    // ground truth. (The SDK reads this too, but only Serial.printf()s it, and
    // serial is not up yet at this point.)
    {
      Preferences prefs;
      if (prefs.begin("hw_calib", true)) {
        if (prefs.isKey("screenType")) {
          _oemScreenTypeSet = true;
          _oemScreenType = prefs.getUChar("screenType", 0);
        }
        prefs.end();
      }
    }

    // Panel-controller probe (UC8253 vs UC8279d). Bit-bangs the EPD pins, so
    // it MUST run before SPI.begin() attaches them to the SPI matrix. X3 only:
    // probing on X4 would also enable the (untested here) SSD1677->UC8179
    // auto-promotion and cost ~66ms per boot.
    freeink::applyXteinkDisplayController();
    if (BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::UC8279) {
      // applyXteinkDisplayController() only rewrites ACTIVE.displayController;
      // ACTIVE.board stays XteinkX3, and FreeInkDisplay::setDisplayX3() resets
      // any board that is not XteinkX3Uc8279 back to the plain X3 profile —
      // which would silently undo the promotion. Switch to the sibling
      // profile explicitly so setDisplayX3() preserves it.
      BoardConfig::selectDevice(BoardConfig::Board::XteinkX3Uc8279);
    }
    // The SDK probe floats RST afterwards (releaseDisplayPins()), but
    // display.begin() is seconds away (SD init, settings load, font scan),
    // and a floating reset input burns current and risks a spurious reset.
    // Keep RST actively driven HIGH until then, as the old in-tree probe did.
    pinMode(EPD_RST, OUTPUT);
    digitalWrite(EPD_RST, HIGH);
  }

  SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS);

  if (deviceIsX4()) {
    pinMode(BAT_GPIO0, INPUT);
    pinMode(UART0_RXD, INPUT);
  }

  // Last: the new InputManager reads BoardConfig::ACTIVE in begin(), so it
  // must run after selectDevice(). (X3/X4 input profiles are identical today,
  // but the ordering keeps this correct if they ever diverge.)
  inputMgr.begin();
}

void HalGPIO::logX3DisplayProbeDiag() const {
  // Replays the panel-controller decision for the serial monitor; the probe
  // itself ran in begin() (freeink::applyXteinkDisplayController()), before
  // Serial.begin(), so anything the SDK printed there never reached the
  // monitor.
  if (!deviceIsX3()) {
    return;
  }
  const freeink::XteinkDisplayProbeDiag& diag = freeink::getXteinkDisplayProbeDiag();
  if (!diag.valid) {
    return;
  }
  if (_oemScreenTypeSet) {
    LOG_INF("XTDET", "NVS hw_calib/screenType=%u [info only]", _oemScreenType);
  } else {
    LOG_INF("XTDET", "NVS hw_calib/screenType: not set [info only]");
  }
  static constexpr const char* VERDICT_NAMES[] = {"UC8253 assumed", "UC8279 confirmed", "inconclusive"};
  const char* verdictName = diag.verdict < 3 ? VERDICT_NAMES[diag.verdict] : "?";
  const uint8_t* ver = diag.ver;
  LOG_INF("XTDET", "bus probe VER=%02X %02X %02X %02X %02X FLG=%02X -> %s", ver[0], ver[1], ver[2], ver[3], ver[4],
          diag.flg, verdictName);
  if (diag.mtpValid) {
    constexpr size_t MTP_LEN = sizeof(diag.mtp);
    char mtpHex[MTP_LEN * 3 + 1];
    for (size_t i = 0; i < MTP_LEN; i++) {
      snprintf(&mtpHex[i * 3], 4, " %02X", diag.mtp[i]);
    }
    LOG_INF("XTDET", "MTP[0x000..0x02F]:%s", mtpHex);
  }
  if (diag.promoted) {
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
  // 短押し復帰の再スリープ経路。Storage.begin()が既にSDレールを再給電して
  // いる場合があるため、ここでもレールをOFFホールドしないとSD給電つきの
  // スリープに戻ってしまう。ホールドをスリープ中も維持するために
  // gpio_deep_sleep_hold_en()が必要（HalPowerManager::startDeepSleepと同じ）。
  freeink::PowerManager::powerDownRailsForSleep();
  gpio_deep_sleep_hold_en();
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
