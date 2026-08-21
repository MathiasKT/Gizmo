#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <HTTPClient.h>
#include <JPEGDEC.h>
#include <ArduinoWebsockets.h>
#include <esp_display_panel.hpp>
//WIKI
//https://docs.waveshare.com/ESP32-S3-Touch-LCD-4.3B/Development-Environment-Setup-Arduino
using namespace esp_panel::drivers;
using namespace esp_panel::board;
using namespace websockets;

/* 

// SERVER endpoints HOME
const char *mjpegURL = "http://192.168.1.75:3000/stream";
const char *wsURL = "ws://192.168.2.75:3000/touch"; 
*/

//MAKE SURE NETWORK WHERE SERVER IS, IS PRIVATE (E.G SERVER IS DISCOVERABLE ON NETWORK SO CLIENT CAN COMMUNICATE WITH IT)
//e.g for windows make network profile type private

// WIFI EP Demo
const char *ssid = "DemoNet"; 
const char *password = "Demo3276";


// SERVER endpoints HOME
const char *mjpegURL = "http://192.168.2.109:3000/stream";
const char *wsURL = "ws://192.168.2.109:3000/touch"; 


// BOARD, LCD & TOUCH
Board *board = nullptr;
LCD *lcd = nullptr;
Touch *touch = nullptr;
WebsocketsClient ws;

// JPEG Decoder
JPEGDEC jpeg;
uint8_t* jpgBuffer;
size_t jpgLength;

// TOUCH VARIABLES
bool touchActive = false;
uint16_t lastTouchX = 0;
uint16_t lastTouchY = 0;
unsigned long lastTouchSend = 0;
const unsigned long TOUCH_SEND_INTERVAL = 50;   // ms
TaskHandle_t touchTaskHandle = NULL;

//core #1 used for handling touch 
void touchTask(void *pvParameters) {
    for (;;) {
        handleTouch();
        vTaskDelay(pdMS_TO_TICKS(5)); // cheap poll, ~200Hz ceiling
    }
}


// JPEG DRAW (draws jpeg image to screen) 
int jpegDraw(JPEGDRAW *pDraw)
{
    int width = pDraw->iWidth;
    int height = pDraw->iHeight;
    
    // Push the image block to the screen
    lcd->drawBitmap(pDraw->x, pDraw->y, width, height, (const uint8_t *)pDraw->pPixels);
    
    return 1;
}

// TOUCH HANDLER
//sends json of touch type and coords to server /touch endpoint
void handleTouch() {
    //  WebSocket reconnect logic (so it works during video playback)
    if (!ws.available()) {
        static unsigned long lastWsRetry = 0;
        if(millis() - lastWsRetry > 5000) {
            Serial.println("WebSocket disconnected. Reconnecting...");
            //ws.connect("192.168.1.75", 3000, "/touch"); 
            ws.connect(wsURL); 
            lastWsRetry = millis();
        }
        return; // Don't try to read touches if not connected
    }
    
    ws.poll(); // Keep connection alive

    if (!touch) return;                   

    TouchPoint point;
    int numPoints = touch->readPoints(&point, 1, -1); //reads current touch position of board
    bool touched = (numPoints > 0);

    if (touched) {
        if (!touchActive) {//single touch event
            Serial.printf("[TOUCH] Pressed at X:%d Y:%d\n", point.x, point.y);
            String msg = "{\"type\":\"touchstart\",\"x\":" + String(point.x) +
                         ",\"y\":" + String(point.y) + "}";
            ws.send(msg);
            touchActive = true;
            lastTouchX = point.x;
            lastTouchY = point.y;
            lastTouchSend = millis();
        } 
        else if (millis() - lastTouchSend >= TOUCH_SEND_INTERVAL) { //drag event
            if (abs(point.x - lastTouchX) > 3 || abs(point.y - lastTouchY) > 3) {
                Serial.printf("[TOUCH] Dragged to X:%d Y:%d\n", point.x, point.y);
                String msg = "{\"type\":\"touchmove\",\"x\":" + String(point.x) +
                             ",\"y\":" + String(point.y) + "}";
                ws.send(msg);
                lastTouchX = point.x;
                lastTouchY = point.y;
                lastTouchSend = millis();
            }
        }
    } 
    else if (touchActive) { //touch end event
        Serial.println("[TOUCH] Released");
        String msg = "{\"type\":\"touchend\"}";
        ws.send(msg);
        touchActive = false;
    }
}

// MJPEG PARSER
void readMJPEG()
{
    WiFiClient client;
    HTTPClient http;

    Serial.println("[VIDEO] Connecting to MJPEG stream...");
    http.begin(client, mjpegURL);
    http.setTimeout(10000);
    http.addHeader("Accept", "multipart/x-mixed-replace");

    int response = http.GET();
    if(response != 200){
        Serial.printf("MJPEG failed: %s (%d)\n",http.errorToString(response).c_str(),response);
        http.end();
        delay(5000);
        return;
    }

    WiFiClient *stream = http.getStreamPtr();
    if(!stream) {
        Serial.println("Failed to get stream pointer");
        http.end();
        return;
    }
    

    uint8_t last_byte = 0x00;
    uint8_t chunk[512];
    unsigned long lastByteTime = millis();
    bool inFrame = false;

    while (stream->connected()) {
        int avail = stream->available();
        if (avail > 0) {
            int toRead = min(avail, (int)sizeof(chunk));
            int n = stream->readBytes(chunk, toRead);
            lastByteTime = millis();

            for (int i = 0; i < n; i++) {
                uint8_t c = chunk[i];

                if (!inFrame) {
                    
                    if (last_byte == 0xFF && c == 0xD8) {
                        jpgLength = 0;
                        jpgBuffer[jpgLength++] = 0xFF;
                        jpgBuffer[jpgLength++] = 0xD8;
                        inFrame = true;
                    }
                    last_byte = c;
                    continue;
                }

                if (jpgLength < 200000) jpgBuffer[jpgLength++] = c;

                if (c == 0xD9 && last_byte == 0xFF) {
                    unsigned long decodeStart = millis();
                    if (jpeg.openRAM(jpgBuffer, jpgLength, jpegDraw)) {
                        jpeg.decode(0, 0, 0);
                        jpeg.close();
                        Serial.printf("[VIDEO] Frame Rendered! Size: %d bytes | Time: %lu ms\n",
                                    jpgLength, millis() - decodeStart);
                    } else {
                        Serial.println("[VIDEO] ERROR: Failed to decode frame!");
                    }
                    inFrame = false; // go find the next frame
                }
                last_byte = c;
            }
        } else {
            
            if (millis() - lastByteTime > 30000) break; // stalled connection
            delay(1);
        }
    }
    Serial.println("[VIDEO] Server closed the connection.");
    http.end();

}

void onWebSocketMessage(WebsocketsMessage message) {
    Serial.print("WS Message: ");
    Serial.println(message.data());
}



void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n\nStarting MJPEG Stream Display...");

    // 1. Create and Initialize the Board
    Serial.println("Initializing Board...");
    board = new Board();
    board->init();

    // 2. Configure Bounce Buffer 
    lcd = board->getLCD();
    auto lcd_bus = lcd->getBus();
    if (lcd_bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) {
        static_cast<BusRGB *>(lcd_bus)->configRGB_BounceBufferSize(lcd->getFrameWidth() * 20);
    }

    // 3. Begin the board (this handles the LCD, and Touch all at once)
    assert(board->begin());
    touch = board->getTouch();

    // 4. Fill screen black
    uint8_t *blackBuffer = (uint8_t*)malloc(lcd->getFrameWidth() * 2); 
    if(blackBuffer) {
        memset(blackBuffer, 0, lcd->getFrameWidth() * 2);
        for(int y = 0; y < lcd->getFrameHeight(); y++) {
            lcd->drawBitmap(0, y, lcd->getFrameWidth(), 1, blackBuffer);
        }
        free(blackBuffer);
    }
    
    Serial.println("Hardware initialized successfully");

    // 5. Connect to WiFi
    Serial.print("Connecting to WiFi");
    WiFi.begin(ssid, password);
    esp_wifi_set_ps(WIFI_PS_NONE);

    int tryCount = 0;
    while (WiFi.status() != WL_CONNECTED && tryCount < 40) {
        delay(500);
        Serial.print(".");
        tryCount++;
    }

    if(WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi connected!");
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());
        
        // Connect the WebSocket once during setup
        Serial.println("Connecting to WebSocket...");
        
        ws.onMessage(onWebSocketMessage);
        ws.connect(wsURL);
    } else {
        Serial.println("\nWiFi connection failed!");
    }

    // 6. Allocate JPEG buffer in PSRAM
    Serial.println("Allocating JPEG buffer...");
    jpgBuffer = (uint8_t*)heap_caps_malloc(200000, MALLOC_CAP_SPIRAM);
    if (jpgBuffer == NULL) {
        Serial.println("CRITICAL: PSRAM allocation failed!");
        while (true) { delay(1000); }
    }
    Serial.println("Setup complete, starting stream...");

    //separate out diff functions to diff cores
    xTaskCreatePinnedToCore(
    touchTask,
    "TouchTask",
    4096,      // stack, plenty for I2C read + JSON string + WS send
    NULL,
    2,         // priority — same as loopTask, core 0 is idle enough
    &touchTaskHandle,
    1          // core 0 — opposite of Arduino's default loop core (1)
    );
}

void loop() {
    // Check WebSocket state. Reconnect if it drops.

    // Check Wi-Fi state and start reading video
    if(WiFi.status() == WL_CONNECTED) {
        readMJPEG();
    } else {
        Serial.println("WiFi disconnected, retrying...");
        WiFi.reconnect();
        delay(5000);
    }
    delay(10);
}