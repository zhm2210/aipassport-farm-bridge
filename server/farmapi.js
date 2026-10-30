/**
 * farmapi.js — 拓竹农场 API 客户端（自动 sec-ts / JWT 续期）
 *
 * 认证四要素：
 *   1. mTLS: CN=farm_client.bambulab.com 证书链 + 私钥 (credentials/)
 *   2. SNI=farm_server.bambulab.com, ALPN http/1.1, 连 192.168.144.1:8888
 *   3. JWT Bearer（从客户端进程内存提取，自动挑选未过期的最新）
 *   4. x-bbl-* 全套头（关键是 x-bbl-app-ver 和 x-bbl-sec-ts）
 *
 * ★ 2026-10-25 重大突破：POST body 加密旁路
 *   CipherForceMiddleware 只拦截 Content-Type 恰好为 "application/json"（16字符）的请求，
 *   返回 {"code":4,"message":"invalid request"}。Chromium 实际发送的是
 *   "application/json;charset=UTF-8"（31字符）——天然绕过！
 *   text/plain 会走 AES-GCM 解密路径（失败也报 invalid request）。
 *   => 所有 POST 用 application/json;charset=UTF-8 + 明文 JSON body，直通处理器。
 *   （真实客户端对 bulk-start 会加密 body，但我们不需要——服务端照样处理明文）
 */
const tls = require('tls');
const net = require('net');
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const { execFileSync } = require('child_process');

const ROOT = path.join(__dirname, '..');
const CRED = path.join(ROOT, 'credentials');
const CHAIN = fs.readFileSync(path.join(CRED, 'client_chain.pem'), 'utf8');
const KEY = fs.readFileSync(path.join(CRED, 'client_key.pem'), 'utf8');

// 农场本地服务地址（mTLS，8888）：
//   旧版 Bambu Farm Manager 会建 192.168.144.1 虚拟网卡并在其上开接口；
//   新版客户端可能只绑 ::（不再建该虚拟网卡）—— 此时 192.168.144.1 根本不存在，
//   请求全部失败，还会被误当成“sec-ts 失效”而把候选逐个耗尽。
//   所以改为依次探测候选地址，用第一个 TCP 能连上的。
//   要固定地址就设环境变量 FARM_HOST=x.x.x.x
const FARM_PORT = 8888;
const FARM_HOST_CANDIDATES = (process.env.FARM_HOST || '127.0.0.1,192.168.144.1,localhost')
  .split(',').map((s) => s.trim()).filter(Boolean);
let FARM_HOST = FARM_HOST_CANDIDATES[0];
let farmHostProbed = false;
const SNI = 'farm_server.bambulab.com';
const SERVER_ID = '2d0f7379-8c26-4065-b25b-2ac942d3bd42ffc3';

const BASE_HEADERS = [
  'user-agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) BambuFarmManagerClient/3.0.0 Chrome/130.0.6723.137 Electron/33.2.1 Safari/537.36',
  'x-bbl-client-name: BambuFarmManager',
  'x-bbl-client-type: farm-web',
  'x-bbl-device-id: 4832ddec-4761-4edc-8ec3-7493eeeb3537',
  'x-bbl-language: zh-CN',
  'x-bbl-os-type: windows',
  'x-bbl-os-version: 10.0',
  'x-bbl-sec-sid: ' + SERVER_ID,
  'x-bbl-sec-ver: 2',
  'x-bbl-app-ver: 3.0.0',
  'accept-language: *',
  'accept-encoding: identity',
];

// ---------- 凭据管理 ----------
let sectPool = [];       // 候选 sec-ts（内存提取的，可能有过期的）
let sectInUse = null;    // 当前验证可用的
let jwt = null;          // 当前 JWT
let lastExtract = 0;
let everExtracted = false;   // 曾成功提取过？用来区分“客户端没运行”和“候选被耗光”
const EXTRACT_MIN_INTERVAL = 10 * 1000; // 两次提取最小间隔

function pickFreshestJwt(cands) {
  let best = null, bestExp = 0;
  for (const c of cands) {
    const parts = c.split('.');
    if (parts.length !== 3) continue;
    try {
      const p = JSON.parse(Buffer.from(parts[1], 'base64url').toString());
      if (p.exp && p.exp > bestExp) { bestExp = p.exp; best = c; }
    } catch (e) { }
  }
  return best;
}

function jwtExpired() {
  if (!jwt) return true;
  const parts = jwt.split('.');
  if (parts.length !== 3) return true;
  try {
    const p = JSON.parse(Buffer.from(parts[1], 'base64url').toString());
    return !p.exp || p.exp * 1000 < Date.now() + 60000; // 预留1分钟余量
  } catch (e) { return true; }
}

/** 从客户端进程内存提取 sec-ts 候选 + 最新 JWT */
function extractCredentials(force = false) {
  if (!force && Date.now() - lastExtract < EXTRACT_MIN_INTERVAL) return;
  lastExtract = Date.now();
  const ps1 = path.join(__dirname, 'sects.ps1');
  try {
    const out = execFileSync('powershell.exe', [
      '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ps1
    ], { encoding: 'utf8', timeout: 60000, windowsHide: true });
    const j = JSON.parse(out.trim());
    if (j.sects && j.sects.length) {
      sectPool = j.sects.map(s => s.val).filter(v => v && v.length >= 40);
      sectPool.reverse(); // 新提取的值优先放前面
      sectInUse = null;
      if (sectPool.length) everExtracted = true;
      console.log(`[farmapi] extracted ${sectPool.length} sec-ts candidates from client pid ${j.pid}`);
    }
    if (j.jwts && j.jwts.length) {
      const fresh = pickFreshestJwt(j.jwts);
      if (fresh && fresh !== jwt) {
        jwt = fresh;
        const p = JSON.parse(Buffer.from(jwt.split('.')[1], 'base64url').toString());
        console.log(`[farmapi] JWT updated: session=${p.session} exp=${new Date(p.exp * 1000).toISOString()}`);
      }
    }
  } catch (e) {
    // 客户端没运行时每 10s 都会失败，别刷屏：同一问题最多 5 分钟提示一次
    const now = Date.now();
    if (!extractCredentials._lastErrLog || now - extractCredentials._lastErrLog > 300000) {
      extractCredentials._lastErrLog = now;
      console.error('[farmapi] 凭据提取失败（Bambu Farm Manager 客户端没在运行？）:', String(e.message).split('\n')[0]);
    }
  }
}

// ---------- HTTP over mTLS ----------
/** TCP 探活：这个地址的 8888 连得上吗 */
function probeHost(host, timeoutMs = 1200) {
  return new Promise((resolve) => {
    const s = net.connect({ host, port: FARM_PORT });
    const done = (ok) => { try { s.destroy(); } catch (e) { } resolve(ok); };
    s.setTimeout(timeoutMs);
    s.once('connect', () => done(true));
    s.once('timeout', () => done(false));
    s.once('error', () => done(false));
  });
}

/** 挑出真正能连上的农场地址（只探测一次，结果缓存） */
async function ensureFarmHost() {
  if (farmHostProbed) return FARM_HOST;
  for (const h of FARM_HOST_CANDIDATES) {
    if (await probeHost(h)) {
      FARM_HOST = h;
      farmHostProbed = true;
      console.log(`[farmapi] 农场服务地址：${h}:${FARM_PORT}`);
      return FARM_HOST;
    }
  }
  farmHostProbed = true;
  console.error('[farmapi] ⚠ 候选地址都连不上 8888：' + FARM_HOST_CANDIDATES.join(' / ') +
    '（请确认 Bambu Farm Manager 客户端已启动）');
  return FARM_HOST;
}

function rawRequest(method, reqPath, body, extraHeaders = []) {
  return new Promise((resolve, reject) => {
    const sock = tls.connect({
      host: FARM_HOST, port: FARM_PORT,
      servername: SNI,
      ALPNProtocols: ['http/1.1'],
      cert: CHAIN, key: KEY,
      rejectUnauthorized: false,
    }, () => {
      const lines = [
        method + ' ' + reqPath + ' HTTP/1.1',
        'host: ' + SNI + ':' + FARM_PORT,
        'connection: close',
        'accept: application/json, text/plain, */*',
      ];
      if (jwt) lines.push('authorization: Bearer ' + jwt);
      if (sectInUse) lines.push('x-bbl-sec-ts: ' + sectInUse);
      lines.push(...BASE_HEADERS, ...extraHeaders);
      if (body) {
        // ★ 旁路加密中间件：不能用恰好 "application/json"（16字符会被拦截）
        lines.push('content-type: application/json;charset=UTF-8');
        lines.push('content-length: ' + Buffer.byteLength(body));
      }
      lines.push('', '');
      sock.write(lines.join('\r\n') + (body || ''));
    });
    let buf = Buffer.alloc(0);
    sock.on('data', (d) => { buf = Buffer.concat([buf, d]); });
    sock.on('end', () => {
      const s = buf.toString('latin1');
      const idx = s.indexOf('\r\n\r\n');
      if (idx === -1) return reject(new Error('malformed response'));
      const status = parseInt(s.match(/HTTP\/1\.1 (\d+)/)[1], 10);
      const headers = {};
      for (const line of s.slice(0, idx).split('\r\n').slice(1)) {
        const c = line.indexOf(':');
        if (c > 0) headers[line.slice(0, c).toLowerCase()] = line.slice(c + 2);
      }
      let bodyBuf = buf.slice(idx + 4);
      if ((headers['transfer-encoding'] || '').includes('chunked')) {
        const out = [];
        let p = 0;
        while (p < bodyBuf.length) {
          const nl = bodyBuf.indexOf('\r\n', p);
          if (nl === -1) break;
          const size = parseInt(bodyBuf.slice(p, nl).toString('latin1'), 16);
          if (!size) break;
          out.push(bodyBuf.slice(nl + 2, nl + 2 + size));
          p = nl + 2 + size + 2;
        }
        bodyBuf = Buffer.concat(out);
      }
      const enc = headers['content-encoding'];
      try {
        if (enc === 'gzip') bodyBuf = zlib.gunzipSync(bodyBuf);
        else if (enc === 'br') bodyBuf = zlib.brotliDecompressSync(bodyBuf);
        else if (enc === 'deflate') bodyBuf = zlib.inflateSync(bodyBuf);
      } catch (e) { }
      resolve({ status, headers, body: bodyBuf.toString('utf8') });
    });
    sock.on('error', reject);
    setTimeout(() => { sock.destroy(); reject(new Error('request timeout')); }, 20000);
  });
}

/**
 * 带 sec-ts 重试的 API 请求。
 * 400 + "invalid request"（code 4）= sec-ts 过期 → 换候选/重新提取
 * 401 = JWT 过期 → 重新提取
 */
async function api(method, reqPath, body, extraHeaders = []) {
  await ensureFarmHost();   // 请求前确认农场地址可达（探测结果会缓存）
  if (jwtExpired()) extractCredentials(true);
  if (!sectInUse && sectPool.length === 0) extractCredentials(true);

  const maxAttempts = sectPool.length + 2;
  for (let attempt = 0; attempt < maxAttempts; attempt++) {
    if (!sectInUse) {
      if (sectPool.length === 0) extractCredentials(true);
      if (sectPool.length === 0) {
        // 这两种情况必须分开说，否则排查时会被严重误导：
        //   从没提取到过   → 客户端确实没在运行
        //   提取到但全用光 → 多半是连不上农场服务（地址/端口不通）
        if (everExtracted) {
          throw new Error('sec-ts 候选已全部试完且都失败（连不上农场服务 ' + FARM_HOST + ':' + FARM_PORT + '？）');
        }
        throw new Error('no sec-ts candidates (client process not running?)');
      }
      sectInUse = sectPool.shift();
    }
    let r;
    try {
      r = await rawRequest(method, reqPath, body, extraHeaders);
    } catch (e) {
      sectInUse = null;
      continue;
    }
    if (r.status === 400 && r.body.includes('"code":4')) {
      sectInUse = null;
      continue;
    }
    if (r.status === 401) {
      jwt = null;
      extractCredentials(true);
      continue;
    }
    let json = null;
    try { json = JSON.parse(r.body); } catch (e) { }
    return { status: r.status, json, raw: r.body };
  }
  throw new Error('all sec-ts candidates exhausted');
}

// ---------- 业务方法 ----------
async function getServerInfo() {
  const r = await api('GET', '/captain');
  return r.json;
}

async function getDevices() {
  const r = await api('GET', '/devices2?use_lite=true');
  return r.json && r.json.devices ? r.json.devices : [];
}

async function getTasks(state = 'ongoing') {
  const r = await api('GET', `/task?task_state=${state}&limit=1000&offset=0&task_print_model=0&use_lite=true`);
  return r.json && r.json.hits ? r.json.hits : [];
}

async function getTaskDetail(taskId) {
  const r = await api('GET', `/task/${taskId}?use_lite=true`);
  return r.json;
}

/**
 * 批量下发任务到多个设备（对应客户端 bulkStartTasks）
 * POST /task/{taskId}/launchprinter
 * body: { device_pool2: [{dev_id, ams_mapping2:[{ams_id,slot_id}], nozzle_mapping}] }
 * 返回 {ret: [{dev_id, code, message}]} — code 0 = 成功
 */
async function launchTaskPrinters(taskId, devicePool, opts = {}) {
  const payload = JSON.stringify({
    device_pool2: devicePool.map(d => ({
      dev_id: String(d.dev_id),
      ams_mapping2: d.ams_mapping2 || [{ ams_id: 255, slot_id: 0 }],
      nozzle_mapping: d.nozzle_mapping || [],
    })),
    ...opts,
  });
  return api('POST', `/task/${taskId}/launchprinter`, payload);
}

/** 单设备下发（对应客户端 startTask）POST /device/{devId}/launchtask */
async function launchOnDevice(devId, taskId, opts = {}) {
  const payload = JSON.stringify({
    task_id: String(taskId),
    ams_mapping2: opts.ams_mapping2 || [{ ams_id: 255, slot_id: 0 }],
    nozzle_mapping: opts.nozzle_mapping || [],
  });
  return api('POST', `/device/${devId}/launchtask`, payload);
}

/**
 * 设备任务操作: pause / resume / stop
 * ★ 必须用 POST /device/{devId}/opt（不是 /taskopt —— 那个是空操作，返回200但无效果）
 * body: {dev_id, opt}，实测 pause→PAUSE、resume→RUNNING
 */
async function taskOpt(devId, action) {
  const payload = JSON.stringify({ dev_id: String(devId), opt: action });
  return api('POST', `/device/${devId}/opt`, payload);
}

module.exports = {
  api, rawRequest,
  getServerInfo, getDevices, getTasks, getTaskDetail,
  launchTaskPrinters, taskOpt, launchOnDevice,
  extractCredentials,
  get state() { return { sectPoolSize: sectPool.length, sectInUse: sectInUse ? sectInUse.slice(0, 8) + '...' : null, jwtExp: jwt ? JSON.parse(Buffer.from(jwt.split('.')[1], 'base64url').toString()).exp : null }; },
};

if (require.main === module) {
  (async () => {
    const info = await getServerInfo();
    console.log('server:', info.name, info.version, '| license:', info.activate_state, info.left_time + 'd');
    const devs = await getDevices();
    console.log('devices:', devs.length);
    const tasks = await getTasks();
    console.log('tasks:', tasks.length);
    process.exit(0);
  })().catch(e => { console.error('FAIL:', e.message); process.exit(1); });
}
