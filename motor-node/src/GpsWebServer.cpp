#include "GpsWebServer.h"
#include <WiFi.h>

GpsWebServer::GpsWebServer(GpsReceiver& gps, uint16_t port)
    : gps_(gps), server_(port) {}

GpsWebServer::~GpsWebServer() {}

String GpsWebServer::snapshotToJson(const GpsSnapshot& s) {
  JsonDocument doc;
  doc["valid"] = s.valid;
  doc["activeFix"] = s.activeFix;
  doc["fixQuality"] = s.fixQuality;
  doc["fixMode"] = s.fixMode;
  doc["satellitesInUse"] = s.satellitesInUse;
  doc["satellitesInView"] = s.satellitesInView;
  doc["latitude"] = s.latitude;
  doc["longitude"] = s.longitude;
  doc["altitudeMsl"] = s.altitudeMsl;
  doc["geoidSeparation"] = s.geoidSeparation;
  doc["speedKmh"] = s.speedKmh;
  doc["courseDeg"] = s.courseDeg;
  doc["hdop"] = s.hdop;
  doc["vdop"] = s.vdop;
  doc["pdop"] = s.pdop;
  doc["utcHour"] = s.utcHour;
  doc["utcMin"] = s.utcMin;
  doc["utcSec"] = s.utcSec;
  doc["utcDay"] = s.utcDay;
  doc["utcMonth"] = s.utcMonth;
  doc["utcYear"] = s.utcYear;
  doc["dateValid"] = s.dateValid;
  doc["lastLineMs"] = s.lastLineMs;
  doc["sentencesParsed"] = s.sentencesParsed;
  doc["checksumErrors"] = s.checksumErrors;

  JsonArray sats = doc["satellites"].to<JsonArray>();
  for (int i = 0; i < kGpsMaxSatellites; i++) {
    if (!s.satellites[i].active) continue;
    JsonObject o = sats.add<JsonObject>();
    o["prn"] = s.satellites[i].prn;
    o["elevation"] = s.satellites[i].elevation;
    o["azimuth"] = s.satellites[i].azimuth;
    o["snr"] = s.satellites[i].snr;
  }

  String json;
  serializeJson(doc, json);
  return json;
}

void GpsWebServer::begin() {
  // GET /api/gps -> JSON snapshot (client polls every few minutes).
  server_.on("/api/gps", HTTP_GET, [this](AsyncWebServerRequest* req) {
    GpsSnapshot s = gps_.snapshot();
    AsyncWebServerResponse* r = req->beginResponse(200, "application/json", snapshotToJson(s));
    // no-store so the browser never serves a stale JSON cache
    r->addHeader("Cache-Control", "no-store");
    req->send(r);
  });

  // GET / -> bounce to /gps
  server_.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    AsyncWebServerResponse* r = req->beginResponse(302);
    r->addHeader("Location", "/gps");
    req->send(r);
  });

  // GET /gps -> self-contained HTML that polls /api/gps every 5 s.
  server_.on("/gps", HTTP_GET, [](AsyncWebServerRequest* req) {
    const char* html =
      "<!DOCTYPE html><html lang=\"de\"><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<title>GPS</title><style>"
      "body{font-family:system-ui,sans-serif;background:#0b0f14;color:#e6edf3;margin:0;padding:16px}"
      "h1{font-size:20px;margin:0 0 14px}"
      ".card{background:#161b22;border:1px solid #30363d;border-radius:10px;padding:14px;margin-bottom:12px}"
      ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px}"
      ".kv{display:flex;justify-content:space-between;border-bottom:1px solid #21262d;padding:4px 0}"
      ".kv b{color:#8b949e;font-weight:500}.fix-ok{color:#3fb950}.fix-no{color:#f85149}"
      "table{width:100%;border-collapse:collapse;font-size:13px}"
      "th,td{text-align:left;padding:4px 8px;border-bottom:1px solid #21262d}"
      ".muted{color:#8b949e;font-size:12px}"
      "</style></head><body><h1>📡 GNSS (u-blox NEO-8M)</h1>"
      "<div class=card><div class=grid id=pos></div></div>"
      "<div class=card><b>Fix</b> <div class=grid id=fix></div></div>"
      "<div class=card><b>Dilution of Precision (DOP)</b><div class=grid id=dop></div></div>"
      "<div class=card><b>Uhrzeit (UTC)</b><div class=grid id=time></div></div>"
      "<div class=card><b>Satelliten in Sicht</b> <span class=muted id=satcount></span>"
      "<table><thead><tr><th>PRN</th><th>Elev</th><th>Azim</th><th>SNR</th></tr></thead>"
      "<tbody id=satrows></tbody></table></div>"
      "<div class=card class=muted>Letzte gültige Satz: <span id=last></span> · "
      "Sätze gepart: <span id=parsed></span> · Checksum-Fehler: <span id=csum></span></div>"
      "<script>async function load(){let r=await fetch('/api/gps');let s=await r.json();"
      "function fmtLt(v){if(v>0)return v.toFixed(6)+'° N';if(v<0)return (-v).toFixed(6)+'° S';return '—'}"
      "function fmtLn(v){if(v>0)return v.toFixed(6)+'° O';if(v<0)return (-v).toFixed(6)+'° W';return '—'}"
      "document.getElementById('pos').innerHTML="
      "kv('Breite',fmtLt(s.latitude))+kv('Länge',fmtLn(s.longitude))+"
      "kv('Höhe MSL',s.altitudeMsl!=0?Math.round(s.altitudeMsl)+' m':'—')+"
      "kv('Geoide',s.geoidSeparation!=0?Math.round(s.geoidSeparation)+' m':'—')+"
      "kv('Geschw.',s.speedKmh!=0?s.speedKmh.toFixed(1)+' km/h':'—')+"
      "kv('Kurs',s.courseDeg!=0?Math.round(s.courseDeg)+'°':'—');"
      "const fix=s.activeFix?`<span class=fix-ok>Fix ✓</span>`:`<span class=fix-no>kein Fix</span>`;"
      "document.getElementById('fix').innerHTML=kv('Status',fix)+"
      "kv('Fix-Qualität',s.fixQuality)+kv('Fix-Modus',s.fixMode)+"
      "kv('Sat. genutzt',s.satellitesInUse)+kv('Sat. in Sicht',s.satellitesInView);"
      "document.getElementById('dop').innerHTML=kv('HDOP',s.hdop)+kv('VDOP',s.vdop)+kv('PDOP',s.pdop);"
      "document.getElementById('time').innerHTML="
      "kv('UTC',(s.utcHour<10?'0':'')+s.utcHour+':'+(s.utcMin<10?'0':'')+s.utcMin+':'+(s.utcSec<10?'0':'')+s.utcSec)+"
      "kv('Datum',s.dateValid?s.utcDay+'.'+s.utcMonth+'.'+s.utcYear:'—');"
      "document.getElementById('satcount').textContent=s.satellitesInView;"
      "let rows='';s.satellites.forEach(x=>{rows+='<tr><td>'+x.prn+'</td><td>'+x.elevation+'°</td><td>'+x.azimuth+'°</td><td>'+(x.snr?x.snr+'dB':'—')+'</td></tr>'});"
      "document.getElementById('satrows').innerHTML=rows;"
      "document.getElementById('last').textContent=s.sentencesParsed>0?Math.round(s.lastLineMs/1000)+'s':'—';"
      "document.getElementById('parsed').textContent=s.sentencesParsed;"
      "document.getElementById('csum').textContent=s.checksumErrors;"
      "}function kv(k,v){return '<div class=kv><b>'+k+'</b><span>'+v+'</span></div>'}"
      "load();setInterval(load,5000);"
      "</script></body></html>";
    AsyncWebServerResponse* r = req->beginResponse(200, "text/html", html);
    req->send(r);
  });

  server_.begin();
  Serial.printf("[WEB] GPS server on http://%s/gps\n", WiFi.softAPIP().toString().c_str());
}