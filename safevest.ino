#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include "MAX30105.h"
#include "heartRate.h"

// wifi dan password untuk esp dan device pemantau
const char* ssid = "xxxx";     
const char* pass = "xxxx";

String textmebotApikey = "xxxx"; // Ganti dengan API Key dari TextMeBot
String targetNomor = "+xxxx"; 

#define I2C_SDA 8
#define I2C_SCL 9
const int mq135Pin = 4;
const int buzzerPin = 45;

const int MPU_addr = 0x68;
float total_G = 0, roll = 0, pitch = 0;
const float threshold_bawah = 1.116; 
const float threshold_atas = 2.303;

MAX30105 particleSensor;
const byte RATE_SIZE = 4; 
byte rates[RATE_SIZE]; 
byte rateSpot = 0;
long lastBeat = 0; 
float beatsPerMinute = 0;
int beatAvg = 0;

int gasValue = 0;
int ambangBahayaGas = 1500;

unsigned long prevTimeMPU = 0, prevTimeSerial = 0;
unsigned long timeLyingDownStart = 0;
unsigned long lastWaSentTime = 0;

bool isLyingDown = false;
bool fallEventDetected = false;
String currentAlertMsg = "Aman";
bool isDanger = false;

WebServer server(80);

// webserver dashboard
const char* htmlPage PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="viewport" content="width=device-width, initial-scale=1">
  <title>SafeVest Dashboard</title>
  <style>
    body { font-family: Arial; text-align: center; background-color: #f4f4f9; margin: 0; padding: 20px; }
    .card { background: white; padding: 20px; margin: 10px auto; border-radius: 10px; box-shadow: 0px 4px 8px rgba(0,0,0,0.1); max-width: 600px; }
    h2 { color: #333; }
    table { width: 100%; border-collapse: collapse; margin-top: 10px; }
    th, td { border: 1px solid #ddd; padding: 8px; font-size: 18px;}
    th { background-color: #4CAF50; color: white; }
    .danger { background-color: #ffcccc; color: red; font-weight: bold; }
    .safe { background-color: #ccffcc; color: green; font-weight: bold; }
    canvas { background: #fff; border: 1px solid #ccc; width: 100%; height: 200px; margin-top: 10px;}
    
    .modal { display: none; position: fixed; z-index: 1; left: 0; top: 0; width: 100%; height: 100%; background-color: rgba(0,0,0,0.6); }
    .modal-content { background-color: #fff; margin: 15% auto; padding: 20px; border: 1px solid #888; width: 80%; border-radius: 10px; text-align: center;}
    .close-btn { background: red; color: white; padding: 10px 20px; border: none; border-radius: 5px; cursor: pointer; font-size: 16px; margin-top: 15px;}
  </style>
</head>
<body>

  <h2>SafeVest Real-Time Monitor</h2>
  <div class="card" id="statusCard">
    <h3>Status Peringatan: <span id="statusText">Memuat...</span></h3>
  </div>

  <div class="card">
    <h3>Tabel Data Sensor</h3>
    <table>
      <tr><th>Sensor</th><th>Nilai</th></tr>
      <tr><td>BPM (Jantung)</td><td id="valBpm">0</td></tr>
      <tr><td>Gas (MQ135)</td><td id="valGas">0</td></tr>
      <tr><td>Gravitasi (G)</td><td id="valG">0</td></tr>
      <tr><td>Roll / Pitch (&deg;)</td><td id="valAngle">0 / 0</td></tr>
    </table>
  </div>

  <div class="card">
    <h3>Grafik Jantung (BPM)</h3>
    <canvas id="bpmChart"></canvas>
  </div>

  <div id="alertModal" class="modal">
    <div class="modal-content">
      <h2 style="color:red;">&#9888; PERINGATAN BAHAYA! &#9888;</h2>
      <p id="modalMsg" style="font-size: 20px; font-weight:bold;">Message</p>
      <button class="close-btn" onclick="closeModal()">Tutup Peringatan</button>
    </div>
  </div>

<script>
  let canvas = document.getElementById('bpmChart');
  let ctx = canvas.getContext('2d');
  let bpmData = new Array(50).fill(0);
  let lastAlert = "";

  function drawChart() {
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    ctx.beginPath();
    let width = canvas.width; let height = canvas.height;
    ctx.moveTo(0, height - (bpmData[0] / 200 * height));
    for(let i = 1; i < bpmData.length; i++) {
      let x = (i / 50) * width;
      let y = height - (bpmData[i] / 200 * height);
      ctx.lineTo(x, y);
    }
    ctx.strokeStyle = "red"; ctx.lineWidth = 2; ctx.stroke();
  }

  function closeModal() {
    document.getElementById('alertModal').style.display = "none";
  }

  setInterval(function() {
    fetch('/data').then(response => response.json()).then(data => {
      document.getElementById('valBpm').innerText = data.bpm;
      document.getElementById('valGas').innerText = data.gas;
      document.getElementById('valG').innerText = data.g;
      document.getElementById('valAngle').innerText = data.roll + " / " + data.pitch;
      
      let statusCard = document.getElementById('statusCard');
      let statusText = document.getElementById('statusText');
      
      if(data.isDanger) {
        statusCard.className = "card danger";
        statusText.innerText = data.msg;
        if(lastAlert !== data.msg) {
          document.getElementById('modalMsg').innerText = data.msg;
          document.getElementById('alertModal').style.display = "block";
          lastAlert = data.msg;
        }
      } else {
        statusCard.className = "card safe";
        statusText.innerText = "Aman";
        lastAlert = "";
      }

      bpmData.push(data.bpm);
      bpmData.shift();
      drawChart();
    });
  }, 1000);
</script>
</body>
</html>
)rawliteral";

void setup() {
  Serial.begin(115200);
  pinMode(buzzerPin, OUTPUT);
  digitalWrite(buzzerPin, LOW);
  analogReadResolution(12);

  Wire.begin(I2C_SDA, I2C_SCL);

  Wire.beginTransmission(MPU_addr);
  Wire.write(0x6B); Wire.write(0);
  Wire.endTransmission(true);

  if (!particleSensor.begin(Wire, I2C_SPEED_STANDARD)) {
    Serial.println("MAX30102 Gagal! Cek kabel SDA/SCL atau daya.");
  } else {
    byte ledBrightness = 60; 
    byte sampleAverage = 4; 
    byte ledMode = 2; 
    int sampleRate = 100;
    int pulseWidth = 411; 
    int adcRange = 4096; 
    
    particleSensor.setup(ledBrightness, sampleAverage, ledMode, sampleRate, pulseWidth, adcRange); 
    particleSensor.setPulseAmplitudeRed(0x3C);
    particleSensor.setPulseAmplitudeIR(0x3C);
    particleSensor.setPulseAmplitudeGreen(0);
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  Serial.print("Menghubungkan ke WiFi HP");
  
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 20) {
    delay(500); Serial.print("."); retries++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nBerhasil Terhubung!");
    Serial.print("Masukkan IP Address ini di Browser Laptop: "); 
    Serial.println(WiFi.localIP());  
  } else {
    Serial.println("\nGagal terhubung ke WiFi HP.");
  }

  server.on("/", []() {
    server.send(200, "text/html", htmlPage);
  });
  server.on("/data", handleSensorData);
  server.begin();
}

void loop() {
  unsigned long currentMillis = millis();
  server.handleClient();

  particleSensor.check(); 
  while (particleSensor.available()) { 
    long irValue = particleSensor.getFIFOIR(); 

    if (checkForBeat(irValue) == true) {
      long delta = millis() - lastBeat;
      lastBeat = millis();
      beatsPerMinute = 60 / (delta / 1000.0);
      
      if (beatsPerMinute < 255 && beatsPerMinute > 20) {
        rates[rateSpot++] = (byte)beatsPerMinute;
        rateSpot %= RATE_SIZE;
        beatAvg = 0;
        for (byte x = 0 ; x < RATE_SIZE ; x++) beatAvg += rates[x];
        beatAvg /= RATE_SIZE;
      }
    }
    
    if (irValue < 50000) {
      beatAvg = 0;
    }

    particleSensor.nextSample(); 
  }

  if (currentMillis - prevTimeMPU >= 20) {
    float dt = (currentMillis - prevTimeMPU) / 1000.0;
    prevTimeMPU = currentMillis;

    Wire.beginTransmission(MPU_addr);
    Wire.write(0x3B);
    Wire.endTransmission(false);
    Wire.requestFrom(MPU_addr, 14, true);

    if (Wire.available() >= 14) {
      float ax = (Wire.read() << 8 | Wire.read()) / 16384.0;
      float ay = (Wire.read() << 8 | Wire.read()) / 16384.0;
      float az = (Wire.read() << 8 | Wire.read()) / 16384.0;
      Wire.read(); Wire.read(); 
      float gx = (Wire.read() << 8 | Wire.read()) / 131.0;
      float gy = (Wire.read() << 8 | Wire.read()) / 131.0;

      total_G = sqrt(ax*ax + ay*ay + az*az);
      
      if (total_G > 0.1) {
        float accRoll = atan2(ay, az) * 180 / PI;
        float accPitch = atan2(-ax, sqrt(ay*ay + az*az)) * 180 / PI;
        roll = 0.96 * (roll + gx * dt) + 0.04 * accRoll;
        pitch = 0.96 * (pitch + gy * dt) + 0.04 * accPitch;
      }
    }
  }

  gasValue = analogRead(mq135Pin);
  evaluateAlarms();

  if (currentMillis - prevTimeSerial >= 100) {
    prevTimeSerial = currentMillis;
    Serial.print("BPM:"); Serial.print(beatAvg);
    Serial.print(",Gas:"); Serial.print(gasValue);
    Serial.print(",Gravitasi:"); Serial.print(total_G);
    Serial.print(",Roll:"); Serial.print(roll);
    Serial.print(",Pitch:"); Serial.println(pitch);
  }
}

void evaluateAlarms() {
  unsigned long now = millis();
  
  bool currentLying = (abs(roll) >= 80 && abs(roll) <= 110) || (abs(pitch) >= 80 && abs(pitch) <= 110);
  
  if (currentLying && !isLyingDown) {
    timeLyingDownStart = now; 
    isLyingDown = true;
  } else if (!currentLying) {
    isLyingDown = false;
    fallEventDetected = false; 
  }

  unsigned long lyingDuration = isLyingDown ? (now - timeLyingDownStart) : 0;
  bool lying1Min = lyingDuration > 60000;
  bool lying5Min = lyingDuration > 300000;

  if (total_G >= threshold_bawah && total_G <= threshold_atas) {
    fallEventDetected = true;
  }

  bool hrFast = (beatAvg > 120);
  bool hrSlow = (beatAvg > 10 && beatAvg < 50); 
  bool gasDanger = (gasValue > ambangBahayaGas);

  isDanger = true; 
  if (fallEventDetected && lying5Min && hrSlow) currentAlertMsg = "Jatuh, Tertidur >5Mnt, HR Lambat!";
  else if (fallEventDetected && lying5Min && hrFast) currentAlertMsg = "Jatuh, Tertidur >5Mnt, HR Cepat!";
  else if (fallEventDetected && lying1Min && hrFast) currentAlertMsg = "Jatuh, Tertidur >1Mnt, HR Cepat!";
  else if (lying1Min) currentAlertMsg = "Tertidur >1Mnt";
  else if (lying5Min && hrSlow) currentAlertMsg = "Pingsan >5Mnt, HR Lambat!";
  else if (lying5Min && hrFast) currentAlertMsg = "Pingsan >5Mnt, HR Cepat!";
  else if (fallEventDetected && lying5Min) currentAlertMsg = "Jatuh & Tertidur > 5Mnt!";
  else if (gasDanger) currentAlertMsg = "Gas Berbahaya (CO/Asap) Terdeteksi!";
  else if (hrFast) currentAlertMsg = "Detak Jantung Terlalu Cepat!";
  else if (hrSlow) currentAlertMsg = "Detak Jantung Terlalu Lambat!";
  else if (fallEventDetected) currentAlertMsg = "Pekerja Terdeteksi Jatuh!";
  else {
    isDanger = false;
    currentAlertMsg = "Aman";
  }

  if (isDanger) digitalWrite(buzzerPin, HIGH);
  else digitalWrite(buzzerPin, LOW);

  if (isDanger && (now - lastWaSentTime > 60000)) {
    sendTextMeBotAlert(currentAlertMsg); // Nama fungsi diubah
    lastWaSentTime = now;
  }
}

// ngirim pesan Whatsapp
void sendTextMeBotAlert(String message) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    
    //isi pesan
    String fullMessage = "*DARURAT SAFEVEST:*\n" + message;
    fullMessage.replace(" ", "%20");
    fullMessage.replace("\n", "%0A");

    // url textgmebot
    String url = "http://api.textmebot.com/send.php?apikey=" + textmebotApikey + "&number=" + targetNomor + "&text=" + fullMessage;

    http.begin(url);
    int httpResponseCode = http.GET();
    
    if (httpResponseCode > 0) {
      Serial.println("Berhasil kirim TextMeBot: " + message);
    } else {
      Serial.print("Gagal kirim TextMeBot. Error Code: ");
      Serial.println(httpResponseCode);
    }
    http.end();
  }
}

// ngirim data dari sensor ke web
void handleSensorData() {
  String json = "{";
  json += "\"bpm\":" + String(beatAvg) + ",";
  json += "\"gas\":" + String(gasValue) + ",";
  json += "\"g\":\"" + String(total_G, 2) + "\",";
  json += "\"roll\":\"" + String(roll, 1) + "\",";
  json += "\"pitch\":\"" + String(pitch, 1) + "\",";
  json += "\"isDanger\":" + String(isDanger ? "true" : "false") + ",";
  json += "\"msg\":\"" + currentAlertMsg + "\"";
  json += "}";
  server.send(200, "application/json", json);
}