#include "DisasterClassifier.h"
#include <Adafruit_BME280.h>
#include <Adafruit_Sensor.h>
#include <LoRa.h>
#include <MPU6050_light.h>
#include <SPI.h>
#include <Wire.h>
#include <math.h>

#define MQ_PIN 2
#define I2C_SDA 4
#define I2C_SCL 5
#define MPU_SDA 21
#define MPU_SCL 16
#define TRIG_PIN 6
#define ECHO_PIN 18
#define SOUND_SPEED 0.0343F
#define LORA_SCK 12
#define LORA_MISO 13
#define LORA_MOSI 11
#define LORA_SS 10
#define LORA_RST 14
#define LORA_DIO0 9
#define LED_PIN 35

// --- SECRET KEY (MUST BE SAME ON RECEIVER) ---
uint8_t SECRET_KEY[8] = {0xA3, 0x5F, 0xB2, 0x91, 0x4C, 0xE7, 0x28, 0x6D};

Adafruit_BME280 bme;
TwoWire MPUWire = TwoWire(1);
MPU6050 mpu(MPUWire);
Eloquent::ML::Port::RandomForest clf;

int labelToAlert(const char *label) {
  if (strcmp(label, "FLASH_FLOOD") == 0)
    return 1;
  if (strcmp(label, "FOREST_FIRE") == 0)
    return 2;
  if (strcmp(label, "LANDSLIDE") == 0)
    return 3;
  return 0;
}

// --- ENCRYPTED 8-BYTE SENDER ---
void sendEncryptedData(float water_cm, float temp_c, float humidity,
                       float gas_ppm, float tilt, int alert) {
  uint8_t payload[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  // 0xFF marks sensor fields that are not connected.
  payload[0] = (uint8_t)constrain((int)water_cm, 0, 255);
  payload[1] = (uint8_t)constrain((int)(temp_c + 40), 0, 255);
  payload[2] = (uint8_t)constrain((int)humidity, 0, 100);
  payload[3] = (uint8_t)constrain((int)(gas_ppm / 4), 0, 255);
  payload[4] = (uint8_t)constrain((int)(tilt * 10), 0, 255);
  payload[7] = (uint8_t)alert;

  // XOR Encryption
  for (int i = 0; i < 8; i++)
    payload[i] ^= SECRET_KEY[i];

  LoRa.beginPacket();
  LoRa.write(payload, 8);
  LoRa.endPacket();
}

float readWaterDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  unsigned long duration = pulseIn(ECHO_PIN, HIGH, 30000UL);
  if (duration == 0)
    return NAN;
  return (duration * SOUND_SPEED) / 2.0F;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n--- LoRa Sender Node Starting ---");

  pinMode(MQ_PIN, INPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  analogReadResolution(12);
  Wire.begin(I2C_SDA, I2C_SCL);
  if (!bme.begin(0x76, &Wire)) {
    Serial.println("Could not find BME280 at 0x76. Trying 0x77...");
    if (!bme.begin(0x77, &Wire)) {
      Serial.println("WARNING: BME280 not detected! Using fallback values.");
    } else {
      Serial.println("BME280 initialized at 0x77");
    }
  } else {
    Serial.println("BME280 initialized at 0x76");
  }

  MPUWire.begin(MPU_SDA, MPU_SCL);
  byte mpu_status = mpu.begin();
  Serial.print("MPU6050 status: ");
  Serial.println(mpu_status);
  
  int mpu_retries = 0;
  while (mpu_status != 0 && mpu_retries < 5) {
    Serial.println("Failed to initialize MPU6050! Retrying...");
    delay(1000);
    mpu_status = mpu.begin();
    mpu_retries++;
  }

  if (mpu_status == 0) {
    Serial.println("Calculating MPU6050 offsets, do not move sensor...");
    delay(500);
    mpu.calcOffsets();
    Serial.println("MPU6050 initialized on custom I2C pins");
  } else {
    Serial.println("WARNING: MPU6050 initialization failed! Using fallback values.");
  }

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(433E6)) {
    Serial.println("WARNING: LoRa initialization failed!");
  } else {
    LoRa.setSpreadingFactor(7);
    LoRa.setSignalBandwidth(125E3);
    Serial.println("LoRa radio initialized at 433MHz.");
  }

  Serial.println("timestamp_ms,water_distance_cm,temperature_c,humidity_pct,"
                 "gas_raw,gas_ppm_est,accel_x_g,accel_y_g,accel_z_g,tilt_deg,"
                 "vibration_g,pressure_hpa,label");
}

void loop() {
  int gas_raw = analogRead(MQ_PIN);
  float gas_ppm = (gas_raw / 4095.0) * 1000.0;
  
  float water_cm = readWaterDistanceCm();
  if (!isfinite(water_cm)) water_cm = 0.0F;

  float temp_c = bme.readTemperature();
  if (!isfinite(temp_c)) temp_c = 25.0F;

  float humidity = bme.readHumidity();
  if (!isfinite(humidity)) humidity = 50.0F;

  float pressure_hpa = bme.readPressure() / 100.0F;
  if (!isfinite(pressure_hpa) || pressure_hpa == 0) pressure_hpa = 1013.25F;

  mpu.update();
  float accel_x_g = mpu.getAccX();
  float accel_y_g = mpu.getAccY();
  float accel_z_g = mpu.getAccZ();
  float tilt_deg = max(abs(mpu.getAngleX()), abs(mpu.getAngleY()));
  float vibration_g = sqrt(accel_x_g * accel_x_g + accel_y_g * accel_y_g +
                           accel_z_g * accel_z_g);

  if (!isfinite(accel_x_g)) accel_x_g = 0.0F;
  if (!isfinite(accel_y_g)) accel_y_g = 0.0F;
  if (!isfinite(accel_z_g)) accel_z_g = 1.0F;
  if (!isfinite(tilt_deg)) tilt_deg = 0.0F;
  if (!isfinite(vibration_g)) vibration_g = 1.0F;

  // AI Feature vector (11 features matching DisasterClassifier.h)
  float features[11] = {water_cm, temp_c,      humidity,    (float)gas_raw,
                        gas_ppm,  accel_x_g,   accel_y_g,   accel_z_g,
                        tilt_deg, vibration_g, pressure_hpa};

  const char *label = clf.predictLabel(features);
  int alert_level = labelToAlert(label);

  if (alert_level > 0) {
    digitalWrite(LED_PIN, HIGH);
  } else {
    digitalWrite(LED_PIN, LOW);
  }

  Serial.printf("%lu,%.1f,%.1f,%.1f,%d,%.1f,%.3f,%.3f,%.3f,%.1f,%.3f,%.1f,%s\n",
                millis(), water_cm, temp_c, humidity, gas_raw, gas_ppm,
                accel_x_g, accel_y_g, accel_z_g, tilt_deg, vibration_g,
                pressure_hpa, label);

  sendEncryptedData(water_cm, temp_c, humidity, gas_ppm, tilt_deg, alert_level);
  delay(1000);
}