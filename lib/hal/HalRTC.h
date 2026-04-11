#pragma once

#include <ctime>

class HalRTC {
  bool _available = false;

 public:
  // DS3231 の存在を確認。X4 では false を返す。
  // I2C は HalPowerManager::begin() で初期化済みの前提。
  bool begin();

  // begin() で DS3231 が検出されたか
  bool isAvailable() const;

  // DS3231 から UTC 時刻を読み取り struct tm に格納
  bool readTime(struct tm& tm) const;

  // struct tm (UTC) を DS3231 に書き込み
  bool writeTime(const struct tm& tm) const;
};

extern HalRTC halRTC;
