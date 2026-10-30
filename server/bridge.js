/**
 * bridge.js — 拓竹农场 ↔ AI Passport (ESP32-C3) 桥接服务
 *
 * 功能：
 *   1. 每 5 秒轮询农场服务器（/devices2 + /task），维护状态快照
 *   2. 状态 diff → 事件流（哪台暂停/完成/出错 + HMS 原因）
 *   3. USB 串口桥接（JSON 行协议，自动探测 ESP32-C3）
 *   4. 本地 HTTP 调试接口（默认 8899）
 *
 * 串口协议（\n 分隔的 JSON）：
 *   ESP32 → bridge:
 *     {"t":"hello"}                          上线握手
 *     {"t":"get_state"}                      请求全量状态
 *     {"t":"launch","task_id":1741650,"dev_ids":["03900D..."]}  下发任务
 *     {"t":"opt","dev_id":"03900D...","opt":"pause"}            暂停/恢复/停止
 *   bridge → ESP32:
 *     {"t":"state",...}                      全量状态（紧凑）
 *     {"t":"event","kind":"pause",...}       状态变化事件
 *     {"t":"ack","ok":true,...}              命令应答
 *
 * 用法：node bridge.js [--port COM5] [--http 8899] [--interval 5000]
 */
const http = require('http');
const fs = require('fs');
const nodePath = require('path');
const farmapi = require('./farmapi.js');
const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');

const args = process.argv.slice(2);
function argOf(name, dflt) {
  const i = args.indexOf(name);
  return i >= 0 && args[i + 1] ? args[i + 1] : dflt;
}
const HTTP_PORT = parseInt(argOf('--http', '8899'), 10);
const ROOT = nodePath.join(__dirname, '..');   // 项目根目录（解析固件相对路径用）

/** 日志同步写一份到文件（托盘模式没有控制台窗口，靠它排错） */
function setupFileLog() {
  try {
    const dir = nodePath.join(__dirname, '..', '.tmp');
    fs.mkdirSync(dir, { recursive: true });
    const file = nodePath.join(dir, 'bridge.log');
    try { if (fs.statSync(file).size > 2 * 1024 * 1024) fs.writeFileSync(file, ''); } catch (e) { }
    fs.appendFileSync(file, `\n===== 启动 ${new Date().toLocaleString()} =====\n`);
    const fmt = (a) => a.map(x => (typeof x === 'string' ? x : String(x))).join(' ');
    const w = (s) => { try { fs.appendFileSync(file, s + '\n'); } catch (e) { } };
    const oLog = console.log.bind(console), oErr = console.error.bind(console);
    console.log = (...a) => { const s = fmt(a); oLog(s); w(s); };
    console.error = (...a) => { const s = fmt(a); oErr(s); w('[ERR] ' + s); };
  } catch (e) { /* 日志落文件失败不影响主流程 */ }
}
setupFileLog();

// ---------- 全局兜底 ----------
// 没有这两行，任何一个偶发的未捕获异常都会让桥接整个退出，
// 而托盘模式下没有控制台窗口，连错误信息都看不到（只能看到"服务没了"）。
// 兜住并写进日志，服务继续跑。
process.on('uncaughtException', (e) => {
  try { console.error('[致命] 未捕获异常：' + ((e && e.stack) || String(e))); } catch (_) { }
});
process.on('unhandledRejection', (e) => {
  try { console.error('[致命] 未处理的 Promise 拒绝：' + ((e && e.stack) || String(e))); } catch (_) { }
});

const DEVICE_TCP_PORT = parseInt(argOf('--device', '8897'), 10);   // 工牌无线连接端口
const POLL_MS = parseInt(argOf('--interval', '5000'), 10);
const FIXED_PORT = argOf('--port', '');

// ---------- 状态管理 ----------
const state = {
  server: null,
  devices: [],       // 原始设备数组
  tasks: [],         // 原始任务数组
  lastPoll: 0,
  pollErrors: 0,
  events: [],        // 最近事件（最多保留 50 条）
  prev: null,        // 上一份快照（用于 diff）
};

const STATE_NAMES = {
  IDLE: '空闲', RUNNING: '打印中', PAUSE: '已暂停', PAUSED: '已暂停',
  FINISH: '已完成', FAILED: '失败', PREPARE: '准备中', SLICING: '切片中',
  INIT: '初始化', OFF: '离线', UNKNOWN: '未知',
};

function pushEvent(ev) {
  ev.ts = Date.now();
  state.events.unshift(ev);
  if (state.events.length > 50) state.events.length = 50;
  broadcast({ t: 'event', ...ev });
  console.log(`[event] ${ev.kind} ${ev.dev || ''} ${ev.reason ? '- ' + ev.reason : ''}`);
}

/** 从设备数据提取紧凑状态 */
function compactDevice(d) {
  const rs = d.report_status || {};
  return {
    id: d.dev_id,
    name: d.name || d.dev_name || d.dev_id,
    model: d.dev_model || '',
    state: rs.gcode_state || 'UNKNOWN',
    progress: rs.mc_percent || 0,
    remain: rs.mc_remaining_time || 0,
    task: rs.subtask_name || rs.gcode_file || '',
    taskId: rs.task_id || '',
    nozzle: Math.round(rs.nozzle_temper || 0),
    bed: Math.round(rs.bed_temper || 0),
    layer: rs.layer_num || 0,
    totalLayer: rs.total_layer_num || 0,
    online: d.online !== false,
    hms: (rs.hms || []).map(h => ({ code: h.code, msg: h.msg || h.content || '' })),
    printError: rs.print_error || 0,
  };
}

/** diff 快照，生成事件 */
function diffAndEvents(prev, cur) {
  if (!prev) {
    // 首次不报事件，只记录
    return;
  }
  const prevMap = new Map(prev.devices.map(d => [d.id, d]));
  for (const d of cur.devices) {
    const p = prevMap.get(d.id);
    if (!p) {
      pushEvent({ kind: 'new', dev: d.name, state: d.state });
      continue;
    }
    if (p.state !== d.state) {
      const ev = { kind: 'state', dev: d.name, from: p.state, to: d.state };
      if (d.state === 'PAUSE' || d.state === 'PAUSED') {
        ev.kind = 'pause';
        // 暂停原因：优先 HMS，其次 print_error
        if (d.hms.length) {
          ev.reason = d.hms.map(h => `${h.code}${h.msg ? ':' + h.msg : ''}`).join('; ');
        } else if (d.printError) {
          ev.reason = `print_error=${d.printError}`;
        } else {
          ev.reason = '未知原因';
        }
      } else if (d.state === 'FAILED') {
        ev.kind = 'fail';
        ev.reason = d.hms.length ? d.hms.map(h => `${h.code}:${h.msg}`).join('; ') : `print_error=${d.printError}`;
      } else if (d.state === 'FINISH') {
        ev.kind = 'finish';
      } else if (d.state === 'RUNNING' && (p.state === 'PAUSE' || p.state === 'PAUSED')) {
        ev.kind = 'resume';
      }
      pushEvent(ev);
    }
    // 打印中新增 HMS：只有设备确实异常时才当故障上报。
    // 否则多半是「热床保温 / 调平 / 校准中」这类**提示**（例如 code 196609），
    // 不该红色报警打扰人 —— 这些信息仍保留在 /state 的 devices[].hms 里，可随时查。
    // （农场 API 只给 code、不给严重级别，所以只能用设备状态来判。）
    if (p.hms.length !== d.hms.length && d.hms.length > p.hms.length) {
      const abnormal = (d.state === 'FAILED' || d.state === 'PAUSE' || d.state === 'PAUSED' || d.printError > 0);
      if (abnormal) {
        pushEvent({ kind: 'hms', dev: d.name, reason: d.hms.map(h => `${h.code}:${h.msg || '?'}`).join('; ') });
      }
    }
  }
}

// ---------- 轮询 ----------
let polling = false;
async function pollOnce() {
  if (polling) return;
  polling = true;
  try {
    const [info, devs, tasks] = await Promise.all([
      farmapi.getServerInfo(),
      farmapi.getDevices(),
      farmapi.getTasks('ongoing'),
    ]);
    state.server = info;
    const cur = {
      devices: devs.map(compactDevice),
      tasks: tasks.map(t => ({
        id: String(t.id),
        name: t.task_name || ('task-' + t.id),
        remaining: t.remaining_count || 0,
        working: (t.working_devices || []).length,
        state: t.state || '',
        print_option: t.print_option || {},
      })),
    };
    diffAndEvents(state.prev, cur);
    state.prev = cur;
    state.devices = devs;
    state.tasks = tasks;
    state.lastPoll = Date.now();
    state.pollErrors = 0;
    broadcast(buildStateMessage());
  } catch (e) {
    state.pollErrors++;
    console.error(`[poll] failed (${state.pollErrors}x): ${e.message}`);
    if (state.pollErrors % 5 === 1) broadcast({ t: 'err', msg: e.message });
  } finally {
    polling = false;
  }
}

/** 构建发给 ESP32 的全量状态消息（紧凑） */
function buildStateMessage() {
  const devs = state.prev ? state.prev.devices : [];
  const tasks = state.prev ? state.prev.tasks : [];
  const running = devs.filter(d => d.state === 'RUNNING' || d.state === 'PREPARE').length;
  const paused = devs.filter(d => d.state === 'PAUSE' || d.state === 'PAUSED').length;
  const idle = devs.filter(d => d.state === 'IDLE' || d.state === 'FINISH').length;
  const offline = devs.filter(d => !d.online).length;
  if (!buildStateMessage._logged) {
    buildStateMessage._logged = true;
    console.log('[state] bid =', JSON.stringify(externalIPv4s()), 'len =', JSON.stringify(buildStateMessage().length));
  }
  return {
    t: 'state',
    srv: state.server ? { name: state.server.name, ver: (state.server.version || '').split(' ')[0] } : null,
    bid: externalIPv4s(),     // 桥接所有候选 IP，工牌挑同网段的连
    n: { run: running, pause: paused, idle, offline, total: devs.length, queue: tasks.length },
    d: devs.map(d => [d.name, d.state, d.progress, d.task, d.remain, d.nozzle, d.bed, d.online ? 1 : 0]),
    q: tasks.map(t => {
      // 打印设置标志位：bit0 自动调平 bit1 流量校准 bit2 延时摄影 bit3 AMS映射模型
      const po = t.print_option || {};
      let flags = 0;
      if (po.auto_bed_leveling) flags |= 1;
      if (po.flow_dynamic_calibration) flags |= 2;
      if (po.timelapse) flags |= 4;
      if (po.ams_mapping_model) flags |= 8;
      return [t.name, t.remaining, t.working, flags];
    }),
  };
}

// ---------- 串口 ----------
let openedSerialOnce = false;
let port = null;
let parser = null;
// 无线设备（TCP）的连接集合
const deviceSockets = new Set();
let otaBusy = false;            // 正在无线升级（升级期间要闭嘴，别往工牌塞别的包）
let tcpServer = null;
let udpServer = null;

function sendToSerial(obj) {
  try {
    if (port && port.writable) port.write(JSON.stringify(obj) + '\n');
  } catch (e) { /* 设备被拔了，忽略 */ }
}
/** 广播给所有通道（USB + 无线设备） */
function broadcast(obj) {
  // 升级期间绝对不能往工牌塞别的包：会把固件数据和状态报文搅在一起
  if (otaBusy) return;
  sendToDeviceLine(JSON.stringify(obj) + '\n');
}

/** 写一行给工牌。★ 有无线就只走无线，绝不串口+无线同时写：
 *  工牌两个通道各有一个行缓冲，但同时喂两份数据时短包会被长包截断
 *  （典型症状：工牌发了 ping 却收不到 pong，于是频繁断开重连）。 */
function sendToDeviceLine(s) {
  // 优先给"刚刚还在说话"的那条（它才是工牌真正在读的）
  const live = (typeof pickLiveSocket === 'function') ? pickLiveSocket(15000) : null;
  if (live) { try { live.write(s); return true; } catch (e) { } }
  for (const sock of deviceSockets) {
    if (sock && sock.writable) {
      try { sock.write(s); return true; } catch (e) { }
    }
  }
  try { if (port && port.writable) { port.write(s); return true; } } catch (e) { }
  return false;
}

async function listPorts() {
  try { return await SerialPort.list(); } catch (e) { return []; }
}

/** 自动探测 ESP32-C3：优先固定端口，其次 USB 串口设备（按 vendorId 识别） */
async function autoDetectPort() {
  if (FIXED_PORT) return FIXED_PORT;
  const ports = await listPorts();
  console.log('[serial] available ports:', ports.map(p => `${p.path}(${p.manufacturer || '?'} vid=${p.vendorId || '?'})`).join(', ') || '(none)');
  const score = (p) => {
    const text = ((p.manufacturer || '') + ' ' + (p.product || '') + ' ' + (p.friendlyName || '') + ' ' + (p.pnpId || '')).toLowerCase();
    // 排除蓝牙串口（BTHENUM / 蓝牙）
    if (/bthenum|bluetooth|蓝牙/.test(text)) return -1;
    // 排除标准端口（无 USB 厂商 ID 的 COM1/COM2）
    if (/^(com1|com2)$/.test(p.path.toLowerCase())) return -1;
    const vid = (p.vendorId || '').toLowerCase();
    let s = 0;
    if (vid === '303a') s += 100;   // Espressif ESP32 USB Serial/JTAG
    if (vid === '1a86') s += 90;    // 沁恒 CH340/CH9102
    if (vid === '10c4') s += 80;    // Silicon Labs CP210x
    if (vid === '0403') s += 70;    // FTDI
    if (/esp32|espressif/.test(text)) s += 60;
    if (/usb.*serial|serial.*usb|串行/.test(text) && vid) s += 20;
    return s;
  };
  const ranked = ports.map(p => ({ p, s: score(p) })).filter(x => x.s > 0).sort((a, b) => b.s - a.s);
  if (ranked.length) return ranked[0].p.path;
  return null; // 没有候选，等待重试
}

async function openSerial() {
  // --nousb：模拟"工牌没插数据线"，只能靠 WiFi 扫描找到它
  if (args.includes('--nousb')) {
    if (!openedSerialOnce) {
      openedSerialOnce = true;
      console.log('[serial] 已禁用 USB 串口（--nousb），只走无线');
    }
    return;
  }
  const path = await autoDetectPort();
  if (!path) {
    console.log('[serial] no port found, retrying in 10s...');
    setTimeout(openSerial, 10000);
    return;
  }
  try {
    port = new SerialPort({ path, baudRate: 115200, autoOpen: false });
    parser = port.pipe(new ReadlineParser({ delimiter: '\n' }));
    await new Promise((resolve, reject) => {
      port.open(err => err ? reject(err) : resolve());
    });
    console.log(`[serial] opened ${path} @115200`);
    port.on('close', () => {
      console.log('[serial] closed, reconnecting in 5s...');
      port = null;
      setTimeout(openSerial, 5000);
    });
    // ★ 串口写错误（比如设备被拔掉）必须有 handler，
    //   否则 serialport 的 'error' 事件没人接 → 整个进程崩掉
    port.on('error', (e) => {
      console.error(`[serial] error: ${e.message}（尝试重连）`);
      try { if (port && port.isOpen) port.close(); } catch (e2) { }
      port = null;
      setTimeout(openSerial, 5000);
    });
    parser.on('data', line => handleDeviceLine(line));
    // 上线立即推一次全量状态
    broadcast({ t: 'welcome', msg: 'farm-bridge online' });
    setTimeout(() => broadcast(buildStateMessage()), 500);
  } catch (e) {
    console.error(`[serial] open ${path} failed: ${e.message}`);
    setTimeout(openSerial, 10000);
  }
}

/** 设备名/ID 解析：接受 "A1-01" 或 "03900D..." 两种 */
function resolveDevId(nameOrId) {
  const s = String(nameOrId || '');
  const devs = state.prev ? state.prev.devices : [];
  const byName = devs.find(d => d.name === s || d.id === s);
  if (byName) return byName.id;
  return s;
}
/** dev_id -> 设备名（结果回显用） */
function devNameOf(devId) {
  const s = String(devId || '');
  const devs = state.prev ? state.prev.devices : [];
  const hit = devs.find(d => d.id === s);
  return hit ? hit.name : s;
}
/** 任务名/ID 解析 */
function resolveTaskId(nameOrIdx) {
  const s = String(nameOrIdx || '');
  const tasks = state.prev ? state.prev.tasks : [];
  // 数字索引
  if (/^\d+$/.test(s)) {
    const byIdx = tasks[parseInt(s, 10)];
    if (byIdx) return byIdx.id;
    // 也可能是 task id 本身
    const byId = tasks.find(t => t.id === s);
    if (byId) return byId.id;
    return s;
  }
  const byName = tasks.find(t => t.name === s);
  return byName ? byName.id : s;
}

/** 处理工牌来的命令（USB 串口和无线 TCP 共用）
 *  src = 消息来自哪个 socket（串口传 null）——用来认出"工牌真正接受的那条连接" */
async function handleDeviceLine(line, src) {
  let msg;
  try { msg = JSON.parse(line.trim()); } catch (e) {
    // 工牌的调试打印（Serial.println）不是 JSON，之前被静默丢掉，排错时完全看不见
    console.log(`[dev-raw] ${line.trim().slice(0, 200)}`);
    return;
  }
  // OTA 回执量太大（几百条），交给等待者，不打日志
  // 记下这条连接"刚刚还在说话"—— 用来认出工牌真正使用的那条
  if (src) src.__lastSeen = Date.now();
  // 保活：必须立刻回，而且升级期间（broadcast 被静音）也要回，
  // 否则工牌会以为连接死了，把正在传固件的连接踢掉。
  if (msg.t === 'ping') {
    const now = Date.now();
    if (now - (handleDeviceLine._lastPingLog || 0) > 30000) {
      handleDeviceLine._lastPingLog = now;
      console.log(`[ping] 收到工牌心跳，来源：${src ? ('tcp ' + src.remoteAddress) : '串口'}`);
    }
    // 原路回一条，同时给串口也回一条做双保险。
    // 工牌那边串口和 TCP 各有独立的行缓冲，所以两边都写不会互相搅乱
    // （以前共用一个缓冲时才必须二选一），任一路收到都算保活成功。
    const reply = JSON.stringify({ t: 'pong', i: msg.i }) + '\n';
    if (src && src.writable) { try { src.write(reply); } catch (e) { } }
    try { if (port && port.writable) port.write(reply); } catch (e) { }
    return;
  }
  // pong 是桥接自己发出去的，不该被当成设备命令再回一次 ack
  if (msg.t === 'pong') {
    if (!handleDeviceLine._pongDbg) {
      handleDeviceLine._pongDbg = true;
      console.log(`[dbg] 收到 pong（自己发的被读回来了），来源：${
        src ? ('tcp ' + src.remoteAddress + ':' + src.remotePort) : '串口'}`);
    }
    return;
  }
  if (msg.t === 'ota') {
    // 每块都打会把日志刷爆，只记关键节点
    if (msg.cmd !== 'chunk') console.log(`[ota<-] ${JSON.stringify(msg).slice(0, 120)}`);
    const w = otaWaiters.shift();
    if (w) { clearTimeout(w.timer); w.resolve(msg); }
    return;
  }
    console.log(`[serial <-]`, JSON.stringify(msg).slice(0, 200));
  try {
    switch (msg.t) {
      case 'hello':
        // 工牌只在「接受连接」的那一刻发 hello（带 ip）。抓到它就说明 src 这条
        // 才是工牌真正在读的连接 —— 同 IP 的其它旧连接一律丢掉，
        // 否则写固件会写到一条没人读的死连接上，表现就是"工牌毫无反应"。
        if (msg.ip && src) {
          console.log(`[tcp] 认领 ${msg.ip}（工牌主动报 hello，这条是活连接）`);
          for (const s of Array.from(deviceSockets)) {
            if (s !== src && (s.remoteAddress === msg.ip || s.remoteAddress === src.remoteAddress)) {
              try { s.destroy(); } catch (e) { }
              deviceSockets.delete(s);
            }
          }
          knownDevices.set(msg.ip, src);
        }
        broadcast(buildStateMessage());
        break;
      case 'get_state':
        broadcast(buildStateMessage());
        break;
      case 'launch': {
        const taskId = resolveTaskId(msg.task_id !== undefined ? msg.task_id : msg.task_idx);
        const devIds = (msg.dev_ids && msg.dev_ids.length) ? msg.dev_ids.map(resolveDevId) : null;
        if (!taskId || taskId === 'undefined') { broadcast({ t: 'ack', ok: false, msg: 'task_id required' }); break; }
        // 料盘映射：ams_id=255 表示外挂，0..N 是 AMS 单元
        const amsId = (msg.ams_id === undefined) ? 255 : Number(msg.ams_id);
        const slotId = (msg.slot_id === undefined) ? 0 : Number(msg.slot_id);
        // 打印设置：po = [自动调平, 流量校准, 延时摄影]，默认 [1,1,0]
        const po = Array.isArray(msg.po) ? msg.po : [1, 1, 0];
        const printOption = {
          auto_bed_leveling: !!po[0],
          flow_dynamic_calibration: !!po[1],
          timelapse: !!po[2],
        };
        console.log(`[launch] 打印设置 调平=${po[0] ? '开' : '关'} 校准=${po[1] ? '开' : '关'} 延时=${po[2] ? '开' : '关'}`);
        let r;
        if (devIds) {
          r = await farmapi.launchTaskPrinters(taskId, devIds.map(id => ({
            dev_id: id,
            ams_mapping2: [{ ams_id: amsId, slot_id: slotId }],
            nozzle_mapping: [],
            print_option: printOption,
          })), { print_option: printOption });
        } else {
          const idle = (state.prev ? state.prev.devices : []).filter(d => (d.state === 'IDLE' || d.state === 'FINISH') && d.online).slice(0, 1);
          if (!idle.length) { broadcast({ t: 'ack', ok: false, msg: 'no idle device' }); break; }
          r = await farmapi.launchTaskPrinters(taskId, idle.map(d => ({
            dev_id: d.id, ams_mapping2: [{ ams_id: amsId, slot_id: slotId }], nozzle_mapping: [],
            print_option: printOption,
          })), { print_option: printOption });
        }
        const ret = (r.json && r.json.ret) ? r.json.ret : [];
        const okCount = ret.filter(x => x.code === 0).length;
        // 结果里把 dev_id 映射回设备名，方便工牌显示
        const res = ret.map(x => [devNameOf(x.dev_id) || x.dev_id, x.code || 0, x.message || '']);
        broadcast({
          t: 'ack',
          ok: okCount > 0,
          msg: okCount ? `已下发 ${okCount} 台` : ((ret[0] && ret[0].message) || '下发失败'),
          res,
        });
        pushEvent({ kind: 'launch', dev: (ret[0] && ret[0].dev_id) || '', reason: `task ${taskId}` });
        setTimeout(pollOnce, 1500);
        break;
      }
      case 'opt': {
        const devId = resolveDevId(msg.dev_id);
        const opt = String(msg.opt || '');
        if (!devId || !opt) { broadcast({ t: 'ack', ok: false, msg: 'dev_id and opt required' }); break; }
        const r = await farmapi.taskOpt(devId, opt);
        broadcast({ t: 'ack', ok: r.status === 200, msg: `${opt} -> ${devId}` });
        setTimeout(pollOnce, 2000);
        break;
      }
      case 'ok':
        // 工牌的状态回执 + 按键电压上报，静默处理不要回错误
        if (msg.v) console.log(`[serial <-] 工牌版本 ${msg.v} | 设备 ${msg.devs} 任务 ${msg.tasks}`);
        break;
      case 'wifi':
        // 工牌在局域网里的 IP —— 桥接主动连过去（出站，不受防火墙入站限制）
        if (msg.ip && msg.tag !== 'hb') console.log(`[serial <-] 工牌 WiFi ${msg.tag} ${msg.ip}`);
        // ★ 必须排除自己的 IP：工牌会把"桥接 IP"也报回来，
        //   不排除就会连上自己 → 广播被自己的 server 收到 → 无限回环
        if (msg.ip && !externalIPv4s().includes(String(msg.ip))) {
          connectDeviceIfNeeded(msg.ip);
        }
        break;
      case 'hello':
        // 工牌上线自报（TCP 连上时发的）
        if (msg.ip) {
          console.log(`[dev] 工牌上线 ${msg.ip} mac=${msg.mac || '?'}`);
          knownDevices.set(msg.ip, knownDevices.get(msg.ip) || null);
        }
        break;
      case 'ack':
        // 工牌回执：只记录，绝不回复（回复会形成回环）
        if (msg.msg) console.log(`[serial <-] 工牌回执: ${msg.msg}`);
        break;
      case 'key':
        // 按键实测电压（用于校准 ADC 阈值）
        console.log(`[serial <-] 按键 mv=${msg.mv}`);
        break;
      case 'batdump':
        // 电池计寄存器原始值（用于对寄存器布局）
        console.log(`[serial <-] 电池寄存器: ${msg.d}`);
        break;
      default:
        broadcast({ t: 'ack', ok: false, msg: 'unknown command: ' + msg.t });
    }
  } catch (e) {
    broadcast({ t: 'ack', ok: false, msg: e.message });
  }
}

// ---------- 无线升级（OTA over link）----------
// 电脑防火墙拦入站，标准 espota 要"板子回连 PC"连不上；
// 这里顺着已有的 PC→工牌 链路把固件推下去：begin / chunk(b64) / end
const otaWaiters = [];

/** 挑一条下发通道：优先无线 TCP，没有才用 USB 串口（同一时刻只用一条，避免两份数据混在一起） */
function otaChannel() {
  for (const s of deviceSockets) {
    const a = (s && s.remoteAddress) || '';
    // 回环地址绝不能当通道（否则写出去被自己读回，工牌永远收不到）
    if (s && s.writable && a !== '127.0.0.1' && a !== '::1' && a !== '::ffff:127.0.0.1')
      return { kind: 'tcp', write: (line) => s.write(line) };
  }
  if (port && port.writable) return { kind: 'serial', write: (line) => port.write(line) };
  return null;
}

/** 挑一条「工牌真正在用」的连接。
 *  判断依据：工牌每 5 秒发一次心跳，心跳落在哪条 socket 上，哪条就是它在读的那条。
 *  （桥接手里往往同时有好几条到同一台工牌的连接，写错一条就等于"点了没反应"。） */
function pickLiveSocket(maxAgeMs) {
  let best = null, bestT = 0;
  for (const s of deviceSockets) {
    if (!s || !s.writable) continue;
    const a = s.remoteAddress || '';
    if (a === '127.0.0.1' || a === '::1' || a === '::ffff:127.0.0.1') continue;  // 永不选回环
    const t = s.__lastSeen || 0;
    if (t > bestT) { bestT = t; best = s; }
  }
  if (best && (Date.now() - bestT) < maxAgeMs) return best;
  return null;
}

/** 等到出现一条活连接，并用 abort 握手确认工牌真的在这条上回应
 *  （abort 顺带把上一轮半途而废的升级清掉，一举两得） */
async function ensureFreshChannel(log) {
  const deadline = Date.now() + 25000;
  const tried = new Set();
  while (Date.now() < deadline) {
    const s = pickLiveSocket(15000);
    if (s && !tried.has(s)) {
      tried.add(s);
      const r = await sendOtaAndWait({ t: 'ota', cmd: 'abort' }, 6000, { write: (line) => s.write(line) });
      if (r) {
        log(`[ota] 通道就绪 ${s.remoteAddress}（工牌有回应，顺带清掉了上轮残留）`);
        return { sock: s, ip: s.remoteAddress };
      }
      log(`[ota] ${s.remoteAddress} 这条没回应，换一条…`);
    }
    await new Promise(r => setTimeout(r, 800));
  }
  return null;
}

/** 发一条命令，等工牌的 ota 回执 */
function sendOtaAndWait(obj, timeoutMs, ch) {
  return new Promise((resolve) => {
    if (!ch) ch = otaChannel();
    if (!ch) return resolve(null);
    const w = {
      timer: setTimeout(() => {
        const i = otaWaiters.indexOf(w);
        if (i >= 0) otaWaiters.splice(i, 1);
        resolve(null);
      }, timeoutMs),
      resolve,
    };
    otaWaiters.push(w);
    try { ch.write(JSON.stringify(obj) + '\n'); } catch (e) {
      clearTimeout(w.timer);
      const i = otaWaiters.indexOf(w);
      if (i >= 0) otaWaiters.splice(i, 1);
      resolve(null);
    }
  });
}

/** 把固件推给工牌 */
async function pushOtaToDevice(buf, log) {
  if (otaBusy) throw new Error('上一次升级还没结束');
  otaBusy = true;
  try {
    // 必须用一条确认是"工牌在读"的连接，否则固件会写进死连接
    const fresh = await ensureFreshChannel(log);
    let CHANNEL = null;
    if (fresh) CHANNEL = { kind: 'tcp', write: (line) => fresh.sock.write(line) };
    else if (port && port.writable) CHANNEL = { kind: 'serial', write: (line) => port.write(line) };
    if (!CHANNEL) throw new Error('连不上工牌：无线没握手成功，USB 也没插');
    log(`[ota] 通道 ${CHANNEL.kind}${fresh ? ' ' + fresh.ip : ''}，固件 ${buf.length} 字节，开始升级…`);

    // 先清场：上一轮要是半途断了，工牌会卡在"升级中"，不 abort 就再也起不来
    await sendOtaAndWait({ t: 'ota', cmd: 'abort' }, 8000, CHANNEL);

    const begin = await sendOtaAndWait({ t: 'ota', cmd: 'begin', size: buf.length }, 20000, CHANNEL);
    if (!begin || !begin.ok) throw new Error('工牌拒绝开始升级：' + ((begin && begin.m) || '无回应'));

    const CH = 1024;
    let off = 0;
    let lastLog = -1;
    while (off < buf.length) {
      const slice = buf.subarray(off, Math.min(off + CH, buf.length));
      const r = await sendOtaAndWait({ t: 'ota', cmd: 'chunk', off, b64: slice.toString('base64') }, 20000, CHANNEL);
      if (!r || !r.ok) {
        await sendOtaAndWait({ t: 'ota', cmd: 'abort' }, 8000, CHANNEL).catch(() => { });
        throw new Error(`写入 ${off} 字节处失败：${(r && r.m) || '无回应'}`);
      }
      off += slice.length;
      const pct = Math.floor((off * 100) / buf.length);
      if (pct !== lastLog && pct % 5 === 0) { lastLog = pct; log(`[ota] ${pct}%`); }
    }

    const end = await sendOtaAndWait({ t: 'ota', cmd: 'end' }, 40000, CHANNEL);
    if (!end || !end.ok) throw new Error('收尾失败：' + ((end && end.m) || '无回应'));
    log('[ota] 升级完成，工牌正在重启');
    return { ok: true, bytes: buf.length };
  } finally {
    otaBusy = false;
  }
}

// ---------- HTTP 调试接口 ----------
const httpServer = http.createServer(async (req, res) => {
  const url = new URL(req.url, 'http://x');
  const send = (code, obj) => {
    res.writeHead(code, { 'content-type': 'application/json; charset=utf-8', 'access-control-allow-origin': '*' });
    res.end(JSON.stringify(obj, null, 1));
  };
  try {
    if (url.pathname === '/state') {
      // 工牌在线情况（托盘图标要靠它判断颜色）
      const badges = [];
      for (const s of deviceSockets) {
        if (s && s.writable) {
          badges.push({ ip: s.remoteAddress, seen: s.__lastSeen || 0, via: 'tcp' });
        }
      }
      if (!badges.length && port && port.writable) badges.push({ ip: 'usb', seen: Date.now(), via: 'usb' });
      return send(200, {
        server: state.server,
        lastPoll: state.lastPoll,
        pollErrors: state.pollErrors,
        summary: buildStateMessage().n,
        badge: {
          online: badges.length > 0,
          via: badges.length ? badges[0].via : null,
          ip: badges.length ? badges[0].ip : null,
          // 10 秒内说过话才算真在线（心跳 5 秒一次）
          fresh: badges.some(b => Date.now() - b.seen < 10000),
          count: badges.length,
        },
        devices: state.prev ? state.prev.devices : [],
        tasks: state.prev ? state.prev.tasks : [],
        events: state.events.slice(0, 10),
        serial: port ? port.path : null,
      });
    }
    if (url.pathname === '/devices') return send(200, state.prev ? state.prev.devices : []);
    if (url.pathname === '/tasks') return send(200, state.prev ? state.prev.tasks : []);
    if (url.pathname === '/events') return send(200, state.events);
    if (url.pathname === '/launch' && req.method === 'POST') {
      let body = '';
      req.on('data', d => body += d);
      req.on('end', async () => {
        try {
          const m = JSON.parse(body || '{}');
          const r = await farmapi.launchTaskPrinters(String(m.task_id), (m.dev_ids || []).map(id => ({ dev_id: id })));
          send(200, r.json || r.raw);
        } catch (e) { send(500, { error: e.message }); }
      });
      return;
    }
    if (url.pathname === '/opt' && req.method === 'POST') {
      let body = '';
      req.on('data', d => body += d);
      req.on('end', async () => {
        try {
          const m = JSON.parse(body || '{}');
          const r = await farmapi.taskOpt(String(m.dev_id), String(m.opt));
          send(200, { status: r.status, body: r.raw });
        } catch (e) { send(500, { error: e.message }); }
      });
      return;
    }
    if (url.pathname === '/poll' && req.method === 'POST') {
      await pollOnce();
      return send(200, { ok: true, lastPoll: state.lastPoll });
    }
    // 无线升级：POST 一个 .bin 过来（application/octet-stream），
    // 或者 POST {"path":"tools/build/xxx.bin"} 让桥接自己去读
    if (url.pathname === '/ota' && req.method === 'POST') {
      const chunks = [];
      for await (const c of req) chunks.push(c);
      const body = Buffer.concat(chunks);
      let buf = null;
      const ctype = String(req.headers['content-type'] || '');
      if (ctype.includes('json')) {
        try {
          const m = JSON.parse(body.toString('utf8'));
          const p = nodePath.isAbsolute(m.path) ? m.path : nodePath.join(ROOT, m.path);
          buf = fs.readFileSync(p);
          console.log(`[ota] 读取文件 ${p} (${buf.length} 字节)`);
        } catch (e) { return send(400, { error: 'path 读取失败: ' + e.message }); }
      } else {
        buf = body;
      }
      if (!buf || !buf.length) return send(400, { error: '没有固件数据' });
      try {
        const r = await pushOtaToDevice(buf, (s) => console.log(s));
        return send(200, r);
      } catch (e) {
        return send(500, { error: e.message });
      }
    }
    // 调试用：把任意 JSON 原样转发给工牌（测告警音、测命令都靠它）
    if (url.pathname === '/send' && req.method === 'POST') {
      let body = '';
      for await (const c of req) body += c;
      const s = body.toString('utf8').trim();
      if (!s) return send(400, { error: 'body 为空' });
      try { JSON.parse(s); } catch (e) { return send(400, { error: '不是合法 JSON: ' + e.message }); }
      const ch = otaChannel();
      if (!ch) return send(503, { error: '没有在线的工牌' });
      ch.write(s + '\n');
      return send(200, { ok: true, sent: s.slice(0, 200) });
    }
    if (url.pathname === '/ota' && req.method === 'GET') {
      const devs = [];
      for (const s of deviceSockets) if (s && s.writable) devs.push(s.remoteAddress);
      return send(200, { busy: otaBusy, wireless: devs, usb: port ? port.path : null });
    }
    send(404, { error: 'not found', endpoints: ['/state', '/devices', '/tasks', '/events', 'POST /launch', 'POST /opt', 'POST /poll', 'POST /ota'] });
  } catch (e) {
    send(500, { error: e.message });
  }
});

let connectDevice = null;
const knownDevices = new Map();   // ip -> socket
function connectDeviceIfNeeded(ip) {
  if (connectDevice) connectDevice(ip);
}

/** 主动连接一台工牌（PC 出站，不受防火墙入站限制） */
/** 把一个已建立的 socket 注册为"工牌通道" */
function registerDeviceSocket(sock, ip) {
  // ★ 绝不能把"连到自己"的 socket 当成工牌：桥接自己也在 8897 上监听，
  //   一旦连上自己，写出去的包会被自己的服务端读回来（回环），
  //   表现为日志里刷 unknown command、状态误判在线、OTA 抢错通道。
  //   注意 externalIPv4s() 不含 127.0.0.1（它是 internal），所以必须单独拦回环。
  const self = externalIPv4s();
  const LOOPBACK = ['127.0.0.1', '::1', '::ffff:127.0.0.1', '0.0.0.0'];
  if (ip && (self.includes(String(ip)) || LOOPBACK.includes(String(ip)))) {
    console.log(`[tcp] 忽略连到本机/回环的连接 ${ip}`);
    try { sock.destroy(); } catch (e) { }
    return;
  }
  deviceSockets.add(sock);
  knownDevices.set(ip, sock);
  sock.on('data', (chunk) => {
    let buf = chunk.toString('utf8');
    let idx;
    while ((idx = buf.indexOf('\n')) >= 0) {
      const line = buf.slice(0, idx);
      buf = buf.slice(idx + 1);
      if (line.trim()) handleDeviceLine(line, sock);
    }
  });
  sock.on('close', () => {
    deviceSockets.delete(sock);
    knownDevices.delete(ip);
    console.log(`[tcp] 工牌断开 ${ip}`);
  });
  sock.on('error', () => {
    deviceSockets.delete(sock);
    knownDevices.delete(ip);
  });
}

function doConnectDevice(ip) {
  const net = require('net');
  if (knownDevices.has(ip)) return;
  const sock = net.createConnection({ host: ip, port: DEVICE_TCP_PORT }, () => {
    console.log(`[tcp] 已连上工牌 ${ip}:${DEVICE_TCP_PORT}`);
    registerDeviceSocket(sock, ip);
    sock.write(JSON.stringify({ t: 'welcome', msg: 'farm-bridge online' }) + '\n');
    setTimeout(() => { try { sock.write(JSON.stringify(buildStateMessage()) + '\n'); } catch (e) { } }, 300);
  });
  sock.on('error', () => { deviceSockets.delete(sock); knownDevices.delete(ip); });
}

/** 认领式探测：连上后等工牌自报 hello，认领成功就**直接用这条连接**（不丢弃）
 *  —— 之前"连上立刻关闭"会把工牌那边的 socket 弄成半死状态，之后就接不上了。 */
function tryAdopt(ip, port, timeoutMs) {
  const net = require('net');
  return new Promise((resolve) => {
    const s = net.createConnection({ host: ip, port });
    let buf = '';
    let settled = false;
    const finish = (v) => {
      if (settled) return;
      settled = true;
      clearTimeout(t);
      if (!v) { try { s.destroy(); } catch (e) { } }
      resolve(v);
    };
    const t = setTimeout(() => finish(null), timeoutMs);
    s.on('data', (d) => {
      buf += d.toString('utf8');
      if (buf.indexOf('"t":"hello"') >= 0) finish({ sock: s, ip });
    });
    s.on('error', () => finish(null));
    s.on('close', () => finish(null));
  });
}

/** 探测某个地址的 8897 端口是否开着（工牌在那里开服务） */
function probeHost(ip, port, timeoutMs) {
  const net = require('net');
  return new Promise((resolve) => {
    const s = net.createConnection({ host: ip, port });
    let done = false;
    const finish = (v) => { if (!done) { done = true; try { s.destroy(); } catch (e) { } resolve(v); } };
    const t = setTimeout(() => finish(false), timeoutMs);
    s.on('connect', () => { clearTimeout(t); finish(true); });
    s.on('error', () => { clearTimeout(t); finish(false); });
  });
}

/** 扫描本地网段找工牌（工牌的 TCP 服务在 8897）
 *  必须扫描：Windows 防火墙挡入站，工牌发过来的 UDP 收不到，
 *  只有 PC 主动连出去这条路是通的 —— 所以 PC 得自己找到工牌的 IP。 */
let scanning = false;
async function scanForDevices() {
  if (scanning) return [];          // 上一次还没扫完就别再起一轮，否则几百个连接叠在一起
  scanning = true;
  try { return await doScan(); } finally { scanning = false; }
}

async function doScan() {
  const os = require('os');
  const nets = os.networkInterfaces();
  const allTargets = [];
  for (const name of Object.keys(nets)) {
    for (const n of nets[name] || []) {
      if (n.family !== 'IPv4' || n.internal) continue;
      const p = n.address.split('.').slice(0, 3).join('.');
      for (let i = 1; i < 255; i++) allTargets.push(`${p}.${i}`);
    }
  }
  const self = externalIPv4s();
  const targets = allTargets.filter(ip => !self.includes(ip));
  console.log(`[scan] 扫描 ${targets.length} 个地址找工牌 (TCP ${DEVICE_TCP_PORT})...`);
  // 分批扫：一次性开 500+ 个连接容易把句柄打满，一批 64 个稳一些
  const results = [];
  const CONC = 64;
  for (let i = 0; i < targets.length; i += CONC) {
    const batch = targets.slice(i, i + CONC);
    const rs = await Promise.all(batch.map(ip =>
      tryAdopt(ip, DEVICE_TCP_PORT, 1200).then(r => r || null).catch(() => null)));
    for (const r of rs) results.push(r);
  }
  for (const r of results) {
    if (r && !knownDevices.has(r.ip)) {
      console.log(`[scan] 发现工牌 ${r.ip}，认领连接`);
      registerDeviceSocket(r.sock, r.ip);
      r.sock.write(JSON.stringify({ t: 'welcome', msg: 'farm-bridge online' }) + '\n');
      setTimeout(() => { try { r.sock.write(JSON.stringify(buildStateMessage()) + '\n'); } catch (e) { } }, 300);
    }
  }
  return results.filter(Boolean).map(r => r.ip);
}

/** 启动无线通道：
 *  策略反转 —— 工牌当 TCP 服务端，PC 主动连过去。
 *  因为 Windows 防火墙默认挡入站：PC 广播(出站) → 工牌应答 → PC 连工牌(出站)，全程不碰入站规则。
 */
function startWireless() {
  const net = require('net');
  const dgram = require('dgram');
  connectDevice = (ip) => { doConnectDevice(ip); };

  // UDP：PC 广播 "FARM?"，工牌回 "HERE:<ip>"
  udpServer = dgram.createSocket('udp4');
  udpServer.on('message', (msg, rinfo) => {
    const txt = msg.toString('utf8');
    if (txt.indexOf('HERE:') < 0) return;
    const ip = txt.slice(5).trim();
    if (!ip || knownDevices.has(ip)) return;
    console.log(`[udp] 工牌应答 ${ip}，准备连过去`);
    connectDevice(ip);
  });
  udpServer.on('error', e => console.error('[udp] error:', e.message));
  udpServer.bind(DEVICE_TCP_PORT, () => {
    console.log(`[udp] 发现服务监听 ${DEVICE_TCP_PORT}`);
    // 每 3 秒广播一次，问局域网里的工牌在哪（有些网络允许广播）
    const bc = Buffer.from('FARM?');
    setInterval(() => {
      try { udpServer.send(bc, DEVICE_TCP_PORT, '255.255.255.255'); } catch (e) { }
      for (const ip of knownDevices.keys()) {
        try { udpServer.send(bc, DEVICE_TCP_PORT, ip); } catch (e) { }
      }
    }, 3000);
    // 广播被防火墙挡了就靠扫描：启动时扫一次，之后每 30 秒再扫一次
    setTimeout(() => scanForDevices(), 3000);
    setInterval(() => { if (knownDevices.size === 0) scanForDevices(); }, 30000);
  });


  // 兼容：工牌主动连过来的情况
  tcpServer = net.createServer((sock) => {
    const peer = `${sock.remoteAddress}:${sock.remotePort}`;
    deviceSockets.add(sock);
    console.log(`[tcp] 工牌主动连接 ${peer}（在线设备 ${deviceSockets.size}）`);
    sock.write(JSON.stringify({ t: 'welcome', msg: 'farm-bridge online' }) + '\n');
    setTimeout(() => { try { sock.write(JSON.stringify(buildStateMessage()) + '\n'); } catch (e) { } }, 300);
    let buf = '';
    sock.on('data', (chunk) => {
      buf += chunk.toString('utf8');
      let idx;
      while ((idx = buf.indexOf('\n')) >= 0) {
        const line = buf.slice(0, idx);
        buf = buf.slice(idx + 1);
        if (line.trim()) handleDeviceLine(line, sock);
      }
      if (buf.length > 8192) buf = '';
    });
    sock.on('close', () => {
      deviceSockets.delete(sock);
      console.log(`[tcp] 工牌断开 ${peer}（在线设备 ${deviceSockets.size}）`);
    });
    sock.on('error', () => deviceSockets.delete(sock));
  });
  tcpServer.on('error', e => console.error('[tcp] error:', e.message));
  tcpServer.listen(DEVICE_TCP_PORT, () => console.log(`[tcp] 设备服务监听 ${DEVICE_TCP_PORT}`));
}

/** 取本机所有外部 IPv4（工牌自己挑同网段的连）
 *  注意：这台机器有多张网卡（Hyper-V 虚拟网卡 192.168.144.1 / LAN 10.10.1.118），
 *  之前只返回第一张，工牌按那个 IP 连不上。 */
function externalIPv4s() {
  const os = require('os');
  const nets = os.networkInterfaces();
  const out = [];
  for (const name of Object.keys(nets)) {
    for (const n of nets[name] || []) {
      if (n.family === 'IPv4' && !n.internal) out.push(n.address);
    }
  }
  return out.length ? out : ['127.0.0.1'];
}
function localIPv4() {
  const list = externalIPv4s();
  // 优先选 10.x（普通局域网），其次 192.168.x
  return list.find(ip => ip.startsWith('10.')) || list[0];
}

// ---------- 启动 ----------
httpServer.on('error', (e) => {
  if (e.code === 'EADDRINUSE') {
    console.error(`[http] port ${HTTP_PORT} busy (another bridge running?) — retrying in 3s`);
    setTimeout(() => httpServer.listen(HTTP_PORT), 3000);
  } else {
    console.error('[http] server error:', e.message);
  }
});

(async () => {
  console.log('=== Farm Bridge for AI Passport (ESP32-C3) ===');
  console.log(`config: http=:${HTTP_PORT} poll=${POLL_MS}ms port=${FIXED_PORT || 'auto'}`);
  httpServer.listen(HTTP_PORT, () => console.log(`[http] listening on http://127.0.0.1:${HTTP_PORT}`));
  startWireless();
  openSerial();
  await pollOnce();
  setInterval(pollOnce, POLL_MS);
})();
