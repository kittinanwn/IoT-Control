#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>

// ---- WiFi Settings ----
const char* ssid = "RMUTI-IoT";
const char* password = "IoT@RMUTI";

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
#define COMPRESSOR_PIN 25 // <<< กำหนดขาสำหรับควบคุมรีเลย์คอมเพรสเซอร์
bool compressorOn = false; // <<< ตัวแปรสถานะคอมเพรสเซอร์

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
bool fanOn = false;
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

// ฟังก์ชันคำนวณขีดสัญญาณ
void updateWifiSignal() {
    if (WiFi.status() != WL_CONNECTED) {
        wifiBars = 0;
        return;
    }
    rssi = WiFi.RSSI();
    if (rssi > -55) wifiBars = 4;
    else if (rssi > -65) wifiBars = 3;
    else if (rssi > -75) wifiBars = 2;
    else if (rssi > -85) wifiBars = 1;
    else wifiBars = 0;
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

// ---------- Function: Compressor Control (ใหม่) ----------
// ---- Compressor Time Tracking ----
unsigned long lastCompressorOffTime = 0;
const unsigned long MIN_OFF_TIME = 300000; // 5 นาที = 300,000 มิลลิวินาที

// 🚀 ฟังก์ชัน: ควบคุมคอมเพรสเซอร์ พร้อมหน่วงเวลา 5 นาที
void compressorControl() {
    unsigned long now = millis();

    // 1. ตรวจสอบเงื่อนไขพื้นฐาน: ต้องเปิดเครื่อง และอยู่ในโหมด COOL
    if (!powerOn || mode != 0) { // mode 0 คือ COOL
        if (compressorOn) {
            digitalWrite(COMPRESSOR_PIN, LOW); // ปิดคอมเพรสเซอร์
            compressorOn = false;
            lastCompressorOffTime = now; // บันทึกเวลาที่ปิด
            Serial.println("Compressor OFF (System Conditions)");
        }
        return;
    }

    // 2. ตรวจสอบอุณหภูมิและสถานะเซ็นเซอร์
    float hysteresis = 0.5; // ค่า Hysteresis 0.5 องศาเซลเซียส

    if (currentTemp == 0.0 || currentTemp <= -100.0) {
        return; // งดการควบคุมถ้าวัดอุณหภูมิไม่ได้
    }

    if (!compressorOn) {
        // ---- เงื่อนไขเปิด (ต้องตรวจสอบเวลาหน่วง) ----

        // 2.1 ตรวจสอบเวลาหน่วง: ต้องรอให้พ้นช่วง MIN_OFF_TIME ก่อน
        if (lastCompressorOffTime > 0 && (now - lastCompressorOffTime < MIN_OFF_TIME)) {
            Serial.print("Compressor WAIT: ");
            Serial.print((MIN_OFF_TIME - (now - lastCompressorOffTime)) / 1000);
            Serial.println("s remaining.");
            return; // หน่วงเวลาอยู่ ไม่อนุญาตให้เปิด
        }

        // 2.2 เงื่อนไขอุณหภูมิเปิด: อุณหภูมิปัจจุบันสูงกว่าอุณหภูมิเป้าหมาย + Hysteresis
        if (currentTemp > targetTemp + hysteresis) {
            digitalWrite(COMPRESSOR_PIN, HIGH); // เปิดคอมเพรสเซอร์ (สมมติ Active-HIGH)
            compressorOn = true;
            Serial.println("Compressor ON");
            lastCompressorOffTime = 0; // รีเซ็ตเวลาปิด เพราะเปิดแล้ว
        }
    } else {
        // ---- เงื่อนไขปิด ----

        // เงื่อนไขปิด: อุณหภูมิปัจจุบันถึงหรือต่ำกว่าอุณหภูมิเป้าหมาย
        if (currentTemp <= targetTemp) {
            digitalWrite(COMPRESSOR_PIN, LOW); // ปิดคอมเพรสเซอร์
            compressorOn = false;
            lastCompressorOffTime = now; // บันทึกเวลาที่ปิด
            Serial.println("Compressor OFF (Target Reached)");
        }
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

    switch (idx) {
        case 0: // Mode
            mode = (mode + 1) % 2;
            compressorControl(); // อัปเดตสถานะคอมเพรสเซอร์ทันทีเมื่อเปลี่ยนโหมด
            updateDisplay();
            break;
        case 1: // Fan
            fanLevel = (fanLevel + 1) % 4;
            compressorControl(); // อัปเดตสถานะคอมเพรสเซอร์ทันทีเมื่อเปลี่ยนระดับพัดลม
            updateDisplay();
            break;
        case 2: // Power
            if (powerOn) {
                powerOn = false;
                compressorControl(); // ปิดคอมเพรสเซอร์ทันทีที่ปิดเครื่อง
                Serial.println("Shutting Down...");
                display.clearDisplay();
                display.display();
            } else {
                powerOn = true;
                Serial.println("Starting Up...");
                showSplashScreen();
                updateWifiSignal();
                compressorControl(); // ตรวจสอบสถานะคอมเพรสเซอร์เมื่อเปิดเครื่อง
                updateDisplay();
            }
            break;
        case 3: // Temp+
            targetTemp++;
            if(targetTemp > 30) targetTemp = 30;
            Serial.print("Set Temp: ");
            Serial.println(targetTemp);
            Serial1.println("Temp+");
            compressorControl(); // อัปเดตสถานะคอมเพรสเซอร์ทันทีเมื่อเปลี่ยนอุณหภูมิเป้าหมาย
            updateDisplay();
            break;
        case 4: // Temp-
            targetTemp--;
            if(targetTemp < 18) targetTemp = 18;
            Serial.print("Set Temp: ");
            Serial.println(targetTemp);
            Serial1.println("Temp-");
            compressorControl(); // อัปเดตสถานะคอมเพรสเซอร์ทันทีเมื่อเปลี่ยนอุณหภูมิเป้าหมาย
            updateDisplay(); 
            break;
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
                        if (i == 3 || i == 4) handleButtonPress(i); // Auto increase/decrease
                        Serial.print(buttonNames[i]);
                        Serial.println(" HELD");
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
    Serial.begin(115200, SERIAL_8N1, -1, -1);
    Serial1.begin(9600, SERIAL_8N1, 3, 10);
    Wire.begin(I2C_SDA, I2C_SCL);

    // กำหนดขาปุ่มเป็น INPUT_PULLUP (หากปุ่มต่อกับ GND)
    for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
        pinMode(buttonPins[i], INPUT_PULLUP);
    }
    
    // กำหนดขาคอมเพรสเซอร์เป็น OUTPUT
    pinMode(COMPRESSOR_PIN, OUTPUT);
    digitalWrite(COMPRESSOR_PIN, LOW); // ปิดคอมเพรสเซอร์เริ่มต้น

    if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
        Serial.println(F("SSD1306 allocation failed"));
        for(;;);
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    showSplashScreen();
    
    powerOn = true;
    sensors.begin();
    
    updateWifiSignal();
    updateDisplay();
    compressorControl(); // ตรวจสอบสถานะคอมเพรสเซอร์เริ่มต้น
    
    Serial.println("System Ready");
}

// ---------- Loop ----------
void loop() {
    unsigned long now = millis();

    // 1. อ่านอุณหภูมิ (DS18B20)
    if (powerOn && (now - lastTempMillis >= tempInterval)) {
        lastTempMillis = now;
        sensors.requestTemperatures();
        float tempReading = sensors.getTempCByIndex(0);
        
        if (tempReading > -100.0 && tempReading != 0.0) {
            currentTemp = tempReading;
            compressorControl(); // <<< เรียกใช้การควบคุมคอมเพรสเซอร์เมื่อได้ค่าอุณหภูมิใหม่
        }
        updateDisplay();
    }

    // 2. เช็ค WiFi
    if (powerOn && (now - lastWifiCheck >= wifiCheckInterval)) {
        lastWifiCheck = now;
        updateWifiSignal();
        updateDisplay();
    }

    scanButtons();
    delay(30);
}