/**
 * set_wifi.js — 给工牌下发 WiFi 凭据（换 WiFi 不用重新烧录固件）
 *
 * 用法：
 *   node tools/set_wifi.js <网络名> <密码>    手动指定
 *   node tools/set_wifi.js                    交互式输入（密码不回显）
 *
 * ★ 本工具不会去读取电脑里保存的 WiFi 密码 —— 一律手动输入。
 *   （不碰 netsh key=clear，不扫描本机已保存的网络）
 *
 * 原理：经桥接的 POST /send 把 {"t":"wificfg","ssid":..,"pass":..} 发给工牌，
 *      工牌存进 NVS 后立刻用新凭据重连。无线已断时自动改走 USB 串口通道。
 *
 * ⚠ ESP32-C3 只支持 2.4GHz，5G 网络连不上。
 */
const http = require('http');
const readline = require('readline');

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

/** 交互式提问；hidden=true 时不回显输入的字符 */
function ask(question, hidden) {
  return new Promise((resolve) => {
    const rl = readline.createInterface({
      input: process.stdin, output: process.stdout, terminal: true,
    });
    if (hidden) rl._writeToOutput = function () {};   // 吞掉回显
    process.stdout.write(question);                   // 提示语照常显示
    rl.question('', (ans) => {
      if (hidden) process.stdout.write('\n');
      rl.close();
      resolve((ans || '').trim());
    });
  });
}

(async () => {
  let ssid = process.argv[2];
  let pass = process.argv[3];

  // 缺哪个问哪个 —— 全程手动，绝不自动读取本机保存的密码
  if (!ssid || !pass) {
    if (!process.stdin.isTTY) {
      // 非交互环境（脚本/CI）：问不了，直接提示用法
      console.error('[setwifi] 需要手动提供 WiFi 名称和密码。');
      console.error('  用法：node tools/set_wifi.js <网络名> <密码>');
      console.error('  （本工具不读取电脑里保存的 WiFi 密码）');
      process.exit(1);
    }
    if (!ssid) {
      ssid = await ask('WiFi 名称：', false);
      if (!ssid) { console.error('[setwifi] 未输入名称，已取消'); process.exit(1); }
    }
    if (!pass) {
      pass = await ask('WiFi 密码（不回显；开放网络直接回车）：', true);
    }
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
