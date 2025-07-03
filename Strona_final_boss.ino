#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>

const char* ssid = "ESP32-AP";
const char* password = "12345678";
const int analogPins[6] = {39, 36, 34, 35, 32, 33};

AsyncWebServer server(80);

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <title>Smart Buty</title>
  <style>
    body { font-family: sans-serif; text-align: center; background: #000; color: white; }
    .plot-grid { display: flex; flex-wrap: wrap; justify-content: center; gap: 20px; }
    .plot { background: black; border: 1px solid #555; padding: 10px; position: relative; }
    svg { width: 300px; height: 150px; background: black; }
    .value-label, .percent-label { font-size: 14px; color: white; margin-top: 5px; }
    table { width: 100%; border-collapse: collapse; color: white; margin-top: 10px; }
    th, td { padding: 6px 10px; border-bottom: 1px solid #444; }
    th { text-align: left; background-color: #111; }
    select, button {
      background-color: black;
      color: red;
      border: 1px solid red;
      padding: 5px 10px;
    }
    button:hover {
      background-color: #200;
    }
    .foot-container {
      position: relative;
      width: 200px;
      margin: 30px auto;
    }
    .foot-container img {
      width: 100%;
    }
    .sensor {
      position: absolute;
      width: 20px;
      height: 20px;
      border-radius: 50%;
      background-color: blue;
      opacity: 0.8;
      transform: translate(-50%, -50%);
    }
  </style>
</head>
<body>
  <h1>Smart Buty</h1>
  <h3>Pomiar poprawnego ułożenia stopy</h3>

  <div class="plot-grid" id="plots"></div>

  <h2>Statystyki sygnału z ostatnich 10 sekund</h2>
  <div id="piezoStats" style="max-width: 800px; margin: auto;"></div>

  <h2>Wizualizacja nacisku na stopę</h2>
  <div class="foot-container">
    <img src="/foot.png" alt="Foot diagram" />
    <div id="sensor0" class="sensor" style="top: 9%; left: 50%;"></div>
    <div id="sensor1" class="sensor" style="top: 22%; left: 50%;"></div>
    <div id="sensor2" class="sensor" style="top: 36%; left: 50%;"></div>
    <div id="sensor3" class="sensor" style="top: 50%; left: 50%;"></div>
    <div id="sensor4" class="sensor" style="top: 66%; left: 50%;"></div>
    <div id="sensor5" class="sensor" style="top: 82%; left: 50%;"></div>
  </div>

  <div style="margin-top: 20px;">
    <label for="rangeSelect">Zakres danych do pobrania:</label>
    <select id="rangeSelect">
      <option value="all">Od początku pomiaru</option>
      <option value="3600000">Ostatnia godzina</option>
      <option value="1800000">Ostatnie 30 minut</option>
      <option value="600000">Ostatnie 10 minut</option>
      <option value="300000">Ostatnie 5 minut</option>
    </select>
    <br><br>
    <button onclick="downloadCSV()">📁 Pobierz dane (CSV)</button>
  </div>

  <script>
    const labels = ["Palce", "Przódstopie", "Pięta", "Końcówki Palców", "Podbicie", "Początek pięty"];
    const pinCount = 6;
    const historyLength = 100;
    const updateInterval = 300;
    const shareWindow = Math.round(3000 / updateInterval);
    const statsWindow = Math.round(10000 / updateInterval);
    const dataHistory = {};
    let csvLog = [];

    function createSVGPlot(id) {
      const div = document.createElement('div');
      div.className = 'plot';
      div.innerHTML = `
        <h2>${labels[id]}</h2>
        <svg id="svg_A${id}">
          <g class="grid">
            ${[0.0, 0.825, 1.65, 2.475, 3.3].map(v => {
              const y = 150 - v / 3.3 * 150;
              return `
                <line x1="0" y1="${y}" x2="300" y2="${y}" stroke="#333" stroke-width="1" />
                <text x="2" y="${y - 2}" font-size="10" fill="#ccc">${v.toFixed(2)}V</text>`;
            }).join('')}
          </g>
          <polyline fill="none" stroke="red" stroke-width="2" points=""/>
        </svg>
        <div class="value-label" id="val_A${id}">Wartość: 0.00 V</div>
        <div class="percent-label" id="pct_A${id}">Użycie: 0.0% zakresu</div>
      `;
      return div;
    }

    function drawPlot(id, values, latestValue) {
      const svg = document.getElementById('svg_A' + id);
      const maxVal = 4095;
      const w = svg.clientWidth;
      const h = svg.clientHeight;
      const stepX = w / (historyLength - 1);
      const points = values.map((v, i) => {
        const x = i * stepX;
        const y = h - (v / maxVal) * h;
        return `${x},${y}`;
      }).join(' ');
      svg.querySelector('polyline').setAttribute('points', points);

      const voltage = (latestValue / 4095 * 3.3).toFixed(2);
      const percent = ((latestValue / 4095) * 100).toFixed(1);
      document.getElementById('val_A' + id).innerText = `Wartość: ${voltage} V`;
      document.getElementById('pct_A' + id).innerText = `Użycie: ${percent}% zakresu`;

      // Zmieniamy kolor czujnika w zależności od napięcia
      const sensor = document.getElementById('sensor' + id);
      const voltageValue = latestValue / 4095 * 3.3;
      if (voltageValue >= 1.39) {
        sensor.style.backgroundColor = 'red';
      } else if (voltageValue > 1.0 && voltageValue < 1.39) {
        sensor.style.backgroundColor = 'yellow';
      } else {
        sensor.style.backgroundColor = 'white';
      }
    }

    function updatePiezoStats() {
      const statsDiv = document.getElementById('piezoStats');
      let totals = [];
      let sumTotal = 0;

      for (let i = 0; i < pinCount; i++) {
        const key = 'A' + i;
        const history = dataHistory[key] || [];
        const recent = history.slice(-shareWindow);
        const total = recent.reduce((a, b) => a + b, 0);
        totals.push(total);
        sumTotal += total;
      }

      let html = `
        <table>
          <tr>
            <th>Pozycja</th>
            <th>Min [V]</th>
            <th>Średnia [V]</th>
            <th>Max [V]</th>
            <th>Udział [%]</th>
          </tr>
      `;

      for (let i = 0; i < pinCount; i++) {
        const key = 'A' + i;
        const history = dataHistory[key] || [];
        const recentStats = history.slice(-statsWindow);
        const min = Math.min(...recentStats);
        const max = Math.max(...recentStats);
        const avg = recentStats.reduce((a, b) => a + b, 0) / recentStats.length;
        const percent = sumTotal > 0 ? (totals[i] / sumTotal * 100).toFixed(1) : "0.0";

        html += `
          <tr>
            <td>${labels[i]}</td>
            <td>${(min / 4095 * 3.3).toFixed(2)}</td>
            <td>${(avg / 4095 * 3.3).toFixed(2)}</td>
            <td>${(max / 4095 * 3.3).toFixed(2)}</td>
            <td>${percent}%</td>
          </tr>
        `;
      }
      html += '</table>';
      statsDiv.innerHTML = html;
    }

    async function updateData() {
      try {
        const res = await fetch('/data');
        const json = await res.json();
        const timestamp = Date.now();
        let entry = { timestamp };
        for (let i = 0; i < pinCount; i++) {
          const key = 'A' + i;
          if (!dataHistory[key]) dataHistory[key] = Array(historyLength).fill(0);
          dataHistory[key].push(json[key]);
          if (dataHistory[key].length > historyLength) dataHistory[key].shift();
          drawPlot(i, dataHistory[key], json[key]);
          entry[key] = json[key];
        }
        csvLog.push(entry);
        updatePiezoStats();
      } catch (e) {
        console.error("Błąd pobierania danych:", e);
      }
    }

    function downloadCSV() {
      if (csvLog.length === 0) return alert("Brak danych do zapisania.");

      const selected = document.getElementById("rangeSelect").value;
      const now = Date.now();
      let filtered = [];

      if (selected === "all") {
        filtered = csvLog;
      } else {
        const rangeMs = parseInt(selected);
        filtered = csvLog.filter(row => row.timestamp >= (now - rangeMs));
      }

      if (filtered.length === 0) {
        alert("Brak danych w wybranym zakresie.");
        return;
      }

      const headers = ['Czas [ms]'];
      for (let i = 0; i < pinCount; i++) headers.push(`Piezo ${i} [V]`);

      const rows = filtered.map(row => {
        const values = [row.timestamp];
        for (let i = 0; i < pinCount; i++) {
          values.push((row['A' + i] / 4095 * 3.3).toFixed(3));
        }
        return values.join(",");
      });

      const csvContent = [headers.join(","), ...rows].join("\n");
      const blob = new Blob([csvContent], { type: 'text/csv' });
      const url = URL.createObjectURL(blob);

      const a = document.createElement('a');
      a.href = url;
      a.download = `smart_buty_data_${Date.now()}.csv`;
      document.body.appendChild(a);
      a.click();
      document.body.removeChild(a);
    }

    const plotsDiv = document.getElementById('plots');
    for (let i = 0; i < pinCount; i++) {
      plotsDiv.appendChild(createSVGPlot(i));
    }
    setInterval(updateData, updateInterval);
    updateData();
  </script>
</body>
</html>
)rawliteral";

String getSensorValuesJson() {
  StaticJsonDocument<256> doc;
  for (int i = 0; i < 6; i++) {
    doc["A" + String(i)] = analogRead(analogPins[i]);
  }
  String json;
  serializeJson(doc, json);
  return json;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  if (!SPIFFS.begin(true)) {
    Serial.println("Błąd inicjalizacji SPIFFS!");
    return;
  }

  WiFi.softAP(ssid, password);
  delay(1000);

  IPAddress IP = WiFi.softAPIP();
  Serial.println("Access Point uruchomiony");
  Serial.print("Adres IP: ");
  Serial.println(IP);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", index_html);
  });

  server.on("/data", HTTP_GET, [](AsyncWebServerRequest *request){
    String json = getSensorValuesJson();
    request->send(200, "application/json", json);
  });

  server.serveStatic("/foot.png", SPIFFS, "/foot.png");

  server.begin();
  Serial.println("Serwer HTTP uruchomiony");
}

void loop() {}
