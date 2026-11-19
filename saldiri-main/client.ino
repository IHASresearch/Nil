/*
 * Proje: ESP8266 ile Seçici Paket Düşürme Saldırısı Simülatörü ve Veri Üretici
 *
 * Bu kod, ESP8266 mikrodenetleyicisini bir Wi-Fi Erişim Noktası olarak yapılandırarak 
 * kablosuz ağlarda seçici paket düşürme saldırılarını simüle eden bir deney yazılımıdır.
 * Sistem, seri porttan aldığı komutla eğitim (1000 deneme) veya test (200 deneme) döngülerini başlatır.
 * Her denemede 18 ms aralıklarla 10 adet 64 baytlık UDP veri paketi gönderir; rastgele seçilen "saldırı" 
 * denemelerinde ara paketlerden (slot 1-8) birini bilerek düşürerek anormallik oluşturur.
 * TCP kontrol portu (4210) üzerinden alıcı sunucu ile senkronizasyon sağlar ve test aşaması sonunda 
 * gerçek saldırı etiketlerini sunucuya ileterek tespit algoritmalarının başarımını ölçer.
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

WiFiUDP udp;

bool awaitToken(WiFiClient& link, uint8_t expected);
void executeRun(uint32_t run, uint32_t count);

bool awaitToken(WiFiClient& link, uint8_t expected) {
    const uint32_t start = millis();
    while (link.available() == 0 && link.connected() && millis() - start < 3000) {
        delay(1);
    }
    return link.read() == expected;
}

void executeRun(uint32_t run, uint32_t count) {
    WiFiClient link;
    if (!link.connect(kServer, kControlPort)) {
        Serial.println("ERROR,C,control_connect");
        return;
    }

    link.setNoDelay(true);
    const RunHeader header{kMagic, run, count};
    if (link.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
        Serial.println("ERROR,C,run_header");
        return;
    }

    randomSeed(run);
    bool attack = false;
    uint8_t testLabels[kTestTrials];

    for (uint16_t trial = 0; trial < count; ++trial) {
        if (!awaitToken(link, kGo)) {
            Serial.println("ERROR,C,window_start");
            return;
        }

        // Balance each pair, but randomize which trial is the attack.
        attack = (trial % 2 == 0) ? random(2) == 1 : !attack;
        if (count == kTestTrials) {
            testLabels[trial] = attack;
        }

        const int dropped = attack ? random(1, 9) : -1;
        const uint32_t start = millis();

        for (uint16_t slot = 0; slot < kSlots; ++slot) {
            while (millis() - start < slot * kIntervalMs) {
                delay(1);
            }
            if (slot == dropped) {
                continue;
            }

            const Datagram packet{run, trial, slot, {}};
            if (!udp.beginPacket(kServer, kDataPort) ||
                udp.write(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet)) != sizeof(packet) ||
                !udp.endPacket()) {
                Serial.println("ERROR,C,local_udp_send");
                return;
            }
        }

        Serial.printf("C,%lu,%d,%d,%d\n", static_cast<unsigned long>(run),
                      trial, attack, dropped);
    }

    // The server requests labels after all predictions are complete.
    if (count == kTestTrials &&
        (!awaitToken(link, kSendLabels) ||
         link.write(testLabels, sizeof(testLabels)) != sizeof(testLabels))) {
        Serial.println("ERROR,C,test_labels");
        return;
    }

    if (!awaitToken(link, kDone)) {
        Serial.println("ERROR,C,run_finish");
        return;
    }

    Serial.printf("DONE,C,%lu\n", static_cast<unsigned long>(run));
}

void setup() {
    Serial.begin(115200);
    Serial.setTimeout(100);
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP);

    if (!WiFi.softAPConfig(kAp, kAp, kMask) ||
        !WiFi.softAP(kSsid, kPassword, kChannel, false, 1) ||
        !udp.begin(kDataPort)) {
        Serial.println("ERROR,C,setup");
        while (true) {
            delay(100);
        }
    }
}

void loop() {
    if (!Serial.available()) {
        delay(2);
        return;
    }

    const String command = Serial.readStringUntil('\n');
    if (command == "?") {
        Serial.println("READY,C");
        return;
    }

    unsigned long run = 0;
    unsigned long count = 0;
    char extra = '\0';

    if (sscanf(command.c_str(), "R %lu %lu %c", &run, &count, &extra) == 2 &&
        run != 0 && (count == kTrainTrials || count == kTestTrials)) {
        executeRun(run, count);
    }
}
