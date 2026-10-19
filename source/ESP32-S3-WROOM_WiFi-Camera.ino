/*
 * ESP32-S3 WROOM Freenove CAM  ->  Telecamera IP in LAN
 * ------------------------------------------------------
 *  - Streaming MJPEG in tempo reale
 *  - Pagina web di controllo (risoluzione, qualità, luminosità, specchio...)
 *  - Foto e registrazione video su microSD (galleria con download / elimina)
 *  - Indirizzo IP e porte stampati sulla console seriale all'avvio
 *
 * IMPOSTAZIONI ARDUINO IDE (core esp32 by Espressif, v2.0.17 o 3.x):
 *   Board            : ESP32S3 Dev Module
 *   PSRAM            : OPI PSRAM
 *   Flash Size       : 8MB (64Mb)   [N8R8]
 *   Partition Scheme : 8M with spiffs (3MB APP/1.5MB SPIFFS)
 *   USB CDC On Boot  : Disabled (se usi la porta UART della Freenove, come di default)
 *                      Enabled solo se colleghi la porta USB nativa
 *   Upload Speed     : 921600
 *
 * Inserisci SSID e password qui sotto, carica e apri il Serial Monitor a 115200.
 */

#include "esp_camera.h"
#include "img_converters.h"
#include <WiFi.h>
#include "esp_http_server.h"
#include "FS.h"
#include "SD_MMC.h"
#include <time.h>

// ====================== CONFIGURAZIONE UTENTE ======================
const char* WIFI_SSID = "IL_TUO_WIFI";
const char* WIFI_PASS = "LA_TUA_PASSWORD";

#define WEB_PORT     8080            // Porta pagina web + API
#define STREAM_PORT  (WEB_PORT + 1)  // Porta del video MJPEG (8081)

const char* NTP_SERVER = "pool.ntp.org";
const char* TIMEZONE   = "CET-1CEST,M3.5.0,M10.5.0/3"; // Europa Centrale (IT/NL)
// ===================================================================

// ---- Pin camera Freenove ESP32-S3-WROOM CAM ----
#define PWDN_GPIO_NUM   -1
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM   15
#define SIOD_GPIO_NUM    4
#define SIOC_GPIO_NUM    5
#define Y9_GPIO_NUM     16
#define Y8_GPIO_NUM     17
#define Y7_GPIO_NUM     18
#define Y6_GPIO_NUM     12
#define Y5_GPIO_NUM     10
#define Y4_GPIO_NUM      8
#define Y3_GPIO_NUM      9
#define Y2_GPIO_NUM     11
#define VSYNC_GPIO_NUM   6
#define HREF_GPIO_NUM    7
#define PCLK_GPIO_NUM   13

// ---- Pin microSD (modalità SD_MMC 1-bit) ----
#define SD_CLK_PIN  39
#define SD_CMD_PIN  38
#define SD_D0_PIN   40

#define PHOTO_DIR "/photos"
#define VIDEO_DIR "/video"

static httpd_handle_t web_httpd    = NULL;
static httpd_handle_t stream_httpd = NULL;

static bool sdOk = false;
static volatile bool recWanted = false;
static volatile bool recActive = false;
static String recName = "";
static volatile int streamClients = 0;

// ---- Gestione sensori senza JPEG hardware (RGB565 -> JPEG software) ----
// ---- Log su UART *e* USB nativa (indipendente da "USB CDC On Boot") ----
class TeeLog : public Print {
 public:
  void begin(unsigned long baud) {
    Serial.begin(baud);
#if ARDUINO_USB_CDC_ON_BOOT
    Serial0.begin(baud);
#endif
  }
  size_t write(uint8_t c) override {
    Serial.write(c);
#if ARDUINO_USB_CDC_ON_BOOT
    Serial0.write(c);
#endif
    return 1;
  }
  size_t write(const uint8_t *b, size_t n) override {
    Serial.write(b, n);
#if ARDUINO_USB_CDC_ON_BOOT
    Serial0.write(b, n);
#endif
    return n;
  }
};
static TeeLog Log;

// NOTA: la struct deve stare PRIMA di ogni funzione (prototipi auto-generati dall'IDE)
struct Frame { camera_fb_t *fb; uint8_t *buf; size_t len; };

static bool softJpeg = false;
static int  softQuality = 85;     // scala 0-100 (più alto = meglio)

static void setSoftQuality(int q) {          // q = scala sensore 4..63 (più basso = meglio)
  softQuality = constrain(map(q, 4, 63, 95, 25), 25, 95);
}


static bool frameGet(Frame &f) {
  f.buf = NULL; f.len = 0;
  f.fb = esp_camera_fb_get();
  if (!f.fb) return false;
  if (!softJpeg) { f.buf = f.fb->buf; f.len = f.fb->len; return true; }
  uint8_t *out = NULL; size_t outLen = 0;
  bool ok = frame2jpg(f.fb, softQuality, &out, &outLen);
  esp_camera_fb_return(f.fb);
  f.fb = NULL;
  if (!ok || !out) return false;
  f.buf = out; f.len = outLen;
  return true;
}

static void frameFree(Frame &f) {
  if (f.fb) esp_camera_fb_return(f.fb);
  else if (softJpeg && f.buf) free(f.buf);
  f.fb = NULL; f.buf = NULL; f.len = 0;
}

static String sensorName(sensor_t *s) {
  switch (s->id.PID) {
    case OV2640_PID: return "OV2640";
    case OV5640_PID: return "OV5640";
    case OV3660_PID: return "OV3660";
    default: { char b[20]; snprintf(b, sizeof(b), "PID 0x%04X", s->id.PID); return String(b); }
  }
}

// ===================================================================
//                          PAGINA HTML
// ===================================================================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="it">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32-S3 Cam</title>
<style>
:root{--bg:#0f1318;--card:#1a2029;--line:#2a3340;--txt:#e6eaf0;--mut:#8b97a7;--acc:#2f9bff;--red:#e5484d;--ok:#30a46c}
@media (prefers-color-scheme: light){:root{--bg:#eef1f5;--card:#fff;--line:#d5dbe3;--txt:#18202a;--mut:#5d6b7c}}
*{box-sizing:border-box}
body{margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--txt)}
header{display:flex;align-items:center;justify-content:space-between;padding:12px 18px;border-bottom:1px solid var(--line);background:var(--card);position:sticky;top:0;z-index:5}
header h1{font-size:1.05rem;margin:0;display:flex;align-items:center;gap:10px}
.dot{width:10px;height:10px;border-radius:50%;background:var(--mut)}
.dot.live{background:var(--ok);box-shadow:0 0 8px var(--ok)}
.dot.rec{background:var(--red);box-shadow:0 0 8px var(--red);animation:b 1s infinite}
@keyframes b{50%{opacity:.3}}
main{display:grid;grid-template-columns:minmax(0,1fr) 320px;gap:16px;padding:16px;max-width:1400px;margin:auto}
@media(max-width:900px){main{grid-template-columns:1fr}}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px}
.card h2{font-size:.8rem;text-transform:uppercase;letter-spacing:.08em;color:var(--mut);margin:0 0 12px}
#view{background:#000;border-radius:10px;overflow:hidden;display:flex;justify-content:center;align-items:center;min-height:240px}
#view img{width:100%;height:auto;display:block;max-height:80vh;object-fit:contain}
.bar{display:flex;flex-wrap:wrap;gap:8px;margin-top:12px}
button,.btn{background:var(--bg);color:var(--txt);border:1px solid var(--line);padding:9px 14px;border-radius:8px;cursor:pointer;font-size:.9rem;text-decoration:none;display:inline-flex;align-items:center;gap:6px}
button:hover,.btn:hover{border-color:var(--acc)}
button.pri{background:var(--acc);border-color:var(--acc);color:#fff}
button.red{background:var(--red);border-color:var(--red);color:#fff}
.field{margin-bottom:12px}
.field label{display:flex;justify-content:space-between;font-size:.85rem;color:var(--mut);margin-bottom:5px}
select,input[type=range]{width:100%}
select{background:var(--bg);color:var(--txt);border:1px solid var(--line);padding:8px;border-radius:8px}
.chk{display:flex;gap:16px}
.chk label{display:flex;gap:6px;align-items:center;font-size:.9rem;color:var(--txt)}
.info{font-size:.82rem;color:var(--mut);line-height:1.7}
.info b{color:var(--txt);font-weight:600}
.bartrack{height:6px;background:var(--line);border-radius:3px;overflow:hidden;margin:6px 0}
.bartrack div{height:100%;background:var(--acc);width:0}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:10px}
.item{border:1px solid var(--line);border-radius:10px;overflow:hidden;background:var(--bg)}
.item img{width:100%;aspect-ratio:4/3;object-fit:cover;display:block;cursor:pointer;background:#000}
.item .meta{font-size:.72rem;color:var(--mut);padding:6px 8px;word-break:break-all}
.item .row{display:flex;gap:6px;padding:0 8px 8px}
.item .row a,.item .row button{flex:1;justify-content:center;padding:6px;font-size:.8rem}
.vid{display:flex;justify-content:space-between;align-items:center;gap:8px;padding:8px 10px;border:1px solid var(--line);border-radius:8px;margin-bottom:8px;font-size:.82rem;word-break:break-all}
.muted{color:var(--mut);font-size:.85rem}
#modal{position:fixed;inset:0;background:rgba(0,0,0,.88);display:none;align-items:center;justify-content:center;z-index:20}
#modal img{max-width:95vw;max-height:92vh}
.full{grid-column:1/-1}
#toast{position:fixed;bottom:18px;left:50%;transform:translateX(-50%);background:var(--card);border:1px solid var(--line);padding:10px 18px;border-radius:10px;display:none;z-index:30}
</style>
</head>
<body>
<header>
  <h1><span id="dot" class="dot"></span> ESP32-S3 Camera LAN</h1>
  <span class="muted" id="hdrinfo">connessione…</span>
</header>

<main>
  <section>
    <div class="card">
      <div id="view"><img id="stream" alt="stream"></div>
      <div class="bar">
        <button class="pri" onclick="toggleStream()" id="btnStream">⏸ Pausa</button>
        <button onclick="savePhoto()">📸 Salva foto su SD</button>
        <a class="btn" href="/capture?dl=1">⬇ Scarica scatto</a>
        <button id="btnRec" onclick="toggleRec()">⏺ Registra su SD</button>
        <button onclick="fs()">⛶ Schermo intero</button>
      </div>
    </div>

    <div class="card" style="margin-top:16px">
      <h2>Foto su microSD</h2>
      <div id="photos" class="grid"></div>
    </div>

    <div class="card" style="margin-top:16px">
      <h2>Video su microSD (MJPEG, apribili con VLC)</h2>
      <div id="videos"></div>
    </div>
  </section>

  <aside>
    <div class="card">
      <h2>Impostazioni immagine</h2>
      <div class="field"><label>Risoluzione</label>
        <select id="framesize" onchange="ctl('framesize',this.value,true)">
          <option value="5">QVGA 320×240</option>
          <option value="6">CIF 400×296</option>
          <option value="8">VGA 640×480</option>
          <option value="9">SVGA 800×600</option>
          <option value="10">XGA 1024×768</option>
          <option value="11">HD 1280×720</option>
          <option value="12">SXGA 1280×1024</option>
          <option value="13">UXGA 1600×1200</option>
        </select></div>
      <div class="field"><label>Qualità JPEG <span id="v_quality"></span></label>
        <input type="range" id="quality" min="4" max="63" oninput="lbl(this)" onchange="ctl('quality',this.value)"></div>
      <div class="field"><label>Luminosità <span id="v_brightness"></span></label>
        <input type="range" id="brightness" min="-2" max="2" oninput="lbl(this)" onchange="ctl('brightness',this.value)"></div>
      <div class="field"><label>Contrasto <span id="v_contrast"></span></label>
        <input type="range" id="contrast" min="-2" max="2" oninput="lbl(this)" onchange="ctl('contrast',this.value)"></div>
      <div class="field"><label>Saturazione <span id="v_saturation"></span></label>
        <input type="range" id="saturation" min="-2" max="2" oninput="lbl(this)" onchange="ctl('saturation',this.value)"></div>
      <div class="chk">
        <label><input type="checkbox" id="hmirror" onchange="ctl('hmirror',this.checked?1:0)"> Specchio</label>
        <label><input type="checkbox" id="vflip" onchange="ctl('vflip',this.checked?1:0)"> Capovolgi</label>
      </div>
      <p class="muted" style="margin-bottom:0">Qualità: numero più basso = immagine migliore ma più pesante.</p>
    </div>

    <div class="card" style="margin-top:16px">
      <h2>Stato</h2>
      <div class="info" id="status">—</div>
      <div class="bartrack"><div id="sdbar"></div></div>
      <div class="info" id="sdtxt"></div>
    </div>

    <div class="card" style="margin-top:16px">
      <h2>Link diretti</h2>
      <div class="info">
        Stream: <b id="lnkStream"></b><br>
        Foto: <b id="lnkCap"></b>
      </div>
    </div>
  </aside>
</main>

<div id="modal" onclick="this.style.display='none'"><img id="modalimg"></div>
<div id="toast"></div>

<script>
const $=id=>document.getElementById(id);
const STREAM_PORT="%STREAM_PORT%";
const streamUrl=()=>`http://${location.hostname}:${STREAM_PORT}/stream`;
let streaming=true, recOn=false;

function toast(t){const e=$('toast');e.textContent=t;e.style.display='block';clearTimeout(e._t);e._t=setTimeout(()=>e.style.display='none',2500)}
function lbl(el){$('v_'+el.id).textContent=el.value}
function fmt(b){if(b<1024)return b+' B';if(b<1048576)return (b/1024).toFixed(1)+' KB';return (b/1048576).toFixed(1)+' MB'}

function startStream(){ $('stream').src=streamUrl()+'?t='+Date.now(); streaming=true; $('btnStream').textContent='⏸ Pausa'; $('dot').classList.add('live') }
function stopStream(){ $('stream').removeAttribute('src'); streaming=false; $('btnStream').textContent='▶ Avvia'; $('dot').classList.remove('live') }
function toggleStream(){ streaming?stopStream():startStream() }
$('stream').onerror=()=>{ if(streaming) setTimeout(startStream,2000) };

async function ctl(v,val,restart){
  try{ await fetch(`/control?var=${v}&val=${val}`); if(restart&&streaming){ setTimeout(startStream,400) } }
  catch(e){ toast('Errore comando') }
}
function fs(){ const v=$('view'); (v.requestFullscreen||v.webkitRequestFullscreen).call(v) }

async function savePhoto(){
  const r=await fetch('/save'); const j=await r.json();
  toast(j.ok?('Salvata: '+j.name):('Errore: '+j.err)); loadFiles();
}
async function toggleRec(){
  const r=await fetch('/rec?on='+(recOn?0:1)); const j=await r.json();
  if(!j.ok) toast('Errore: '+j.err); else toast(j.rec?'Registrazione avviata':'Registrazione fermata');
  setTimeout(()=>{loadStatus();loadFiles()},600);
}
async function del(t,n){
  if(!confirm('Eliminare '+n+'?')) return;
  await fetch(`/delete?t=${t}&n=${n}`); loadFiles(); loadStatus();
}
function openImg(n){ $('modalimg').src=`/file?t=p&n=${n}`; $('modal').style.display='flex' }

async function loadFiles(){
  try{
    const d=await (await fetch('/files')).json();
    if(!d.sd){ $('photos').innerHTML='<p class="muted">microSD non disponibile</p>'; $('videos').innerHTML=''; return }
    $('photos').innerHTML=d.photos.map(f=>`<div class="item"><img loading="lazy" src="/file?t=p&n=${f.n}" onclick="openImg('${f.n}')"><div class="meta">${f.n}<br>${fmt(f.s)}</div><div class="row"><a class="btn" href="/file?t=p&n=${f.n}&dl=1">⬇</a><button onclick="del('p','${f.n}')">🗑</button></div></div>`).join('')||'<p class="muted">Nessuna foto</p>';
    $('videos').innerHTML=d.videos.map(f=>`<div class="vid"><span>${f.n}<br><span class="muted">${fmt(f.s)}</span></span><span style="display:flex;gap:6px"><a class="btn" href="/file?t=v&n=${f.n}&dl=1">⬇</a><button onclick="del('v','${f.n}')">🗑</button></span></div>`).join('')||'<p class="muted">Nessun video</p>';
  }catch(e){}
}

let firstStatus=true;
async function loadStatus(){
  try{
    const s=await (await fetch('/status')).json();
    if(firstStatus){
      ['framesize','quality','brightness','contrast','saturation'].forEach(k=>{$(k).value=s[k]; if($('v_'+k)) lbl($(k))});
      $('hmirror').checked=!!s.hmirror; $('vflip').checked=!!s.vflip; firstStatus=false;
    }
    if(s.soft){ [...$('framesize').options].forEach(o=>{ if(+o.value>9) o.disabled=true }) }
    recOn=!!s.rec;
    $('btnRec').textContent=recOn?'⏹ Ferma registrazione':'⏺ Registra su SD';
    $('btnRec').className=recOn?'red':'';
    $('dot').className='dot'+(recOn?' rec':(streaming?' live':''));
    $('hdrinfo').textContent=`${location.hostname} · Wi-Fi ${s.rssi} dBm`;
    $('status').innerHTML=`Uptime: <b>${Math.floor(s.uptime/3600)}h ${Math.floor(s.uptime%3600/60)}m</b><br>Client stream: <b>${s.clients}</b><br>RAM libera: <b>${fmt(s.heap)}</b> · PSRAM: <b>${fmt(s.psram)}</b><br>Sensore: <b>${s.sensor}</b>`+(recOn?`<br>Registrando: <b>${s.recname}</b>`:'');
    if(s.sd){ const p=s.sdtotal?Math.round(100*s.sdused/s.sdtotal):0; $('sdbar').style.width=p+'%'; $('sdtxt').textContent=`microSD: ${fmt(s.sdused)} usati su ${fmt(s.sdtotal)} (${p}%)` }
    else { $('sdbar').style.width='0'; $('sdtxt').textContent='microSD non presente / non montata' }
  }catch(e){ $('hdrinfo').textContent='offline' }
}

$('lnkStream').textContent=streamUrl();
$('lnkCap').textContent=`http://${location.host}/capture`;
startStream(); loadStatus(); loadFiles();
setInterval(loadStatus,3000);
</script>
</body>
</html>
)rawliteral";

// ===================================================================
//                          FUNZIONI DI SUPPORTO
// ===================================================================
static bool getParam(httpd_req_t *req, const char *key, char *out, size_t len) {
  size_t ql = httpd_req_get_url_query_len(req) + 1;
  if (ql <= 1) return false;
  char *q = (char *)malloc(ql);
  if (!q) return false;
  bool ok = (httpd_req_get_url_query_str(req, q, ql) == ESP_OK) &&
            (httpd_query_key_value(q, key, out, len) == ESP_OK);
  free(q);
  return ok;
}

static bool validName(const char *n) {
  size_t l = strlen(n);
  if (l == 0 || l > 48) return false;
  if (strstr(n, "..")) return false;
  for (size_t i = 0; i < l; i++) {
    char c = n[i];
    if (!(isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.')) return false;
  }
  return true;
}

static String timeStamp() {
  struct tm t;
  if (getLocalTime(&t, 10) && t.tm_year > 120) {
    char b[24];
    strftime(b, sizeof(b), "%Y%m%d_%H%M%S", &t);
    return String(b);
  }
  return String(millis());
}

static String uniquePath(const char *dir, const char *prefix, const char *ext, String &nameOut) {
  String stamp = timeStamp();
  String name = String(prefix) + stamp + ext;
  String path = String(dir) + "/" + name;
  int n = 1;
  while (SD_MMC.exists(path)) {
    name = String(prefix) + stamp + "_" + String(n++) + ext;
    path = String(dir) + "/" + name;
  }
  nameOut = name;
  return path;
}

static const char *mimeFor(const char *name) {
  if (strstr(name, ".jpg")) return "image/jpeg";
  if (strstr(name, ".mjpg")) return "video/x-motion-jpeg";
  return "application/octet-stream";
}

static String listDir(const char *dir, int maxItems) {
  String out = "[";
  File root = SD_MMC.open(dir);
  if (root && root.isDirectory()) {
    File f = root.openNextFile();
    int n = 0;
    while (f && n < maxItems) {
      if (!f.isDirectory()) {
        String nm = String(f.name());
        int sl = nm.lastIndexOf('/');
        if (sl >= 0) nm = nm.substring(sl + 1);
        if (n++) out += ",";
        out += "{\"n\":\"" + nm + "\",\"s\":" + String((unsigned long)f.size()) + "}";
      }
      f = root.openNextFile();
    }
  }
  out += "]";
  return out;
}

static void sendJson(httpd_req_t *req, const String &s) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, s.c_str(), s.length());
}

// ===================================================================
//                              HANDLER
// ===================================================================
static esp_err_t index_handler(httpd_req_t *req) {
  String page = FPSTR(INDEX_HTML);
  page.replace("%STREAM_PORT%", String(STREAM_PORT));
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, page.c_str(), page.length());
}

// ---- Stream MJPEG ----
static esp_err_t stream_handler(httpd_req_t *req) {
  esp_err_t res = httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

  char part[64];
  streamClients++;
  while (true) {
    Frame fr;
    if (!frameGet(fr)) { res = ESP_FAIL; break; }

    res = httpd_resp_send_chunk(req, "\r\n--frame\r\n", 10);
    if (res == ESP_OK) {
      size_t hl = snprintf(part, sizeof(part),
                           "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", (unsigned)fr.len);
      res = httpd_resp_send_chunk(req, part, hl);
    }
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fr.buf, fr.len);
    frameFree(fr);
    if (res != ESP_OK) break;  // client disconnesso
  }
  streamClients--;
  return res;
}

// ---- Singolo scatto ----
static esp_err_t capture_handler(httpd_req_t *req) {
  Frame fr;
  if (!frameGet(fr)) { httpd_resp_send_500(req); return ESP_FAIL; }
  httpd_resp_set_type(req, "image/jpeg");
  char dl[8];
  if (getParam(req, "dl", dl, sizeof(dl)))
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=scatto.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  esp_err_t res = httpd_resp_send(req, (const char *)fr.buf, fr.len);
  frameFree(fr);
  return res;
}

// ---- Controllo parametri sensore ----
static esp_err_t control_handler(httpd_req_t *req) {
  char var[24], val[12];
  if (!getParam(req, "var", var, sizeof(var)) || !getParam(req, "val", val, sizeof(val))) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "parametri mancanti");
    return ESP_FAIL;
  }
  int v = atoi(val);
  sensor_t *s = esp_camera_sensor_get();
  int r = 0;
  if      (!strcmp(var, "framesize"))  r = (softJpeg && v > 9) ? -1 : s->set_framesize(s, (framesize_t)v);
  else if (!strcmp(var, "quality"))  { r = s->set_quality(s, v); setSoftQuality(v); }
  else if (!strcmp(var, "brightness")) r = s->set_brightness(s, v);
  else if (!strcmp(var, "contrast"))   r = s->set_contrast(s, v);
  else if (!strcmp(var, "saturation")) r = s->set_saturation(s, v);
  else if (!strcmp(var, "hmirror"))    r = s->set_hmirror(s, v);
  else if (!strcmp(var, "vflip"))      r = s->set_vflip(s, v);
  else r = -1;
  if (r < 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "comando non valido"); return ESP_FAIL; }
  sendJson(req, "{\"ok\":true}");
  return ESP_OK;
}

// ---- Stato ----
static esp_err_t status_handler(httpd_req_t *req) {
  sensor_t *s = esp_camera_sensor_get();
  String j = "{";
  j += "\"framesize\":" + String(s->status.framesize);
  j += ",\"quality\":" + String(s->status.quality);
  j += ",\"brightness\":" + String(s->status.brightness);
  j += ",\"contrast\":" + String(s->status.contrast);
  j += ",\"saturation\":" + String(s->status.saturation);
  j += ",\"hmirror\":" + String(s->status.hmirror);
  j += ",\"vflip\":" + String(s->status.vflip);
  j += ",\"sensor\":\"" + sensorName(s) + (softJpeg ? " (JPEG software)" : "") + "\"";
  j += ",\"soft\":" + String(softJpeg ? 1 : 0);
  j += ",\"rssi\":" + String(WiFi.RSSI());
  j += ",\"uptime\":" + String((unsigned long)(millis() / 1000));
  j += ",\"heap\":" + String((unsigned long)ESP.getFreeHeap());
  j += ",\"psram\":" + String((unsigned long)ESP.getFreePsram());
  j += ",\"clients\":" + String((int)streamClients);
  j += ",\"rec\":" + String(recActive ? 1 : 0);
  j += ",\"recname\":\"" + recName + "\"";
  j += ",\"sd\":" + String(sdOk ? 1 : 0);
  if (sdOk) {
    j += ",\"sdtotal\":" + String((unsigned long long)SD_MMC.totalBytes());
    j += ",\"sdused\":" + String((unsigned long long)SD_MMC.usedBytes());
  }
  j += "}";
  sendJson(req, j);
  return ESP_OK;
}

// ---- Salva foto su SD ----
static esp_err_t save_handler(httpd_req_t *req) {
  if (!sdOk) { sendJson(req, "{\"ok\":false,\"err\":\"microSD non disponibile\"}"); return ESP_OK; }
  Frame fr;
  if (!frameGet(fr)) { sendJson(req, "{\"ok\":false,\"err\":\"cattura fallita\"}"); return ESP_OK; }
  String name;
  String path = uniquePath(PHOTO_DIR, "IMG_", ".jpg", name);
  File f = SD_MMC.open(path, FILE_WRITE);
  bool ok = false;
  if (f) { ok = (f.write(fr.buf, fr.len) == fr.len); f.close(); }
  frameFree(fr);
  if (ok) sendJson(req, "{\"ok\":true,\"name\":\"" + name + "\"}");
  else    sendJson(req, "{\"ok\":false,\"err\":\"scrittura fallita\"}");
  return ESP_OK;
}

// ---- Elenco file ----
static esp_err_t files_handler(httpd_req_t *req) {
  if (!sdOk) { sendJson(req, "{\"sd\":0,\"photos\":[],\"videos\":[]}"); return ESP_OK; }
  String j = "{\"sd\":1,\"photos\":" + listDir(PHOTO_DIR, 60) + ",\"videos\":" + listDir(VIDEO_DIR, 40) + "}";
  sendJson(req, j);
  return ESP_OK;
}

// ---- Invio / download file ----
static esp_err_t file_handler(httpd_req_t *req) {
  char t[4], n[64], dl[4];
  if (!sdOk || !getParam(req, "t", t, sizeof(t)) || !getParam(req, "n", n, sizeof(n)) || !validName(n)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "richiesta non valida");
    return ESP_FAIL;
  }
  String path = String(t[0] == 'v' ? VIDEO_DIR : PHOTO_DIR) + "/" + n;
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) { httpd_resp_send_404(req); return ESP_FAIL; }

  httpd_resp_set_type(req, mimeFor(n));
  if (getParam(req, "dl", dl, sizeof(dl))) {
    String cd = String("attachment; filename=") + n;
    httpd_resp_set_hdr(req, "Content-Disposition", cd.c_str());
  }
  uint8_t *buf = (uint8_t *)malloc(4096);
  if (!buf) { f.close(); httpd_resp_send_500(req); return ESP_FAIL; }
  esp_err_t res = ESP_OK;
  size_t r;
  while ((r = f.read(buf, 4096)) > 0) {
    res = httpd_resp_send_chunk(req, (const char *)buf, r);
    if (res != ESP_OK) break;
  }
  if (res == ESP_OK) httpd_resp_send_chunk(req, NULL, 0);
  free(buf);
  f.close();
  return res;
}

// ---- Elimina file ----
static esp_err_t delete_handler(httpd_req_t *req) {
  char t[4], n[64];
  if (!sdOk || !getParam(req, "t", t, sizeof(t)) || !getParam(req, "n", n, sizeof(n)) || !validName(n)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "richiesta non valida");
    return ESP_FAIL;
  }
  String path = String(t[0] == 'v' ? VIDEO_DIR : PHOTO_DIR) + "/" + n;
  // Non cancellare il file in registrazione
  if (recActive && recName == String(n)) { sendJson(req, "{\"ok\":false}"); return ESP_OK; }
  bool ok = SD_MMC.remove(path);
  sendJson(req, String("{\"ok\":") + (ok ? "true" : "false") + "}");
  return ESP_OK;
}

// ---- Avvio / stop registrazione ----
static esp_err_t rec_handler(httpd_req_t *req) {
  char on[4];
  if (!getParam(req, "on", on, sizeof(on))) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "manca on"); return ESP_FAIL; }
  if (!sdOk) { sendJson(req, "{\"ok\":false,\"err\":\"microSD non disponibile\"}"); return ESP_OK; }
  recWanted = (on[0] == '1');
  sendJson(req, String("{\"ok\":true,\"rec\":") + (recWanted ? "true" : "false") + "}");
  return ESP_OK;
}

// ===================================================================
//                  TASK DI REGISTRAZIONE SU microSD
// ===================================================================
static void recTask(void *arg) {
  File f;
  uint32_t frames = 0;
  for (;;) {
    if (recWanted) {
      if (!recActive) {
        String name;
        String path = uniquePath(VIDEO_DIR, "REC_", ".mjpg", name);
        f = SD_MMC.open(path, FILE_WRITE);
        if (f) { recName = name; recActive = true; frames = 0; Log.printf("[REC] Avviata: %s\n", path.c_str()); }
        else   { recWanted = false; Log.println("[REC] Impossibile creare il file"); }
      }
      if (recActive) {
        Frame fr;
        if (frameGet(fr)) {
          size_t w = f.write(fr.buf, fr.len);
          frameFree(fr);
          frames++;
          // Ferma se scrittura fallita o spazio quasi finito (<8 MB)
          if (w == 0 || (SD_MMC.totalBytes() - SD_MMC.usedBytes()) < 8UL * 1024 * 1024) {
            Log.println("[REC] Stop: errore o SD piena");
            recWanted = false;
          }
          if ((frames % 100) == 0) f.flush();
        }
        vTaskDelay(1);
      }
    } else {
      if (recActive) {
        f.close();
        recActive = false;
        Log.printf("[REC] Fermata (%u frame)\n", (unsigned)frames);
        recName = "";
      }
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
}

// ===================================================================
//                           AVVIO SERVER
// ===================================================================
static void startServers() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = WEB_PORT;
  cfg.ctrl_port = 32768;
  cfg.max_uri_handlers = 16;
  cfg.stack_size = 8192;
  cfg.lru_purge_enable = true;

  httpd_uri_t uris[] = {
    {"/",        HTTP_GET, index_handler,   NULL},
    {"/capture", HTTP_GET, capture_handler, NULL},
    {"/control", HTTP_GET, control_handler, NULL},
    {"/status",  HTTP_GET, status_handler,  NULL},
    {"/save",    HTTP_GET, save_handler,    NULL},
    {"/files",   HTTP_GET, files_handler,   NULL},
    {"/file",    HTTP_GET, file_handler,    NULL},
    {"/delete",  HTTP_GET, delete_handler,  NULL},
    {"/rec",     HTTP_GET, rec_handler,     NULL},
  };

  if (httpd_start(&web_httpd, &cfg) == ESP_OK) {
    for (auto &u : uris) httpd_register_uri_handler(web_httpd, &u);
  } else {
    Log.println("ERRORE: avvio server web fallito");
  }

  cfg.server_port = STREAM_PORT;
  cfg.ctrl_port = 32769;
  httpd_uri_t stream_uri = {"/stream", HTTP_GET, stream_handler, NULL};
  if (httpd_start(&stream_httpd, &cfg) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  } else {
    Log.println("ERRORE: avvio server stream fallito");
  }
}

// ===================================================================
//                               SETUP
// ===================================================================
static void fillConfig(camera_config_t &c, bool soft) {
  memset(&c, 0, sizeof(c));
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM;  c.pin_d1 = Y3_GPIO_NUM;
  c.pin_d2 = Y4_GPIO_NUM;  c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;  c.pin_d5 = Y7_GPIO_NUM;
  c.pin_d6 = Y8_GPIO_NUM;  c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM;
  c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM;
  c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM;
  c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM;
  c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = soft ? PIXFORMAT_RGB565 : PIXFORMAT_JPEG;

  if (psramFound()) {
    c.frame_size = FRAMESIZE_SVGA;     // in modalità software è anche il massimo consentito
    c.jpeg_quality = 12;
    c.fb_count = 2;
    c.fb_location = CAMERA_FB_IN_PSRAM;
    c.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    Log.println("ATTENZIONE: PSRAM non trovata (imposta PSRAM = OPI PSRAM)");
    c.frame_size = FRAMESIZE_QVGA;
    c.jpeg_quality = 15;
    c.fb_count = 1;
    c.fb_location = CAMERA_FB_IN_DRAM;
    c.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  }
}

static bool initCamera() {
  camera_config_t c;
  fillConfig(c, false);
  esp_err_t err = esp_camera_init(&c);

  if (err != ESP_OK) {
    Log.printf("Init JPEG fallita (0x%x): provo RGB565 + conversione JPEG software...\n", err);
    esp_camera_deinit();
    delay(100);
    fillConfig(c, true);
    err = esp_camera_init(&c);
    if (err != ESP_OK) {
      Log.printf("ERRORE inizializzazione camera: 0x%x\n", err);
      Log.println("Controlla che il flat della camera sia inserito bene e che la scheda sia una Freenove ESP32-S3-WROOM CAM.");
      return false;
    }
    softJpeg = true;
  }

  sensor_t *s = esp_camera_sensor_get();
  setSoftQuality(12);
  Log.printf("Sensore rilevato: %s  -> modalita' %s\n", sensorName(s).c_str(),
                softJpeg ? "JPEG SOFTWARE (max SVGA, fps ridotti)" : "JPEG hardware");
  return true;
}

static void initSD() {
  SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN);
  if (!SD_MMC.begin("/sdcard", true /*1-bit*/, false, 20000)) {
    Log.println("microSD: NON montata (inserita? formattata FAT32?)");
    sdOk = false;
    return;
  }
  if (SD_MMC.cardType() == CARD_NONE) { Log.println("microSD: nessuna scheda"); sdOk = false; return; }
  if (!SD_MMC.exists(PHOTO_DIR)) SD_MMC.mkdir(PHOTO_DIR);
  if (!SD_MMC.exists(VIDEO_DIR)) SD_MMC.mkdir(VIDEO_DIR);
  sdOk = true;
  Log.printf("microSD: OK  (%llu MB totali, %llu MB usati)\n",
                SD_MMC.totalBytes() / (1024ULL * 1024ULL), SD_MMC.usedBytes() / (1024ULL * 1024ULL));
}

static void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // fondamentale per uno streaming fluido
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Log.printf("Connessione a \"%s\"", WIFI_SSID);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Log.print(".");
    if (millis() - t0 > 30000) {
      Log.printf("\nTimeout Wi-Fi (stato %d). Controlla SSID/password e che la rete sia a 2.4 GHz. Riavvio...\n", (int)WiFi.status());
      ESP.restart();
    }
  }
  Log.println(" connesso!");
}

void setup() {
  Log.begin(115200);
  delay(1500);
  Log.println("\n\n=== ESP32-S3 Freenove CAM ===");

  if (!initCamera()) { delay(5000); ESP.restart(); }
  initSD();
  connectWiFi();

  configTzTime(TIMEZONE, NTP_SERVER);   // orario per i nomi dei file

  startServers();
  xTaskCreatePinnedToCore(recTask, "recTask", 4096, NULL, 1, NULL, 0);

  String ip = WiFi.localIP().toString();
  Log.println();
  Log.println("==============================================================");
  Log.println("  TELECAMERA PRONTA");
  Log.printf ("  Indirizzo IP      : %s\n", ip.c_str());
  Log.printf ("  Pagina web        : http://%s:%d/\n", ip.c_str(), WEB_PORT);
  Log.printf ("  Stream video      : http://%s:%d/stream   (porta %d)\n", ip.c_str(), STREAM_PORT, STREAM_PORT);
  Log.printf ("  Singolo scatto    : http://%s:%d/capture\n", ip.c_str(), WEB_PORT);
  Log.println("  (lo stream si apre anche con VLC: Media > Apri flusso di rete)");
  Log.println("==============================================================");
}

void loop() {
  // Riconnessione Wi-Fi se cade il collegamento
  static uint32_t last = 0;
  if (WiFi.status() != WL_CONNECTED && millis() - last > 10000) {
    last = millis();
    Log.println("Wi-Fi perso, riconnessione...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
  delay(500);
}
