/**
 * launch.js — 农场桥接一键启动器
 *  - 单实例保护：8899 端口已被占用 = 已在运行 → 直接打开浏览器后退出
 *  - 启动 bridge.js 子进程并等待就绪，然后用默认浏览器打开调试页
 *  - 本进程作为父进程常驻（关闭本窗口 = 停止桥接）
 */
const { spawn, exec } = require('child_process');
const http = require('http');
const path = require('path');

const HTTP_PORT = 8899;
const URL = `http://127.0.0.1:${HTTP_PORT}/state`;

function ping(cb) {
  const req = http.get(URL, res => { res.resume(); cb(true); });
  req.on('error', () => cb(false));
  req.setTimeout(1000, () => { req.destroy(); cb(false); });
}

function openBrowser() {
  if (process.env.NO_BROWSER || process.argv.includes('--no-browser')) {
    console.log(`[launch] 静默模式，跳过打开浏览器（状态页 http://127.0.0.1:${HTTP_PORT}/state）`);
    return;
  }
  exec(`cmd /c start "" "${URL}"`, () => {});
  console.log(`[launch] opened ${URL} in default browser`);
}

(async () => {
  // 1. 单实例检查
  const running = await new Promise(ping);
  if (running) {
    console.log('[launch] bridge already running, opening browser...');
    openBrowser();
    setTimeout(() => process.exit(0), 800);
    return;
  }

  // 2. 启动桥接
  console.log('[launch] starting farm bridge...');
  // 透传额外参数（如 --nousb）
  const extra = process.argv.slice(2).filter(a => a.startsWith('--') && a !== '--no-browser');
  const child = spawn(process.execPath, [path.join(__dirname, 'bridge.js'), ...extra], {
    stdio: 'inherit',
    cwd: path.join(__dirname, '..'),
  });
  child.on('exit', code => {
    console.log(`[launch] bridge exited (${code})`);
    process.exit(code || 0);
  });

  // 3. 等待就绪（最多 40 秒）
  let ok = false;
  for (let i = 0; i < 40; i++) {
    await new Promise(r => setTimeout(r, 1000));
    ok = await new Promise(ping);
    if (ok) break;
  }
  if (ok) {
    console.log('[launch] bridge ready');
    openBrowser();
  } else {
    console.log('[launch] WARNING: bridge did not become ready in 40s (check the log above)');
  }
  // 4. 常驻，等 bridge 退出
})();
