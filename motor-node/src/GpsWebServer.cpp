#include "GpsWebServer.h"
#include <WiFi.h>

GpsWebServer::GpsWebServer(GpsReceiver& gps, VehicleState& vehicle, uint16_t port)
    : gps_(gps), vehicle_(vehicle), server_(port) {}

GpsWebServer::~GpsWebServer() {
  // AsyncWebServer never frees handlers registered via addHandler(), so the
  // event source must be deleted by us - and only after the server is gone.
  if (logEvents_) {
    delete logEvents_;
    logEvents_ = nullptr;
  }
}

void GpsWebServer::loop() {
  // Push any new LOG lines to /api/log/events SSE subscribers.
  if (!logEvents_ || logEvents_->count() == 0) return;
  uint32_t now = millis();
  if (now - logLastPollMs_ < 250) return;   // ~4 Hz is plenty for a log tail
  logLastPollMs_ = now;

  uint64_t maxSeq = logLastSeq_;
  char buf[2048];
  size_t n = LogTail::instance().dumpAfter(logLastSeq_, &maxSeq, buf, sizeof(buf));
  if (n > 0) {
    logEvents_->send(buf, "log", millis());
    logLastSeq_ = maxSeq;
  }
}

String GpsWebServer::vehicleToJson(const VehicleState& v) {
  JsonDocument doc;
  doc["rpm"] = v.rpm;
  doc["coolantTemperature"] = v.coolantTemperature;
  doc["speedKmh"] = v.speedKmh;
  doc["fuelPercent"] = v.fuelPercent;
  doc["ignitionAdvanceDeg"] = v.ignitionAdvanceDeg;
  doc["runtimeSec"] = v.runtimeSec;

  // Raw OBD bytes as the ELM327 emulator would answer for each Mode 01 PID,
  // so the page shows both the engineering value AND the wire value.
  doc["obd"]["rpmRaw"] = v.rpmToObdRaw();
  doc["obd"]["coolantRaw"] = v.coolantToObdRaw();
  doc["obd"]["speedRaw"] = v.speedToObdRaw();
  doc["obd"]["fuelRaw"] = v.fuelToObdRaw();
  doc["obd"]["ignitionAdvanceRaw"] = v.ignitionAdvanceToObdRaw();
  doc["obd"]["runtimeMinutes"] = v.runtimeToObdMinutes();

  String json;
  serializeJson(doc, json);
  return json;
}

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
  doc["bytesReceived"] = s.bytesReceived;
  doc["linesReceived"] = s.linesReceived;
  doc["lastByteMs"] = s.lastByteMs;
  doc["dataAgeSec"] = s.lastLineMs ? (int)((millis() - s.lastLineMs) / 1000) : -1;
  doc["lastByteAgeSec"] = s.lastByteMs ? (int)((millis() - s.lastByteMs) / 1000) : -1;

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

  // GET /api/vehicle -> JSON snapshot of all ELM327/OBD live values.
  server_.on("/api/vehicle", HTTP_GET, [this](AsyncWebServerRequest* req) {
    AsyncWebServerResponse* r = req->beginResponse(200, "application/json", vehicleToJson(vehicle_));
    r->addHeader("Cache-Control", "no-store");
    req->send(r);
  });

  // GET / -> bounce to /gps
  server_.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    AsyncWebServerResponse* r = req->beginResponse(302);
    r->addHeader("Location", "/gps");
    req->send(r);
  });

  // GET /gps -> self-contained HTML that polls /api/gps every 2.5 s.
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
      "#conn{font-size:15px;font-weight:600;text-align:center}"
      ".st-ok{background:#12261b;border-color:#2ea043}.st-warn{background:#2b1d0d;border-color:#d29922}.st-bad{background:#2c1214;border-color:#f85149}"
      ".ok{color:#3fb950}.warn{color:#d29922}.bad{color:#f85149}"
      ".bar{display:inline-block;height:6px;border-radius:3px;background:#21262d;vertical-align:middle;margin-right:6px;min-width:18px}"
      ".bar i{display:block;height:100%;border-radius:3px;background:linear-gradient(90deg,#0d7377,#3fb950)}"
      "table{width:100%;border-collapse:collapse;font-size:13px}"
      "th,td{text-align:left;padding:4px 8px;border-bottom:1px solid #21262d}"
      ".muted{color:#8b949e;font-size:12px}"
      ".top{display:flex;justify-content:space-between;align-items:center;margin-bottom:14px}"
      ".top a{color:#58a6ff;text-decoration:none;font-size:14px}"
      ".mapbtn{display:inline-block;margin-top:12px;padding:8px 14px;border-radius:8px;text-decoration:none;"
      "background:#1f6feb;color:#fff;font-size:13px;font-weight:600}"
      ".mapbtn:disabled{background:#21262d;color:#8b949e;cursor:not-allowed}"
      "</style></head><body>"
      "<div class=top><h1>📡 GNSS (u-blox NEO-8M)</h1><span><a href=/vehicle>🚗 Fahrzeug</a> · <a href=/log>📋 Live-Log</a></span></div>"
      "<div class=card id=conn>…</div>"
      "<div class=card><div class=grid id=pos></div>"
      "<a id=mapbtn class=mapbtn href=# onclick='return openMap()' style='display:none'>🗺 In Google Maps öffnen</a></div>"
      "<div class=card><b>Fix</b> <div class=grid id=fix></div></div>"
      "<div class=card><b>Dilution of Precision (DOP)</b><div class=grid id=dop></div></div>"
      "<div class=card><b>Uhrzeit (UTC)</b><div class=grid id=time></div></div>"
      "<div class=card><b>UART / Verbindung</b><span class=muted> GPIO16 RX · GPIO17 TX · 9600</span>"
      "<div class=grid id=uart></div></div>"
      "<div class=card><b>Satelliten in Sicht</b> <span class=muted id=satcount></span>"
      "<table><thead><tr><th>PRN</th><th>Elev</th><th>Azim</th><th>SNR</th></tr></thead>"
      "<tbody id=satrows></tbody></table></div>"
      "<div class=card class=muted>Letzte gültige Satz: <span id=last></span> · "
      "Sätze gepart: <span id=parsed></span> · Checksum-Fehler: <span id=csum></span></div>"
      "<script>"
      "function fmtAgo(t){if(t<0)return 'nie';if(t<60)return t+'s';if(t<3600)return Math.floor(t/60)+'m';return Math.floor(t/3600)+'h'}"
      "async function load(){let r=await fetch('/api/gps');let s=await r.json();"
      "function fmtLt(v){if(v>0)return v.toFixed(6)+'° N';if(v<0)return (-v).toFixed(6)+'° S';return '—'}"
      "function fmtLn(v){if(v>0)return v.toFixed(6)+'° O';if(v<0)return (-v).toFixed(6)+'° W';return '—'}"
      "let _lat=0,_lon=0;"
      "function openMap(){if(!_lat&&!_lon)return false;var w=window.open('https://www.google.com/maps?q='+_lat+','+_lon,'_blank');if(!w)return false;return true;}"
      "let conn=document.getElementById('conn');"
      "if(s.activeFix){conn.className='card st-ok';conn.innerHTML='<span class=ok>GPS aktiv · Fix ✓ · '+s.satellitesInUse+' Satelliten</span>'}"
      "else if(s.bytesReceived>0){conn.className='card st-warn';conn.innerHTML='<span class=warn>Daten empfangen, aber noch kein Fix · letzter Empfang vor '+fmtAgo(s.lastByteAgeSec)+'</span>'}"
      "else{conn.className='card st-bad';conn.innerHTML='<span class=bad>⚠ Keine GPS-Daten · prüfe Verkabelung/Strom (GPIO16/17 @9600)</span>'}"
      "document.getElementById('pos').innerHTML="
      "kv('Breite',fmtLt(s.latitude))+kv('Länge',fmtLn(s.longitude))+"
      "kv('Höhe MSL',s.altitudeMsl!=0?Math.round(s.altitudeMsl)+' m':'—')+"
      "kv('Geoide',s.geoidSeparation!=0?Math.round(s.geoidSeparation)+' m':'—')+"
      "kv('Geschw.',s.speedKmh!=0?s.speedKmh.toFixed(1)+' km/h':'—')+"
      "kv('Kurs',s.courseDeg!=0?Math.round(s.courseDeg)+'°':'—');"
      "_lat=s.latitude||0;_lon=s.longitude||0;"
      "document.getElementById('mapbtn').style.display=(_lat||_lon)?'inline-block':'none';"
      "const fix=s.activeFix?`<span class=fix-ok>Fix ✓</span>`:`<span class=fix-no>kein Fix</span>`;"
      "document.getElementById('fix').innerHTML=kv('Status',fix)+"
      "kv('Fix-Qualität',s.fixQuality)+kv('Fix-Modus',s.fixMode)+"
      "kv('Sat. genutzt',s.satellitesInUse)+kv('Sat. in Sicht',s.satellitesInView);"
      "document.getElementById('dop').innerHTML=kv('HDOP',s.hdop)+kv('VDOP',s.vdop)+kv('PDOP',s.pdop);"
      "document.getElementById('time').innerHTML="
      "kv('UTC',(s.utcHour<10?'0':'')+s.utcHour+':'+(s.utcMin<10?'0':'')+s.utcMin+':'+(s.utcSec<10?'0':'')+s.utcSec)+"
      "kv('Datum',s.dateValid?s.utcDay+'.'+s.utcMonth+'.'+s.utcYear:'—');"
      "document.getElementById('uart').innerHTML="
      "kv('Bytes empfangen',s.bytesReceived)+kv('Zeilen (NMEA)',s.linesReceived)+"
      "kv('Sätze gepart',s.sentencesParsed)+kv('Checksum-Fehler',s.checksumErrors)+"
      "kv('Letzter Empfang vor',fmtAgo(s.lastByteAgeSec));"
      "document.getElementById('satcount').textContent='('+s.satellitesInView+')';"
      "let rows='';s.satellites.forEach(x=>{"
      "let snr=x.snr||0;let sw=Math.min(100,Math.max(0,(snr/50)*100));"
      "let bg=snr>=30?'ok':snr>=20?'warn':'bad';"
      "rows+='<tr><td>'+x.prn+'</td><td>'+x.elevation+'°</td><td>'+x.azimuth+'°</td>'"
      "+'<td><span class=bar><i style=\"width:'+sw+'%\"></i></span>'+(snr?snr+' <span class='+bg+'>dB</span>':'—')+'</td></tr>'});"
      "document.getElementById('satrows').innerHTML=rows;"
      "document.getElementById('last').textContent=fmtAgo(s.dataAgeSec);"
      "document.getElementById('parsed').textContent=s.sentencesParsed;"
      "document.getElementById('csum').textContent=s.checksumErrors;"
      "}function kv(k,v){return '<div class=kv><b>'+k+'</b><span>'+v+'</span></div>'}"
      "load();setInterval(load,2500);"
      "</script></body></html>";
    AsyncWebServerResponse* r = req->beginResponse(200, "text/html", html);
    req->send(r);
  });

  // GET /vehicle -> all ELM327/OBD live values (+ GPS UTC clock).
  server_.on("/vehicle", HTTP_GET, [](AsyncWebServerRequest* req) {
    const char* html =
      "<!DOCTYPE html><html lang=\"de\"><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<title>Fahrzeug</title><style>"
      "body{font-family:system-ui,sans-serif;background:#0b0f14;color:#e6edf3;margin:0;padding:16px}"
      "h1{font-size:20px;margin:0 0 14px}"
      ".card{background:#161b22;border:1px solid #30363d;border-radius:10px;padding:14px;margin-bottom:12px}"
      ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px}"
      ".kv{display:flex;justify-content:space-between;border-bottom:1px solid #21262d;padding:4px 0}"
      ".kv b{color:#8b949e;font-weight:500}"
      ".clock{font-size:44px;font-weight:700;text-align:center;color:#58a6ff;margin-bottom:4px}"
      ".date{font-size:14px;text-align:center;color:#8b949e}"
      ".top{display:flex;justify-content:space-between;align-items:center;margin-bottom:14px}"
      ".top a{color:#58a6ff;text-decoration:none;font-size:14px}"
      "table{width:100%;border-collapse:collapse;font-size:13px}"
      "th,td{text-align:left;padding:4px 8px;border-bottom:1px solid #21262d}"
      ".muted{color:#8b949e;font-size:12px}"
      "</style></head><body>"
      "<div class=top><h1>🚗 Fahrzeug · OBD</h1><span><a href=/gps>📡 GPS</a> · <a href=/log>📋 Live-Log</a></span></div>"
      "<div class=card><div class=clock id=clock>--:--:--</div>"
      "<div class=date id=date>UTC · wartet auf GPS-Zeit …</div></div>"
      "<div class=card><b>Motor</b><div class=grid id=motor></div></div>"
      "<div class=card><b>Fahrzeug</b><div class=grid id=vehicle></div></div>"
      "<div class=card><b>Rohdaten (Mode 01 · OBD-Bytes)</b>"
      "<table><thead><tr><th>PID</th><th>Name</th><th>Raw-Wert</th><th>→ Dezimal</th></tr></thead>"
      "<tbody id=rawrows></tbody></table></div>"
      "<div class=card class=muted>Laufzeit seit Motorstart: <span id=runtime></span></div>"
      "<script>"
      "let _ch=0,_cm=0,_cs=0,_t0=0;"
      "function fmt2(n){return (n<10?'0':'')+n}"
      "function hhmmss(sec){sec=Math.floor(sec)%86400;return fmt2(Math.floor(sec/3600))+':'+fmt2(Math.floor(sec%3600/60))+':'+fmt2(sec%60)}"
      "async function loadV(){let r=await fetch('/api/vehicle');let v=await r.json();let p=document.getElementById('motor');"
      "p.innerHTML=kv('Drehzahl',v.rpm+' U/min')+kv('Kühlmitteltemp.',v.coolantTemperature.toFixed(1)+' °C')+"
      "kv('Zündwinkel',v.ignitionAdvanceDeg.toFixed(1)+' °')+"
      "kv('Tankinhalt',v.fuelPercent.toFixed(0)+' %');"
      "let q=document.getElementById('vehicle');"
      "q.innerHTML=kv('Geschwindigkeit',v.speedKmh.toFixed(1)+' km/h');"
      "let rows='';function row(pid,name,raw){rows+='<tr><td>'+pid+'</td><td>'+name+'</td><td>'+raw+'</td><td>'+parseInt(raw,16)+'</td></tr>'}"
      "row('01','Monitors','00 00 00 00');row('04','Load','00');row('05','Kühlmitteltemp.',v.obd.coolantRaw.toString(16).padStart(2,'0'));"
      "row('0C','Drehzahl',v.obd.rpmRaw.toString(16).padStart(4,'0'));row('0D','Geschw.',v.obd.speedRaw.toString(16).padStart(2,'0'));"
      "row('0E','Zündwinkel',v.obd.ignitionAdvanceRaw.toString(16).padStart(2,'0'));row('11','Drosselklappe','00');"
      "row('2F','Tankinhalt',v.obd.fuelRaw.toString(16).padStart(2,'0'));row('4D','Laufzeit',v.obd.runtimeMinutes.toString(16).padStart(4,'0'));"
      "document.getElementById('rawrows').innerHTML=rows;"
      "document.getElementById('runtime').textContent=hhmmss(v.runtimeSec);"
      "}"
      "async function loadClock(){let r=await fetch('/api/gps');let s=await r.json();"
      "if(s.dateValid){_ch=s.utcHour;_cm=s.utcMin;_cs=s.utcSec;_t0=Date.now();"
      "document.getElementById('date').textContent='UTC · '+s.utcDay+'.'+s.utcMonth+'.'+s.utcYear;}"
      "else{document.getElementById('date').textContent='UTC · wartet auf GPS-Zeit …'}"
      "}"
      "function tickClock(){if(!_t0){return}"
      "document.getElementById('clock').textContent=hhmmss((_ch*3600+_cm*60+_cs)+Math.floor((Date.now()-_t0)/1000))}"
      "function kv(k,v){return '<div class=kv><b>'+k+'</b><span>'+v+'</span></div>'}"
      "loadV();setInterval(loadV,2500);"
      "loadClock();setInterval(loadClock,10000);"
      "setInterval(tickClock,1000);"
      "</script></body></html>";
    AsyncWebServerResponse* r = req->beginResponse(200, "text/html", html);
    req->send(r);
  });

  setupLogTail();

  server_.begin();
  LOG.printf("[WEB] GPS server on http://%s/gps\n", WiFi.softAPIP().toString().c_str());
}

void GpsWebServer::setupLogTail() {
  // GET /api/log -> last captured debug lines as JSON text (web tail without serial).
  if (!logEvents_) logEvents_ = new AsyncEventSource("/api/log/events");
  if (!logEvents_) {
    LOG.println(F("[WEB] Failed to create log event source"));
    return;
  }
  logEvents_->onConnect([](AsyncEventSourceClient* client) {
    LOG.println(F("[WEB] Log SSE client connected"));
    char buf[8192];
    LogTail::instance().dump(buf, sizeof(buf));
    client->send(buf, "log", millis());
  });
  server_.addHandler(logEvents_);

  server_.on("/api/log", HTTP_GET, [](AsyncWebServerRequest* req) {
    char buf[8192];
    size_t n = LogTail::instance().dump(buf, sizeof(buf));
    // JSON-encode so the browser can display control chars / quotes safely.
    String json = "\"";
    for (size_t i = 0; i < n; i++) {
      char c = buf[i];
      if (c == '"') json += "\\\"";
      else if (c == '\\') json += "\\\\";
      else if (c == '\n') json += "\\n";
      else if (c == '\r') { /* skip */ }
      else if ((unsigned char)c < 0x20) json += '?';
      else json += c;
    }
    json += "\"";
    AsyncWebServerResponse* r = req->beginResponse(200, "application/json", json);
    r->addHeader("Cache-Control", "no-store");
    req->send(r);
  });

  // GET /log -> self-contained web log tail page (no serial, no LittleFS).
  server_.on("/log", HTTP_GET, [](AsyncWebServerRequest* req) {
    const char* html =
      "<!DOCTYPE html><html lang=\"de\"><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<title>Motor-Node · Log</title>"
      "<style>body{font-family:ui-monospace,Consolas,monospace;background:#0d1117;color:#c9d1d9;margin:0;}"
      "header{padding:10px 14px;background:#161b22;border-bottom:1px solid #30363d;"
      "font-weight:bold;position:sticky;top:0;display:flex;gap:10px;align-items:center;}"
      "header span{font-size:12px;color:#8b949e;font-weight:normal;}"
      "button{background:#21262d;color:#c9d1d9;border:1px solid #30363d;border-radius:6px;padding:4px 10px;cursor:pointer;}"
      "pre{white-space:pre-wrap;word-break:break-all;padding:10px 14px;margin:0;font-size:12.5px;line-height:1.5;}"
      "</style></head><body>"
      "<header>📋 Live-Log <span>tail -f · ohne serielle Verbindung</span> "
      "<a href=/gps style=margin-left:auto;color:#58a6ff;font-size:12px>← GPS</a> "
      "<span id=dim style=color:#56d364>● verbunden</span></header>"
      "<pre id=log></pre>"
      "<script>"
      "const pre=document.getElementById('log');"
      "const es=new EventSource('/api/log/events');"
      "es.addEventListener('log',e=>{pre.textContent+=e.data;if(pre.textContent.length>80000)pre.textContent=pre.textContent.slice(-60000);window.scrollTo(0,document.body.scrollHeight);});"
      "es.onerror=()=>document.getElementById('dim').textContent='getrennt · Verbinde neu …';"
      "</script></body></html>";
    AsyncWebServerResponse* r = req->beginResponse(200, "text/html", html);
    req->send(r);
  });
}