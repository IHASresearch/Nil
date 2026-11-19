/*
 * Proje: ESP8266 Tabanlı Gerçek Zamanlı Seçici Paket Düşürme Saldırısı Tespit Sunucusu
 *
 * Bu kod, ESP8266 mikrodenetleyicisini bir kablosuz ağ istemcisi ve analiz sunucusu olarak 
 * yapılandırarak gelen UDP veri trafiğindeki seçici paket düşürme saldırılarını tespit eden alıcı yazılımıdır.
 * Sistem, her 250 ms'lik zaman penceresinde beklenen 10 paketin varış sürelerini ve 3 farklı noktadaki 
 * Wi-Fi sinyal gücü değerlerini ölçer; doğal sinyal zayıflaması ile kasıtlı paket düşürmeyi 
 * ayırt edebilmek için 5 temel istatistiksel nitelik çıkarır.
 * İçerisine gömülü makine öğrenmesi modeli (model.h) aracılığıyla gerçek zamanlı saldırı tahmini yapar, 
 * çıkarım süresini ölçer ve test aşaması sonunda vericiden aldığı gerçek etiketlerle 
 * kendi tahminlerini karşılaştırarak sistemin doğruluk yüzdesini hesaplar.
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>

constexpr char kSsid[] = "ESP8266-Drop-Lab";
constexpr char kPassword[] = "LabOnly-8266";
constexpr uint8_t kChannel = 6;
const IPAddress kAp(192, 168, 4, 1);
const IPAddress kServer(192, 168, 4, 2);
const IPAddress kMask(255, 255, 255, 0);
constexpr uint16_t kControlPort = 4210;
constexpr uint16_t kDataPort = 4211;
constexpr uint32_t kMagic = 0x44524F50;
constexpr uint16_t kSlots = 10;  // Slots 0 and 9 are reference packets.
constexpr uint16_t kIntervalMs = 18;
constexpr uint16_t kWindowMs = 250;
constexpr uint32_t kTrainTrials = 1000;
constexpr uint32_t kTestTrials = 200;
constexpr uint8_t kGo = 'G';
constexpr uint8_t kDone = 'D';
constexpr uint8_t kSendLabels = 'L';

struct RunHeader {
    uint32_t magic;
    uint32_t run;
    uint32_t count;
};

struct Datagram {
    uint32_t run;
    uint16_t trial;
    uint16_t slot;
    uint8_t payload[56];
};

static_assert(sizeof(RunHeader) == 12 && sizeof(Datagram) == 64,
              "Unexpected wire format");

#if __has_include("model.h")
#include "model.h"
#else
constexpr char kModelId[] = "none";
inline int predict(const int16_t*) { return -1; }
#endif

WiFiServer control(kControlPort);
WiFiUDP udp;

struct Observation {
    int16_t arrival[kSlots];
    int16_t rssi[3];
};

bool captureWindow(WiFiClient& link, uint32_t run, uint16_t trial, Observation& obs);
int report(uint32_t run, uint16_t trial, const Observation& obs);
bool reportAccuracy(WiFiClient& link, uint32_t run, const int8_t (&predictions)[kTestTrials]);
void serve(WiFiClient& link);

bool captureWindow(WiFiClient& link, uint32_t run, uint16_t trial, Observation& obs) {
    for (auto& value : obs.arrival) {
        value = -1;
    }

    uint8_t nextSample = 0;
    constexpr uint16_t sampleMs[] = {0, 90, 180};
    const uint32_t start = millis();

    if (link.write(&kGo, 1) != 1) {
        return false;
    }

    while (millis() - start < kWindowMs) {
        const uint32_t elapsed = millis() - start;
        if (nextSample < 3 && elapsed >= sampleMs[nextSample]) {
            const int32_t value = WiFi.RSSI();
            if (WiFi.status() != WL_CONNECTED || value >= 0 || value < -127) {
                return false;
            }
            obs.rssi[nextSample++] = static_cast<int16_t>(value);
        }

        const int size = udp.parsePacket();
        if (size > 0) {
            Datagram packet{};
            if (size == sizeof(packet) && udp.remoteIP() == kAp &&
                udp.read(reinterpret_cast<uint8_t*>(&packet), sizeof(packet)) == sizeof(packet) &&
                packet.run == run && packet.trial == trial && packet.slot < kSlots &&
                obs.arrival[packet.slot] < 0) {
                const uint32_t received = millis() - start;
                if (received < kWindowMs) {
                    obs.arrival[packet.slot] = static_cast<int16_t>(received);
                }
            }

            while (udp.available()) {
                udp.read();
            }
        }

        delay(1);
    }

    return nextSample == 3 && link.connected();
}

int report(uint32_t run, uint16_t trial, const Observation& obs) {
    int16_t missing = 0;
    for (uint8_t slot = 1; slot < kSlots - 1; ++slot) {
        missing += obs.arrival[slot] < 0;
    }

    const int16_t refsMissing = (obs.arrival[0] < 0) + (obs.arrival[kSlots - 1] < 0);
    const int16_t refSpan = refsMissing ? -1 : obs.arrival[kSlots - 1] - obs.arrival[0];
    const int16_t minRssi = min(obs.rssi[0], min(obs.rssi[1], obs.rssi[2]));
    const int16_t maxRssi = max(obs.rssi[0], max(obs.rssi[1], obs.rssi[2]));

    const int16_t features[] = {
        missing,
        refsMissing,
        refSpan,
        static_cast<int16_t>((obs.rssi[0] + obs.rssi[1] + obs.rssi[2]) / 3),
        static_cast<int16_t>(maxRssi - minRssi)
    };

    const uint32_t started = micros();
    const int prediction = predict(features);
    const uint32_t inferenceUs = micros() - started;

    Serial.printf("S,%lu,%d", static_cast<unsigned long>(run), trial);
    for (auto value : obs.rssi) {
        Serial.printf(",%d", value);
    }
    for (auto value : obs.arrival) {
        Serial.printf(",%d", value);
    }
    for (auto value : features) {
        Serial.printf(",%d", value);
    }
    Serial.printf(",%d,%lu\n", prediction, static_cast<unsigned long>(inferenceUs));

    return prediction;
}

bool reportAccuracy(WiFiClient& link, uint32_t run, const int8_t (&predictions)[kTestTrials]) {
    uint8_t labels[kTestTrials];

    if (link.write(&kSendLabels, 1) != 1 ||
        link.readBytes(reinterpret_cast<char*>(labels), sizeof(labels)) != sizeof(labels)) {
        return false;
    }

    uint16_t correct = 0;
    uint16_t attacks = 0;

    for (uint16_t trial = 0; trial < kTestTrials; ++trial) {
        if (labels[trial] > 1) {
            return false;
        }
        attacks += labels[trial];
        correct += predictions[trial] == labels[trial];
    }

    if (attacks != kTestTrials / 2) {
        return false;
    }

    if (strcmp(kModelId, "none") == 0) {
        Serial.printf("RESULT,S,%lu,NA\n", static_cast<unsigned long>(run));
        return true;
    }

    const unsigned long hundredths = correct * 10000UL / kTestTrials;
    Serial.printf("RESULT,S,%lu,%u,%lu,%lu.%02lu%%\n",
                  static_cast<unsigned long>(run),
                  static_cast<unsigned int>(correct),
                  static_cast<unsigned long>(kTestTrials),
                  hundredths / 100,
                  hundredths % 100);

    return true;
}

void serve(WiFiClient& link) {
    link.setNoDelay(true);
    link.setTimeout(2000);

    RunHeader header{};
    if (link.readBytes(reinterpret_cast<char*>(&header), sizeof(header)) != sizeof(header) ||
        header.magic != kMagic || header.run == 0 ||
        (header.count != kTrainTrials && header.count != kTestTrials)) {
        Serial.println("ERROR,S,invalid_header");
        return;
    }

    Serial.printf("META,S,%lu,%ld,%s,%s\n",
                  static_cast<unsigned long>(header.run),
                  static_cast<long>(WiFi.channel()),
                  WiFi.BSSIDstr().c_str(),
                  kModelId);

    int8_t testPredictions[kTestTrials];
    for (uint16_t trial = 0; trial < header.count; ++trial) {
        Observation obs{};
        if (!captureWindow(link, header.run, trial, obs)) {
            Serial.println("ERROR,S,window_capture");
            return;
        }

        const int prediction = report(header.run, trial, obs);
        if (header.count == kTestTrials) {
            testPredictions[trial] = static_cast<int8_t>(prediction);
        }
    }

    if (header.count == kTestTrials && !reportAccuracy(link, header.run, testPredictions)) {
        Serial.println("ERROR,S,test_labels");
        return;
    }

    if (link.write(&kDone, 1) != 1) {
        Serial.println("ERROR,S,run_finish");
        return;
    }

    Serial.printf("DONE,S,%lu\n", static_cast<unsigned long>(header.run));
}

void setup() {
    Serial.begin(115200);
    Serial.setTimeout(100);
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);

    if (!WiFi.config(kServer, kAp, kMask)) {
        Serial.println("ERROR,S,static_ip");
    }

    WiFi.begin(kSsid, kPassword);

    if (!udp.begin(kDataPort)) {
        Serial.println("ERROR,S,udp_bind");
        while (true) {
            delay(100);
        }
    }

    control.begin();
}

void loop() {
    if (Serial.available() && Serial.readStringUntil('\n') == "?" &&
        WiFi.status() == WL_CONNECTED) {
        Serial.printf("READY,S,%s\n", kModelId);
    }

    WiFiClient link = control.accept();
    if (link && link.remoteIP() == kAp) {
        serve(link);
        link.stop();
    }

    delay(1);
}
