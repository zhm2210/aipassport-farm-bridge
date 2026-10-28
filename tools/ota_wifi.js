/**
 * ota_wifi.js —— 通过桥接把固件无线刷进工牌
 *
 * 为什么不用标准 espota：电脑防火墙默认拦入站，标准 OTA 要"板子回连电脑"，
 * 回连会被防火墙挡掉。这里顺着已有的「电脑 → 桥接 → 工牌」链路推固件，
 * 方向是电脑主动出站，防火墙放行。
 *
 * 用法：node tools/ota_wifi.js [固件.bin]
 *   默认刷 tools/build/aipassport_farm.ino.bin
 *   桥接服务需要已经在跑（双击 启动桥接.bat）
 */
const fs = require('fs');
const path = require('path');
const http = require('http');

const ROOT = path.join(__dirname, '..');
const BIN = process.argv[2] || path.join(ROOT, 'tools', 'build', 'aipassport_farm.ino.bin');
const BRIDGE = '127.0.0.1:8899';
const LOG = path.join(ROOT, '.tmp', 'bridge.log');

const sleep = (ms) => new Promise(r => setTimeout(r, ms));

function get(urlPath) {
  return new Promise((resolve) => {
    const req = http.request({ host: '127.0.0.1', port: 8899, path: urlPath, method: 'GET', timeout: 3000 }, (res) => {
      let b = '';
      res.on('data', d => b += d);
      res.on('end', () => { try { resolve(JSON.parse(b)); } catch (e) { resolve(null); } });
    });
    req.on('error', () => resolve(null));
    req.on('timeout', () => { req.destroy(); resolve(null); });
    req.end();
  });
}

function postJson(obj) {
  return new Promise((resolve) => {
    const body = Buffer.from(JSON.stringify(obj), 'utf8');
    const req = http.request({
      host: '127.0.0.1', port: 8899, path: '/ota', method: 'POST',
      headers: { 'content-type': 'application/json', 'content-length': body.length },
      timeout: 600000,
    }, (res) => {
      let b = '';
      res.on('data', d => b += d);
      res.on('end', () => { try { resolve(JSON.parse(b)); } catch (e) { resolve({ error: b.slice(0, 300) }); } });
    });
    req.on('error', (e) => resolve({ error: e.message }));
    req.on('timeout', () => { req.destroy(); resolve({ error: '超时' }); });
    req.write(body);
    req.end();
  });
}

(async () => {
  if (!fs.existsSync(BIN)) {
    console.error(`[错误] 找不到固件：${BIN}\n       先编译一次（一键烧录.bat 里选 1 或 2 都会编译）`);
    process.exit(1);
  }

  const st = await get('/ota');
  if (!st) {
    console.error('[错误] 桥接服务没在跑（http://127.0.0.1:8899 连不上）');
    console.error('       请先双击「启动桥接.bat」，等右下角出现图标后再刷。');
    process.exit(1);
  }
  if (st.busy) { console.error('[错误] 上一次升级还没结束'); process.exit(1); }
  if (!st.wireless.length && !st.usb) {
    console.error('[错误] 没有在线的工牌：');
    console.error('       - 无线：桥接日志里要有 [tcp] 已连上工牌 的行');
    console.error('       - 或者先用 USB 数据线连上（这次刷完以后就不用线了）');
    process.exit(1);
  }

  const via = st.wireless.length ? `无线 ${st.wireless.join(',')}` : `USB ${st.usb}`;
  const size = fs.statSync(BIN).size;
  console.log(`=== 无线刷机 ===`);
  console.log(`固件：${path.relative(ROOT, BIN)}  ${size} 字节`);
  console.log(`通道：${via}`);
  console.log(`工牌屏幕会显示「正在无线升级固件」进度条，请勿断电。\n`);

  // 边刷边把桥接日志里的进度打出来
  let off = fs.existsSync(LOG) ? fs.statSync(LOG).size : 0;
  const timer = setInterval(() => {
    try {
      const cur = fs.statSync(LOG).size;
      if (cur > off) {
        const chunk = fs.readFileSync(LOG, 'utf8').slice(off);
        off = cur;
        chunk.split('\n').filter(l => l.includes('[ota]')).forEach(l => console.log('   ' + l.trim()));
      }
    } catch (e) { }
  }, 400);

  const t0 = Date.now();
  const r = await postJson({ path: path.relative(ROOT, BIN).replace(/\\/g, '/') });
  clearInterval(timer);

  if (r && r.ok) {
    console.log(`\n=== 刷入成功（${Math.round((Date.now() - t0) / 1000)} 秒）===`);
    console.log('工牌会自动重启，几秒后重新连回桥接。');
  } else {
    console.error(`\n[错误] 刷入失败：${(r && r.error) || JSON.stringify(r)}`);
    console.error('排查：');
    console.error('  1. 工牌屏幕是否亮着（息屏不影响，但休眠断网会失败）');
    console.error('  2. 桥接日志 .tmp/bridge.log 里看 [ota] 相关行');
    console.error('  3. 实在不行就用 USB 数据线刷一次（选 1）');
    process.exit(1);
  }
})();
