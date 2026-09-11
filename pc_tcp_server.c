#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wlanapi.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "wlanapi.lib")

#define PICO_TCP_PORT 4242
#define HTTP_PORT 8080
#define MAX_TELEMETRY_HISTORY 2000
#define HTTP_JSON_CAPACITY 524288
#define TELEMETRY_STALE_MS 7000ULL
#define PICO_AP_SSID "PICOW_DEMO"
#define SESSION_SAVE_TEMP_FILENAME "pico_session_save.tmp"
#define BOARD_IMAGE_PATH L"C:\\Users\\takuc\\Pictures\\\u753b\u50cf1.png"
#define MAX_FRAME_BYTES (1024u * 1024u)
#define CAMERA_MAX_WIDTH 640u
#define CAMERA_MAX_HEIGHT 480u

typedef enum {
    EVENT_TELEMETRY,
    EVENT_PC_COMMAND,
    EVENT_PICO_REPLY,
    EVENT_CONNECTION,
    EVENT_CAMERA
} event_type_t;

typedef struct {
    event_type_t type;
    unsigned long long serial;
    char received_at[24];
    char event_text[256];
    unsigned long uptime_s;
    double temp_c;
    unsigned long random_value;
    long command_value;
    double gyro_z_dps;
    bool gyro_z_valid;
    double gyro_z_angle_deg;
    bool gyro_z_angle_valid;
    unsigned int photodiode_adc[4];
    unsigned int photoreflector_adc_value;
    bool photoreflector_adc_valid;
    int wifi_signal_quality;
} telemetry_entry_t;

static telemetry_entry_t history[MAX_TELEMETRY_HISTORY];
static int history_count;
static int history_next;
static unsigned long long next_history_serial;
static CRITICAL_SECTION history_lock;
static CRITICAL_SECTION autosave_lock;
static volatile LONG pico_connected;
static volatile LONG pico_ever_connected;
static volatile LONG server_running;
static ULONGLONG last_telemetry_tick_ms;
static bool dummy_mode;
static volatile LONG dummy_running;
static volatile LONG dummy_command_value;
static SOCKET active_pico_client = INVALID_SOCKET;
static CRITICAL_SECTION send_lock;
static bool autosave_error_reported;
static char session_save_filename[MAX_PATH];
static unsigned long long session_first_serial;
static CRITICAL_SECTION camera_lock;
static char latest_camera_path[MAX_PATH];
static char latest_camera_filename[MAX_PATH];

static bool send_ground_command(const char *raw_command);
static void autosave_history(void);
static void begin_session_save(void);

static const char DASHBOARD_HTML[] =
"<!doctype html><html lang=\"ja\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>Pico telemetry</title><style>"
"*{box-sizing:border-box}body{margin:0;background:#0b121a;color:#e8f0f7;font-family:system-ui,sans-serif}main{max-width:1800px;margin:auto;padding:12px}.top{display:flex;justify-content:space-between;gap:10px;flex-wrap:wrap;align-items:center}h1{margin:0;font-size:1.45rem}.sub{color:#9eb0c0;margin:3px 0}.actions{display:flex;gap:6px;flex-wrap:wrap}button,label.file,input.command{background:#182838;border:1px solid #38546d;color:#e8f0f7;border-radius:7px;padding:7px 9px;font:inherit;font-size:.88rem}button,label.file{cursor:pointer}button:disabled{cursor:default;opacity:.45}input.command{min-width:160px}button.primary{background:#1977b5}label.file input{display:none}.status{display:flex;gap:8px;align-items:center;margin:10px 0;padding:8px 10px;background:#111e2b;border:1px solid #293f53;border-radius:8px}.dot{width:10px;height:10px;border-radius:50%;background:#8192a2}.receiving{background:#42d99a}.stale{background:#f2b35a}.waiting{background:#64b7ff}.reconnecting{background:#f2b35a}.disconnected{background:#f06778}.metrics{display:grid;grid-template-columns:repeat(auto-fit,minmax(132px,1fr));gap:8px}.card,.section{background:#111e2b;border:1px solid #293f53;border-radius:10px}.card{padding:9px}.label{font-size:.69rem;color:#9eb0c0;text-transform:uppercase;letter-spacing:.05em}.value{font-size:1.18rem;font-weight:650;margin:4px 0 6px;font-variant-numeric:tabular-nums}.range{display:flex;gap:12px;font-size:.68rem;color:#9eb0c0}.range b{display:block;color:#e8f0f7;font-size:.78rem;margin-top:1px;font-weight:500}.charts{display:grid;grid-template-columns:repeat(auto-fit,minmax(250px,1fr));gap:8px;margin-top:10px}.section{padding:10px}.head{display:flex;justify-content:space-between;gap:8px;align-items:baseline;margin-bottom:6px}.head h2{font-size:.96rem;margin:0}.meta{color:#9eb0c0;font-size:.76rem}.chart{width:100%;height:auto;display:block;background:#0b151f;border-radius:6px}.grid{stroke:#263d51;stroke-width:1}.axis{fill:#9eb0c0;font-size:11px}.line{fill:none;stroke-width:2.4;stroke-linejoin:round;stroke-linecap:round}.temp{stroke:#49c4ff}.gyro{stroke:#9dde68}.pd0{stroke:#ffba5c}.pd1{stroke:#7dc9ff}.pd2{stroke:#e38fff}.pd3{stroke:#70e0b0}.scroll{max-height:180px;overflow:auto;border:1px solid #293f53;border-radius:7px}table{width:100%;border-collapse:collapse;font-size:.78rem}th,td{padding:6px 8px;border-bottom:1px solid #213447;text-align:right;white-space:nowrap;font-variant-numeric:tabular-nums}tr.event td{background:#162636;color:#b9d7ea;text-align:left;font-style:italic}th{position:sticky;top:0;background:#182838;color:#c4d7e7;font-weight:500}th:first-child,td:first-child{text-align:left}@media(max-width:800px){main{padding:10px}.scroll{max-height:240px}input.command{min-width:140px}}</style></head><body><main>"
"<header class=\"top\"><div><h1>Pico W Telemetry</h1><p class=\"sub\">Live monitor, command timeline, portable snapshots and replay</p></div><div class=\"actions\"><input class=\"command\" id=\"command-input\" placeholder=\"SET_VALUE,-100..100\"><button class=\"primary\" id=\"send-command\" type=\"button\">Send command</button><button id=\"capture-camera\" type=\"button\">Capture camera</button><button id=\"save\" type=\"button\">Save CSV</button><button id=\"export-html\" type=\"button\">Export HTML</button><label class=\"file\">Load CSV<input id=\"load\" type=\"file\" accept=\".csv,text/csv\"></label><label class=\"file\">Load images<input id=\"load-images\" type=\"file\" accept=\".bmp,.png,.jpg,.jpeg,image/bmp,image/png,image/jpeg\" multiple></label><button id=\"live\" type=\"button\">Resume live</button></div></header>"
"<style>.workspace{display:grid;grid-template-columns:minmax(500px,1fr) minmax(270px,.36fr) minmax(400px,.70fr);grid-template-rows:auto auto auto;gap:10px;margin-top:10px;align-items:start}.workspace>.metrics{grid-column:1;grid-row:1;grid-template-columns:repeat(4,minmax(0,1fr));gap:7px}.workspace .card{min-width:0;padding:7px}.workspace .label{font-size:.64rem}.workspace .value{font-size:1.06rem;margin:3px 0 5px}.workspace .range{gap:9px;font-size:.62rem}.workspace .range b{font-size:.72rem}.camera-panel{grid-column:1;grid-row:2;justify-self:start;width:calc(50% - 5px);margin:0}.camera-content{display:grid;gap:8px;width:100%}.camera-image{display:block;width:100%;height:270px;object-fit:contain;background:#080e14;border-radius:7px}.camera-tools{width:100%;min-width:0;display:grid;gap:6px}.camera-nav{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:5px;width:100%}.camera-nav button{width:100%}.camera-link{padding:2px 5px;border-radius:4px;color:#bfe5ff;font-style:normal}.history-section{grid-column:2;grid-row:1 / span 3;align-self:stretch;min-height:620px;margin:0}.history-section .scroll{height:565px;max-height:calc(100vh - 230px)}.graph-stack{grid-column:3;grid-row:1 / span 3;display:grid;gap:6px;align-content:start;min-width:0}.graph-range{padding:8px}.graph-range .head{display:grid;gap:4px;align-items:start;margin:0!important}.graph-range .head>div{display:flex!important;width:100%;min-width:0}.graph-range input[type=range]{flex:1 1 0;min-width:0;width:auto!important}.graph-range .meta{white-space:nowrap}.graph-charts{display:grid;grid-template-columns:1fr;gap:6px;margin:0}.graph-charts .section{padding:8px}.board-section{grid-column:1;grid-row:2;justify-self:end;width:calc(50% - 5px);margin:0}.board-map{position:relative;max-width:180px;margin:auto;line-height:1}.board-map img{display:block;width:100%;height:auto;border-radius:8px}.sensor-dot{position:absolute;transform:translate(-50%,-50%);width:clamp(25px,3vw,34px);height:clamp(25px,3vw,34px);border:2px solid #fff;border-radius:50%;display:flex;align-items:center;justify-content:center;text-align:center;white-space:pre-line;line-height:1.08;font-size:clamp(7px,.85vw,9px);font-weight:750;color:#fff;text-shadow:0 1px 2px #000;box-shadow:0 0 0 2px #101820,0 0 14px rgba(255,55,55,.65);transition:background .35s,box-shadow .35s}.board-key{display:flex;justify-content:space-between;gap:8px;flex-wrap:wrap;color:#9eb0c0;font-size:.72rem;margin:0 0 7px}.red-scale{width:80px;height:8px;border-radius:8px;background:linear-gradient(90deg,rgba(240,40,40,.2),rgba(240,40,40,1))}.sequence-section{grid-column:1;grid-row:3;margin:0}.sequence-rows{display:grid;gap:5px}.sequence-row{display:grid;grid-template-columns:74px minmax(0,1fr) 34px;gap:5px}.sequence-row input{min-width:0;width:100%;background:#182838;border:1px solid #38546d;color:#e8f0f7;border-radius:6px;padding:6px;font:inherit;font-size:.8rem}.sequence-actions{display:flex;gap:6px;flex-wrap:wrap;margin-top:7px}.command-marker{fill:rgba(255,202,92,.12);stroke:#ffd276;stroke-width:1.5;stroke-dasharray:4 3;cursor:help}.photoreflector{stroke:#ff8be8}@media(max-width:1180px){.workspace{grid-template-columns:minmax(430px,1fr) minmax(360px,.75fr)}.workspace>.metrics{grid-column:1;grid-row:1}.camera-panel{grid-column:1;grid-row:3}.board-section{grid-column:1;grid-row:3}.sequence-section{grid-column:1;grid-row:4}.history-section{grid-column:1;grid-row:2}.graph-stack{grid-column:2;grid-row:1 / span 4}}@media(max-width:900px){.workspace{grid-template-columns:1fr;grid-template-rows:auto}.workspace>.metrics,.camera-panel,.graph-stack,.history-section,.board-section,.sequence-section{grid-column:1;grid-row:auto;width:100%}.workspace>.metrics{grid-template-columns:repeat(auto-fit,minmax(122px,1fr))}.history-section{min-height:0}.history-section .scroll{height:240px;max-height:240px}.camera-image{height:260px}}</style>"
"<style>.camera-image{height:220px}.camera-panel,.board-section{align-self:stretch}.history-section{min-height:590px}.history-section .scroll{height:535px}.sequence-section{align-self:start}.sequence-section>.head{cursor:pointer;user-select:none}.sequence-section>.head h2:after{content:'  ▸';color:#9eb0c0}.sequence-section.sequence-expanded>.head h2:after{content:'  ▾'}.sequence-section:not(.sequence-expanded)>:not(.head){display:none}.graph-charts{gap:4px}.graph-charts .section{padding:6px}.graph-charts .head{margin-bottom:3px}.gyroangle{stroke:#f2b35a}.sequence-burst{display:grid;grid-template-columns:auto minmax(80px,1fr) minmax(70px,1fr) auto;gap:6px;align-items:center;margin-top:9px;padding-top:8px;border-top:1px solid #293f53;font-size:.78rem}.sequence-burst input{min-width:0;width:100%;background:#182838;border:1px solid #38546d;color:#e8f0f7;border-radius:6px;padding:6px;font:inherit;font-size:.8rem}.sequence-burst label{display:grid;grid-template-columns:auto minmax(0,1fr);align-items:center;gap:4px;color:#9eb0c0}</style>"
"<style>.flight-controls{grid-column:1;grid-row:3;margin:0}.flight-controls .flight-actions{display:flex;gap:6px;flex-wrap:wrap;align-items:center}.flight-controls label{display:flex;gap:5px;align-items:center;color:#9eb0c0;font-size:.78rem}.flight-controls input{width:74px;background:#182838;border:1px solid #38546d;color:#e8f0f7;border-radius:6px;padding:6px;font:inherit;font-size:.8rem}.flight-controls .danger{background:#8e3542;border-color:#db6473}.sequence-section{grid-row:4}@media(max-width:1180px){.flight-controls{grid-column:1;grid-row:4}.sequence-section{grid-column:1;grid-row:5}}@media(max-width:900px){.flight-controls,.sequence-section{grid-column:1;grid-row:auto;width:100%}}</style>"
"<div class=\"status\"><span id=\"dot\" class=\"dot disconnected\"></span><span id=\"status\">Starting...</span></div><div class=\"workspace\"><section id=\"metrics\" class=\"metrics\"></section>"
"<section class=\"section flight-controls\"><div class=\"head\"><h2>Flight controls</h2><span class=\"meta\">Commands are sent directly to Pico</span></div><div class=\"flight-actions\"><label>Wheel speed <input id=\"wheel-speed\" type=\"number\" min=\"-100\" max=\"100\" step=\"1\" value=\"0\"> %</label><button id=\"wheel-set\" class=\"primary\" type=\"button\">Set speed</button><button id=\"wheel-stop\" class=\"danger\" type=\"button\">Wheel stop</button><button id=\"slew-abort\" class=\"danger\" type=\"button\">Abort slew</button><button id=\"angle-reset\" type=\"button\">Angle reset</button><label>Relative angle <input id=\"relative-slew-angle\" type=\"number\" min=\"-360\" max=\"360\" step=\"1\" value=\"1\"> deg</label><button id=\"relative-slew\" class=\"primary\" type=\"button\">Slew</button></div><span class=\"meta\">Wheel speed is applied immediately from -100% to +100%. Relative slew is limited to -360 to +360 degrees. Angle reset is rejected during an active slew.</span></section>"
"<section class=\"section camera-panel\"><div class=\"head\"><h2>Camera gallery</h2><span id=\"camera-name\" class=\"meta\">No image received</span></div><div class=\"camera-content\"><img id=\"camera-image\" class=\"camera-image\" alt=\"OV7675 capture\" hidden><div class=\"camera-tools\"><span id=\"camera-count\" class=\"meta\">No saved images</span><div class=\"camera-nav\"><button id=\"camera-prev\" type=\"button\" title=\"Previous image (Left Arrow)\">&lt;</button><button id=\"camera-next\" type=\"button\" title=\"Next image (Right Arrow)\">&gt;</button><button id=\"camera-latest\" type=\"button\">Latest</button></div><span class=\"meta\">Low-latency QQVGA RGB565 frames are received without compression and converted to BMP for display.</span></div></div></section>"
"<aside class=\"graph-stack\"><section class=\"section graph-range\"><div class=\"head\" style=\"margin:0\"><h2>Graph range</h2><div style=\"display:flex;gap:9px;align-items:center;flex-wrap:wrap\"><input id=\"graph-window\" type=\"range\" min=\"0\" max=\"0\" value=\"0\" style=\"width:min(28vw,340px)\"><span id=\"graph-window-label\" class=\"meta\">Live: latest 60 seconds</span><button id=\"graph-latest\" type=\"button\">Latest</button></div></div></section>"
"<section class=\"charts graph-charts\"><article class=\"section\"><div class=\"head\"><h2>Gyro Z</h2><span class=\"meta\">auto scale</span></div><svg id=\"gyro-chart\" class=\"chart\" viewBox=\"0 0 720 170\"></svg></article><article class=\"section\"><div class=\"head\"><h2>Gyro angle</h2><span class=\"meta\">auto scale</span></div><svg id=\"gyro-angle-chart\" class=\"chart\" viewBox=\"0 0 720 170\"></svg></article><article class=\"section\"><div class=\"head\"><h2>Photodiode ADC</h2><span class=\"meta\">auto scale</span></div><svg id=\"pd-chart\" class=\"chart\" viewBox=\"0 0 720 170\"></svg></article><article class=\"section\"><div class=\"head\"><h2>Photoreflector</h2><span class=\"meta\">auto scale</span></div><svg id=\"photoreflector-chart\" class=\"chart\" viewBox=\"0 0 720 170\"></svg></article></section></aside>"
"<section class=\"section history-section\"><div class=\"head\"><h2 id=\"history-title\">Command history</h2><div class=\"actions\"><span id=\"count\" class=\"meta\"></span><button id=\"history-mode\" type=\"button\">Show all history</button></div></div><div class=\"scroll\"><table><thead id=\"history-head\"><tr><th>Received</th><th>Direction / message</th></tr></thead><tbody id=\"rows\"></tbody></table></div></section>"
"<section class=\"section board-section\"><div class=\"head\"><h2>Photodiode position map</h2><span class=\"meta\">PD0 upper-left, clockwise to PD3</span></div><div class=\"board-key\"><span>Low</span><span class=\"red-scale\" aria-label=\"Light red means low, dark red means high\"></span><span>High</span></div><div class=\"board-map\"><img src=\"/board-image.png\" alt=\"Satellite photodiode layout\"><div class=\"sensor-dot\" id=\"sensor0\" style=\"left:25%;top:13.7%\">PD0\n--</div><div class=\"sensor-dot\" id=\"sensor1\" style=\"left:75.4%;top:13.7%\">PD1\n--</div><div class=\"sensor-dot\" id=\"sensor2\" style=\"left:75.4%;top:86.7%\">PD2\n--</div><div class=\"sensor-dot\" id=\"sensor3\" style=\"left:25%;top:86.7%\">PD3\n--</div></div></section><section class=\"section sequence-section\"><div class=\"head\"><h2>Command sequence</h2><span id=\"sequence-status\" class=\"meta\">Idle</span></div><div class=\"sequence-row sequence-labels\"><span>Delay (s)</span><span>Command</span><span></span></div><div id=\"sequence-rows\" class=\"sequence-rows\"></div><div class=\"sequence-actions\"><button id=\"sequence-add\" type=\"button\">+ Step</button><button id=\"sequence-start\" class=\"primary\" type=\"button\">Start</button><button id=\"sequence-stop\" type=\"button\" disabled>Stop</button></div><span class=\"meta\">Delay is the wait before each command. The sequence stops if Pico is disconnected.</span><div class=\"sequence-burst\"><b>Capture burst</b><label>Every <input id=\"burst-interval\" type=\"number\" min=\"0\" step=\"0.1\" value=\"2\"> s</label><label><input id=\"burst-count\" type=\"number\" min=\"1\" step=\"1\" value=\"3\"> shots</label><button id=\"burst-start\" class=\"primary\" type=\"button\">Run c burst</button></div><span class=\"meta\">Sends c immediately once, then repeats it at the selected interval. This replaces the listed steps.</span></section></div>"
"</main><script>"
"const $=id=>document.getElementById(id),fmt=(v,d=2)=>Number.isFinite(v)?v.toFixed(d):'NA',num=v=>Number.isFinite(v),esc=v=>String(v).replace(/[&<>]/g,c=>c==='&'?'&amp;':c==='<'?'&lt;':'&gt;');let current=null,loaded=false,graphEndTime=null,cameraShown='',historyMode='commands';"
"const fields=[['Uptime',v=>v.uptime_s,' s',0],['Temperature',v=>v.temp_c,' C',2],['Gyro Z',v=>v.gyro_z_dps,' dps',2],['Gyro angle',v=>v.gyro_z_angle_deg,' deg',2],['PD0',v=>v.photodiode_adc?.[0],'',0],['PD1',v=>v.photodiode_adc?.[1],'',0],['PD2',v=>v.photodiode_adc?.[2],'',0],['PD3',v=>v.photodiode_adc?.[3],'',0],['Photoreflector',v=>v.photoreflector_adc,'',0],['Wi-Fi signal',v=>v.wifi_signal_quality,' %',0],['Command',v=>v.command_value,'',0]];"
"$('metrics').innerHTML=fields.map((f,i)=>`<article class=\"card\"><div class=\"label\">${f[0]}</div><div class=\"value\" id=\"v${i}\">--</div><div class=\"range\"><span>MIN<b id=\"n${i}\">--</b></span><span>MAX<b id=\"x${i}\">--</b></span></div></article>`).join('')+`<article class=\"card\"><div class=\"label\">Last ground command</div><div class=\"value\" id=\"last-command\">--</div><div class=\"range\"><span>TIME<b id=\"last-command-time\">--</b></span></div></article>`;"
"function metric(h,l,f,i){const a=h.map(f[1]).filter(num),s=a.length?Math.min(...a):null,x=a.length?Math.max(...a):null;$('v'+i).textContent=fmt(f[1](l),f[3])+f[2];$('n'+i).textContent=fmt(s,f[3])+f[2];$('x'+i).textContent=fmt(x,f[3])+f[2]}"
"function renderBoard(l){const values=(l?.photodiode_adc||[]).map(v=>Number.isFinite(v)?v:null),valid=values.filter(num),lo=valid.length?Math.min(...valid):0,hi=valid.length?Math.max(...valid):1;values.forEach((v,i)=>{const dot=$('sensor'+i);if(!num(v)){dot.textContent=`PD${i}\\n--`;dot.style.background='#465666';dot.style.color='#dce8f2';return}const ratio=hi===lo?.5:(v-lo)/(hi-lo),alpha=.18+.82*ratio;dot.textContent=`PD${i}\\n${fmt(v,0)}`;dot.style.background=`rgba(235,38,38,${alpha})`;dot.style.color='#fff';dot.style.boxShadow=`0 0 0 2px #101820,0 0 ${8+14*ratio}px rgba(255,55,55,${.25+.7*ratio})`;dot.title=`PD${i}: ${fmt(v,0)}`})}"
"function renderCamera(d){const name=d.latest_camera||'',image=$('camera-image');$('camera-name').textContent=name||'No image received';if(!name){image.hidden=true;return}if(cameraShown!==name){cameraShown=name;image.onerror=()=>{if(cameraShown===name){$('camera-name').textContent=name+' (image file unavailable)';image.hidden=true}};image.src='/camera/latest.bmp?name='+encodeURIComponent(name);image.hidden=false}}"
"function plot(id,h,series){const svg=$(id),W=720,H=250,L=58,R=18,T=18,B=35,pts=[];series.forEach(s=>h.forEach((v,i)=>{const n=s[1](v);if(num(n))pts.push(n)}));if(!pts.length){svg.innerHTML='<text class=\"axis\" x=\"18\" y=\"32\">No data</text>';return}let lo=Math.min(...pts),hi=Math.max(...pts);if(lo===hi){lo-=1;hi+=1}const px=i=>L+(W-L-R)*i/Math.max(h.length-1,1),py=v=>T+(H-T-B)*(1-(v-lo)/(hi-lo));let g='';for(let i=0;i<5;i++){const y=T+(H-T-B)*i/4;g+=`<line class=\"grid\" x1=\"${L}\" x2=\"${W-R}\" y1=\"${y}\" y2=\"${y}\"/><text class=\"axis\" x=\"3\" y=\"${y+4}\">${(hi-(hi-lo)*i/4).toFixed(1)}</text>`}let paths=series.map(s=>{let started=false,d='';h.forEach((v,i)=>{const n=s[1](v);if(num(n)){d+=(started?'L':'M')+px(i).toFixed(1)+','+py(n).toFixed(1);started=true}});return d?`<path class=\"line ${s[2]}\" d=\"${d}\"/>`:''}).join('');svg.innerHTML=g+paths+`<text class=\"axis\" x=\"${L}\" y=\"${H-10}\">${esc(h[0]?.received_at||'')}</text><text class=\"axis\" text-anchor=\"end\" x=\"${W-R}\" y=\"${H-10}\">${esc(h[h.length-1]?.received_at||'')}</text>`}"
"function clockMs(v){const p=String(v.received_at||'').split(':').map(Number);return p.length===3&&p.every(Number.isFinite)?((p[0]*3600+p[1]*60+p[2])*1000):0}"
"function graphWindow(h){if(!h.length){$('graph-window').min=0;$('graph-window').max=0;$('graph-window').value=0;$('graph-window-label').textContent='No telemetry';return []}const times=h.map(clockMs),slider=$('graph-window');let index=h.length-1;if(graphEndTime!==null){index=0;for(let i=0;i<times.length;i++){if(times[i]<=graphEndTime)index=i;else break}}else graphEndTime=times[index];slider.min=0;slider.max=h.length-1;slider.value=index;const end=times[index],start=end-60000;$('graph-window-label').textContent=`${h[index].received_at} · previous 60 seconds`;return h.filter((v,i)=>i<=index&&times[i]>=start)}"
"function eventPrefix(v){return v.type==='pc_command'?'PC -> Pico >':v.type==='camera'?'Camera:':v.type==='connection'?'System:':'Pico -> PC:'}"
"function render(d){current=d;const h=d.history||[],t=h.filter(v=>v.type==='telemetry'),commands=h.filter(v=>v.type==='pc_command'||v.type==='camera'||(v.type==='pico_reply'&&v.event_text!=='PICO_CONNECTED')),l=d.latest,all=historyMode==='all';const labels={receiving:'Receiving telemetry',stale:'Connected, telemetry delayed',waiting:'Connected, waiting for telemetry',reconnecting:'Disconnected, waiting for Pico reconnection',disconnected:'Waiting for first Pico connection'};$('dot').className='dot '+d.link_status;$('status').textContent=(d.dummy_mode?'Dummy simulation · ':'')+(labels[d.link_status]||'Unknown')+(d.last_telemetry_age_s===null?'':` · last telemetry ${d.last_telemetry_age_s.toFixed(1)} s ago`)+(loaded?' · loaded file (live paused)':'');if(l)fields.forEach((f,i)=>metric(t,l,f,i));renderBoard(l);renderCamera(d);const last=[...h].reverse().find(v=>v.type==='pc_command');$('last-command').textContent=last?last.event_text:'--';$('last-command-time').textContent=last?last.received_at:'--';$('history-title').textContent=all?'All history':'Command history';$('history-mode').textContent=all?'Commands only':'Show all history';$('history-head').innerHTML=all?'<tr><th>Received</th><th>Uptime</th><th>Temp C</th><th>Gyro dps</th><th>Command</th><th>Random</th><th>PD0</th><th>PD1</th><th>PD2</th><th>PD3</th><th>Photoreflector</th></tr>':'<tr><th>Received</th><th>Direction / message</th></tr>';$('count').textContent=all?`${h.length} events · ${t.length} telemetry samples · latest at top · scroll for all`:`${commands.length} command events · latest at top`;if(!all){$('rows').innerHTML=commands.slice().reverse().map(v=>`<tr class=\"event\"><td>${esc(v.received_at)}</td><td>${eventPrefix(v)} ${esc(v.event_text)}</td></tr>`).join('')}else $('rows').innerHTML=h.slice().reverse().map(v=>{if(v.type!=='telemetry')return `<tr class=\"event\"><td>${esc(v.received_at)}</td><td colspan=\"10\">${eventPrefix(v)} ${esc(v.event_text)}</td></tr>`;const pd=(v.photodiode_adc||[null,null,null,null]).map(n=>`<td>${fmt(n,0)}</td>`).join('');return `<tr><td>${esc(v.received_at)}</td><td>${fmt(v.uptime_s,0)}</td><td>${fmt(v.temp_c)}</td><td>${fmt(v.gyro_z_dps)}</td><td>${fmt(v.command_value,0)}</td><td>${fmt(v.random,0)}</td>${pd}<td>${fmt(v.photoreflector_adc,0)}</td></tr>`}).join('');const g=graphWindow(t);plot('temp-chart',g,[['temp',v=>v.temp_c,'temp']]);plot('gyro-chart',g,[['gyro',v=>v.gyro_z_dps,'gyro']]);plot('pd-chart',g,[['pd0',v=>v.photodiode_adc?.[0],'pd0'],['pd1',v=>v.photodiode_adc?.[1],'pd1'],['pd2',v=>v.photodiode_adc?.[2],'pd2'],['pd3',v=>v.photodiode_adc?.[3],'pd3']]);plot('photoreflector-chart',g,[['photoreflector',v=>v.photoreflector_adc,'photoreflector']])}"
"function save(){if(!current?.history?.length)return;const rows=['type,received_at,event_text_encoded,uptime_s,temp_c,gyro_z_dps,command_value,random,pd0,pd1,pd2,pd3,photoreflector_adc,wifi_signal_quality'];current.history.forEach(v=>rows.push([v.type||'telemetry',v.received_at,encodeURIComponent(v.event_text||''),v.uptime_s,v.temp_c,num(v.gyro_z_dps)?v.gyro_z_dps:'NA',v.command_value,v.random,...(v.photodiode_adc||[]),num(v.photoreflector_adc)?v.photoreflector_adc:'NA',num(v.wifi_signal_quality)?v.wifi_signal_quality:'NA'].join(',')));const a=document.createElement('a');a.href=URL.createObjectURL(new Blob([rows.join('\\n')],{type:'text/csv'}));a.download='pico-telemetry.csv';a.click();URL.revokeObjectURL(a.href)}"
"function load(file){const r=new FileReader();r.onload=()=>{const rows=String(r.result).trim().split(/\\r?\\n/),head=rows.shift().split(','),at=(a,k)=>a[head.indexOf(k)],n=(a,k)=>{const v=at(a,k);return v===undefined||v==='NA'?null:Number(v)},h=rows.filter(Boolean).map(z=>{const a=z.split(','),encoded=at(a,'event_text_encoded');return{type:at(a,'type')||'telemetry',received_at:at(a,'received_at')||'',event_text:encoded?decodeURIComponent(encoded):'',uptime_s:n(a,'uptime_s'),temp_c:n(a,'temp_c'),gyro_z_dps:n(a,'gyro_z_dps'),command_value:n(a,'command_value'),random:n(a,'random'),photodiode_adc:['pd0','pd1','pd2','pd3'].map(k=>n(a,k)),photoreflector_adc:n(a,'photoreflector_adc'),wifi_signal_quality:n(a,'wifi_signal_quality')}});loaded=true;graphEndTime=null;cameraShown='';render({dummy_mode:false,link_status:'disconnected',last_telemetry_age_s:null,latest:[...h].reverse().find(v=>v.type==='telemetry')||null,history:h})};r.readAsText(file)}"
"async function sendCommand(forced){const input=$('command-input'),text=(forced??input.value).trim();if(!text)return;try{const response=await fetch('/api/command',{method:'POST',headers:{'Content-Type':'text/plain'},body:text}),result=await response.json();if(!result.ok)throw Error('send failed');if(!forced)input.value='';loaded=false;await poll()}catch(e){$('status').textContent='Command send failed: Pico is not connected'}}"
"$('save').onclick=save;$('load').onchange=e=>e.target.files[0]&&load(e.target.files[0]);$('live').onclick=()=>{loaded=false;graphEndTime=null;poll()};$('send-command').onclick=sendCommand;$('capture-camera').onclick=()=>sendCommand('CAPTURE');$('history-mode').onclick=()=>{historyMode=historyMode==='all'?'commands':'all';if(current)render(current)};$('command-input').onkeydown=e=>{if(e.key==='Enter')sendCommand()};$('graph-window').oninput=e=>{const t=(current?.history||[]).filter(v=>v.type==='telemetry'),i=Math.max(0,Math.min(t.length-1,Number(e.target.value)));if(t.length){graphEndTime=clockMs(t[i]);render(current)}};$('graph-latest').onclick=()=>{graphEndTime=null;if(current)render(current)};async function poll(){if(loaded)return;try{render(await (await fetch('/api/telemetry',{cache:'no-store'})).json())}catch(e){$('dot').className='dot disconnected';$('status').textContent='Dashboard connection error'}}poll();setInterval(poll,1000);"
"function graphWindow(h){if(!h.length){$('graph-window').min=0;$('graph-window').max=0;$('graph-window').value=0;$('graph-window-label').textContent='No telemetry';return []}const times=h.map(clockMs),slider=$('graph-window'),live=graphEndTime===null;let index=h.length-1;if(!live){index=0;for(let i=0;i<times.length;i++){if(times[i]<=graphEndTime)index=i;else break}}slider.min=0;slider.max=h.length-1;slider.value=index;const end=times[index],start=end-60000;$('graph-window-label').textContent=live?'Live · latest 60 seconds':h[index].received_at+' · previous 60 seconds';return h.filter((v,i)=>i<=index&&times[i]>=start)}"
"const chartRanges={'temp-chart':[-40,85],'gyro-chart':[-2000,2000],'pd-chart':[0,4095],'photoreflector-chart':[0,4095]};let graphStartTime=0,graphDisplayEndTime=0;"
"function formatGraphTime(milliseconds){let seconds=((Math.floor(milliseconds/1000)%86400)+86400)%86400,hours=Math.floor(seconds/3600);seconds%=3600;const minutes=Math.floor(seconds/60);seconds%=60;return String(hours).padStart(2,'0')+':'+String(minutes).padStart(2,'0')+':'+String(seconds).padStart(2,'0')}"
"function graphWindow(h){if(!h.length){graphStartTime=graphDisplayEndTime=0;$('graph-window').min=0;$('graph-window').max=0;$('graph-window').value=0;$('graph-window-label').textContent='No telemetry';return []}const times=h.map(clockMs),slider=$('graph-window'),live=graphEndTime===null;let index=h.length-1;if(!live){index=0;for(let i=0;i<times.length;i++){if(times[i]<=graphEndTime)index=i;else break}}graphDisplayEndTime=times[index];graphStartTime=graphDisplayEndTime-60000;slider.min=0;slider.max=h.length-1;slider.value=index;$('graph-window-label').textContent=live?'Live: latest 60 seconds':h[index].received_at+' : previous 60 seconds';return h.filter((v,i)=>i<=index&&times[i]>=graphStartTime)}"
"function plot(id,h,series){const svg=$(id),W=720,H=250,L=58,R=18,T=18,B=35,range=chartRanges[id]||[0,1],lo=range[0],hi=range[1],clamp=v=>Math.max(lo,Math.min(hi,v)),px=v=>L+(W-L-R)*Math.max(0,Math.min(1,(clockMs(v)-graphStartTime)/60000)),py=v=>T+(H-T-B)*(1-(clamp(v)-lo)/(hi-lo));let grid='';for(let i=0;i<5;i++){const y=T+(H-T-B)*i/4,value=hi-(hi-lo)*i/4,x=L+(W-L-R)*i/4,age=60-i*15;grid+='<line class=\\\"grid\\\" x1=\\\"'+L+'\\\" x2=\\\"'+(W-R)+'\\\" y1=\\\"'+y+'\\\" y2=\\\"'+y+'\\\"/><text class=\\\"axis\\\" x=\\\"3\\\" y=\\\"'+(y+4)+'\\\">'+value.toFixed(id==='temp-chart'||id==='gyro-chart'?1:0)+'</text><line class=\\\"grid\\\" x1=\\\"'+x+'\\\" x2=\\\"'+x+'\\\" y1=\\\"'+T+'\\\" y2=\\\"'+(H-B)+'\\\"/><text class=\\\"axis\\\" text-anchor=\\\"middle\\\" x=\\\"'+x+'\\\" y=\\\"'+(H-10)+'\\\">'+(age?' -'+age+'s':'now')+'</text>'}const paths=series.map(s=>{let started=false,path='';h.forEach(v=>{const n=s[1](v);if(num(n)){path+=(started?'L':'M')+px(v).toFixed(1)+','+py(n).toFixed(1);started=true}});return path?'<path class=\\\"line '+s[2]+'\\\" d=\\\"'+path+'\\\"/>':''}).join('');const markers=(current?.history||[]).filter(v=>v.type==='pc_command'&&clockMs(v)>=graphStartTime&&clockMs(v)<=graphDisplayEndTime).map(v=>{const x=px(v).toFixed(1),title=esc(v.received_at+' PC -> Pico > '+v.event_text);return '<g class=\\\"command-marker\\\"><title>'+title+'</title><rect x=\\\"'+(x-3)+'\\\" y=\\\"'+T+'\\\" width=\\\"6\\\" height=\\\"'+(H-T-B)+'\\\"/><line x1=\\\"'+x+'\\\" x2=\\\"'+x+'\\\" y1=\\\"'+T+'\\\" y2=\\\"'+(H-B)+'\\\"/></g>'}).join('');svg.innerHTML=grid+paths+markers+'<text class=\\\"axis\\\" x=\\\"'+L+'\\\" y=\\\"13\\\">'+formatGraphTime(graphStartTime)+'</text><text class=\\\"axis\\\" text-anchor=\\\"end\\\" x=\\\"'+(W-R)+'\\\" y=\\\"13\\\">'+formatGraphTime(graphDisplayEndTime)+'</text>'}"
"let selectedCamera='',cameraFollowLatest=true;"
"function cameraNames(d){const names=[];(d.history||[]).forEach(v=>{const m=/^Camera image saved: (camera_[A-Za-z0-9_.]+\\.(?:bmp|jpe?g))$/i.exec(v.event_text||'');if(m&&names.indexOf(m[1])<0)names.push(m[1])});return names}"
"function renderCamera(d){const files=cameraNames(d),latest=d.latest_camera||'',image=$('camera-image');if(latest&&files.indexOf(latest)<0)files.push(latest);if(cameraFollowLatest&&latest)selectedCamera=latest;if(!selectedCamera||files.indexOf(selectedCamera)<0)selectedCamera=latest||files[files.length-1]||'';const name=selectedCamera,index=files.indexOf(name);$('camera-name').textContent=name||'No image received';$('camera-count').textContent=name?(index+1)+' / '+files.length:'No saved images';$('camera-prev').disabled=index<=0;$('camera-next').disabled=index<0||index>=files.length-1;if(!name){image.hidden=true;return}if(cameraShown!==name){cameraShown=name;image.onerror=()=>{if(cameraShown===name){$('camera-name').textContent=name+' (image file unavailable)';image.hidden=true}};image.src='/camera/'+encodeURIComponent(name);image.hidden=false}}"
"function selectCamera(name,followLatest){selectedCamera=name||'';cameraFollowLatest=!!followLatest;if(current)renderCamera(current)}"
"function decorateCameraHistory(){document.querySelectorAll('#rows tr.event td:last-child').forEach(cell=>{const m=/^Camera: Camera image saved: (camera_[A-Za-z0-9_.]+\\.(?:bmp|jpe?g))$/i.exec(cell.textContent.trim());if(!m||cell.dataset.cameraDecorated)return;const name=m[1];cell.dataset.cameraDecorated='1';cell.innerHTML='Camera: <button class=\\\"camera-link\\\" type=\\\"button\\\" data-camera-file=\\\"'+name+'\\\">Camera image saved: '+esc(name)+'</button>'})}"
"new MutationObserver(decorateCameraHistory).observe($('rows'),{childList:true});document.addEventListener('click',e=>{const button=e.target.closest('[data-camera-file]');if(button)selectCamera(button.dataset.cameraFile,false)});$('camera-prev').onclick=()=>{const files=current?cameraNames(current):[],index=files.indexOf(selectedCamera);if(index>0)selectCamera(files[index-1],false)};$('camera-next').onclick=()=>{const files=current?cameraNames(current):[],index=files.indexOf(selectedCamera);if(index>=0&&index<files.length-1)selectCamera(files[index+1],false)};$('camera-latest').onclick=()=>selectCamera(current?.latest_camera||'',true);"
"document.addEventListener('keydown',e=>{const target=e.target,editing=target?.matches?.('input,textarea,select')||target?.isContentEditable;if(editing||e.altKey||e.ctrlKey||e.metaKey)return;if(e.key==='c'||e.key==='C'){e.preventDefault();sendCommand('CAPTURE')}else if(e.key==='ArrowLeft'){e.preventDefault();$('camera-prev').click()}else if(e.key==='ArrowRight'){e.preventDefault();$('camera-next').click()}});"
"function plot(id,h,series){const svg=$(id),W=720,H=250,L=58,R=18,T=18,B=35,points=[];series.forEach(s=>h.forEach(v=>{const n=s[1](v);if(num(n))points.push(n)}));if(!points.length){svg.innerHTML='<text class=\\\"axis\\\" x=\\\"18\\\" y=\\\"32\\\">No data</text>';return}let low=Math.min(...points),high=Math.max(...points),minimum=id==='pd-chart'||id==='photoreflector-chart'?50:id==='gyro-chart'?5:.5,span=high-low;if(span<minimum){const middle=(high+low)/2;low=middle-minimum/2;high=middle+minimum/2;span=minimum}const padding=Math.max(span*.12,minimum*.1),lo=low-padding,hi=high+padding,digits=hi-lo<10?2:hi-lo<100?1:0,px=v=>L+(W-L-R)*Math.max(0,Math.min(1,(clockMs(v)-graphStartTime)/60000)),py=v=>T+(H-T-B)*(1-(v-lo)/(hi-lo));let grid='';for(let i=0;i<5;i++){const y=T+(H-T-B)*i/4,value=hi-(hi-lo)*i/4,x=L+(W-L-R)*i/4,age=60-i*15;grid+='<line class=\\\"grid\\\" x1=\\\"'+L+'\\\" x2=\\\"'+(W-R)+'\\\" y1=\\\"'+y+'\\\" y2=\\\"'+y+'\\\"/><text class=\\\"axis\\\" x=\\\"3\\\" y=\\\"'+(y+4)+'\\\">'+value.toFixed(digits)+'</text><line class=\\\"grid\\\" x1=\\\"'+x+'\\\" x2=\\\"'+x+'\\\" y1=\\\"'+T+'\\\" y2=\\\"'+(H-B)+'\\\"/><text class=\\\"axis\\\" text-anchor=\\\"middle\\\" x=\\\"'+x+'\\\" y=\\\"'+(H-10)+'\\\">'+(age?' -'+age+'s':'now')+'</text>'}const paths=series.map(s=>{let started=false,path='';h.forEach(v=>{const n=s[1](v);if(num(n)){path+=(started?'L':'M')+px(v).toFixed(1)+','+py(n).toFixed(1);started=true}});return path?'<path class=\\\"line '+s[2]+'\\\" d=\\\"'+path+'\\\"/>':''}).join('');const markers=(current?.history||[]).filter(v=>v.type==='pc_command'&&clockMs(v)>=graphStartTime&&clockMs(v)<=graphDisplayEndTime).map(v=>{const x=px(v).toFixed(1),title=esc(v.received_at+' PC -> Pico > '+v.event_text);return '<g class=\\\"command-marker\\\"><title>'+title+'</title><rect x=\\\"'+(x-3)+'\\\" y=\\\"'+T+'\\\" width=\\\"6\\\" height=\\\"'+(H-T-B)+'\\\"/><line x1=\\\"'+x+'\\\" x2=\\\"'+x+'\\\" y1=\\\"'+T+'\\\" y2=\\\"'+(H-B)+'\\\"/></g>'}).join('');svg.innerHTML=grid+paths+markers+'<text class=\\\"axis\\\" x=\\\"'+L+'\\\" y=\\\"13\\\">'+formatGraphTime(graphStartTime)+'</text><text class=\\\"axis\\\" text-anchor=\\\"end\\\" x=\\\"'+(W-R)+'\\\" y=\\\"13\\\">'+formatGraphTime(graphDisplayEndTime)+'</text>'}"
"const replayImages={};"
"function replayImageUrl(name){const snapshot=window.__snapshot;return replayImages[name]||(snapshot&&snapshot.images&&snapshot.images[name])||''}"
"function renderCamera(d){const files=cameraNames(d),latest=d.latest_camera||'',image=$('camera-image');if(latest&&files.indexOf(latest)<0)files.push(latest);if(cameraFollowLatest&&latest)selectedCamera=latest;if(!selectedCamera||files.indexOf(selectedCamera)<0)selectedCamera=latest||files[files.length-1]||'';const name=selectedCamera,index=files.indexOf(name),source=name?(replayImageUrl(name)||'/camera/'+encodeURIComponent(name)):'';$('camera-name').textContent=name||'No image received';$('camera-count').textContent=name?(index+1)+' / '+files.length:'No saved images';$('camera-prev').disabled=index<=0;$('camera-next').disabled=index<0||index>=files.length-1;if(!name){image.hidden=true;return}if(cameraShown!==source){cameraShown=source;image.onerror=()=>{if(cameraShown===source){$('camera-name').textContent=name+' (image unavailable: select Load images for CSV replay)';image.hidden=true}};image.src=source;image.hidden=false}}"
"function blobDataUrl(blob){return new Promise((resolve,reject)=>{const reader=new FileReader();reader.onload=()=>resolve(String(reader.result));reader.onerror=()=>reject(reader.error);reader.readAsDataURL(blob)})}"
"async function loadSelectedImages(files){let count=0;for(const file of Array.from(files||[])){if(!/\\.(bmp|png|jpe?g)$/i.test(file.name))continue;try{replayImages[file.name]=await blobDataUrl(file);count++}catch(e){}}cameraShown='';if(current)renderCamera(current);if(count)$('camera-name').textContent=$('camera-name').textContent+' (local image loaded)'}"
"$('load-images').onchange=e=>{loadSelectedImages(e.target.files);e.target.value=''};"
"let sequenceSteps=(window.__snapshot&&Array.isArray(window.__snapshot.sequence)&&window.__snapshot.sequence.length?window.__snapshot.sequence:[{delay:0,command:''}]).map(v=>({delay:Math.max(0,Number(v.delay)||0),command:String(v.command||'')})),sequenceRunning=false,sequenceIndex=0,sequenceTimer=0;"
"function sequenceAttr(value){return String(value).replace(/[^A-Za-z0-9,._:+ -]/g,'')}"
"function renderSequence(){const rows=$('sequence-rows');rows.innerHTML=sequenceSteps.map((step,index)=>'<div class=\\\"sequence-row\\\"><input type=\\\"number\\\" min=\\\"0\\\" step=\\\"0.1\\\" data-sequence-index=\\\"'+index+'\\\" data-sequence-field=\\\"delay\\\" value=\\\"'+sequenceAttr(step.delay)+'\\\"><input type=\\\"text\\\" data-sequence-index=\\\"'+index+'\\\" data-sequence-field=\\\"command\\\" value=\\\"'+sequenceAttr(step.command)+'\\\" placeholder=\\\"CAPTURE or SERVO,10\\\"><button type=\\\"button\\\" data-sequence-remove=\\\"'+index+'\\\" title=\\\"Remove step\\\">x</button></div>').join('')}"
"function setSequenceStatus(text){$('sequence-status').textContent=text;$('sequence-start').disabled=sequenceRunning;$('sequence-stop').disabled=!sequenceRunning}"
"$('sequence-rows').oninput=e=>{const index=Number(e.target.dataset.sequenceIndex),field=e.target.dataset.sequenceField;if(!Number.isInteger(index)||!field||!sequenceSteps[index])return;if(field==='delay')sequenceSteps[index].delay=Math.max(0,Number(e.target.value)||0);else sequenceSteps[index].command=e.target.value};$('sequence-rows').onclick=e=>{const index=Number(e.target.dataset.sequenceRemove);if(Number.isInteger(index)){sequenceSteps.splice(index,1);if(!sequenceSteps.length)sequenceSteps.push({delay:0,command:''});renderSequence()}};$('sequence-add').onclick=()=>{sequenceSteps.push({delay:0,command:''});renderSequence()};"
"async function sendSequenceCommand(command){const normalized=String(command).trim().toLowerCase()==='c'?'CAPTURE':command,response=await fetch('/api/command',{method:'POST',headers:{'Content-Type':'text/plain'},body:normalized}),result=await response.json();if(!response.ok||!result.ok)throw Error('Pico is not connected');loaded=false}"
"function stopSequence(message){if(sequenceTimer)clearTimeout(sequenceTimer);sequenceTimer=0;sequenceRunning=false;setSequenceStatus(message||'Stopped')}"
"function runSequenceNext(){if(!sequenceRunning)return;if(sequenceIndex>=sequenceSteps.length){stopSequence('Complete');return}const step=sequenceSteps[sequenceIndex],delay=Math.max(0,Number(step.delay)||0);setSequenceStatus('Waiting '+delay.toFixed(1)+' s for step '+(sequenceIndex+1)+' / '+sequenceSteps.length);sequenceTimer=setTimeout(async()=>{if(!sequenceRunning)return;setSequenceStatus('Sending '+step.command);try{await sendSequenceCommand(step.command);sequenceIndex++;runSequenceNext()}catch(e){stopSequence('Stopped: '+e.message)}},Math.round(delay*1000))}"
"$('sequence-start').onclick=()=>{const steps=sequenceSteps.map(v=>({delay:Math.max(0,Number(v.delay)||0),command:String(v.command||'').trim()})).filter(v=>v.command);if(!steps.length){setSequenceStatus('Add a command first');return}sequenceSteps=steps;renderSequence();sequenceIndex=0;sequenceRunning=true;setSequenceStatus('Starting');runSequenceNext()};$('sequence-stop').onclick=()=>stopSequence('Stopped');renderSequence();setSequenceStatus('Idle');"
"async function fetchImageData(name){const local=replayImageUrl(name);if(local)return local;try{const response=await fetch('/camera/'+encodeURIComponent(name));if(!response.ok||!String(response.headers.get('content-type')||'').startsWith('image/'))return '';return await blobDataUrl(await response.blob())}catch(e){return ''}}"
"async function exportStaticSnapshot(){if(!current){$('status').textContent='Nothing to export yet';return}const button=$('export-html'),names=cameraNames(current),latest=current.latest_camera;if(latest&&names.indexOf(latest)<0)names.push(latest);button.disabled=true;const images={};try{for(let i=0;i<names.length;i++){button.textContent='Export '+(i+1)+' / '+names.length;const data=await fetchImageData(names[i]);if(data)images[names[i]]=data}let board='';try{const response=await fetch('/board-image.png');if(response.ok)board=await blobDataUrl(await response.blob())}catch(e){}const snapshot={data:current,images:images,sequence:sequenceSteps,historyMode:historyMode},json=JSON.stringify(snapshot).replace(/</g,String.fromCharCode(92)+'u003c'),bootstrap='<script>window.__snapshot='+json+';window.fetch=function(){return Promise.resolve(new Response(JSON.stringify(window.__snapshot.data)))};<'+ '/script>';let html=document.documentElement.outerHTML.replace('</head>',bootstrap+'</head>');html=html.replace('/board-image.png',board||'');const link=document.createElement('a'),stamp=new Date().toISOString().replace(/[:.]/g,'-');link.href=URL.createObjectURL(new Blob([html],{type:'text/html'}));link.download='pico-session-'+stamp+'.html';link.click();setTimeout(()=>URL.revokeObjectURL(link.href),1000);$('status').textContent='Portable HTML exported: '+Object.keys(images).length+' / '+names.length+' images'}catch(e){$('status').textContent='HTML export failed: '+e.message}finally{button.disabled=false;button.textContent='Export HTML'}}"
"$('export-html').onclick=exportStaticSnapshot;if(window.__snapshot&&window.__snapshot.historyMode)historyMode=window.__snapshot.historyMode;"
"function plot(id,h,series){const svg=$(id),W=720,H=170,L=54,R=12,T=14,B=25,points=[];series.forEach(s=>h.forEach(v=>{const n=s[1](v);if(num(n))points.push(n)}));if(!points.length){svg.innerHTML=`<text class='axis' x='18' y='28'>No data</text>`;return}let low=Math.min(...points),high=Math.max(...points),minimum=id==='pd-chart'||id==='photoreflector-chart'?50:id==='gyro-chart'?5:.5,span=high-low;if(span<minimum){const middle=(high+low)/2;low=middle-minimum/2;high=middle+minimum/2;span=minimum}const padding=Math.max(span*.12,minimum*.1),lo=low-padding,hi=high+padding,digits=hi-lo<10?2:hi-lo<100?1:0,px=v=>L+(W-L-R)*Math.max(0,Math.min(1,(clockMs(v)-graphStartTime)/60000)),py=v=>T+(H-T-B)*(1-(v-lo)/(hi-lo));let grid='';for(let i=0;i<5;i++){const y=T+(H-T-B)*i/4,value=hi-(hi-lo)*i/4,x=L+(W-L-R)*i/4,age=60-i*15;grid+=`<line class='grid' x1='${L}' x2='${W-R}' y1='${y}' y2='${y}'/><text class='axis' x='2' y='${y+4}'>${value.toFixed(digits)}</text><line class='grid' x1='${x}' x2='${x}' y1='${T}' y2='${H-B}'/><text class='axis' text-anchor='middle' x='${x}' y='${H-7}'>${age?' -'+age+'s':'now'}</text>`}const paths=series.map(s=>{let started=false,path='';h.forEach(v=>{const n=s[1](v);if(num(n)){path+=(started?'L':'M')+px(v).toFixed(1)+','+py(n).toFixed(1);started=true}});return path?`<path class='line ${s[2]}' d='${path}'/>`:''}).join('');const markers=(current?.history||[]).filter(v=>v.type==='pc_command'&&clockMs(v)>=graphStartTime&&clockMs(v)<=graphDisplayEndTime).map(v=>{const x=px(v).toFixed(1),title=esc(v.received_at+' PC -> Pico > '+v.event_text);return `<g class='command-marker'><title>${title}</title><rect x='${x-3}' y='${T}' width='6' height='${H-T-B}'/><line x1='${x}' x2='${x}' y1='${T}' y2='${H-B}'/></g>`}).join('');svg.innerHTML=grid+paths+markers+`<text class='axis' x='${L}' y='11'>${formatGraphTime(graphStartTime)}</text><text class='axis' text-anchor='end' x='${W-R}' y='11'>${formatGraphTime(graphDisplayEndTime)}</text>`}"
"document.querySelectorAll('.graph-charts svg').forEach(svg=>svg.setAttribute('viewBox','0 0 720 170'));document.querySelector('.sequence-section>.head').onclick=()=>document.querySelector('.sequence-section').classList.toggle('sequence-expanded');"
"const renderWithoutAngle=render;render=function(d){renderWithoutAngle(d);if(historyMode!=='all')return;const h=d.history||[];$('history-head').innerHTML='<tr><th>Received</th><th>Uptime</th><th>Temp C</th><th>Gyro dps</th><th>Gyro angle deg</th><th>Command</th><th>Random</th><th>PD0</th><th>PD1</th><th>PD2</th><th>PD3</th><th>Photoreflector</th></tr>';$('rows').innerHTML=h.slice().reverse().map(v=>{if(v.type!=='telemetry')return `<tr class='event'><td>${esc(v.received_at)}</td><td colspan='11'>${eventPrefix(v)} ${esc(v.event_text)}</td></tr>`;const pd=(v.photodiode_adc||[null,null,null,null]).map(n=>`<td>${fmt(n,0)}</td>`).join('');return `<tr><td>${esc(v.received_at)}</td><td>${fmt(v.uptime_s,0)}</td><td>${fmt(v.temp_c)}</td><td>${fmt(v.gyro_z_dps)}</td><td>${fmt(v.gyro_z_angle_deg)}</td><td>${fmt(v.command_value,0)}</td><td>${fmt(v.random,0)}</td>${pd}<td>${fmt(v.photoreflector_adc,0)}</td></tr>`}).join('')};"
"function saveWithAngle(){if(!current?.history?.length)return;const rows=['type,received_at,event_text_encoded,uptime_s,temp_c,gyro_z_dps,gyro_z_angle_deg,command_value,random,pd0,pd1,pd2,pd3,photoreflector_adc,wifi_signal_quality'];current.history.forEach(v=>rows.push([v.type||'telemetry',v.received_at,encodeURIComponent(v.event_text||''),v.uptime_s,v.temp_c,num(v.gyro_z_dps)?v.gyro_z_dps:'NA',num(v.gyro_z_angle_deg)?v.gyro_z_angle_deg:'NA',v.command_value,v.random,...(v.photodiode_adc||[]),num(v.photoreflector_adc)?v.photoreflector_adc:'NA',num(v.wifi_signal_quality)?v.wifi_signal_quality:'NA'].join(',')));const a=document.createElement('a');a.href=URL.createObjectURL(new Blob([rows.join('\\n')],{type:'text/csv'}));a.download='pico-telemetry.csv';a.click();URL.revokeObjectURL(a.href)}"
"function loadWithAngle(file){const r=new FileReader();r.onload=()=>{const rows=String(r.result).trim().split(/\\r?\\n/),head=rows.shift().split(','),at=(a,k)=>a[head.indexOf(k)],n=(a,k)=>{const v=at(a,k);return v===undefined||v==='NA'?null:Number(v)},h=rows.filter(Boolean).map(z=>{const a=z.split(','),encoded=at(a,'event_text_encoded');return{type:at(a,'type')||'telemetry',received_at:at(a,'received_at')||'',event_text:encoded?decodeURIComponent(encoded):'',uptime_s:n(a,'uptime_s'),temp_c:n(a,'temp_c'),gyro_z_dps:n(a,'gyro_z_dps'),gyro_z_angle_deg:n(a,'gyro_z_angle_deg'),command_value:n(a,'command_value'),random:n(a,'random'),photodiode_adc:['pd0','pd1','pd2','pd3'].map(k=>n(a,k)),photoreflector_adc:n(a,'photoreflector_adc'),wifi_signal_quality:n(a,'wifi_signal_quality')}});loaded=true;graphEndTime=null;cameraShown='';render({dummy_mode:false,link_status:'disconnected',last_telemetry_age_s:null,latest:[...h].reverse().find(v=>v.type==='telemetry')||null,history:h})};r.readAsText(file)}"
"$('save').onclick=saveWithAngle;$('load').onchange=e=>e.target.files[0]&&loadWithAngle(e.target.files[0]);"
"const compactPlot=plot;plot=(id,h,series)=>$(id)?compactPlot(id,h,series):undefined;const renderWithAngleHistory=render;render=function(d){renderWithAngleHistory(d);const telemetry=(d.history||[]).filter(v=>v.type==='telemetry'),windowed=graphWindow(telemetry);plot('gyro-chart',windowed,[['gyro',v=>v.gyro_z_dps,'gyro']]);plot('gyro-angle-chart',windowed,[['gyro angle',v=>v.gyro_z_angle_deg,'gyroangle']]);plot('pd-chart',windowed,[['pd0',v=>v.photodiode_adc?.[0],'pd0'],['pd1',v=>v.photodiode_adc?.[1],'pd1'],['pd2',v=>v.photodiode_adc?.[2],'pd2'],['pd3',v=>v.photodiode_adc?.[3],'pd3']]);plot('photoreflector-chart',windowed,[['photoreflector',v=>v.photoreflector_adc,'photoreflector']])};"
"function submitCommandInput(){const input=$('command-input'),text=input.value.trim();if(text.toLowerCase()==='c'){input.value='';sendCommand('CAPTURE')}else sendCommand()}$('send-command').onclick=submitCommandInput;$('command-input').onkeydown=e=>{if(e.key==='Enter'){e.preventDefault();submitCommandInput()}};"
"function sendWheelSpeed(){const input=$('wheel-speed'),speed=Number(input.value);if(!Number.isInteger(speed)||speed<-100||speed>100){$('status').textContent='Wheel speed must be an integer between -100 and +100 percent';input.focus();return}sendCommand('SET_VALUE,'+speed)}function sendRelativeSlew(){const input=$('relative-slew-angle'),angle=Number(input.value);if(!Number.isFinite(angle)||angle<-360||angle>360){$('status').textContent='Relative angle must be between -360 and +360 degrees';input.focus();return}sendCommand('SLEW_REL,'+angle)}$('wheel-set').onclick=sendWheelSpeed;$('wheel-speed').onkeydown=e=>{if(e.key==='Enter'){e.preventDefault();sendWheelSpeed()}};$('wheel-stop').onclick=()=>{ $('wheel-speed').value='0';sendCommand('SET_VALUE,0')};$('slew-abort').onclick=()=>sendCommand('SLEW_ABORT');$('angle-reset').onclick=()=>sendCommand('ANGLE_RESET');$('relative-slew').onclick=sendRelativeSlew;$('relative-slew-angle').onkeydown=e=>{if(e.key==='Enter'){e.preventDefault();sendRelativeSlew()}};"
"function cameraFileTime(name){const m=/^camera_(\\d{4})(\\d{2})(\\d{2})_(\\d{2})(\\d{2})(\\d{2})_(\\d{3})\\.(?:bmp|jpe?g)$/i.exec(String(name||''));return m?new Date(Number(m[1]),Number(m[2])-1,Number(m[3]),Number(m[4]),Number(m[5]),Number(m[6]),Number(m[7])).getTime():NaN}const renderCameraWithInterval=renderCamera;renderCamera=function(d){renderCameraWithInterval(d);const files=cameraNames(d),latest=d.latest_camera||'';if(latest&&files.indexOf(latest)<0)files.push(latest);const index=files.indexOf(selectedCamera);if(index<0)return;let text=(index+1)+' / '+files.length;if(index>0){const elapsed=cameraFileTime(selectedCamera)-cameraFileTime(files[index-1]);if(Number.isFinite(elapsed)&&elapsed>=0)text+=' · +'+(elapsed/1000).toFixed(elapsed<10000?3:1)+' s since previous'}$('camera-count').textContent=text};"
"$('burst-start').onclick=()=>{if(sequenceRunning){setSequenceStatus('Stop the current sequence first');return}const interval=Number($('burst-interval').value),count=Math.floor(Number($('burst-count').value));if(!Number.isFinite(interval)||interval<0||!Number.isFinite(count)||count<1){setSequenceStatus('Set a non-negative interval and a count of 1 or more');return}const safeCount=Math.min(count,200);sequenceSteps=Array.from({length:safeCount},(_,i)=>({delay:i?interval:0,command:'c'}));renderSequence();sequenceIndex=0;sequenceRunning=true;setSequenceStatus('Capture burst: '+safeCount+' shots every '+interval+' s');runSequenceNext()};"
"</script></body></html>";

static int send_all(SOCKET socket, const char *data, int length) {
    int total = 0;
    while (total < length) {
        int sent = send(socket, data + total, length - total, 0);
        if (sent == SOCKET_ERROR || sent == 0) return SOCKET_ERROR;
        total += sent;
    }
    return total;
}

static const char *event_type_name(event_type_t type) {
    switch (type) {
        case EVENT_TELEMETRY: return "telemetry";
        case EVENT_PC_COMMAND: return "pc_command";
        case EVENT_PICO_REPLY: return "pico_reply";
        case EVENT_CONNECTION: return "connection";
        case EVENT_CAMERA: return "camera";
        default: return "unknown";
    }
}

/* Returns the Windows Wi-Fi quality (0-100) only for the Pico AP, or -1 when unavailable. */
static int pico_wifi_signal_quality(void) {
    HANDLE handle = NULL;
    DWORD version;
    WLAN_INTERFACE_INFO_LIST *interfaces = NULL;
    int quality = -1;

    if (WlanOpenHandle(2, NULL, &version, &handle) != ERROR_SUCCESS) return -1;
    if (WlanEnumInterfaces(handle, NULL, &interfaces) != ERROR_SUCCESS) {
        WlanCloseHandle(handle, NULL);
        return -1;
    }

    for (DWORD i = 0; i < interfaces->dwNumberOfItems && quality < 0; ++i) {
        WLAN_CONNECTION_ATTRIBUTES *connection = NULL;
        DWORD size = 0;
        if (interfaces->InterfaceInfo[i].isState != wlan_interface_state_connected ||
            WlanQueryInterface(handle, &interfaces->InterfaceInfo[i].InterfaceGuid,
                               wlan_intf_opcode_current_connection, NULL, &size,
                               (PVOID *)&connection, NULL) != ERROR_SUCCESS) continue;
        DOT11_SSID ssid = connection->wlanAssociationAttributes.dot11Ssid;
        if (ssid.uSSIDLength == strlen(PICO_AP_SSID) &&
            memcmp(ssid.ucSSID, PICO_AP_SSID, ssid.uSSIDLength) == 0) {
            quality = (int)connection->wlanAssociationAttributes.wlanSignalQuality;
        }
        WlanFreeMemory(connection);
    }
    WlanFreeMemory(interfaces);
    WlanCloseHandle(handle, NULL);
    return quality;
}

static bool field(const char *line, const char *name, char *out, size_t out_size) {
    const char *p = line;
    size_t name_length = strlen(name);
    while (*p) {
        const char *end = strchr(p, ',');
        size_t length = end ? (size_t)(end - p) : strlen(p);
        if (length > name_length && strncmp(p, name, name_length) == 0 &&
            p[name_length] == '=') {
            size_t value_length = length - name_length - 1;
            if (value_length >= out_size) return false;
            memcpy(out, p + name_length + 1, value_length);
            out[value_length] = '\0';
            return true;
        }
        if (!end) break;
        p = end + 1;
    }
    return false;
}

static bool parse_telemetry(const char *line, telemetry_entry_t *entry) {
    char value[80];
    char *end;
    time_t now;
    struct tm local_time;

    if (strncmp(line, "TELEMETRY,", 10) != 0) return false;
    memset(entry, 0, sizeof(*entry));
    entry->type = EVENT_TELEMETRY;
    entry->wifi_signal_quality = pico_wifi_signal_quality();
    now = time(NULL);
    localtime_s(&local_time, &now);
    strftime(entry->received_at, sizeof(entry->received_at), "%H:%M:%S", &local_time);
    if (!field(line, "uptime_s", value, sizeof(value))) return false;
    entry->uptime_s = strtoul(value, &end, 10);
    if (*end) return false;
    if (!field(line, "temp_c", value, sizeof(value))) return false;
    entry->temp_c = strtod(value, &end);
    if (*end) return false;
    if (field(line, "random", value, sizeof(value))) entry->random_value = strtoul(value, NULL, 10);
    if (field(line, "command_value", value, sizeof(value))) entry->command_value = strtol(value, NULL, 10);
    if (field(line, "gyro_z_dps", value, sizeof(value)) && strcmp(value, "NA") != 0) {
        entry->gyro_z_dps = strtod(value, &end);
        entry->gyro_z_valid = *end == '\0';
    }
    if (field(line, "gyro_z_angle_deg", value, sizeof(value)) && strcmp(value, "NA") != 0) {
        entry->gyro_z_angle_deg = strtod(value, &end);
        entry->gyro_z_angle_valid = *end == '\0';
    }
    if (field(line, "photodiode_adc", value, sizeof(value)) &&
        sscanf(value, "%u|%u|%u|%u", &entry->photodiode_adc[0],
               &entry->photodiode_adc[1], &entry->photodiode_adc[2],
               &entry->photodiode_adc[3]) != 4) return false;
    if (field(line, "photoreflector_adc", value, sizeof(value))) {
        entry->photoreflector_adc_value = (unsigned int)strtoul(value, &end, 10);
        entry->photoreflector_adc_valid = *end == '\0';
    }
    return true;
}

static void store_telemetry(const char *line) {
    telemetry_entry_t entry;
    if (!parse_telemetry(line, &entry)) {
        printf("\nInvalid telemetry ignored: %s\n", line);
        return;
    }
    EnterCriticalSection(&history_lock);
    entry.serial = next_history_serial++;
    history[history_next] = entry;
    history_next = (history_next + 1) % MAX_TELEMETRY_HISTORY;
    if (history_count < MAX_TELEMETRY_HISTORY) history_count++;
    last_telemetry_tick_ms = GetTickCount64();
    LeaveCriticalSection(&history_lock);
    autosave_history();
}

static void store_event(event_type_t type, const char *text) {
    telemetry_entry_t entry;
    time_t now = time(NULL);
    struct tm local_time;

    memset(&entry, 0, sizeof(entry));
    entry.type = type;
    entry.wifi_signal_quality = pico_wifi_signal_quality();
    localtime_s(&local_time, &now);
    strftime(entry.received_at, sizeof(entry.received_at), "%H:%M:%S", &local_time);
    snprintf(entry.event_text, sizeof(entry.event_text), "%s", text);
    for (size_t i = 0; entry.event_text[i] != '\0'; ++i) {
        if (entry.event_text[i] == '"' || entry.event_text[i] == '\\') {
            entry.event_text[i] = '/';
        } else if ((unsigned char)entry.event_text[i] < 0x20) {
            entry.event_text[i] = ' ';
        }
    }

    EnterCriticalSection(&history_lock);
    entry.serial = next_history_serial++;
    history[history_next] = entry;
    history_next = (history_next + 1) % MAX_TELEMETRY_HISTORY;
    if (history_count < MAX_TELEMETRY_HISTORY) history_count++;
    LeaveCriticalSection(&history_lock);
    autosave_history();
}

static void write_percent_encoded(FILE *file, const char *text) {
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            fputc(*p, file);
        } else {
            fprintf(file, "%%%02X", *p);
        }
    }
}

/* Start a separate, timestamped CSV for each new Pico connection. */
static void begin_session_save(void) {
    time_t now = time(NULL);
    struct tm local_time;
    char stamp[16];

    localtime_s(&local_time, &now);
    strftime(stamp, sizeof(stamp), "%m%d%H%M", &local_time);

    EnterCriticalSection(&autosave_lock);
    EnterCriticalSection(&history_lock);
    session_first_serial = next_history_serial;
    LeaveCriticalSection(&history_lock);
    session_save_filename[0] = '\0';
    for (unsigned int suffix = 0; suffix < 1000; ++suffix) {
        char candidate[MAX_PATH];
        if (suffix == 0) snprintf(candidate, sizeof(candidate), "%s.csv", stamp);
        else snprintf(candidate, sizeof(candidate), "%s_%02u.csv", stamp, suffix);
        if (GetFileAttributesA(candidate) == INVALID_FILE_ATTRIBUTES &&
            GetLastError() == ERROR_FILE_NOT_FOUND) {
            snprintf(session_save_filename, sizeof(session_save_filename), "%s", candidate);
            break;
        }
    }
    LeaveCriticalSection(&autosave_lock);

    if (session_save_filename[0]) {
        printf("Auto-save session: %s\n", session_save_filename);
    } else {
        printf("Auto-save failed: no available session filename\n");
    }
}

/* Atomically replace only the CSV belonging to the current connection session. */
static void autosave_history(void) {
    FILE *file;
    int oldest;

    EnterCriticalSection(&autosave_lock);
    if (!session_save_filename[0]) {
        LeaveCriticalSection(&autosave_lock);
        return;
    }
    file = fopen(SESSION_SAVE_TEMP_FILENAME, "wb");
    if (!file) {
        if (!autosave_error_reported) {
            printf("\nAuto-save failed: cannot write %s\n", session_save_filename);
            autosave_error_reported = true;
        }
        LeaveCriticalSection(&autosave_lock);
        return;
    }

    fprintf(file, "type,received_at,event_text_encoded,uptime_s,temp_c,gyro_z_dps,gyro_z_angle_deg,command_value,random,pd0,pd1,pd2,pd3,photoreflector_adc,wifi_signal_quality\n");
    EnterCriticalSection(&history_lock);
    oldest = (history_next - history_count + MAX_TELEMETRY_HISTORY) % MAX_TELEMETRY_HISTORY;
    for (int i = 0; i < history_count; ++i) {
        const telemetry_entry_t *entry = &history[(oldest + i) % MAX_TELEMETRY_HISTORY];
        if (entry->serial < session_first_serial) continue;
        fprintf(file, "%s,%s,", event_type_name(entry->type), entry->received_at);
        write_percent_encoded(file, entry->event_text);
        fprintf(file, ",%lu,%.2f,", entry->uptime_s, entry->temp_c);
        if (entry->gyro_z_valid) fprintf(file, "%.2f", entry->gyro_z_dps);
        else fputs("NA", file);
        fputc(',', file);
        if (entry->gyro_z_angle_valid) fprintf(file, "%.2f", entry->gyro_z_angle_deg);
        else fputs("NA", file);
        fprintf(file, ",%ld,%lu,%u,%u,%u,%u,",
                entry->command_value, entry->random_value,
                entry->photodiode_adc[0], entry->photodiode_adc[1],
                entry->photodiode_adc[2], entry->photodiode_adc[3]);
        if (entry->photoreflector_adc_valid) fprintf(file, "%u", entry->photoreflector_adc_value);
        else fputs("NA", file);
        fputc(',', file);
        if (entry->wifi_signal_quality >= 0) fprintf(file, "%d", entry->wifi_signal_quality);
        else fputs("NA", file);
        fputc('\n', file);
    }
    LeaveCriticalSection(&history_lock);

    if (fclose(file) != 0 || !MoveFileExA(SESSION_SAVE_TEMP_FILENAME, session_save_filename,
                                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (!autosave_error_reported) {
            printf("\nAuto-save failed: cannot update %s (is it open in Excel?)\n", session_save_filename);
            autosave_error_reported = true;
        }
    } else {
        autosave_error_reported = false;
    }
    LeaveCriticalSection(&autosave_lock);
}

static void display_pico_line(const char *line) {
    if (strncmp(line, "TELEMETRY,", 10) == 0) {
        store_telemetry(line);
        printf("\nTelemetry <- Pico: %s\n", line + 10);
    } else {
        store_event(EVENT_PICO_REPLY, line);
        printf("\nPico -> PC: %s\n", line);
    }
    printf("PC -> Pico > ");
    fflush(stdout);
}

static uint32_t crc32(const uint8_t *data, size_t size) {
    uint32_t crc = 0xffffffffu;
    while (size--) {
        crc ^= *data++;
        for (int i = 0; i < 8; ++i) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t size) {
    while (size--) {
        crc ^= *data++;
        for (int i = 0; i < 8; ++i) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

/* Streaming decoder for the run-length format produced by
 * rle_encode_rgb565_chunk() on the Pico (pico_satellite_controller/rle.c):
 * a run of 2-128 identical pixels is packed as {0x80|(run-1), lo, hi};
 * otherwise up to 128 pixels are stored literally as {count-1, pixel...}.
 *
 * The Pico streams compressed bytes without ever announcing the total
 * compressed length, so this decoder is fed one byte at a time (see
 * rle_decoder_feed below) and the caller just watches `produced` reach
 * the known decompressed frame size. */
typedef enum {
    RLE_DECODE_CONTROL = 0,
    RLE_DECODE_RUN_LO,
    RLE_DECODE_RUN_HI,
    RLE_DECODE_LITERAL,
} rle_decode_phase_t;

typedef struct {
    rle_decode_phase_t phase;
    size_t run_pixels_remaining; /* RLE_DECODE_RUN_HI: pixels left to expand */
    size_t literal_bytes_remaining;
    uint8_t run_lo;
    uint8_t *out;
    size_t out_capacity;
    size_t produced;
    bool overflowed;
} rle_decoder_t;

typedef enum {
    IMAGE_ENCODING_RAW_RGB565,
    IMAGE_ENCODING_RLE_RGB565,
    IMAGE_ENCODING_JPEG,
} image_encoding_t;

static void rle_decoder_reset(rle_decoder_t *dec, uint8_t *out, size_t out_capacity) {
    dec->phase = RLE_DECODE_CONTROL;
    dec->run_pixels_remaining = 0;
    dec->literal_bytes_remaining = 0;
    dec->run_lo = 0;
    dec->out = out;
    dec->out_capacity = out_capacity;
    dec->produced = 0;
    dec->overflowed = false;
}

/* Feeds one compressed byte into the decoder. Sets dec->overflowed if the
 * stream would produce more than out_capacity bytes (a framing bug or
 * transmission corruption), after which the frame should be discarded. */
static void rle_decoder_feed(rle_decoder_t *dec, uint8_t byte) {
    if (dec->overflowed) return;

    switch (dec->phase) {
    case RLE_DECODE_CONTROL:
        if (byte & 0x80u) {
            dec->run_pixels_remaining = (size_t)(byte & 0x7fu) + 1u;
            dec->phase = RLE_DECODE_RUN_LO;
        } else {
            dec->literal_bytes_remaining = ((size_t)(byte & 0x7fu) + 1u) * 2u;
            dec->phase = RLE_DECODE_LITERAL;
        }
        break;
    case RLE_DECODE_RUN_LO:
        dec->run_lo = byte;
        dec->phase = RLE_DECODE_RUN_HI;
        break;
    case RLE_DECODE_RUN_HI: {
        size_t bytes_needed = dec->run_pixels_remaining * 2u;
        if (dec->produced + bytes_needed > dec->out_capacity) {
            dec->overflowed = true;
            break;
        }
        for (size_t k = 0; k < dec->run_pixels_remaining; ++k) {
            dec->out[dec->produced++] = dec->run_lo;
            dec->out[dec->produced++] = byte;
        }
        dec->phase = RLE_DECODE_CONTROL;
        break;
    }
    case RLE_DECODE_LITERAL:
        if (dec->produced + 1u > dec->out_capacity) {
            dec->overflowed = true;
            break;
        }
        dec->out[dec->produced++] = byte;
        if (--dec->literal_bytes_remaining == 0) {
            dec->phase = RLE_DECODE_CONTROL;
        }
        break;
    }
}

static void put_le16(uint8_t *destination, uint16_t value) {
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *destination, uint32_t value) {
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
    destination[2] = (uint8_t)(value >> 16);
    destination[3] = (uint8_t)(value >> 24);
}

/* Images always go in captures beside the executable, not the launch folder. */
static bool build_camera_path(char *path, size_t path_size, const SYSTEMTIME *now,
                              const char *extension) {
    char executable_path[MAX_PATH];
    char capture_directory[MAX_PATH];
    char *separator;
    DWORD attributes;
    DWORD length = GetModuleFileNameA(NULL, executable_path, sizeof(executable_path));
    int written;

    if (!now || !extension || length == 0 || length >= sizeof(executable_path)) return false;
    separator = strrchr(executable_path, '\\');
    if (!separator) return false;
    *separator = '\0';
    written = snprintf(capture_directory, sizeof(capture_directory), "%s\\captures", executable_path);
    if (written < 0 || (size_t)written >= sizeof(capture_directory)) return false;
    attributes = GetFileAttributesA(capture_directory);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (!CreateDirectoryA(capture_directory, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    } else if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        return false;
    }
    written = snprintf(path, path_size, "%s\\camera_%04u%02u%02u_%02u%02u%02u_%03u%s",
                       capture_directory, now->wYear, now->wMonth, now->wDay, now->wHour,
                       now->wMinute, now->wSecond, now->wMilliseconds, extension);
    return written > 0 && (size_t)written < path_size;
}

/* Only files created by this program can be requested through /camera/<name>. */
static bool camera_filename_is_safe(const char *filename) {
    size_t length;
    const char *extension;

    if (!filename) return false;
    length = strlen(filename);
    extension = strrchr(filename, '.');
    if (length <= strlen("camera_.bmp") || strncmp(filename, "camera_", 7) != 0 ||
        extension == NULL ||
        (strcmp(extension, ".bmp") != 0 && strcmp(extension, ".jpg") != 0 &&
         strcmp(extension, ".jpeg") != 0)) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)filename[i];
        if (!(isalnum(c) || c == '_' || c == '.')) return false;
    }
    return true;
}

static bool build_saved_camera_path(char *path, size_t path_size, const char *filename) {
    char executable_path[MAX_PATH];
    char *separator;
    DWORD length = GetModuleFileNameA(NULL, executable_path, sizeof(executable_path));
    int written;

    if (!camera_filename_is_safe(filename) || length == 0 || length >= sizeof(executable_path)) return false;
    separator = strrchr(executable_path, '\\');
    if (!separator) return false;
    *separator = '\0';
    written = snprintf(path, path_size, "%s\\captures\\%s", executable_path, filename);
    return written > 0 && (size_t)written < path_size;
}

/* Convert the OV7675 RGB565 payload to a browser-displayable BMP. */
static bool save_rgb565_bmp(const uint8_t *frame, unsigned width, unsigned height,
                             char *filename, size_t filename_size) {
    SYSTEMTIME now;
    uint32_t row_size, pixel_bytes;
    uint8_t header[54] = {0};
    uint8_t *pixels;
    FILE *file;

    if (!frame || width == 0 || height == 0 || width > CAMERA_MAX_WIDTH ||
        height > CAMERA_MAX_HEIGHT) return false;
    GetLocalTime(&now);
    if (!build_camera_path(filename, filename_size, &now, ".bmp")) return false;
    file = fopen(filename, "wb");
    if (!file) return false;

    row_size = (width * 3u + 3u) & ~3u;
    pixel_bytes = row_size * height;
    pixels = (uint8_t *)calloc(1, pixel_bytes);
    if (!pixels) {
        fclose(file);
        DeleteFileA(filename);
        return false;
    }
    header[0] = 'B'; header[1] = 'M';
    put_le32(header + 2, 54u + pixel_bytes);
    put_le32(header + 10, 54u);
    put_le32(header + 14, 40u);
    put_le32(header + 18, width);
    put_le32(header + 22, height);
    put_le16(header + 26, 1u);
    put_le16(header + 28, 24u);
    put_le32(header + 34, pixel_bytes);
    if (fwrite(header, 1, sizeof(header), file) != sizeof(header)) {
        free(pixels);
        fclose(file);
        DeleteFileA(filename);
        return false;
    }

    for (unsigned output_y = 0; output_y < height; ++output_y) {
        unsigned source_y = height - 1u - output_y;
        for (unsigned x = 0; x < width; ++x) {
            size_t position = ((size_t)source_y * width + x) * 2u;
            /* PIO/DMA stores this camera's RGB565 bytes as low byte then high byte. */
            uint16_t pixel = ((uint16_t)frame[position + 1] << 8) | frame[position];
            size_t output_position = (size_t)output_y * row_size + x * 3u;
            pixels[output_position] =
                (uint8_t)(((pixel & 0x1fu) * 255u) / 31u);
            pixels[output_position + 1u] =
                (uint8_t)((((pixel >> 5) & 0x3fu) * 255u) / 63u);
            pixels[output_position + 2u] =
                (uint8_t)((((pixel >> 11) & 0x1fu) * 255u) / 31u);
        }
    }
    if (fwrite(pixels, 1, pixel_bytes, file) != pixel_bytes) {
        free(pixels);
        fclose(file);
        DeleteFileA(filename);
        return false;
    }
    free(pixels);
    if (fclose(file) == 0) return true;
    DeleteFileA(filename);
    return false;
}

static void register_camera_file(const char *path) {
    char message[320];
    const char *filename = strrchr(path, '\\');
    filename = filename ? filename + 1 : path;
    EnterCriticalSection(&camera_lock);
    snprintf(latest_camera_path, sizeof(latest_camera_path), "%s", path);
    snprintf(latest_camera_filename, sizeof(latest_camera_filename), "%s", filename);
    LeaveCriticalSection(&camera_lock);
    snprintf(message, sizeof(message), "Camera image saved: %s", filename);
    store_event(EVENT_CAMERA, message);
}

static bool append(char *buffer, size_t capacity, size_t *length, const char *format, ...) {
    va_list args;
    int written;
    if (*length >= capacity) return false;
    va_start(args, format);
    written = vsnprintf(buffer + *length, capacity - *length, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= capacity - *length) return false;
    *length += (size_t)written;
    return true;
}

static bool append_entry(char *buffer, size_t cap, size_t *length, const telemetry_entry_t *e) {
    if (!append(buffer, cap, length,
                "{\"type\":\"%s\",\"received_at\":\"%s\",\"event_text\":\"%s\",\"uptime_s\":%lu,\"temp_c\":%.2f,\"random\":%lu,\"command_value\":%ld,\"gyro_z_dps\":",
                event_type_name(e->type), e->received_at, e->event_text, e->uptime_s, e->temp_c,
                e->random_value, e->command_value)) return false;
    if (e->gyro_z_valid) {
        if (!append(buffer, cap, length, "%.2f", e->gyro_z_dps)) return false;
    } else if (!append(buffer, cap, length, "null")) return false;
    if (!append(buffer, cap, length, ",\"gyro_z_angle_deg\":")) return false;
    if (e->gyro_z_angle_valid) {
        if (!append(buffer, cap, length, "%.2f", e->gyro_z_angle_deg)) return false;
    } else if (!append(buffer, cap, length, "null")) return false;
    if (!append(buffer, cap, length, ",\"photodiode_adc\":[%u,%u,%u,%u],\"photoreflector_adc\":",
                   e->photodiode_adc[0], e->photodiode_adc[1],
                   e->photodiode_adc[2], e->photodiode_adc[3])) return false;
    if (e->photoreflector_adc_valid) {
        if (!append(buffer, cap, length, "%u", e->photoreflector_adc_value)) return false;
    } else if (!append(buffer, cap, length, "null")) return false;
    if (!append(buffer, cap, length, ",\"wifi_signal_quality\":")) return false;
    if (e->wifi_signal_quality >= 0) return append(buffer, cap, length, "%d}", e->wifi_signal_quality);
    return append(buffer, cap, length, "null}");
}

static char *telemetry_json(void) {
    char *json = malloc(HTTP_JSON_CAPACITY);
    size_t length = 0;
    LONG connected;
    ULONGLONG now, last;
    const char *status;
    int oldest;
    char camera_filename[MAX_PATH];
    if (!json) return NULL;
    EnterCriticalSection(&camera_lock);
    snprintf(camera_filename, sizeof(camera_filename), "%s", latest_camera_filename);
    LeaveCriticalSection(&camera_lock);
    EnterCriticalSection(&history_lock);
    connected = InterlockedCompareExchange(&pico_connected, 0, 0);
    now = GetTickCount64();
    last = last_telemetry_tick_ms;
    status = !connected ? (InterlockedCompareExchange(&pico_ever_connected, 0, 0) ? "reconnecting" : "disconnected") :
             (!last ? "waiting" : now - last > TELEMETRY_STALE_MS ? "stale" : "receiving");
    if (!append(json, HTTP_JSON_CAPACITY, &length,
                "{\"pico_connected\":%s,\"dummy_mode\":%s,\"link_status\":\"%s\",\"last_telemetry_age_s\":",
                connected ? "true" : "false", dummy_mode ? "true" : "false", status) ||
        (!last && !append(json, HTTP_JSON_CAPACITY, &length, "null")) ||
        (last && !append(json, HTTP_JSON_CAPACITY, &length, "%.1f", (double)(now - last) / 1000.0)) ||
        !append(json, HTTP_JSON_CAPACITY, &length, ",\"latest_camera\":")) goto fail;
    if (camera_filename[0]) {
        if (!append(json, HTTP_JSON_CAPACITY, &length, "\"%s\"", camera_filename)) goto fail;
    } else if (!append(json, HTTP_JSON_CAPACITY, &length, "null")) goto fail;
    if (!append(json, HTTP_JSON_CAPACITY, &length, ",\"latest\":")) goto fail;
    if (history_count) {
        bool found_telemetry = false;
        for (int i = 0; i < history_count; ++i) {
            int latest = (history_next - 1 - i + MAX_TELEMETRY_HISTORY) % MAX_TELEMETRY_HISTORY;
            if (history[latest].type == EVENT_TELEMETRY) {
                if (!append_entry(json, HTTP_JSON_CAPACITY, &length, &history[latest])) goto fail;
                found_telemetry = true;
                break;
            }
        }
        if (!found_telemetry && !append(json, HTTP_JSON_CAPACITY, &length, "null")) goto fail;
    } else if (!append(json, HTTP_JSON_CAPACITY, &length, "null")) goto fail;
    if (!append(json, HTTP_JSON_CAPACITY, &length, ",\"history\":[")) goto fail;
    oldest = (history_next - history_count + MAX_TELEMETRY_HISTORY) % MAX_TELEMETRY_HISTORY;
    for (int i = 0; i < history_count; ++i) {
        int index = (oldest + i) % MAX_TELEMETRY_HISTORY;
        if ((i && !append(json, HTTP_JSON_CAPACITY, &length, ",")) ||
            !append_entry(json, HTTP_JSON_CAPACITY, &length, &history[index])) goto fail;
    }
    if (!append(json, HTTP_JSON_CAPACITY, &length, "]}")) goto fail;
    LeaveCriticalSection(&history_lock);
    return json;
fail:
    LeaveCriticalSection(&history_lock);
    free(json);
    return NULL;
}

static void http_reply(SOCKET client, const char *type, const char *body) {
    char header[256];
    int length = snprintf(header, sizeof(header),
                          "HTTP/1.1 200 OK\r\nContent-Type: %s; charset=utf-8\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                          type, strlen(body));
    if (length > 0 && length < (int)sizeof(header)) {
        send_all(client, header, length);
        send_all(client, body, (int)strlen(body));
    }
}

static void http_reply_board_image(SOCKET client) {
    FILE *file = _wfopen(BOARD_IMAGE_PATH, L"rb");
    char header[256];
    char buffer[8192];
    long size;

    if (!file) {
        http_reply(client, "text/plain", "Board image is unavailable");
        return;
    }
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    rewind(file);
    if (size < 0 || snprintf(header, sizeof(header),
                             "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: %ld\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                             size) <= 0) {
        fclose(file);
        return;
    }
    if (send_all(client, header, (int)strlen(header)) != SOCKET_ERROR) {
        size_t read;
        while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
            if (send_all(client, buffer, (int)read) == SOCKET_ERROR) break;
        }
    }
    fclose(file);
}

static void http_reply_camera_path(SOCKET client, const char *path) {
    char header[256];
    char buffer[8192];
    const char *content_type;
    long size;
    FILE *file;

    if (!path[0] || !(file = fopen(path, "rb"))) {
        http_reply(client, "text/plain", "No camera image has been received");
        return;
    }
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    rewind(file);
    content_type = strrchr(path, '.') != NULL &&
                   (_stricmp(strrchr(path, '.'), ".jpg") == 0 ||
                    _stricmp(strrchr(path, '.'), ".jpeg") == 0)
                       ? "image/jpeg" : "image/bmp";
    if (size < 0 || snprintf(header, sizeof(header),
                             "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                             content_type, size) <= 0) {
        fclose(file);
        return;
    }
    if (send_all(client, header, (int)strlen(header)) != SOCKET_ERROR) {
        size_t read;
        while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
            if (send_all(client, buffer, (int)read) == SOCKET_ERROR) break;
        }
    }
    fclose(file);
}

static void http_reply_latest_camera(SOCKET client) {
    char path[MAX_PATH];

    EnterCriticalSection(&camera_lock);
    snprintf(path, sizeof(path), "%s", latest_camera_path);
    LeaveCriticalSection(&camera_lock);
    http_reply_camera_path(client, path);
}

static void http_reply_saved_camera(SOCKET client, const char *request) {
    static const char prefix[] = "GET /camera/";
    char filename[MAX_PATH];
    char path[MAX_PATH];
    const char *name = request + sizeof(prefix) - 1;
    const char *end = strchr(name, ' ');
    size_t length;

    if (!end || end == name) {
        http_reply(client, "text/plain", "Camera image is unavailable");
        return;
    }
    length = (size_t)(end - name);
    if (length >= sizeof(filename)) {
        http_reply(client, "text/plain", "Camera image is unavailable");
        return;
    }
    memcpy(filename, name, length);
    filename[length] = '\0';
    if (!build_saved_camera_path(path, sizeof(path), filename)) {
        http_reply(client, "text/plain", "Camera image is unavailable");
        return;
    }
    http_reply_camera_path(client, path);
}

static int receive_http_request(SOCKET client, char *request, size_t capacity) {
    size_t used = 0;
    for (;;) {
        char *body;
        size_t required;
        int received;

        if (used >= capacity - 1) return SOCKET_ERROR;
        received = recv(client, request + used, (int)(capacity - 1 - used), 0);
        if (received <= 0) return SOCKET_ERROR;
        used += (size_t)received;
        request[used] = '\0';

        body = strstr(request, "\r\n\r\n");
        if (!body) continue;
        required = (size_t)(body + 4 - request);
        char *content_length = strstr(request, "Content-Length:");
        if (content_length && content_length < body) {
            char *end;
            unsigned long value = strtoul(content_length + 15, &end, 10);
            if (end == content_length + 15 || value > capacity - required - 1) return SOCKET_ERROR;
            required += value;
        }
        if (used >= required) return (int)used;
    }
}

static DWORD WINAPI http_client(LPVOID parameter) {
    SOCKET client = (SOCKET)(UINT_PTR)parameter;
    char request[1024];
    int received = receive_http_request(client, request, sizeof(request));
    if (received > 0) {
        if (strncmp(request, "GET /api/telemetry ", 19) == 0) {
            char *json = telemetry_json();
            if (json) { http_reply(client, "application/json", json); free(json); }
        } else if (strncmp(request, "POST /api/command ", 18) == 0) {
            char *body = strstr(request, "\r\n\r\n");
            bool ok = body != NULL && send_ground_command(body + 4);
            http_reply(client, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
        } else if (strncmp(request, "GET /board-image.png ", 21) == 0) {
            http_reply_board_image(client);
        } else if (strncmp(request, "GET /camera/latest.bmp", 22) == 0 &&
                   (request[22] == ' ' || request[22] == '?')) {
            http_reply_latest_camera(client);
        } else if (strncmp(request, "GET /camera/", 12) == 0) {
            http_reply_saved_camera(client, request);
        } else http_reply(client, "text/html", DASHBOARD_HTML);
    }
    closesocket(client);
    return 0;
}

static SOCKET listener(unsigned short port) {
    SOCKET listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in address;
    int reuse = 1;
    if (listen_socket == INVALID_SOCKET) return INVALID_SOCKET;
    setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(listen_socket, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR ||
        listen(listen_socket, SOMAXCONN) == SOCKET_ERROR) { closesocket(listen_socket); return INVALID_SOCKET; }
    return listen_socket;
}

static DWORD WINAPI http_server(LPVOID parameter) {
    SOCKET server = *(SOCKET *)parameter;
    for (;;) {
        SOCKET client = accept(server, NULL, NULL);
        if (client == INVALID_SOCKET) return 0;
        HANDLE thread = CreateThread(NULL, 0, http_client, (LPVOID)(UINT_PTR)client, 0, NULL);
        if (thread) CloseHandle(thread); else closesocket(client);
    }
}

static bool generate_dummy_camera_image(void) {
    const unsigned width = 160, height = 120;
    const size_t size = (size_t)width * height * 2u;
    uint8_t *frame = malloc(size);
    char filename[MAX_PATH];
    bool saved;

    if (!frame) return false;
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            uint16_t red = (uint16_t)((x * 31u) / (width - 1u));
            uint16_t green = (uint16_t)((y * 63u) / (height - 1u));
            uint16_t blue = (uint16_t)(((x + y) * 31u) / (width + height - 2u));
            uint16_t pixel = (uint16_t)((red << 11) | (green << 5) | blue);
            size_t position = ((size_t)y * width + x) * 2u;
            frame[position] = (uint8_t)pixel;
            frame[position + 1] = (uint8_t)(pixel >> 8);
        }
    }
    saved = save_rgb565_bmp(frame, width, height, filename, sizeof(filename));
    free(frame);
    if (saved) register_camera_file(filename);
    return saved;
}

static void dummy_command(const char *command) {
    char reply[128];
    if (strcmp(command, "CAPTURE") == 0 || strcmp(command, "CAPTURE_TEST") == 0) {
        snprintf(reply, sizeof(reply), generate_dummy_camera_image() ?
                 "CAMERA_CAPTURED" : "ERROR,CAMERA_SAVE_FAILED");
    } else if (strncmp(command, "SET_VALUE,", 10) == 0) {
        char *end;
        long value = strtol(command + 10, &end, 10);
        if (*end == '\n' || *end == '\r' || *end == '\0') {
            InterlockedExchange(&dummy_command_value, value);
            snprintf(reply, sizeof(reply), "ACK,SET_VALUE,%ld", value);
        } else snprintf(reply, sizeof(reply), "ERROR,INVALID_VALUE");
    } else if (strncmp(command, "GET_VALUE", 9) == 0) {
        snprintf(reply, sizeof(reply), "VALUE,%ld", InterlockedCompareExchange(&dummy_command_value, 0, 0));
    } else {
        snprintf(reply, sizeof(reply), "PICO_REPLY: %s", command);
    }
    store_event(EVENT_PICO_REPLY, reply);
    printf("\nDummy -> PC: %s\n", reply);
}

static bool send_ground_command(const char *raw_command) {
    char command[256];
    char message[258];
    size_t length = strcspn(raw_command, "\r\n");

    if (length == 0 || length >= sizeof(command)) return false;
    memcpy(command, raw_command, length);
    command[length] = '\0';

    EnterCriticalSection(&send_lock);
    if (!dummy_mode && active_pico_client == INVALID_SOCKET) {
        LeaveCriticalSection(&send_lock);
        return false;
    }

    store_event(EVENT_PC_COMMAND, command);
    if (dummy_mode) {
        dummy_command(command);
        LeaveCriticalSection(&send_lock);
        return true;
    }

    snprintf(message, sizeof(message), "%s\n", command);
    if (send_all(active_pico_client, message, (int)strlen(message)) == SOCKET_ERROR) {
        LeaveCriticalSection(&send_lock);
        return false;
    }
    LeaveCriticalSection(&send_lock);
    return true;
}

static DWORD WINAPI dummy_telemetry(LPVOID parameter) {
    unsigned long sample = 0, random_value = 13579;
    (void)parameter;
    InterlockedExchange(&pico_connected, 1);
    while (InterlockedCompareExchange(&dummy_running, 0, 0)) {
        char line[256];
        int temp = 2450 + (int)(sample % 21) * 5;
        int gyro = ((int)(sample % 33) - 16) * 125;
        int gyro_abs = abs(gyro);
        int gyro_angle = ((int)(sample * 175) % 36000) - 18000;
        int gyro_angle_abs = abs(gyro_angle);
        unsigned int pd0 = 1100 + (sample * 37) % 900;
        unsigned int pd1 = 1300 + (sample * 53) % 850;
        unsigned int pd2 = 1500 + (sample * 71) % 700;
        unsigned int pd3 = 1700 + (sample * 29) % 600;
        unsigned int photoreflector = 300 + (sample * 83) % 3700;
        random_value = random_value * 1103515245u + 12345u;
        snprintf(line, sizeof(line),
                 "TELEMETRY,uptime_s=%lu,temp_c=%d.%02d,random=%lu,command_value=%ld,gyro_z_dps=%s%d.%02d,gyro_z_angle_deg=%s%d.%02d,photodiode_adc=%u|%u|%u|%u,photoreflector_adc=%u",
                 sample * 2, temp / 100, temp % 100, random_value % 1000,
                 InterlockedCompareExchange(&dummy_command_value, 0, 0), gyro < 0 ? "-" : "",
                 gyro_abs / 100, gyro_abs % 100, gyro_angle < 0 ? "-" : "",
                 gyro_angle_abs / 100, gyro_angle_abs % 100, pd0, pd1, pd2, pd3, photoreflector);
        display_pico_line(line);
        sample++;
        Sleep(2000);
    }
    InterlockedExchange(&pico_connected, 0);
    return 0;
}

static void finish_camera_frame(const uint8_t *frame, size_t frame_size,
                                unsigned width, unsigned height,
                                uint32_t expected_crc) {
    uint32_t actual_crc = crc32(frame, frame_size);

    if (actual_crc != expected_crc) {
        store_event(EVENT_CAMERA, "Camera image rejected: CRC mismatch");
        printf("\nCamera image rejected: CRC mismatch (expected %08lx, got %08lx)\n",
               (unsigned long)expected_crc, (unsigned long)actual_crc);
        return;
    }

    char filename[MAX_PATH];
    if (save_rgb565_bmp(frame, width, height, filename, sizeof(filename))) {
        register_camera_file(filename);
        printf("\nCamera image saved: %s\n", filename);
    } else {
        store_event(EVENT_CAMERA, "Camera image could not be saved");
        printf("\nCamera image could not be saved\n");
    }
}

static void finish_camera_jpeg(FILE **file, const char *filename,
                               bool write_failed, bool check_crc,
                               uint32_t expected_crc, uint32_t actual_crc) {
    bool saved = !write_failed && file != NULL && *file != NULL &&
                 fclose(*file) == 0;
    if (file != NULL) *file = NULL;

    if (!saved || (check_crc && actual_crc != expected_crc)) {
        if (filename != NULL && filename[0] != '\0') DeleteFileA(filename);
        if (check_crc && actual_crc != expected_crc) {
            store_event(EVENT_CAMERA, "Camera JPEG rejected: CRC mismatch");
            printf("\nCamera JPEG rejected: CRC mismatch (expected %08lx, got %08lx)\n",
                   (unsigned long)expected_crc, (unsigned long)actual_crc);
        } else {
            store_event(EVENT_CAMERA, "Camera JPEG could not be saved");
            printf("\nCamera JPEG could not be saved\n");
        }
        return;
    }

    register_camera_file(filename);
    printf("\nCamera image saved: %s\n", filename);
}

static DWORD WINAPI receive_pico(LPVOID parameter) {
    SOCKET client = *(SOCKET *)parameter;
    uint8_t data[8192];
    char line[768];
    size_t line_length = 0;
    uint8_t *frame = NULL; /* decoded (raw RGB565) pixel buffer, while receiving */
    unsigned frame_width = 0;
    unsigned frame_height = 0;
    size_t frame_size = 0;
    size_t raw_bytes_received = 0;
    image_encoding_t image_encoding = IMAGE_ENCODING_RAW_RGB565;
    uint32_t expected_crc = 0;
    rle_decoder_t decoder;
    FILE *jpeg_file = NULL;
    char jpeg_filename[MAX_PATH] = {0};
    bool jpeg_receiving = false;
    bool jpeg_write_failed = false;
    bool jpeg_until_eoi = false;
    bool jpeg_previous_was_ff = false;
    size_t jpeg_size = 0;
    size_t jpeg_bytes_received = 0;
    uint32_t jpeg_expected_crc = 0;
    uint32_t jpeg_crc_state = 0xffffffffu;

    for (;;) {
        int received = recv(client, (char *)data, sizeof(data), 0);
        if (received <= 0) {
            InterlockedExchange(&pico_connected, 0);
            EnterCriticalSection(&send_lock);
            active_pico_client = INVALID_SOCKET;
            LeaveCriticalSection(&send_lock);
            free(frame);
            if (jpeg_file != NULL) fclose(jpeg_file);
            if (jpeg_receiving && jpeg_filename[0] != '\0') DeleteFileA(jpeg_filename);
            store_event(EVENT_CONNECTION, "Pico disconnected; waiting for reconnection");
            printf("\nPico disconnected\n");
            return 0;
        }
        for (int i = 0; i < received;) {
            if (jpeg_receiving) {
                if (jpeg_until_eoi) {
                    while (i < received && jpeg_receiving) {
                        uint8_t byte = data[i++];
                        jpeg_crc_state = crc32_update(jpeg_crc_state, &byte, 1);
                        ++jpeg_bytes_received;
                        if (jpeg_file != NULL && fwrite(&byte, 1, 1, jpeg_file) != 1) {
                            fclose(jpeg_file);
                            jpeg_file = NULL;
                            jpeg_write_failed = true;
                        }
                        if (jpeg_previous_was_ff && byte == 0xd9u) {
                            finish_camera_jpeg(&jpeg_file, jpeg_filename,
                                               jpeg_write_failed, false, 0, 0);
                            jpeg_receiving = false;
                            jpeg_filename[0] = '\0';
                            printf("PC -> Pico > ");
                            fflush(stdout);
                            break;
                        }
                        jpeg_previous_was_ff = byte == 0xffu;
                    }
                    continue;
                }

                size_t available = (size_t)(received - i);
                size_t needed = jpeg_size - jpeg_bytes_received;
                size_t copied = available < needed ? available : needed;
                jpeg_crc_state = crc32_update(jpeg_crc_state, data + i, copied);
                if (jpeg_file != NULL &&
                    fwrite(data + i, 1, copied, jpeg_file) != copied) {
                    fclose(jpeg_file);
                    jpeg_file = NULL;
                    jpeg_write_failed = true;
                }
                jpeg_bytes_received += copied;
                i += (int)copied;
                if (jpeg_bytes_received != jpeg_size) continue;

                finish_camera_jpeg(&jpeg_file, jpeg_filename, jpeg_write_failed,
                                   true, jpeg_expected_crc, ~jpeg_crc_state);
                jpeg_receiving = false;
                jpeg_filename[0] = '\0';
                printf("PC -> Pico > ");
                fflush(stdout);
                continue;
            }

            if (frame != NULL) {
                if (image_encoding == IMAGE_ENCODING_RAW_RGB565) {
                    size_t available = (size_t)(received - i);
                    size_t needed = frame_size - raw_bytes_received;
                    size_t copied = available < needed ? available : needed;
                    memcpy(frame + raw_bytes_received, data + i, copied);
                    raw_bytes_received += copied;
                    i += (int)copied;
                    if (raw_bytes_received != frame_size) continue;
                    finish_camera_frame(frame, frame_size, frame_width, frame_height,
                                        expected_crc);
                    free(frame);
                    frame = NULL;
                    printf("PC -> Pico > ");
                    fflush(stdout);
                    continue;
                }

                rle_decoder_feed(&decoder, data[i++]);
                if (decoder.overflowed ||
                    (decoder.produced == frame_size &&
                     decoder.phase != RLE_DECODE_CONTROL)) {
                    store_event(EVENT_CAMERA, "Camera image rejected: corrupt RLE stream");
                    printf("\nCamera image rejected: corrupt RLE stream\n");
                    free(frame);
                    frame = NULL;
                    printf("PC -> Pico > ");
                    fflush(stdout);
                } else if (decoder.produced == frame_size) {
                    finish_camera_frame(frame, frame_size, frame_width, frame_height,
                                        expected_crc);
                    free(frame);
                    frame = NULL;
                    printf("PC -> Pico > ");
                    fflush(stdout);
                }
                continue;
            }

            char character = (char)data[i++];
            if (character == '\r') continue;
            if (character == '\n') {
                unsigned width, height, size;
                unsigned long received_crc;
                char format[16];
                line[line_length] = '\0';
                int fields = sscanf(line,
                    "FRAME_STREAM,%u,%u,%15[^,],%u,%lx",
                    &width, &height, format, &size, &received_crc);
                bool stream_header = fields == 5;
                if (!stream_header) {
                    fields = sscanf(line, "FRAME,%u,%u,%15[^,],%u,%lx",
                                    &width, &height, format, &size,
                                    &received_crc);
                }
                if (fields == 5) {
                    bool is_rle = strcmp(format, "RGB565RLE") == 0;
                    bool is_raw = strcmp(format, "RGB565") == 0;
                    bool is_jpeg = strcmp(format, "JPEG") == 0;
                    size_t expected_size = (size_t)width * height * 2u;
                    if ((!is_rle && !is_raw && !is_jpeg) || width == 0 || height == 0 ||
                        width > CAMERA_MAX_WIDTH || height > CAMERA_MAX_HEIGHT ||
                        (!is_jpeg && (size != expected_size || size == 0)) ||
                        size > MAX_FRAME_BYTES) {
                        store_event(EVENT_CAMERA, "Camera frame rejected: invalid image header");
                        printf("\nInvalid FRAME header: %s\n", line);
                    } else if (is_jpeg) {
                        SYSTEMTIME now;
                        GetLocalTime(&now);
                        jpeg_receiving = true;
                        jpeg_write_failed = false;
                        jpeg_until_eoi = size == 0;
                        jpeg_previous_was_ff = false;
                        jpeg_size = size;
                        jpeg_bytes_received = 0;
                        jpeg_expected_crc = (uint32_t)received_crc;
                        jpeg_crc_state = 0xffffffffu;
                        jpeg_filename[0] = '\0';
                        if (!build_camera_path(jpeg_filename, sizeof(jpeg_filename), &now,
                                               ".jpg") ||
                            (jpeg_file = fopen(jpeg_filename, "wb")) == NULL) {
                            jpeg_write_failed = true;
                            store_event(EVENT_CAMERA, "Camera JPEG could not be opened for saving");
                            printf("\nCamera JPEG will be discarded: cannot create output file\n");
                        }
                        printf("\nReceiving %s JPEG %ux%u (%s)...\n",
                               stream_header ? "streamed" : "captured", width, height,
                               jpeg_until_eoi ? "EOI-delimited" : "length-delimited");
                    } else {
                        frame = (uint8_t *)malloc(size);
                        if (frame == NULL) {
                            store_event(EVENT_CAMERA, "Camera frame rejected: insufficient memory");
                            printf("\nNot enough memory for image\n");
                        } else {
                            frame_width = width;
                            frame_height = height;
                            frame_size = size;
                            raw_bytes_received = 0;
                            image_encoding = is_rle ? IMAGE_ENCODING_RLE_RGB565
                                                    : IMAGE_ENCODING_RAW_RGB565;
                            expected_crc = (uint32_t)received_crc;
                            if (is_rle) rle_decoder_reset(&decoder, frame, frame_size);
                            printf("\nReceiving %s %ux%u %s image (%u bytes%s)...\n",
                                   stream_header ? "streamed" : "captured", width, height,
                                   is_rle ? "RGB565RLE" : "RGB565",
                                   size, is_rle ? " decoded" : "");
                        }
                    }
                } else {
                    display_pico_line(line);
                }
                line_length = 0;
            } else if (line_length < sizeof(line) - 1) {
                line[line_length++] = character;
            } else {
                store_event(EVENT_CAMERA, "Camera/control line discarded: too long");
                printf("\nPico message too long; discarded\n");
                line_length = 0;
            }
        }
    }
}

/* Keep accepting new Pico TCP sessions after each disconnect. */
static DWORD WINAPI pico_accept_server(LPVOID parameter) {
    SOCKET server = *(SOCKET *)parameter;

    while (InterlockedCompareExchange(&server_running, 0, 0)) {
        SOCKET client;
        printf("Waiting on Pico TCP port %d...\n", PICO_TCP_PORT);
        client = accept(server, NULL, NULL);
        if (client == INVALID_SOCKET) {
            if (InterlockedCompareExchange(&server_running, 0, 0)) {
                printf("Pico accept failed: %d\n", WSAGetLastError());
                Sleep(200);
            }
            continue;
        }

        EnterCriticalSection(&send_lock);
        active_pico_client = client;
        LeaveCriticalSection(&send_lock);
        InterlockedExchange(&pico_connected, 1);
        InterlockedExchange(&pico_ever_connected, 1);
        begin_session_save();
        store_event(EVENT_CONNECTION, "Pico connected");
        printf("Pico connected\n");

        receive_pico(&client);
        closesocket(client);
    }
    return 0;
}

int main(int argc, char *argv[]) {
    WSADATA wsa;
    SOCKET pico_listener = INVALID_SOCKET, http_listener;
    HANDLE http_thread, telemetry_thread;
    char command[256];
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--dummy") != 0)) { printf("Usage: %s [--dummy]\n", argv[0]); return 1; }
    dummy_mode = argc == 2;
    InitializeCriticalSection(&history_lock);
    InitializeCriticalSection(&autosave_lock);
    InitializeCriticalSection(&send_lock);
    InitializeCriticalSection(&camera_lock);
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
    if (!dummy_mode) pico_listener = listener(PICO_TCP_PORT);
    http_listener = listener(HTTP_PORT);
    if (http_listener == INVALID_SOCKET || (!dummy_mode && pico_listener == INVALID_SOCKET)) {
        printf("Could not open required TCP port: %d\n", WSAGetLastError());
        if (pico_listener != INVALID_SOCKET) closesocket(pico_listener);
        if (http_listener != INVALID_SOCKET) closesocket(http_listener);
        WSACleanup(); DeleteCriticalSection(&camera_lock); DeleteCriticalSection(&send_lock); DeleteCriticalSection(&autosave_lock); DeleteCriticalSection(&history_lock); return 1;
    }
    InterlockedExchange(&server_running, 1);
    http_thread = CreateThread(NULL, 0, http_server, &http_listener, 0, NULL);
    if (!http_thread) { closesocket(http_listener); if (pico_listener != INVALID_SOCKET) closesocket(pico_listener); WSACleanup(); DeleteCriticalSection(&camera_lock); DeleteCriticalSection(&send_lock); DeleteCriticalSection(&autosave_lock); DeleteCriticalSection(&history_lock); return 1; }
    printf("Dashboard: http://localhost:%d\n", HTTP_PORT);
    if (dummy_mode) {
        InterlockedExchange(&dummy_running, 1);
        telemetry_thread = CreateThread(NULL, 0, dummy_telemetry, NULL, 0, NULL);
        printf("Dummy mode: simulated telemetry every 2 seconds\n");
    } else {
        telemetry_thread = CreateThread(NULL, 0, pico_accept_server, &pico_listener, 0, NULL);
    }
    if (!telemetry_thread) { printf("Telemetry thread creation failed\n"); return 1; }
    printf("Type a message and press Enter. Type /quit to exit.\n");
    for (;;) {
        printf("PC -> Pico > "); fflush(stdout);
        if (!fgets(command, sizeof(command), stdin)) { if (dummy_mode) { Sleep(100); continue; } break; }
        if (strcmp(command, "/quit\n") == 0 || strcmp(command, "/quit\r\n") == 0) break;
        if (!send_ground_command(command)) { printf("command send failed: Pico is not connected\n"); }
    }
    InterlockedExchange(&server_running, 0);
    if (dummy_mode) {
        InterlockedExchange(&dummy_running, 0);
    } else {
        SOCKET client;
        EnterCriticalSection(&send_lock);
        client = active_pico_client;
        LeaveCriticalSection(&send_lock);
        if (client != INVALID_SOCKET) shutdown(client, SD_BOTH);
        closesocket(pico_listener);
    }
    WaitForSingleObject(telemetry_thread, INFINITE); CloseHandle(telemetry_thread);
    EnterCriticalSection(&send_lock);
    active_pico_client = INVALID_SOCKET;
    LeaveCriticalSection(&send_lock);
    if (dummy_mode && pico_listener != INVALID_SOCKET) closesocket(pico_listener);
    closesocket(http_listener); WaitForSingleObject(http_thread, INFINITE); CloseHandle(http_thread);
    WSACleanup(); DeleteCriticalSection(&camera_lock); DeleteCriticalSection(&send_lock); DeleteCriticalSection(&autosave_lock); DeleteCriticalSection(&history_lock); return 0;
}
