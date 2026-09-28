#include <LoRa.h>
#include <SPI.h>
#include <WiFi.h>
#include <PubSubClient.h>

#define WIFI_SSID "TIG_NT"
#define WIFI_PASSWORD "Tig@1566"
#define MQTT_HOST "broker.hivemq.com"
#define MQTT_PORT 1883
#define MQTT_TOPIC "disaster/lora/node1"

#define MQTT_CLIENT_ID "lora-receiver-node1"

#define LORA_SCK 18
#define LORA_MISO 19
#define LORA_MOSI 23
#define LORA_SS 5
#define LORA_RST 14
#define LORA_DIO0 2
uint8_t SECRET_KEY[8] = {0xA3, 0x5F, 0xB2, 0x91, 0x4C, 0xE7, 0x28, 0x6D};

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.print("Connecting to Wi-Fi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 15000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(" connected, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(" failed");
  }
}

void connectMqtt() {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;

  Serial.print("Connecting to MQTT");
  if (mqtt.connect(MQTT_CLIENT_ID)) {
    Serial.println(" connected");
  } else {
    Serial.print(" failed, state=");
    Serial.println(mqtt.state());
  }
}

void publishReading(float water, float temp, float hum, float gas, float tilt,
                   float vib, float bat, int alert, const char* type, int rssi) {
  if (!mqtt.connected()) return;

  char message[320];
  snprintf(message, sizeof(message),
    "{\"water_cm\":%.1f,\"temperature_c\":%.1f,\"humidity_pct\":%.1f,\"gas_ppm\":%.1f,\"tilt\":%.2f,\"vibration\":%.2f,\"battery_v\":%.2f,\"alert_code\":%d,\"alert\":\"%s\",\"rssi\":%d,\"timestamp_ms\":%lu}",
    water, temp, hum, gas, tilt, vib, bat, alert, type, rssi, millis());

  if (mqtt.publish(MQTT_TOPIC, message)) {
    Serial.print("MQTT published: ");
    Serial.println(message);
  } else {
    Serial.println("MQTT publish failed");
  }
}

void setup(){
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  connectWiFi();
  connectMqtt();

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(433E6)) {
    Serial.println("LoRa initialization failed");
    while (true) delay(1000);
  }
  Serial.println("Gateway Decryptor Ready");
}
void loop(){
  connectWiFi();
  connectMqtt();
  mqtt.loop();

  int packetSize = LoRa.parsePacket();
  if(packetSize > 0){
    if (packetSize != 8) {
      while (LoRa.available()) LoRa.read();
      Serial.printf("Ignored packet with unexpected size: %d bytes\n", packetSize);
      return;
    }

    uint8_t enc[8];
    for(int i=0;i<8;i++) enc[i]=LoRa.read();
    uint8_t dec[8]; for(int i=0;i<8;i++) dec[i]=enc[i]^SECRET_KEY[i];
    float water=dec[0]; float temp=dec[1]-40; float hum=dec[2]; float gas=dec[3]*4;
    float tilt=dec[4]/10.0; float vib=dec[5]/100.0; float bat=dec[6]/50.0; int alert=dec[7];
    const char* type = "UNKNOWN";
    if(alert==0) type="NORMAL";
    else if(alert==1) type="FLASH_FLOOD";
    else if(alert==2) type="FOREST_FIRE";
    else if(alert==3) type="LANDSLIDE";
    else if(alert==4) type="POLLUTION";

    int rssi = LoRa.packetRssi();
    Serial.printf("RX RSSI:%d dBm | Water:%.0f cm Temp:%.0f C Hum:%.0f%% Gas:%.0f ppm Tilt:%.1f Vib:%.2f Bat:%.2f V | Class:%s (code %d)\n",
      rssi, water, temp, hum, gas, tilt, vib, bat, type, alert);
    publishReading(water, temp, hum, gas, tilt, vib, bat, alert, type, rssi);
  }
}