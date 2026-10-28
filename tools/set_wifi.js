/**
 * set_wifi.js — 给工牌下发 WiFi 凭据（换 WiFi 不用重新烧录固件）
 *
 * 用法：
 *   node tools/set_wifi.js                  自动：取本机当前连的 WiFi
 *   node tools/set_wifi.js <网络名>          从本机已保存的网络里取出密码再下发
 *   node tools/set_wifi.js <网络名> <密码>   直接指定
 *
 * 原理：经桥接的 POST /send 把 {"t":"wificfg","ssid":..,"pass":..} 发给工牌，
 *      工牌存进 NVS 后立刻用新凭据重连。无线已断时自动改走 USB 串口通道。
 *
 * ⚠ ESP32-C3 只支持 2.4GHz，5G 网络连不上（名字里带 5G 的会先警告）。
 */
const http = require('http');

const BRIDGE_HOST = '127.0.0.1';
const BRIDGE_PORT = 8899;

function postJSON(path, obj) {
  return new Promise((resolve, reject) => {
    const data = JSON.stringify(obj);
    const req = http.request(
      { host: BRIDGE_HOST, port: BRIDGE_PORT, path, method: 'POST',
        headers: { 'content-type': 'application/json', 'content-length': Buffer.byteLength(data) } },
      (res) => {
        let d = '';
        res.on('data', (c) => (d += c));
        res.on('end', () => resolve({ code: res.statusCode, body: d }));
      }
    );
    req.on('error', reject);
    req.write(data);
    req.end();
  });
}

const isWin = process.platform === 'win32';

function sh(cmd) {
  // netsh 只认 GBK/本地代码页，先切到 65001 保证中文网络名不乱码
  return require('child_process').execSync(
    isWin ? `cmd /c "chcp 65001 >nul & ${cmd}"` : cmd,
    { encoding: 'utf8', windowsHide: true }
  );
}

/** 本机当前连接的 WiFi 名（没连就返回 null） */
function currentSsid() {
  if (!isWin) return null;
  try {
    const out = sh('netsh wlan show interfaces');
    if (!/状态\s*:\s*已连接|State\s*:\s*connected/i.test(out)) return null;
    // 行首是 SSID 才是网络名（BSSID 是路由器的 MAC，别匹配错）
    const m = out.match(/^\s*SSID\s*:\s*(.+?)\s*$/m);
    return m ? m[1].trim() : null;
  } catch (e) { return null; }
}

/** 本机保存过的所有 WiFi 名 */
function savedProfiles() {
  if (!isWin) return [];
  try {
    const out = sh('netsh wlan show profile');
    const list = [];
    const re = /(?:所有用户配置文件|All User Profile)\s*:\s*(.+?)\s*$/gm;
    let m;
    while ((m = re.exec(out)) !== null) if (m[1].trim()) list.push(m[1].trim());
    return list;
  } catch (e) { return []; }
}

/** 取某个已保存网络的明文密码 */
function profileKey(name) {
  if (!isWin) return '';
  try {
    const out = sh(`netsh wlan show profile name="${name}" key=clear`);
    const m = out.match(/(?:Key Content|关键内容)\s*:\s*(.+?)\s*$/m);
    return m ? m[1].trim() : '';
  } catch (e) { return ''; }
}

(async () => {
  let ssid = process.argv[2];
  let pass = process.argv[3];

  // 没给网络名 → 先取当前连接的
  if (!ssid) {
    const cur = currentSsid();
    if (cur) {
      ssid = cur;
      console.log(`[setwifi] 本机当前连接：${ssid}`);
    } else {
      const list = savedProfiles();
      console.error('[setwifi] 本机没连 WiFi（可能在用有线网）。');
      if (list.length) {
        console.error('  本机保存过这些网络，挑一个 2.4G 的：');
        list.forEach((n) => console.error(`    node tools/set_wifi.js ${n}`));
      }
      console.error('  或直接指定：node tools/set_wifi.js <网络名> <密码>');
      process.exit(1);
    }
  }

  // 没给密码 → 从本机已保存的网络里取
  if (!pass) {
    pass = profileKey(ssid);
    if (pass) console.log(`[setwifi] 已从本机取到「${ssid}」的密码`);
    else console.warn(`[setwifi] 本机没保存「${ssid}」的密码，按无密码下发`);
  }

  // ★ ESP32-C3 只支持 2.4GHz
  if (/5g/i.test(ssid)) {
    console.warn(`[setwifi] ⚠ 「${ssid}」看着像 5G 网络 —— ESP32-C3 只支持 2.4GHz，可能连不上！`);
    console.warn('          若连不上，请换 2.4G 的网络名重新下发。');
  }

  try {
    const r = await postJSON('/send', { t: 'wificfg', ssid, pass });
    if (r.code !== 200) {
      console.error(`[setwifi] 下发失败（HTTP ${r.code}）：${r.body}`);
      console.error('  先确认桥接在跑：http://127.0.0.1:8899/state');
      console.error('  工牌要在线（无线或 USB 串口任一连通）');
      process.exit(1);
    }
    console.log(`[setwifi] ✅ 已把 WiFi「${ssid}」下发给工牌，工牌会自动重连`);
    console.log('[setwifi]    连上后工牌重新上报状态，托盘图标恢复绿色。');
  } catch (e) {
    console.error('[setwifi] 连不上桥接：' + e.message);
    console.error('  先启动桥接（双击 启动桥接.bat 或 npm start）');
    process.exit(1);
  }
})();
