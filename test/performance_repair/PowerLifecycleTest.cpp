#include <gtest/gtest.h>
#include <HalPowerManager.h>
#include <HalTiltSensor.h>
#include <WiFi.h>

bool HalGPIO::isXteinkDevice() const { return true; }

TEST(PowerRepair, BusySuspendsOnlyWaitingTaskLocksAndRestoresOnExit) {
  powerManager.begin();
  {
    HalPowerManager::Lock render;
    HalPowerManager::Lock nested;
    powerManager.setPowerSaving(true);
    EXPECT_EQ(testCpuMHz, 160);
    HalPowerManager::beginDisplayBusyWait();
    EXPECT_EQ(testCpuMHz, HalPowerManager::LOW_POWER_FREQ);
    powerManager.setPowerSaving(false);
    EXPECT_EQ(testCpuMHz, HalPowerManager::LOW_POWER_FREQ);
    testTask = reinterpret_cast<void*>(2);
    {
      HalPowerManager::Lock input;
      EXPECT_EQ(testCpuMHz, 160);
      powerManager.setPowerSaving(true);
      EXPECT_EQ(testCpuMHz, 160);
    }
    EXPECT_EQ(testCpuMHz, HalPowerManager::LOW_POWER_FREQ);
    testTask = reinterpret_cast<void*>(1);
    WiFi.mode = 1;
    powerManager.setPowerSaving(true);
    EXPECT_EQ(testCpuMHz, 160);
    WiFi.mode = WIFI_MODE_NULL;
    powerManager.setPowerSaving(false);
    EXPECT_EQ(testCpuMHz, HalPowerManager::LOW_POWER_FREQ);
    HalPowerManager::endDisplayBusyWait();
    EXPECT_EQ(testCpuMHz, 160);
  }
  powerManager.setPowerSaving(true);
  EXPECT_EQ(testCpuMHz, HalPowerManager::LOW_POWER_FREQ);
  powerManager.setPowerSaving(false);
}

TEST(PowerRepair, TiltStaysAsleepOutsideReaderAndRetriesFailedStandby) {
  HalTiltSensor sensor;
  Imu::wakes = Imu::sleeps = Imu::reads = 0;
  sensor.begin();
  ASSERT_EQ(Imu::sleeps, 1);
  for (int i = 0; i < 5; ++i) sensor.update(1, 0, false);
  EXPECT_EQ(Imu::wakes, 0);
  sensor.update(1, 0, true);
  EXPECT_EQ(Imu::wakes, 1);
  testMillis += 1000;
  sensor.update(1, 0, true);
  EXPECT_EQ(Imu::reads, 1);
  Imu::sleepWorks = false;
  sensor.update(1, 0, false);
  EXPECT_EQ(Imu::sleeps, 2);
  Imu::sleepWorks = true;
  sensor.update(1, 0, false);
  EXPECT_EQ(Imu::sleeps, 3);
  testMillis += 1000;
  sensor.update(1, 0, false);
  EXPECT_EQ(Imu::reads, 1);
  sensor.update(1, 0, true);
  EXPECT_EQ(Imu::wakes, 2);
  sensor.update(0, 0, true);
  EXPECT_EQ(Imu::sleeps, 4);
}
