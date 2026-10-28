// Capture longer boot log + protocol test
const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');

const port = new SerialPort({ path: 'COM16', baudRate: 115200 });
const parser = port.pipe(new ReadlineParser({ delimiter: '\n' }));
const t0 = Date.now();
let lines = 0;

port.on('open', () => {
  console.log('[test] open');
  setTimeout(() => port.write(JSON.stringify({ t: 'hello' }) + '\n'), 3000);
});
parser.on('data', line => {
  lines++;
  const s = line.trim();
  if (!s) return;
  // 只打关键行，避免刷屏
  if (/panic|Guru|abort|assert|Backtrace|rst:|Brownout|watchdog|WDT|hello|get_state|state|error|Error/i.test(s) || lines < 40) {
    console.log(`[${Date.now() - t0}ms] ${s.slice(0, 200)}`);
  }
});
port.on('error', e => console.log('[err]', e.message));
setTimeout(() => {
  console.log(`[test] done, total lines=${lines}`);
  port.close(() => process.exit(0));
}, 15000);
