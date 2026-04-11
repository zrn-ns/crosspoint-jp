#include "HalIMU.h"

#include <HalGPIO.h>
#include <Logging.h>
#include <Wire.h>

// QMI8658 register addresses (from QMI8658 datasheet Rev 1.0)
namespace {
constexpr uint8_t REG_CTRL1 = 0x02;  // Interface and sensor enable config
constexpr uint8_t REG_CTRL2 = 0x03;  // Accelerometer settings (ODR, range)
constexpr uint8_t REG_CTRL7 = 0x08;  // Enable/disable accel and gyro
constexpr uint8_t REG_RESET = 0x60;  // Soft reset register
constexpr uint8_t REG_AX_L = 0x35;   // Accel X low byte
// AX_H = 0x36, AY_L = 0x37, AY_H = 0x38 (read as burst from REG_AX_L)

// Soft reset command
constexpr uint8_t RESET_CMD = 0xB0;

// CTRL1: Enable address auto-increment for burst reads (bit 6),
// little-endian data (bit 0 = 0)
constexpr uint8_t CTRL1_ADDR_AI = 0x40;

// CTRL2 config: ±2G range (bits 6:4 = 000), ODR = 31.25Hz (bits 3:0 = 1000)
// 31.25Hz gives new data every ~32ms which is fast enough for tilt detection.
constexpr uint8_t CTRL2_ACCEL_2G_31HZ = 0x08;

// CTRL7: enable accelerometer only (bit 0 = 1, bit 1 = 0 for gyro off)
constexpr uint8_t CTRL7_ACCEL_ONLY = 0x01;

// CTRL7: disable both (standby)
constexpr uint8_t CTRL7_STANDBY = 0x00;
}  // namespace

// Global instance
HalIMU imu;

bool HalIMU::writeReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(chipAddr);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool HalIMU::readReg(uint8_t reg, uint8_t* outValue) {
  Wire.beginTransmission(chipAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(chipAddr, static_cast<uint8_t>(1), static_cast<uint8_t>(true)) < 1) return false;
  *outValue = Wire.read();
  return true;
}

bool HalIMU::begin() {
  if (!gpio.deviceIsX3()) {
    LOG_DBG("IMU", "Not X3, skipping IMU init");
    return false;
  }

  // Try primary address, then fallback
  uint8_t whoami = 0;
  chipAddr = I2C_ADDR_QMI8658;
  if (!readReg(QMI8658_WHO_AM_I_REG, &whoami) || whoami != QMI8658_WHO_AM_I_VALUE) {
    chipAddr = I2C_ADDR_QMI8658_ALT;
    if (!readReg(QMI8658_WHO_AM_I_REG, &whoami) || whoami != QMI8658_WHO_AM_I_VALUE) {
      LOG_ERR("IMU", "QMI8658 not found");
      chipAddr = 0;
      return false;
    }
  }

  // Soft reset to ensure clean state
  writeReg(REG_RESET, RESET_CMD);
  delay(15);  // Wait for reset to complete

  // Re-verify after reset
  whoami = 0;
  if (!readReg(QMI8658_WHO_AM_I_REG, &whoami) || whoami != QMI8658_WHO_AM_I_VALUE) {
    LOG_ERR("IMU", "QMI8658 not responding after reset");
    chipAddr = 0;
    return false;
  }

  // Enable address auto-increment for burst reads
  if (!writeReg(REG_CTRL1, CTRL1_ADDR_AI)) {
    LOG_ERR("IMU", "Failed to configure CTRL1");
    return false;
  }

  // Configure accelerometer: ±2G, 31.25Hz ODR
  if (!writeReg(REG_CTRL2, CTRL2_ACCEL_2G_31HZ)) {
    LOG_ERR("IMU", "Failed to configure CTRL2");
    return false;
  }

  // Enable accelerometer only (gyro disabled)
  if (!writeReg(REG_CTRL7, CTRL7_ACCEL_ONLY)) {
    LOG_ERR("IMU", "Failed to configure CTRL7");
    return false;
  }

  // Wait for first sample to be ready
  delay(35);

  available = true;
  LOG_INF("IMU", "QMI8658 initialized at 0x%02X", chipAddr);
  return true;
}

void HalIMU::update() {
  if (!available) return;

  // Burst read 4 bytes: AX_L, AX_H, AY_L, AY_H
  // Requires CTRL1 address auto-increment to be enabled.
  Wire.beginTransmission(chipAddr);
  Wire.write(REG_AX_L);
  if (Wire.endTransmission(false) != 0) return;
  if (Wire.requestFrom(chipAddr, static_cast<uint8_t>(4), static_cast<uint8_t>(true)) < 4) {
    while (Wire.available()) Wire.read();
    return;
  }
  const uint8_t axl = Wire.read();
  const uint8_t axh = Wire.read();
  const uint8_t ayl = Wire.read();
  const uint8_t ayh = Wire.read();
  accelX = static_cast<int16_t>((static_cast<uint16_t>(axh) << 8) | axl);
  accelY = static_cast<int16_t>((static_cast<uint16_t>(ayh) << 8) | ayl);
}

void HalIMU::standby() {
  if (!available) return;
  writeReg(REG_CTRL7, CTRL7_STANDBY);
  available = false;
  LOG_DBG("IMU", "Standby");
}
