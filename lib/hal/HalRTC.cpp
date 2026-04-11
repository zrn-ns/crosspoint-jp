#include "HalRTC.h"

#include <Logging.h>
#include <Wire.h>

#include "HalGPIO.h"

HalRTC halRTC;

static constexpr uint8_t DS3231_ADDR = 0x68;

static uint8_t toBCD(uint8_t val) { return ((val / 10) << 4) | (val % 10); }
static uint8_t fromBCD(uint8_t bcd) { return (bcd >> 4) * 10 + (bcd & 0x0F); }

bool HalRTC::begin() {
  if (gpio.deviceIsX4()) {
    _available = false;
    return false;
  }

  // DS3231 の存在確認: 秒レジスタを読んで BCD として妥当か検証
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) {
    LOG_DBG("RTC", "DS3231 not found");
    _available = false;
    return false;
  }
  if (Wire.requestFrom(DS3231_ADDR, static_cast<uint8_t>(1)) < 1) {
    _available = false;
    return false;
  }
  const uint8_t sec = Wire.read();
  const uint8_t tens = (sec >> 4) & 0x07;
  const uint8_t ones = sec & 0x0F;
  if (tens > 5 || ones > 9) {
    LOG_ERR("RTC", "DS3231 seconds invalid: 0x%02x", sec);
    _available = false;
    return false;
  }

  _available = true;
  LOG_DBG("RTC", "DS3231 detected");
  return true;
}

bool HalRTC::isAvailable() const { return _available; }

bool HalRTC::readTime(struct tm& tm) const {
  if (!_available) return false;

  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) {
    LOG_ERR("RTC", "DS3231 read seek failed");
    return false;
  }
  if (Wire.requestFrom(DS3231_ADDR, static_cast<uint8_t>(7)) < 7) {
    LOG_ERR("RTC", "DS3231 read failed");
    return false;
  }

  const uint8_t sec = Wire.read();
  const uint8_t min = Wire.read();
  const uint8_t hour = Wire.read();
  Wire.read();  // 曜日 (使用しない)
  const uint8_t day = Wire.read();
  const uint8_t month = Wire.read();
  const uint8_t year = Wire.read();

  tm.tm_sec = fromBCD(sec & 0x7F);
  tm.tm_min = fromBCD(min & 0x7F);
  tm.tm_hour = fromBCD(hour & 0x3F);  // 24H モード前提
  tm.tm_mday = fromBCD(day & 0x3F);
  tm.tm_mon = fromBCD(month & 0x1F) - 1;  // struct tm は 0-11
  tm.tm_year = fromBCD(year) + 100;       // struct tm は 1900 ベース、DS3231 は 2000 ベース
  tm.tm_isdst = 0;

  // 妥当性チェック: 2024年以降かつ月日が範囲内
  if (tm.tm_year < 124 || tm.tm_mon < 0 || tm.tm_mon > 11 || tm.tm_mday < 1 || tm.tm_mday > 31) {
    LOG_ERR("RTC", "DS3231 time invalid: %d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return false;
  }

  LOG_DBG("RTC", "DS3231 read: %d-%02d-%02d %02d:%02d:%02d UTC", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
          tm.tm_hour, tm.tm_min, tm.tm_sec);
  return true;
}

bool HalRTC::writeTime(const struct tm& tm) const {
  if (!_available) return false;

  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(0x00);                              // レジスタ開始アドレス
  Wire.write(toBCD(tm.tm_sec));                  // 0x00: 秒
  Wire.write(toBCD(tm.tm_min));                  // 0x01: 分
  Wire.write(toBCD(tm.tm_hour));                 // 0x02: 時 (24H)
  Wire.write(toBCD(tm.tm_wday + 1));             // 0x03: 曜日 (1-7)
  Wire.write(toBCD(tm.tm_mday));                 // 0x04: 日
  Wire.write(toBCD(tm.tm_mon + 1));              // 0x05: 月 (1-12)
  Wire.write(toBCD((tm.tm_year + 1900) % 100));  // 0x06: 年 (下2桁)
  if (Wire.endTransmission() != 0) {
    LOG_ERR("RTC", "DS3231 write failed");
    return false;
  }

  LOG_DBG("RTC", "DS3231 write: %d-%02d-%02d %02d:%02d:%02d UTC", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
          tm.tm_hour, tm.tm_min, tm.tm_sec);
  return true;
}
