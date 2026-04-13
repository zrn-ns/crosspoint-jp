#pragma once

#include <cstdint>

// QMI8658 IMU driver for tilt-based page turning (X3 only).
// Reads accelerometer data via I2C. Gyroscope is disabled to save power.
class HalIMU {
 public:
  HalIMU() = default;

  // Initialize QMI8658 accelerometer. Returns false on X4 or if chip not found.
  // Precondition: Wire.begin() must have been called (done by HalPowerManager for X3).
  bool begin();

  // Read current accelerometer data. Call once per main loop iteration.
  void update();

  // Put QMI8658 into standby mode (call before deep sleep).
  void standby();

  // X-axis acceleration (raw 16-bit signed value, ±2G range).
  int16_t getAccelX() const { return accelX; }

  // Y-axis acceleration (raw 16-bit signed value, ±2G range).
  int16_t getAccelY() const { return accelY; }

  // Z-axis acceleration (raw 16-bit signed value, ±2G range).
  // Perpendicular to screen surface.
  int16_t getAccelZ() const { return accelZ; }

  // True if begin() succeeded and chip is active.
  bool isAvailable() const { return available; }

 private:
  int16_t accelX = 0;
  int16_t accelY = 0;
  int16_t accelZ = 0;
  bool available = false;
  uint8_t chipAddr = 0;  // Resolved I2C address (0x6B or 0x6A)

  bool writeReg(uint8_t reg, uint8_t value);
  bool readReg(uint8_t reg, uint8_t* outValue);
};

extern HalIMU imu;
