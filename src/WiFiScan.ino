#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <PubSubClient.h> 

// ---- WiFi Settings ----
const char* ssid = "RMUTI-IoT";
const char* password = "IoT@RMUTI";

// ----------------- ThingsBoard Settings -----------------
#define TOKEN_TB "tpnd8vfvtoj20r927uhs" // Access Token ของคุณ
const char* mqtt_server = "iot-so.rmuti.ac.th"; 
const int mqtt_port = 1883;

WiFiClient espClient;
PubSubClient client(espClient);

// ----------------- Data Topics -----------------
const char* TB_TELEMETRY_TOPIC = "v1/devices/me/telemetry";
const char* TB_RPC_TOPIC_REQUEST = "v1/devices/me/rpc/request/+"; // สำหรับรับคำสั่ง RPC

// ----------------- Time Tracking -----------------
unsigned long lastTBMillis = 0;
const unsigned long tbInterval = 60000; // ส่งข้อมูลทุก 60 วินาที

// ---- OLED Display Settings ----
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C
#define I2C_SDA 18
#define I2C_SCL 19
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ---- DS18B20 ----
#define ONE_WIRE_BUS 6
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// ---- Compressor Control ----
#define COMPRESSOR_PIN 25 
bool compressorOn = false; 
unsigned long lastCompressorOffTime = 0;
const unsigned long MIN_OFF_TIME = 300000; // 5 นาที 

// ---- Buttons ----
const uint8_t buttonPins[] = {2, 8, 9, 21, 20};
const char *buttonNames[] = {"Mode", "Fan", "Power", "Temp+", "Temp-"};
const uint8_t BUTTON_COUNT = sizeof(buttonPins) / sizeof(buttonPins[0]);
bool buttonState[BUTTON_COUNT];
bool lastButtonState[BUTTON_COUNT];
unsigned long lastDebounceTime[BUTTON_COUNT];
unsigned long lastButtonHeldReport[BUTTON_COUNT];
const unsigned long debounceDelay = 50;

// ---- System States ----
int mode = 0; // 0: COOL, 1: FAN
int fanLevel = 0; // 0: Auto, 1: LOW, 2: MID, 3: HIGH
bool powerOn = true;
float currentTemp = 0.0;
int targetTemp = 25; // Setpoint

// ---- Temperature Read ----
unsigned long lastTempMillis = 0;
const unsigned long tempInterval = 5000;

// ---- WiFi & Signal Logic ----
long rssi = 0;
int wifiBars = 0;
unsigned long lastWifiCheck = 0;
const unsigned long wifiCheckInterval = 5000;

// ** Forward Declaration **
void sendTelemetry();
void updateDisplay();
void compressorControl();


// ฟังก์ชันคำนวณขีดสัญญาณ
void updateWifiSignal() {
    if (WiFi.status() != WL_CONNECTED) {
        wifiBars = 0;
    } else {
        rssi = WiFi.RSSI();
        if (rssi > -55) wifiBars = 4;
        else if (rssi > -65) wifiBars = 3;
        else if (rssi > -75) wifiBars = 2;
        else if (rssi > -85) wifiBars = 1;
        else wifiBars = 0;
    }
}

// ฟังก์ชันวาดไอคอน WiFi บนจอ
void drawWifiIcon() {
    int x = 110;
    int y = 0;
    if (WiFi.status() != WL_CONNECTED) {
        display.setTextSize(1);
        display.setCursor(x, y);
        display.print("x");
        return;
    }
    for (int i = 0; i < 4; i++) {
        int barHeight = (i + 1) * 2;
        if (i < wifiBars) {
            display.fillRect(x + (i * 3), y + (8 - barHeight), 2, barHeight, SSD1306_WHITE);
        } else {
            display.drawRect(x + (i * 3), y + (8 - barHeight), 2, barHeight, SSD1306_WHITE);
        }
    }
}

// ---------- Function: Show Splash Screen ----------
void showSplashScreen() {
    display.clearDisplay();
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(10, 10);
    display.println(F("AC SYSTEM"));
    display.setTextSize(1);
    display.setCursor(20, 30);
    if (WiFi.status() == WL_CONNECTED) {
        display.println(F("WiFi Connected"));
    } else {
        display.println(F("Connecting WiFi.."));
    }
    display.display();

    if (WiFi.status() != WL_CONNECTED) {
        WiFi.begin(ssid, password);
    }
    int barWidth = 100;
    int barHeight = 10;
    int barX = (SCREEN_WIDTH - barWidth) / 2;
    int barY = 45;
    display.drawRect(barX, barY, barWidth, barHeight, SSD1306_WHITE);

    for (int i = 0; i <= 100; i += 5) {
        int currentWidth = map(i, 0, 100, 0, barWidth - 4);
        display.fillRect(barX + 2, barY + 2, currentWidth, barHeight - 4, SSD1306_WHITE);
        display.display();
        delay(10);
    }
    delay(300);
    display.clearDisplay();
}

// 📤 ฟังก์ชัน: ส่งข้อมูล Telemetry ไปยัง ThingsBoard
void sendTelemetry() {
    if (!client.connected()) {
        return;
    }
    
    // สร้าง Payload แบบ JSON
    String payload = "{";
    payload += "\"currentTemp\":" + String(currentTemp, 1) + ",";
    payload += "\"targetTemp\":" + String(targetTemp) + ",";
    payload += "\"powerOn\":" + String(powerOn ? "true" : "false") + ",";
    payload += "\"compressorOn\":" + String(compressorOn ? "true" : "false") + ",";
    payload += "\"mode\":\"" + (String(mode == 0 ? "COOL" : "FAN")) + "\",";
    
    String fanStr;
    if (fanLevel == 0) fanStr = "Auto";
    else if (fanLevel == 1) fanStr = "LOW";
    else if (fanLevel == 2) fanStr = "MID";
    else if (fanLevel == 3) fanStr = "HIGH";
    
    payload += "\"fanLevel\":\"" + fanStr + "\",";
    
    // เพิ่มข้อมูล WiFi
    payload += "\"rssi\":" + String(rssi) + ",";
    payload += "\"wifiBars\":" + String(wifiBars) + "";

    payload += "}";

    // Publish ข้อมูล
    client.publish(TB_TELEMETRY_TOPIC, payload.c_str());
}

// 🚀 ฟังก์ชัน: จัดการเมื่อได้รับข้อความ MQTT (Callback)
void callback(char* topic, byte* payload, unsigned int length) {
    Serial.print("Message arrived [");
    Serial.print(topic);
    Serial.print("] ");
    
    String payloadStr;
    for (int i = 0; i < length; i++) {
        payloadStr += (char)payload[i];
    }
    Serial.println(payloadStr);

    // ตรวจสอบว่าเป็นคำสั่ง RPC หรือไม่
    if (String(topic).startsWith("v1/devices/me/rpc/request/")) {
        
        String topicStr = String(topic);
        String requestId = topicStr.substring(topicStr.lastIndexOf('/') + 1);
        
        if (payloadStr.indexOf("setTemperature") > 0) {
            // String Parsing สำหรับ {"method":"setTemperature","params":22}
            int tempIndex = payloadStr.lastIndexOf(':') + 1;
            int endTempIndex = payloadStr.lastIndexOf('}');
            String tempStr = payloadStr.substring(tempIndex, endTempIndex);
            int newTargetTemp = tempStr.toInt();
            
            if (newTargetTemp >= 18 && newTargetTemp <= 30) {
                targetTemp = newTargetTemp;
                Serial.print("RPC SUCCESS: Target Temp set to ");
                Serial.println(targetTemp);
                compressorControl();
                updateDisplay();
                sendTelemetry();
            } else {
                Serial.println("RPC ERROR: Invalid target temperature.");
            }
        }
        
        // ตอบกลับ (สำคัญมากสำหรับ RPC Widget บน ThingsBoard)
        String responseTopic = "v1/devices/me/rpc/response/" + requestId;
        client.publish(responseTopic.c_str(), "{\"status\":\"ok\"}");
    }
}


// 🚀 ฟังก์ชัน: เชื่อมต่อ/เชื่อมต่อใหม่กับ ThingsBoard
void reconnectThingsBoard() {
    if (WiFi.status() != WL_CONNECTED) return;
    
    Serial.println("Attempting MQTT connection to ThingsBoard...");

    while (!client.connected() && WiFi.status() == WL_CONNECTED) {
        if (client.connect("ESP32_AC_Control", TOKEN_TB, NULL)) {
            Serial.println("Connected to ThingsBoard!");
            
            client.subscribe(TB_RPC_TOPIC_REQUEST); // Subscribe RPC Request
            Serial.println("Subscribed to RPC topic.");
            
            sendTelemetry(); // ส่งสถานะเริ่มต้น
        } else {
            Serial.print("Failed, rc=");
            Serial.print(client.state());
            Serial.println(" Try again in 5 seconds");
            delay(5000);
        }
    }
}


// 🚀 ฟังก์ชัน: ควบคุมคอมเพรสเซอร์ พร้อมหน่วงเวลา 5 นาที
void compressorControl() {
    unsigned long now = millis();
    bool oldCompressorOn = compressorOn;

    // 1. ตรวจสอบเงื่อนไขพื้นฐาน: ต้องเปิดเครื่อง และอยู่ในโหมด COOL
    if (!powerOn || mode != 0) { // mode 0 คือ COOL
        if (compressorOn) {
            digitalWrite(COMPRESSOR_PIN, LOW); // ปิดคอมเพรสเซอร์
            compressorOn = false;
            lastCompressorOffTime = now; // บันทึกเวลาที่ปิด
            Serial.println("Compressor OFF (System Conditions)");
        }
        if (oldCompressorOn != compressorOn) sendTelemetry(); 
        return;
    }

    // 2. ตรวจสอบอุณหภูมิและสถานะเซ็นเซอร์
    float hysteresis = 0.5; 

    if (currentTemp == 0.0 || currentTemp <= -100.0) {
        return; 
    }

    if (!compressorOn) {
        // ---- เงื่อนไขเปิด (ต้องตรวจสอบเวลาหน่วง) ----
        if (lastCompressorOffTime > 0 && (now - lastCompressorOffTime < MIN_OFF_TIME)) {
            return; 
        }

        // เงื่อนไขอุณหภูมิเปิด: อุณหภูมิปัจจุบันสูงกว่าอุณหภูมิเป้าหมาย + Hysteresis
        if (currentTemp > targetTemp + hysteresis) {
            digitalWrite(COMPRESSOR_PIN, HIGH); // เปิดคอมเพรสเซอร์
            compressorOn = true;
            Serial.println("Compressor ON");
            lastCompressorOffTime = 0; 
        }
    } else {
        // เงื่อนไขปิด: อุณหภูมิปัจจุบันถึงหรือต่ำกว่าอุณหภูมิเป้าหมาย
        if (currentTemp <= targetTemp) {
            digitalWrite(COMPRESSOR_PIN, LOW); // ปิดคอมเพรสเซอร์
            compressorOn = false;
            lastCompressorOffTime = now; 
            Serial.println("Compressor OFF (Target Reached)");
        }
    }
    
    // ส่งข้อมูลไป ThingsBoard เมื่อสถานะคอมเพรสเซอร์เปลี่ยน
    if (oldCompressorOn != compressorOn) {
        sendTelemetry(); 
    }
}

// ---------- Function: Update OLED ----------
void updateDisplay() {
    if (!powerOn) {
        display.clearDisplay();
        display.display();
        return;
    }

    display.clearDisplay();

    // -- Header Line (PWR & COMP) --
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.print("PWR:ON");
    
    // แสดงสถานะคอมเพรสเซอร์
    display.setCursor(45, 0);
    display.print("COMP:");
    if (compressorOn) {
        display.print("ON");
    } else {
        display.print("OFF");
    }

    // -- Draw WiFi Icon --
    drawWifiIcon();

    // -- Current Temperature (ซ้าย) --
    display.setCursor(0, 16);
    display.setTextSize(2);
    if (currentTemp <= -100.0 || currentTemp == 0.0) {
        display.print("--");
    } else {
        display.print(currentTemp, 0);
    }
    display.setTextSize(1);
    display.print("o"); 

    // -- Target Temperature (ขวา) --
    display.setCursor(65, 16);
    display.setTextSize(1);
    display.print("SET:");
    display.setCursor(90, 16);
    display.setTextSize(2); 
    display.print(targetTemp);

    // -- Mode & Fan Info --
    display.setTextSize(1);
    int modeCursorX = 40;
    int modeCursorY = 45; 
    
    display.setCursor(0, modeCursorY);
    display.print("Mode:");
    if (mode == 0) {
        display.setCursor(modeCursorX, modeCursorY);
        display.print("COOL");
    } else if (mode == 1) {
        display.setCursor(modeCursorX, modeCursorY);
        display.print("FAN");
    }
    
    display.setCursor(0, 56);
    display.print("Fan :");
    display.setCursor(40, 56);
    if (fanLevel == 0) display.print("Auto");
    else if (fanLevel == 1) display.print("LOW");
    else if (fanLevel == 2) display.print("MID");
    else if (fanLevel == 3) display.print("HIGH");

    display.display();
}

// ---------- Stable digital read ----------
int stableRead(uint8_t pin) {
    int r1 = digitalRead(pin);
    delayMicroseconds(300);
    int r2 = digitalRead(pin);
    return (r1 == r2) ? r1 : r1;
}

// ---------- Handle Button ----------
void handleButtonPress(uint8_t idx) {
    if (!powerOn && idx != 2) {
        return;
    }
    
    bool stateChanged = true;

    switch (idx) {
        case 0: // Mode
            mode = (mode + 1) % 2;
            compressorControl(); 
            break;
        case 1: // Fan
            fanLevel = (fanLevel + 1) % 4;
            break;
        case 2: // Power
            if (powerOn) {
                powerOn = false;
                compressorControl(); 
                Serial.println("Shutting Down...");
                display.clearDisplay();
                display.display();
            } else {
                powerOn = true;
                Serial.println("Starting Up...");
                showSplashScreen();
                updateWifiSignal();
                compressorControl(); 
            }
            break;
        case 3: // Temp+
            targetTemp++;
            if(targetTemp > 30) targetTemp = 30;
            Serial.print("Set Temp: ");
            Serial.println(targetTemp);
            Serial1.println("Temp+");
            compressorControl(); 
            break;
        case 4: // Temp-
            targetTemp--;
            if(targetTemp < 18) targetTemp = 18;
            Serial.print("Set Temp: ");
            Serial.println(targetTemp);
            Serial1.println("Temp-");
            compressorControl(); 
            break;
        default:
            stateChanged = false;
    }
    
    if (stateChanged && powerOn) {
        updateDisplay();
        sendTelemetry(); 
    } else if (idx == 2) {
        sendTelemetry();
    }
}

// ---------- Scan Buttons ----------
void scanButtons() {
    unsigned long now = millis();
    const unsigned long heldReportInterval = 500;
    
    for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
        int reading = stableRead(buttonPins[i]);
        if (reading != lastButtonState[i]) lastDebounceTime[i] = now;

        if ((now - lastDebounceTime[i]) > debounceDelay) {
            if (reading != buttonState[i]) {
                buttonState[i] = reading;
                if (buttonState[i] == LOW) {
                    Serial.print(buttonNames[i]);
                    Serial.println(" Pressed");
                    handleButtonPress(i);
                    lastButtonHeldReport[i] = now;
                }
            } else {
                if (buttonState[i] == LOW && (now - lastButtonHeldReport[i] >= heldReportInterval)) {
                    if (powerOn || i == 2) {
                        if (i == 3 || i == 4) handleButtonPress(i); 
                        lastButtonHeldReport[i] = now;
                    }
                }
            }
        }
        lastButtonState[i] = reading;
    }
}

// ---------- Setup ----------
void setup() {
    Serial.begin(115200);
    Serial1.begin(9600, SERIAL_8N1, 3, 10);
    Wire.begin(I2C_SDA, I2C_SCL);

    for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
        pinMode(buttonPins[i], INPUT_PULLUP);
    }
    
    pinMode(COMPRESSOR_PIN, OUTPUT);
    digitalWrite(COMPRESSOR_PIN, LOW); 

    if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
        Serial.println(F("SSD1306 allocation failed"));
        for(;;);
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    showSplashScreen();
    
    // Setup ThingsBoard Client
    client.setServer(mqtt_server, mqtt_port);
    client.setCallback(callback); // ตั้งค่า Callback สำหรับ RPC

    powerOn = true;
    sensors.begin();
    
    updateWifiSignal();
    
    if (WiFi.status() == WL_CONNECTED) {
      reconnectThingsBoard();
    }

    updateDisplay();
    compressorControl(); 
    
    Serial.println("System Ready");
}

// ---------- Loop ----------
void loop() {
    unsigned long now = millis();
    
    // 1. ตรวจสอบการเชื่อมต่อ WiFi และ MQTT
    if (WiFi.status() == WL_CONNECTED) {
        if (!client.connected()) {
            reconnectThingsBoard();
        }
        client.loop(); // ต้องเรียกใช้เสมอเพื่อรับคำสั่ง RPC
    }

    // 2. อ่านอุณหภูมิ (DS18B20)
    if (powerOn && (now - lastTempMillis >= tempInterval)) {
        lastTempMillis = now;
        sensors.requestTemperatures();
        float tempReading = sensors.getTempCByIndex(0);
        
        if (tempReading > -100.0 && tempReading != 0.0) {
            currentTemp = tempReading;
            compressorControl(); 
        }
        updateDisplay();
        
        // ส่ง Telemetry ทันทีที่มีการอ่านอุณหภูมิใหม่
        sendTelemetry();
    }
    
    // 3. ส่งข้อมูลไปยัง ThingsBoard ตามช่วงเวลา (60 วินาที)
    if (powerOn && client.connected() && (now - lastTBMillis >= tbInterval)) {
        lastTBMillis = now;
        sendTelemetry();
    }

    // 4. เช็ค WiFi และส่งข้อมูล (5 วินาที)
    if (powerOn && (now - lastWifiCheck >= wifiCheckInterval)) {
        lastWifiCheck = now;
        updateWifiSignal();
        updateDisplay();
        
        if (client.connected()) {
            sendTelemetry(); // ส่งข้อมูล RSSI และสถานะอื่น ๆ ที่อัปเดตแล้ว
        }
    }

    scanButtons();
    delay(30);
}