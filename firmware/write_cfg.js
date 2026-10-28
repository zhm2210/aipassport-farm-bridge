const fs = require('fs');
const cfg = {
  bat: 'C:\\Users\\zzz\\WorkBuddy\\农场\\start-bridge.bat',
  dir: 'C:\\Users\\zzz\\WorkBuddy\\农场',
  ico: 'C:\\Users\\zzz\\WorkBuddy\\农场\\app.ico',
  desc: '一键启动拓竹农场桥接：自动读取农场状态并用串口转发给 AI Passport 工牌，关闭窗口即停止'
};
fs.writeFileSync('C:/Users/zzz/AppData/Local/Temp/lnk_cfg.json', JSON.stringify(cfg, null, 1));
console.log(JSON.stringify(cfg, null, 1));
