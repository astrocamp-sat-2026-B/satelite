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
"*{box-sizing:border-box}body{margin:0;background:#0b121a;color:#e8f0f7;font-family:system-ui,sans-serif}main{max-width:1440px;margin:auto;padding:24px}.top{display:flex;justify-content:space-between;gap:16px;flex-wrap:wrap;align-items:center}h1{margin:0;font-size:1.8rem}.sub{color:#9eb0c0;margin:4px 0}.actions{display:flex;gap:8px;flex-wrap:wrap}button,label.file,input.command{background:#182838;border:1px solid #38546d;color:#e8f0f7;border-radius:7px;padding:9px 12px;font:inherit}button,label.file{cursor:pointer}input.command{min-width:190px}button.primary{background:#1977b5}label.file input{display:none}.status{display:flex;gap:9px;align-items:center;margin:18px 0;padding:11px 13px;background:#111e2b;border:1px solid #293f53;border-radius:8px}.dot{width:10px;height:10px;border-radius:50%;background:#8192a2}.receiving{background:#42d99a}.stale{background:#f2b35a}.waiting{background:#64b7ff}.reconnecting{background:#f2b35a}.disconnected{background:#f06778}.metrics{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:10px}.card,.section{background:#111e2b;border:1px solid #293f53;border-radius:10px}.card{padding:13px}.label{font-size:.76rem;color:#9eb0c0;text-transform:uppercase;letter-spacing:.06em}.value{font-size:1.45rem;font-weight:650;margin:7px 0 10px;font-variant-numeric:tabular-nums}.range{display:flex;gap:16px;font-size:.75rem;color:#9eb0c0}.range b{display:block;color:#e8f0f7;font-size:.86rem;margin-top:2px;font-weight:500}.charts{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:10px;margin-top:10px}.section{padding:14px}.head{display:flex;justify-content:space-between;gap:10px;align-items:baseline;margin-bottom:8px}.head h2{font-size:1rem;margin:0}.meta{color:#9eb0c0;font-size:.82rem}.chart{width:100%;height:auto;display:block;background:#0b151f;border-radius:6px}.grid{stroke:#263d51;stroke-width:1}.axis{fill:#9eb0c0;font-size:11px}.line{fill:none;stroke-width:2.4;stroke-linejoin:round;stroke-linecap:round}.temp{stroke:#49c4ff}.gyro{stroke:#9dde68}.pd0{stroke:#ffba5c}.pd1{stroke:#7dc9ff}.pd2{stroke:#e38fff}.pd3{stroke:#70e0b0}.scroll{max-height:540px;overflow:auto;border:1px solid #293f53;border-radius:7px}table{width:100%;border-collapse:collapse;font-size:.84rem}th,td{padding:9px 11px;border-bottom:1px solid #213447;text-align:right;white-space:nowrap;font-variant-numeric:tabular-nums}tr.event td{background:#162636;color:#b9d7ea;text-align:left;font-style:italic}th{position:sticky;top:0;background:#182838;color:#c4d7e7;font-weight:500}th:first-child,td:first-child{text-align:left}@media(max-width:560px){main{padding:14px}.scroll{max-height:500px}input.command{min-width:140px}}</style></head><body><main>"
"<header class=\"top\"><div><h1>Pico W Telemetry</h1><p class=\"sub\">Live monitor, command timeline, CSV export and replay</p></div><div class=\"actions\"><input class=\"command\" id=\"command-input\" placeholder=\"SERVO,123 or SET_VALUE,123\"><button class=\"primary\" id=\"send-command\" type=\"button\">Send command</button><button id=\"capture-camera\" type=\"button\">Capture camera</button><button id=\"save\" type=\"button\">Save CSV</button><label class=\"file\">Load CSV<input id=\"load\" type=\"file\" accept=\".csv,text/csv\"></label><button id=\"live\" type=\"button\">Resume live</button></div></header>"
"<style>.board-section{margin-top:10px}.board-map{position:relative;max-width:330px;margin:auto;line-height:1}.board-map img{display:block;width:100%;height:auto;border-radius:8px}.sensor-dot{position:absolute;transform:translate(-50%,-50%);width:clamp(34px,6vw,54px);height:clamp(34px,6vw,54px);border:2px solid #fff;border-radius:50%;display:flex;align-items:center;justify-content:center;text-align:center;white-space:pre-line;line-height:1.08;font-size:clamp(9px,1.3vw,12px);font-weight:750;color:#fff;text-shadow:0 1px 2px #000;box-shadow:0 0 0 2px #101820,0 0 14px rgba(255,55,55,.65);transition:background .35s,box-shadow .35s}.board-key{display:flex;justify-content:space-between;gap:12px;flex-wrap:wrap;color:#9eb0c0;font-size:.82rem;margin:0 0 10px}.red-scale{width:120px;height:10px;border-radius:8px;background:linear-gradient(90deg,rgba(240,40,40,.2),rgba(240,40,40,1))}.camera-image{display:block;width:min(100%,420px);max-height:300px;object-fit:contain;background:#080e14;border-radius:8px;margin:auto}.photoreflector{stroke:#ff8be8}</style>"
"<div class=\"status\"><span id=\"dot\" class=\"dot disconnected\"></span><span id=\"status\">Starting...</span></div><section id=\"metrics\" class=\"metrics\"></section>"
"<section class=\"section\" style=\"margin-top:10px\"><div class=\"head\"><h2>Latest camera image</h2><span id=\"camera-name\" class=\"meta\">No image received</span></div><img id=\"camera-image\" class=\"camera-image\" alt=\"Latest OV7675 capture\" hidden></section>"
"<section class=\"section\" style=\"margin-top:10px;padding:10px 14px\"><div class=\"head\" style=\"margin:0\"><h2>Graph range</h2><div style=\"display:flex;gap:9px;align-items:center;flex-wrap:wrap\"><input id=\"graph-window\" type=\"range\" min=\"0\" max=\"0\" value=\"0\" style=\"width:min(48vw,380px)\"><span id=\"graph-window-label\" class=\"meta\">Latest 60 seconds</span><button id=\"graph-latest\" type=\"button\">Latest</button></div></div></section>"
"<section class=\"charts\"><article class=\"section\"><div class=\"head\"><h2>Temperature</h2><span class=\"meta\">deg C</span></div><svg id=\"temp-chart\" class=\"chart\" viewBox=\"0 0 720 250\"></svg></article><article class=\"section\"><div class=\"head\"><h2>Gyro Z</h2><span class=\"meta\">dps</span></div><svg id=\"gyro-chart\" class=\"chart\" viewBox=\"0 0 720 250\"></svg></article><article class=\"section\"><div class=\"head\"><h2>Photodiode ADC</h2><span class=\"meta\">PD0 / PD1 / PD2 / PD3</span></div><svg id=\"pd-chart\" class=\"chart\" viewBox=\"0 0 720 250\"></svg></article><article class=\"section\"><div class=\"head\"><h2>Photoreflector</h2><span class=\"meta\">ADC</span></div><svg id=\"photoreflector-chart\" class=\"chart\" viewBox=\"0 0 720 250\"></svg></article></section>"
"<section class=\"section\" style=\"margin-top:10px\"><div class=\"head\"><h2 id=\"history-title\">Command history</h2><div class=\"actions\"><span id=\"count\" class=\"meta\"></span><button id=\"history-mode\" type=\"button\">Show all history</button></div></div><div class=\"scroll\"><table><thead id=\"history-head\"><tr><th>Received</th><th>Direction / message</th></tr></thead><tbody id=\"rows\"></tbody></table></div></section>"
"<section class=\"section board-section\"><div class=\"head\"><h2>Photodiode position map</h2><span class=\"meta\">PD0 upper-left, clockwise to PD3</span></div><div class=\"board-key\"><span>Low</span><span class=\"red-scale\" aria-label=\"Light red means low, dark red means high\"></span><span>High</span></div><div class=\"board-map\"><img src=\"/board-image.png\" alt=\"Satellite photodiode layout\"><div class=\"sensor-dot\" id=\"sensor0\" style=\"left:25%;top:13.7%\">PD0\n--</div><div class=\"sensor-dot\" id=\"sensor1\" style=\"left:75.4%;top:13.7%\">PD1\n--</div><div class=\"sensor-dot\" id=\"sensor2\" style=\"left:75.4%;top:86.7%\">PD2\n--</div><div class=\"sensor-dot\" id=\"sensor3\" style=\"left:25%;top:86.7%\">PD3\n--</div></div></section>"
"</main><script>"
"const $=id=>document.getElementById(id),fmt=(v,d=2)=>Number.isFinite(v)?v.toFixed(d):'NA',num=v=>Number.isFinite(v),esc=v=>String(v).replace(/[&<>]/g,c=>c==='&'?'&amp;':c==='<'?'&lt;':'&gt;');let current=null,loaded=false,graphEndTime=null,cameraShown='',historyMode='commands';"
"const fields=[['Uptime',v=>v.uptime_s,' s',0],['Temperature',v=>v.temp_c,' C',2],['Gyro Z',v=>v.gyro_z_dps,' dps',2],['Command',v=>v.command_value,'',0],['PD0',v=>v.photodiode_adc?.[0],'',0],['PD1',v=>v.photodiode_adc?.[1],'',0],['PD2',v=>v.photodiode_adc?.[2],'',0],['PD3',v=>v.photodiode_adc?.[3],'',0],['Photoreflector',v=>v.photoreflector_adc,'',0],['Wi-Fi signal',v=>v.wifi_signal_quality,' %',0]];"
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

    fprintf(file, "type,received_at,event_text_encoded,uptime_s,temp_c,gyro_z_dps,command_value,random,pd0,pd1,pd2,pd3,photoreflector_adc,wifi_signal_quality\n");
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
static bool build_camera_path(char *path, size_t path_size, const SYSTEMTIME *now) {
    char executable_path[MAX_PATH];
    char capture_directory[MAX_PATH];
    char *separator;
    DWORD attributes;
    DWORD length = GetModuleFileNameA(NULL, executable_path, sizeof(executable_path));
    int written;

    if (!now || length == 0 || length >= sizeof(executable_path)) return false;
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
    written = snprintf(path, path_size, "%s\\camera_%04u%02u%02u_%02u%02u%02u_%03u.bmp",
                       capture_directory, now->wYear, now->wMonth, now->wDay, now->wHour,
                       now->wMinute, now->wSecond, now->wMilliseconds);
    return written > 0 && (size_t)written < path_size;
}

/* Convert the OV7675 RGB565 payload to a browser-displayable BMP. */
static bool save_rgb565_bmp(const uint8_t *frame, unsigned width, unsigned height,
                             char *filename, size_t filename_size) {
    SYSTEMTIME now;
    uint32_t row_size, pixel_bytes;
    uint8_t header[54] = {0};
    const uint8_t padding[3] = {0};
    FILE *file;

    if (!frame || width == 0 || height == 0 || width > CAMERA_MAX_WIDTH ||
        height > CAMERA_MAX_HEIGHT) return false;
    GetLocalTime(&now);
    if (!build_camera_path(filename, filename_size, &now)) return false;
    file = fopen(filename, "wb");
    if (!file) return false;

    row_size = (width * 3u + 3u) & ~3u;
    pixel_bytes = row_size * height;
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
        fclose(file);
        DeleteFileA(filename);
        return false;
    }

    for (unsigned output_y = 0; output_y < height; ++output_y) {
        unsigned source_y = height - 1u - output_y;
        for (unsigned x = 0; x < width; ++x) {
            size_t position = ((size_t)source_y * width + x) * 2u;
            uint16_t pixel = ((uint16_t)frame[position] << 8) | frame[position + 1];
            uint8_t bgr[3] = {
                (uint8_t)(((pixel & 0x1fu) * 255u) / 31u),
                (uint8_t)((((pixel >> 5) & 0x3fu) * 255u) / 63u),
                (uint8_t)((((pixel >> 11) & 0x1fu) * 255u) / 31u),
            };
            if (fwrite(bgr, 1, sizeof(bgr), file) != sizeof(bgr)) {
                fclose(file);
                DeleteFileA(filename);
                return false;
            }
        }
        size_t padding_size = row_size - width * 3u;
        if (padding_size && fwrite(padding, 1, padding_size, file) != padding_size) {
            fclose(file);
            DeleteFileA(filename);
            return false;
        }
    }
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

static void http_reply_latest_camera(SOCKET client) {
    char path[MAX_PATH];
    char header[256];
    char buffer[8192];
    long size;
    FILE *file;

    EnterCriticalSection(&camera_lock);
    snprintf(path, sizeof(path), "%s", latest_camera_path);
    LeaveCriticalSection(&camera_lock);
    if (!path[0] || !(file = fopen(path, "rb"))) {
        http_reply(client, "text/plain", "No camera image has been received");
        return;
    }
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    rewind(file);
    if (size < 0 || snprintf(header, sizeof(header),
                             "HTTP/1.1 200 OK\r\nContent-Type: image/bmp\r\nContent-Length: %ld\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
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
            frame[position] = (uint8_t)(pixel >> 8);
            frame[position + 1] = (uint8_t)pixel;
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
        unsigned int pd0 = 1100 + (sample * 37) % 900;
        unsigned int pd1 = 1300 + (sample * 53) % 850;
        unsigned int pd2 = 1500 + (sample * 71) % 700;
        unsigned int pd3 = 1700 + (sample * 29) % 600;
        unsigned int photoreflector = 300 + (sample * 83) % 3700;
        random_value = random_value * 1103515245u + 12345u;
        snprintf(line, sizeof(line),
                 "TELEMETRY,uptime_s=%lu,temp_c=%d.%02d,random=%lu,command_value=%ld,gyro_z_dps=%s%d.%02d,photodiode_adc=%u|%u|%u|%u,photoreflector_adc=%u",
                 sample * 2, temp / 100, temp % 100, random_value % 1000,
                 InterlockedCompareExchange(&dummy_command_value, 0, 0), gyro < 0 ? "-" : "",
                 gyro_abs / 100, gyro_abs % 100, pd0, pd1, pd2, pd3, photoreflector);
        display_pico_line(line);
        sample++;
        Sleep(2000);
    }
    InterlockedExchange(&pico_connected, 0);
    return 0;
}

static DWORD WINAPI receive_pico(LPVOID parameter) {
    SOCKET client = *(SOCKET *)parameter;
    uint8_t data[512];
    char line[384];
    size_t line_length = 0;
    uint8_t *frame = NULL;
    size_t frame_size = 0, frame_received = 0;
    unsigned frame_width = 0, frame_height = 0;
    uint32_t expected_crc = 0;

    for (;;) {
        int received = recv(client, (char *)data, sizeof(data), 0);
        if (received <= 0) {
            InterlockedExchange(&pico_connected, 0);
            EnterCriticalSection(&send_lock);
            active_pico_client = INVALID_SOCKET;
            LeaveCriticalSection(&send_lock);
            free(frame);
            store_event(EVENT_CONNECTION, "Pico disconnected; waiting for reconnection");
            printf("\nPico disconnected\n");
            return 0;
        }
        for (int i = 0; i < received;) {
            if (frame) {
                size_t available = (size_t)(received - i);
                size_t needed = frame_size - frame_received;
                size_t copy_size = available < needed ? available : needed;
                memcpy(frame + frame_received, data + i, copy_size);
                frame_received += copy_size;
                i += (int)copy_size;
                if (frame_received == frame_size) {
                    char filename[MAX_PATH];
                    if (crc32(frame, frame_size) != expected_crc) {
                        store_event(EVENT_CAMERA, "Camera image rejected: CRC mismatch");
                        printf("\nCamera image rejected: CRC mismatch\n");
                    } else if (save_rgb565_bmp(frame, frame_width, frame_height,
                                                filename, sizeof(filename))) {
                        register_camera_file(filename);
                        printf("\nCamera image saved: %s\n", filename);
                    } else {
                        store_event(EVENT_CAMERA, "Camera image could not be saved");
                        printf("\nCamera image could not be saved\n");
                    }
                    free(frame);
                    frame = NULL;
                    frame_size = frame_received = 0;
                }
                continue;
            }

            char character = (char)data[i++];
            if (character == '\r') continue;
            if (character == '\n') {
                unsigned width, height, bytes;
                unsigned long crc;
                char format[16];
                line[line_length] = '\0';
                if (sscanf(line, "FRAME,%u,%u,%15[^,],%u,%lx",
                           &width, &height, format, &bytes, &crc) == 5) {
                    size_t expected_size = (size_t)width * height * 2u;
                    if (strcmp(format, "RGB565") != 0 || width == 0 || height == 0 ||
                        width > CAMERA_MAX_WIDTH || height > CAMERA_MAX_HEIGHT ||
                        expected_size != bytes || bytes > MAX_FRAME_BYTES) {
                        store_event(EVENT_CAMERA, "Camera frame rejected: invalid header");
                        printf("\nCamera frame rejected: %s\n", line);
                    } else if (!(frame = malloc(bytes))) {
                        store_event(EVENT_CAMERA, "Camera frame rejected: insufficient memory");
                        printf("\nCamera frame rejected: insufficient memory\n");
                    } else {
                        frame_width = width;
                        frame_height = height;
                        frame_size = bytes;
                        frame_received = 0;
                        expected_crc = (uint32_t)crc;
                        printf("\nReceiving camera image: %ux%u (%u bytes)\n", width, height, bytes);
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