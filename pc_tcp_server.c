#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#pragma comment(lib, "ws2_32.lib")

#define PICO_TCP_PORT 4242
#define HTTP_PORT 8080
#define MAX_TELEMETRY_HISTORY 2000
#define HTTP_JSON_CAPACITY 524288
#define TELEMETRY_STALE_MS 7000ULL

typedef enum {
    EVENT_TELEMETRY,
    EVENT_PC_COMMAND,
    EVENT_PICO_REPLY
} event_type_t;

typedef struct {
    event_type_t type;
    char received_at[24];
    char event_text[256];
    unsigned long uptime_s;
    double temp_c;
    unsigned long random_value;
    long command_value;
    double gyro_z_dps;
    bool gyro_z_valid;
    unsigned int photodiode_adc[4];
} telemetry_entry_t;

static telemetry_entry_t history[MAX_TELEMETRY_HISTORY];
static int history_count;
static int history_next;
static CRITICAL_SECTION history_lock;
static volatile LONG pico_connected;
static ULONGLONG last_telemetry_tick_ms;
static bool dummy_mode;
static volatile LONG dummy_running;
static volatile LONG dummy_command_value;
static SOCKET active_pico_client = INVALID_SOCKET;
static CRITICAL_SECTION send_lock;

static bool send_ground_command(const char *raw_command);

static const char DASHBOARD_HTML[] =
"<!doctype html><html lang=\"ja\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>Pico telemetry</title><style>"
"*{box-sizing:border-box}body{margin:0;background:#0b121a;color:#e8f0f7;font-family:system-ui,sans-serif}main{max-width:1440px;margin:auto;padding:24px}.top{display:flex;justify-content:space-between;gap:16px;flex-wrap:wrap;align-items:center}h1{margin:0;font-size:1.8rem}.sub{color:#9eb0c0;margin:4px 0}.actions{display:flex;gap:8px;flex-wrap:wrap}button,label.file,input.command{background:#182838;border:1px solid #38546d;color:#e8f0f7;border-radius:7px;padding:9px 12px;font:inherit}button,label.file{cursor:pointer}input.command{min-width:190px}button.primary{background:#1977b5}label.file input{display:none}.status{display:flex;gap:9px;align-items:center;margin:18px 0;padding:11px 13px;background:#111e2b;border:1px solid #293f53;border-radius:8px}.dot{width:10px;height:10px;border-radius:50%;background:#8192a2}.receiving{background:#42d99a}.stale{background:#f2b35a}.waiting{background:#64b7ff}.disconnected{background:#f06778}.metrics{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:10px}.card,.section{background:#111e2b;border:1px solid #293f53;border-radius:10px}.card{padding:13px}.label{font-size:.76rem;color:#9eb0c0;text-transform:uppercase;letter-spacing:.06em}.value{font-size:1.45rem;font-weight:650;margin:7px 0 10px;font-variant-numeric:tabular-nums}.range{display:flex;gap:16px;font-size:.75rem;color:#9eb0c0}.range b{display:block;color:#e8f0f7;font-size:.86rem;margin-top:2px;font-weight:500}.charts{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:10px;margin-top:10px}.section{padding:14px}.head{display:flex;justify-content:space-between;gap:10px;align-items:baseline;margin-bottom:8px}.head h2{font-size:1rem;margin:0}.meta{color:#9eb0c0;font-size:.82rem}.chart{width:100%;height:auto;display:block;background:#0b151f;border-radius:6px}.grid{stroke:#263d51;stroke-width:1}.axis{fill:#9eb0c0;font-size:11px}.line{fill:none;stroke-width:2.4;stroke-linejoin:round;stroke-linecap:round}.temp{stroke:#49c4ff}.gyro{stroke:#9dde68}.pd0{stroke:#ffba5c}.pd1{stroke:#7dc9ff}.pd2{stroke:#e38fff}.pd3{stroke:#70e0b0}.scroll{max-height:540px;overflow:auto;border:1px solid #293f53;border-radius:7px}table{width:100%;border-collapse:collapse;font-size:.84rem}th,td{padding:9px 11px;border-bottom:1px solid #213447;text-align:right;white-space:nowrap;font-variant-numeric:tabular-nums}tr.event td{background:#162636;color:#b9d7ea;text-align:left;font-style:italic}th{position:sticky;top:0;background:#182838;color:#c4d7e7;font-weight:500}th:first-child,td:first-child{text-align:left}@media(max-width:560px){main{padding:14px}.scroll{max-height:500px}input.command{min-width:140px}}</style></head><body><main>"
"<header class=\"top\"><div><h1>Pico W Telemetry</h1><p class=\"sub\">Live monitor, command timeline, CSV export and replay</p></div><div class=\"actions\"><input class=\"command\" id=\"command-input\" placeholder=\"SERVO,123 or SET_VALUE,123\"><button class=\"primary\" id=\"send-command\" type=\"button\">Send command</button><button id=\"save\" type=\"button\">Save CSV</button><label class=\"file\">Load CSV<input id=\"load\" type=\"file\" accept=\".csv,text/csv\"></label><button id=\"live\" type=\"button\">Resume live</button></div></header>"
"<div class=\"status\"><span id=\"dot\" class=\"dot disconnected\"></span><span id=\"status\">Starting...</span></div><section id=\"metrics\" class=\"metrics\"></section>"
"<section class=\"charts\"><article class=\"section\"><div class=\"head\"><h2>Temperature</h2><span class=\"meta\">deg C</span></div><svg id=\"temp-chart\" class=\"chart\" viewBox=\"0 0 720 250\"></svg></article><article class=\"section\"><div class=\"head\"><h2>Gyro Z</h2><span class=\"meta\">dps</span></div><svg id=\"gyro-chart\" class=\"chart\" viewBox=\"0 0 720 250\"></svg></article><article class=\"section\"><div class=\"head\"><h2>Photodiode ADC</h2><span class=\"meta\">PD0 / PD1 / PD2 / PD3</span></div><svg id=\"pd-chart\" class=\"chart\" viewBox=\"0 0 720 250\"></svg></article></section>"
"<section class=\"section\" style=\"margin-top:10px\"><div class=\"head\"><h2>History</h2><span id=\"count\" class=\"meta\"></span></div><div class=\"scroll\"><table><thead><tr><th>Received</th><th>Uptime</th><th>Temp C</th><th>Gyro dps</th><th>Command</th><th>Random</th><th>PD0</th><th>PD1</th><th>PD2</th><th>PD3</th></tr></thead><tbody id=\"rows\"></tbody></table></div></section>"
"</main><script>"
"const $=id=>document.getElementById(id),fmt=(v,d=2)=>Number.isFinite(v)?v.toFixed(d):'NA',num=v=>Number.isFinite(v),esc=v=>String(v).replace(/[&<>]/g,c=>c==='&'?'&amp;':c==='<'?'&lt;':'&gt;');let current=null,loaded=false;"
"const fields=[['Uptime',v=>v.uptime_s,' s',0],['Temperature',v=>v.temp_c,' C',2],['Gyro Z',v=>v.gyro_z_dps,' dps',2],['Command',v=>v.command_value,'',0],['PD0',v=>v.photodiode_adc?.[0],'',0],['PD1',v=>v.photodiode_adc?.[1],'',0],['PD2',v=>v.photodiode_adc?.[2],'',0],['PD3',v=>v.photodiode_adc?.[3],'',0]];"
"$('metrics').innerHTML=fields.map((f,i)=>`<article class=\"card\"><div class=\"label\">${f[0]}</div><div class=\"value\" id=\"v${i}\">--</div><div class=\"range\"><span>MIN<b id=\"n${i}\">--</b></span><span>MAX<b id=\"x${i}\">--</b></span></div></article>`).join('')+`<article class=\"card\"><div class=\"label\">Last ground command</div><div class=\"value\" id=\"last-command\">--</div><div class=\"range\"><span>TIME<b id=\"last-command-time\">--</b></span></div></article>`;"
"function metric(h,l,f,i){const a=h.map(f[1]).filter(num),s=a.length?Math.min(...a):null,x=a.length?Math.max(...a):null;$('v'+i).textContent=fmt(f[1](l),f[3])+f[2];$('n'+i).textContent=fmt(s,f[3])+f[2];$('x'+i).textContent=fmt(x,f[3])+f[2]}"
"function plot(id,h,series){const svg=$(id),W=720,H=250,L=58,R=18,T=18,B=35,pts=[];series.forEach(s=>h.forEach((v,i)=>{const n=s[1](v);if(num(n))pts.push(n)}));if(!pts.length){svg.innerHTML='<text class=\"axis\" x=\"18\" y=\"32\">No data</text>';return}let lo=Math.min(...pts),hi=Math.max(...pts);if(lo===hi){lo-=1;hi+=1}const px=i=>L+(W-L-R)*i/Math.max(h.length-1,1),py=v=>T+(H-T-B)*(1-(v-lo)/(hi-lo));let g='';for(let i=0;i<5;i++){const y=T+(H-T-B)*i/4;g+=`<line class=\"grid\" x1=\"${L}\" x2=\"${W-R}\" y1=\"${y}\" y2=\"${y}\"/><text class=\"axis\" x=\"3\" y=\"${y+4}\">${(hi-(hi-lo)*i/4).toFixed(1)}</text>`}let paths=series.map(s=>{let started=false,d='';h.forEach((v,i)=>{const n=s[1](v);if(num(n)){d+=(started?'L':'M')+px(i).toFixed(1)+','+py(n).toFixed(1);started=true}});return d?`<path class=\"line ${s[2]}\" d=\"${d}\"/>`:''}).join('');svg.innerHTML=g+paths+`<text class=\"axis\" x=\"${L}\" y=\"${H-10}\">${esc(h[0]?.received_at||'')}</text><text class=\"axis\" text-anchor=\"end\" x=\"${W-R}\" y=\"${H-10}\">${esc(h[h.length-1]?.received_at||'')}</text>`}"
"function render(d){current=d;const h=d.history||[],t=h.filter(v=>v.type==='telemetry'),l=d.latest;const labels={receiving:'Receiving telemetry',stale:'Connected, telemetry delayed',waiting:'Connected, waiting for telemetry',disconnected:'Disconnected'};$('dot').className='dot '+d.link_status;$('status').textContent=(d.dummy_mode?'Dummy simulation · ':'')+(labels[d.link_status]||'Unknown')+(d.last_telemetry_age_s===null?'':` · last telemetry ${d.last_telemetry_age_s.toFixed(1)} s ago`)+(loaded?' · loaded file (live paused)':'');if(l)fields.forEach((f,i)=>metric(t,l,f,i));const last=[...h].reverse().find(v=>v.type==='pc_command');$('last-command').textContent=last?last.event_text:'--';$('last-command-time').textContent=last?last.received_at:'--';$('count').textContent=`${h.length} events · ${t.length} telemetry samples · latest at top · scroll for all`;$('rows').innerHTML=h.slice().reverse().map(v=>{if(v.type!=='telemetry'){const prefix=v.type==='pc_command'?'PC -> Pico >':'Pico -> PC:';return `<tr class=\"event\"><td>${esc(v.received_at)}</td><td colspan=\"9\">${prefix} ${esc(v.event_text)}</td></tr>`}return `<tr><td>${esc(v.received_at)}</td><td>${v.uptime_s}</td><td>${fmt(v.temp_c)}</td><td>${fmt(v.gyro_z_dps)}</td><td>${v.command_value}</td><td>${v.random}</td>${(v.photodiode_adc||[null,null,null,null]).map(n=>`<td>${fmt(n,0)}</td>`).join('')}</tr>`}).join('');plot('temp-chart',t,[['temp',v=>v.temp_c,'temp']]);plot('gyro-chart',t,[['gyro',v=>v.gyro_z_dps,'gyro']]);plot('pd-chart',t,[['pd0',v=>v.photodiode_adc?.[0],'pd0'],['pd1',v=>v.photodiode_adc?.[1],'pd1'],['pd2',v=>v.photodiode_adc?.[2],'pd2'],['pd3',v=>v.photodiode_adc?.[3],'pd3']])}"
"function save(){if(!current?.history?.length)return;const rows=['type,received_at,event_text_encoded,uptime_s,temp_c,gyro_z_dps,command_value,random,pd0,pd1,pd2,pd3'];current.history.forEach(v=>rows.push([v.type||'telemetry',v.received_at,encodeURIComponent(v.event_text||''),v.uptime_s,v.temp_c,v.gyro_z_dps===null?'NA':v.gyro_z_dps,v.command_value,v.random,...(v.photodiode_adc||[])].join(',')));const a=document.createElement('a');a.href=URL.createObjectURL(new Blob([rows.join('\\n')],{type:'text/csv'}));a.download='pico-telemetry.csv';a.click();URL.revokeObjectURL(a.href)}"
"function load(file){const r=new FileReader();r.onload=()=>{const rows=String(r.result).trim().split(/\\r?\\n/),head=rows.shift().split(','),at=(a,k)=>a[head.indexOf(k)],n=(a,k)=>{const v=at(a,k);return v===undefined||v==='NA'?null:Number(v)},h=rows.filter(Boolean).map(z=>{const a=z.split(','),encoded=at(a,'event_text_encoded');return{type:at(a,'type')||'telemetry',received_at:at(a,'received_at')||'',event_text:encoded?decodeURIComponent(encoded):'',uptime_s:n(a,'uptime_s'),temp_c:n(a,'temp_c'),gyro_z_dps:n(a,'gyro_z_dps'),command_value:n(a,'command_value'),random:n(a,'random'),photodiode_adc:['pd0','pd1','pd2','pd3'].map(k=>n(a,k))}});loaded=true;render({dummy_mode:false,link_status:'disconnected',last_telemetry_age_s:null,latest:[...h].reverse().find(v=>v.type==='telemetry')||null,history:h})};r.readAsText(file)}"
"async function sendCommand(){const input=$('command-input'),text=input.value.trim();if(!text)return;try{const response=await fetch('/api/command',{method:'POST',headers:{'Content-Type':'text/plain'},body:text}),result=await response.json();if(!result.ok)throw Error('send failed');input.value='';loaded=false;await poll()}catch(e){$('status').textContent='Command send failed: Pico is not connected'}}"
"$('save').onclick=save;$('load').onchange=e=>e.target.files[0]&&load(e.target.files[0]);$('live').onclick=()=>{loaded=false;poll()};$('send-command').onclick=sendCommand;$('command-input').onkeydown=e=>{if(e.key==='Enter')sendCommand()};async function poll(){if(loaded)return;try{render(await (await fetch('/api/telemetry',{cache:'no-store'})).json())}catch(e){$('dot').className='dot disconnected';$('status').textContent='Dashboard connection error'}}poll();setInterval(poll,1000);"
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
    return true;
}

static void store_telemetry(const char *line) {
    telemetry_entry_t entry;
    if (!parse_telemetry(line, &entry)) {
        printf("\nInvalid telemetry ignored: %s\n", line);
        return;
    }
    EnterCriticalSection(&history_lock);
    history[history_next] = entry;
    history_next = (history_next + 1) % MAX_TELEMETRY_HISTORY;
    if (history_count < MAX_TELEMETRY_HISTORY) history_count++;
    last_telemetry_tick_ms = GetTickCount64();
    LeaveCriticalSection(&history_lock);
}

static void store_event(event_type_t type, const char *text) {
    telemetry_entry_t entry;
    time_t now = time(NULL);
    struct tm local_time;

    memset(&entry, 0, sizeof(entry));
    entry.type = type;
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
    history[history_next] = entry;
    history_next = (history_next + 1) % MAX_TELEMETRY_HISTORY;
    if (history_count < MAX_TELEMETRY_HISTORY) history_count++;
    LeaveCriticalSection(&history_lock);
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
    const char *type = e->type == EVENT_TELEMETRY ? "telemetry" :
                       e->type == EVENT_PC_COMMAND ? "pc_command" : "pico_reply";
    if (!append(buffer, cap, length,
                "{\"type\":\"%s\",\"received_at\":\"%s\",\"event_text\":\"%s\",\"uptime_s\":%lu,\"temp_c\":%.2f,\"random\":%lu,\"command_value\":%ld,\"gyro_z_dps\":",
                type, e->received_at, e->event_text, e->uptime_s, e->temp_c,
                e->random_value, e->command_value)) return false;
    if (e->gyro_z_valid) {
        if (!append(buffer, cap, length, "%.2f", e->gyro_z_dps)) return false;
    } else if (!append(buffer, cap, length, "null")) return false;
    return append(buffer, cap, length, ",\"photodiode_adc\":[%u,%u,%u,%u]}",
                  e->photodiode_adc[0], e->photodiode_adc[1],
                  e->photodiode_adc[2], e->photodiode_adc[3]);
}

static char *telemetry_json(void) {
    char *json = malloc(HTTP_JSON_CAPACITY);
    size_t length = 0;
    LONG connected;
    ULONGLONG now, last;
    const char *status;
    int oldest;
    if (!json) return NULL;
    EnterCriticalSection(&history_lock);
    connected = InterlockedCompareExchange(&pico_connected, 0, 0);
    now = GetTickCount64();
    last = last_telemetry_tick_ms;
    status = !connected ? "disconnected" :
             (!last ? "waiting" : now - last > TELEMETRY_STALE_MS ? "stale" : "receiving");
    if (!append(json, HTTP_JSON_CAPACITY, &length,
                "{\"pico_connected\":%s,\"dummy_mode\":%s,\"link_status\":\"%s\",\"last_telemetry_age_s\":",
                connected ? "true" : "false", dummy_mode ? "true" : "false", status) ||
        (!last && !append(json, HTTP_JSON_CAPACITY, &length, "null")) ||
        (last && !append(json, HTTP_JSON_CAPACITY, &length, "%.1f", (double)(now - last) / 1000.0)) ||
        !append(json, HTTP_JSON_CAPACITY, &length, ",\"latest\":")) goto fail;
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

static void dummy_command(const char *command) {
    char reply[128];
    if (strncmp(command, "SET_VALUE,", 10) == 0) {
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
        random_value = random_value * 1103515245u + 12345u;
        snprintf(line, sizeof(line),
                 "TELEMETRY,uptime_s=%lu,temp_c=%d.%02d,random=%lu,command_value=%ld,gyro_z_dps=%s%d.%02d,photodiode_adc=%u|%u|%u|%u",
                 sample * 2, temp / 100, temp % 100, random_value % 1000,
                 InterlockedCompareExchange(&dummy_command_value, 0, 0), gyro < 0 ? "-" : "",
                 gyro_abs / 100, gyro_abs % 100, pd0, pd1, pd2, pd3);
        display_pico_line(line);
        sample++;
        Sleep(2000);
    }
    InterlockedExchange(&pico_connected, 0);
    return 0;
}

static DWORD WINAPI receive_pico(LPVOID parameter) {
    SOCKET client = *(SOCKET *)parameter;
    char data[128], line[384];
    size_t line_length = 0;
    for (;;) {
        int received = recv(client, data, sizeof(data), 0);
        if (received <= 0) {
            InterlockedExchange(&pico_connected, 0);
            EnterCriticalSection(&send_lock);
            active_pico_client = INVALID_SOCKET;
            LeaveCriticalSection(&send_lock);
            printf("\nPico disconnected\n");
            return 0;
        }
        for (int i = 0; i < received; ++i) {
            if (data[i] == '\r') continue;
            if (data[i] == '\n') { line[line_length] = '\0'; display_pico_line(line); line_length = 0; }
            else if (line_length < sizeof(line) - 1) line[line_length++] = data[i];
            else { printf("\nPico message too long; discarded\n"); line_length = 0; }
        }
    }
}

int main(int argc, char *argv[]) {
    WSADATA wsa;
    SOCKET pico_listener = INVALID_SOCKET, pico_client = INVALID_SOCKET, http_listener;
    HANDLE http_thread, telemetry_thread;
    char command[256];
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--dummy") != 0)) { printf("Usage: %s [--dummy]\n", argv[0]); return 1; }
    dummy_mode = argc == 2;
    InitializeCriticalSection(&history_lock);
    InitializeCriticalSection(&send_lock);
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
    if (!dummy_mode) pico_listener = listener(PICO_TCP_PORT);
    http_listener = listener(HTTP_PORT);
    if (http_listener == INVALID_SOCKET || (!dummy_mode && pico_listener == INVALID_SOCKET)) {
        printf("Could not open required TCP port: %d\n", WSAGetLastError());
        if (pico_listener != INVALID_SOCKET) closesocket(pico_listener);
        if (http_listener != INVALID_SOCKET) closesocket(http_listener);
        WSACleanup(); DeleteCriticalSection(&history_lock); return 1;
    }
    http_thread = CreateThread(NULL, 0, http_server, &http_listener, 0, NULL);
    if (!http_thread) { closesocket(http_listener); if (pico_listener != INVALID_SOCKET) closesocket(pico_listener); WSACleanup(); DeleteCriticalSection(&history_lock); return 1; }
    printf("Dashboard: http://localhost:%d\n", HTTP_PORT);
    if (dummy_mode) {
        InterlockedExchange(&dummy_running, 1);
        telemetry_thread = CreateThread(NULL, 0, dummy_telemetry, NULL, 0, NULL);
        printf("Dummy mode: simulated telemetry every 2 seconds\n");
    } else {
        printf("Waiting on Pico TCP port %d...\n", PICO_TCP_PORT);
        pico_client = accept(pico_listener, NULL, NULL);
        if (pico_client == INVALID_SOCKET) { closesocket(http_listener); WaitForSingleObject(http_thread, INFINITE); CloseHandle(http_thread); closesocket(pico_listener); WSACleanup(); DeleteCriticalSection(&history_lock); return 1; }
        InterlockedExchange(&pico_connected, 1);
        EnterCriticalSection(&send_lock);
        active_pico_client = pico_client;
        LeaveCriticalSection(&send_lock);
        printf("Pico connected\n");
        telemetry_thread = CreateThread(NULL, 0, receive_pico, &pico_client, 0, NULL);
    }
    if (!telemetry_thread) { printf("Telemetry thread creation failed\n"); return 1; }
    printf("Type a message and press Enter. Type /quit to exit.\n");
    for (;;) {
        printf("PC -> Pico > "); fflush(stdout);
        if (!fgets(command, sizeof(command), stdin)) { if (dummy_mode) { Sleep(100); continue; } break; }
        if (strcmp(command, "/quit\n") == 0 || strcmp(command, "/quit\r\n") == 0) break;
        if (!send_ground_command(command)) { printf("command send failed: Pico is not connected\n"); }
    }
    if (dummy_mode) InterlockedExchange(&dummy_running, 0); else shutdown(pico_client, SD_BOTH);
    WaitForSingleObject(telemetry_thread, INFINITE); CloseHandle(telemetry_thread);
    EnterCriticalSection(&send_lock);
    active_pico_client = INVALID_SOCKET;
    LeaveCriticalSection(&send_lock);
    if (pico_client != INVALID_SOCKET) closesocket(pico_client);
    if (pico_listener != INVALID_SOCKET) closesocket(pico_listener);
    closesocket(http_listener); WaitForSingleObject(http_thread, INFINITE); CloseHandle(http_thread);
    WSACleanup(); DeleteCriticalSection(&send_lock); DeleteCriticalSection(&history_lock); return 0;
}
