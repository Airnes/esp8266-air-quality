#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>  // For HTTPS connections
#include <SoftwareSerial.h>
#include <DHT.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <time.h>  // For NTP time

// Define DHT22 sensor
#define DHTPIN D7  // Pin connected to DHT22 data pin (GPIO13)
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// Define SDS011 sensor (Serial connection)
SoftwareSerial sdsSerial(D1, D2); // RX, TX for SDS011 (D1 = GPIO5, D2 = GPIO4)

// Web server running on port 80
ESP8266WebServer server(80);

// AQICN API credentials
const String AQICN_TOKEN = "";  // Replace with your actual AQICN token
const String STATION_ID = "";  // Replace with your actual AQICN station ID
const String STATION_NAME = "";  // Replace with your actual AQICN station name

// Wi-Fi credentials
const char SSID[] = "";
const char PASS[] = "";

// Variables to hold sensor data
float temp = 0;
float hum = 0;
float pm25 = 0;
float pm10 = 0;
String errorMessages = "";  // To collect error messages during sensor reads

// Timing variables
unsigned long lastSensorReadTime = 0;  // Stores the last sensor read time
unsigned long lastRebootTime = 0;
const unsigned long rebootInterval = 12 * 3600000;  // Reboot twice a day (12 hours in milliseconds)
const unsigned long sensorReadInterval = 300000;    // 5 minutes in milliseconds

// Retry configuration
const int maxDataRetries = 3;

WiFiClientSecure client;  // Use secure WiFi client

void setup() {
  // Initialize Serial and Sensors
  Serial.begin(115200);
  sdsSerial.begin(9600);
  dht.begin();

  // Connect to Wi-Fi
  WiFi.begin(SSID, PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }
  Serial.println("\nConnected to WiFi");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // Initialize web server routes
  server.on("/", handleRoot);  
  server.on("/trigger", handleTrigger);  
  server.begin();
  Serial.println("Web server started");

  // Set client insecure for now (you can add certificate if needed)
  client.setInsecure();
  
  // Read sensor data once on startup
  wakeUpSDS011();  // Wake up the SDS011 before taking readings
  delay(10000);  // Wait for SDS011 to stabilize
  if (!updateSensorData()) {
    Serial.println("Sensor reading failed. Rebooting...");
    ESP.restart();  // Reboot if sensor reading fails
  }
  sendDataToAQICN(temp, hum, pm25, pm10);
  sleepSDS011();  // Put SDS011 to sleep after readings

  // Go into light sleep mode to save power
  goToLightSleep();
}

void loop() {
  // Handle web requests
  server.handleClient();

  // Check if it's time to read the sensors and send data
  if (millis() - lastSensorReadTime >= sensorReadInterval) {
    Serial.println("Time to read sensors and send data");
    lastSensorReadTime = millis();  // Update the last read time

    // Wake up SDS011 before reading sensor data
    wakeUpSDS011();
    delay(10000);  // Wait for SDS011 to stabilize
    Serial.println("SDS011 Waked Up");

    // Read sensor data and check for failures
    if (!updateSensorData()) {
      Serial.println("Sensor reading failed. Rebooting...");
      ESP.restart();  // Reboot if sensor reading fails
    }

    Serial.println("Sending data to AQICN");
    // Only send data if both sensors succeeded
    sendDataToAQICN(temp, hum, pm25, pm10);
    Serial.println("Data to AQICN sent");
    // Put SDS011 to sleep after reading sensor data
    sleepSDS011();
    Serial.println("SDS011 is sleeping ZZzzzzzZZZZzzzz...");

    // Enter light-sleep mode to conserve power
    goToLightSleep();
  }

  // Check if it's time for a scheduled reboot (every 12 hours)
  if (millis() - lastRebootTime >= rebootInterval) {
    Serial.println("Scheduled reboot...");
    ESP.restart();  // Reboot the device twice a day
  }
}

// SDS011 wake-up function
void wakeUpSDS011() {
  static constexpr uint8_t WAKEUPCMD[19] = { 
    0xAA, 0xB4, 0x06, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x06, 0xAB 
  };
  sdsSerial.write(WAKEUPCMD, sizeof(WAKEUPCMD));
  delay(1000);  // Ensure wake-up command is processed
}

// SDS011 sleep function
void sleepSDS011() {
  static constexpr uint8_t SLEEPCMD[19] = { 
    0xAA, 0xB4, 0x06, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x05, 0xAB 
  };
  sdsSerial.write(SLEEPCMD, sizeof(SLEEPCMD));
  delay(1000);  // Ensure sleep command is processed
}

// Function to put ESP8266 into light sleep mode
void goToLightSleep() {
  Serial.println("Entering light sleep mode...");
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);        // Turn off WiFi
  WiFi.forceSleepBegin();     // Enter light sleep mode
  delay(sensorReadInterval);  // Delay for sensorReadInterval duration (5 minutes)
  WiFi.forceSleepWake();      // Wake up from light sleep mode
  WiFi.mode(WIFI_STA);        // Re-enable WiFi
  WiFi.begin(SSID, PASS);     // Reconnect to WiFi
}

// Function to update sensor data and handle errors
bool updateSensorData() {
  errorMessages = "";  // Clear previous errors
  Serial.println("Waking up the SDS011 sensor...");

  wakeUpSDS011();  // Ensure the sensor is awake before reading
  delay(10000);  // Wait 10 seconds for the SDS011 to stabilize after waking up

  Serial.println("Reading sensor data...");

  // Collect data from DHT22
  temp = dht.readTemperature();
  hum = dht.readHumidity();

  if (isnan(temp) || isnan(hum)) {
    String dhtError = "Failed to read from DHT22 sensor!";
    Serial.println(dhtError);
    errorMessages += dhtError + "<br>";
    dht.begin();  // Reinitialize DHT
    return false;  // Return false to indicate failure
  } else {
    Serial.print("Temperature: ");
    Serial.println(temp);
    Serial.print("Humidity: ");
    Serial.println(hum);
  }

  // Collect data from SDS011
  if (sdsSerial.available() > 0) {
    byte data[10];
    sdsSerial.readBytes(data, 10);

    // Validate SDS011 packet
    if (data[0] == 0xAA && data[9] == 0xAB) {
      pm25 = ((data[3] * 256) + data[2]) / 10.0;
      pm10 = ((data[5] * 256) + data[4]) / 10.0;
      Serial.print("PM2.5: ");
      Serial.println(pm25);
      Serial.print("PM10: ");
      Serial.println(pm10);
      return true;  // Sensor reading successful
    } else {
      String sdsInvalidPacket = "Invalid SDS011 data packet received.";
      Serial.println(sdsInvalidPacket);
      errorMessages += sdsInvalidPacket + "<br>";
      return false;  // Return false to indicate failure
    }
  } else {
    String sdsNoData = "SDS011 sensor not responding or no data available.";
    Serial.println(sdsNoData);
    errorMessages += sdsNoData + "<br>";
    return false;  // Return false to indicate failure
  }
}

// Function to send data to AQICN, if both sensors succeeded
void sendDataToAQICN(float temperature, float humidity, float pm25, float pm10) {
  int retries = 0;
  bool success = false;
  
  // Get the current time in ISO 8601 format
  String currentTime = getISO8601Time();

  // Prepare JSON payload according to AQICN API format
  String jsonPayload = "{";
  jsonPayload += "\"token\":\"" + AQICN_TOKEN + "\",";
  jsonPayload += "\"station\":{";
  jsonPayload += "\"id\":\"" + STATION_ID + "\",";
  jsonPayload += "\"name\":\"" + STATION_NAME + "\",";
  jsonPayload += "\"latitude\":49.981126,";
  jsonPayload += "\"longitude\":20.052055},";
  jsonPayload += "\"readings\":[";
  jsonPayload += "{\"time\":\"" + currentTime + "\",\"specie\":\"pm2.5\",\"value\":" + String(pm25, 1) + ",\"unit\":\"µg/m³\"},";
  jsonPayload += "{\"time\":\"" + currentTime + "\",\"specie\":\"pm10\",\"value\":" + String(pm10, 1) + ",\"unit\":\"µg/m³\"},";
  jsonPayload += "{\"time\":\"" + currentTime + "\",\"specie\":\"temp\",\"value\":" + String(temperature, 1) + ",\"unit\":\"°C\"},";
  jsonPayload += "{\"time\":\"" + currentTime + "\",\"specie\":\"humidity\",\"value\":" + String(humidity, 1) + ",\"unit\":\"%\"}";
  jsonPayload += "]}";

  // Debugging the payload
  Serial.print("JSON Payload: ");
  Serial.println(jsonPayload);

  // Correct AQICN API URL with HTTPS
  String AQICN_API_URL = "https://aqicn.org/sensor/upload/";

  while (!success && retries < maxDataRetries) {
    if (WiFi.status() == WL_CONNECTED) {
      HTTPClient http;
      http.begin(client, AQICN_API_URL);  // Using secure client
      http.addHeader("Content-Type", "application/json");

      int httpResponseCode = http.POST(jsonPayload);

      if (httpResponseCode > 0) {
        String response = http.getString();
        Serial.println("Data posted to AQICN.");
        Serial.println("Response:");
        Serial.println(response);
        success = true;
      } else {
        Serial.print("Error posting data to AQICN. HTTP Response code: ");
        Serial.println(httpResponseCode);
      }

      http.end();
    } else {
      Serial.println("WiFi not connected, unable to send data to AQICN.");
    }

    retries++;
    if (!success) {
      Serial.println("Retrying data send...");
      delay(1000);  // Small delay before retry
    }
  }
}

// Function to get current time in ISO 8601 format
String getISO8601Time() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);

  char timeString[30];
  strftime(timeString, sizeof(timeString), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);

  return String(timeString);
}

// Function to handle the root page ("/") for the web server
void handleRoot() {
  Serial.println("HandleRoot: HTTP request received: /");

  // Trigger sensor update
  if (!updateSensorData()) {
    server.send(500, "text/html", "<html><body><h1>Sensor Error</h1></body></html>");
    return;
  }

  // Send a simple web page showing the current sensor readings with colors
  String message = "<html><head><title>ESP8266 Sensor Data</title></head><body>";
  message += "<h1>Sensor Readings</h1>";

  // Display errors, if any
  if (errorMessages.length() > 0) {
    message += "<p><strong>Errors:</strong><br>" + errorMessages + "</p>";
  }

  // Temperature with color coding
  message += "<p><strong>Temperature: </strong>" + String(temp) + " &#8451;</p>";
  
  // Humidity with color coding
  message += "<p><strong>Humidity: </strong>" + String(hum) + " %</p>";

  // PM2.5 with color coding
  message += "<p><strong>PM2.5: </strong>" + String(pm25) + " &micro;g/m&sup3;</p>";

  // PM10 with color coding
  message += "<p><strong>PM10: </strong>" + String(pm10) + " &micro;g/m&sup3;</p>";

  message += "</body></html>";
  
  server.send(200, "text/html", message);  
}

// Function to handle manual sensor read trigger via HTTP request
void handleTrigger() {
  Serial.println("HTTP request received: /trigger");

  // Update sensor data on trigger
  if (!updateSensorData()) {
    Serial.println("Sensor reading failed. Rebooting...");
    ESP.restart();  // Reboot if sensor reading fails
  }

  // Send sensor data to AQICN after updating the data
  sendDataToAQICN(temp, hum, pm25, pm10);

  // Send a simple HTTP response indicating that the sensor reading was triggered
  String message = "<html><head><title>Triggered Sensor Read</title></head><body>";
  message += "<h1>Sensor Read Triggered</h1>";
  message += "<p>Temperature: " + String(temp) + " &#8451;</p>";
  message += "<p>Humidity: " + String(hum) + " %</p>";
  message += "<p>PM2.5: " + String(pm25) + " &micro;g/m&sup3;</p>";
  message += "<p>PM10: " + String(pm10) + " &micro;g/m&sup3;</p>";
  message += "</body></html>";
  
  server.send(200, "text/html", message);  

  // Log the updated readings
  Serial.println("Updated sensor readings:");
  Serial.print("Temperature: ");
  Serial.println(temp);
  Serial.print("Humidity: ");
  Serial.println(hum);
  Serial.print("PM2.5: ");
  Serial.println(pm25);
  Serial.print("PM10: ");
  Serial.println(pm10);
}