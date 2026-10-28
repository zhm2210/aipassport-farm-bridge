// 农场状态总览 — 服务器信息 + 设备状态 + 任务队列
const { getJson } = require('./farmapi.js');

function fmtTime(t) { return t ? new Date(t).toLocaleString('zh-CN', { hour12: false }) : '-'; }

(async () => {
  console.log('========== 拓竹农场实时状态 ==========\n');

  // 1) 服务器信息
  const cap = await getJson('/captain');
  if (cap.status === 200 && cap.json) {
    const c = cap.json;
    console.log(`【服务器】${c.name}  版本 ${c.version}`);
    console.log(`  许可证: ${c.activate_state}  剩余 ${c.left_time} 天 (至 ${(c.expire_time || '').slice(0, 10)})  环境: ${c.env}`);
  } else {
    console.log('【服务器】状态获取失败:', cap.status);
  }

  // 2) 设备状态
  const dev = await getJson('/devices2?use_lite=true');
  if (dev.status === 200 && dev.json && dev.json.devices) {
    const ds = dev.json.devices;
    const running = ds.filter(d => (d.report_status || {}).gcode_state === 'RUNNING');
    const idle = ds.filter(d => (d.report_status || {}).gcode_state !== 'RUNNING');
    console.log(`\n【设备】共 ${ds.length} 台 | 打印中 ${running.length} | 空闲 ${idle.length}\n`);
    console.log('状态  设备         dev_id                 型号  进度    层数        WiFi     任务');
    console.log('-'.repeat(105));
    const sorted = [...running, ...idle].sort((a, b) => a.name.localeCompare(b.name));
    for (const d of sorted) {
      const t = d.report_status || {};
      const st = t.gcode_state === 'RUNNING' ? '打印' : (t.gcode_state === 'IDLE' ? '空闲' : (t.gcode_state || '?'));
      const pct = t.mc_percent != null ? t.mc_percent + '%' : '-';
      const layer = `${t.layer_num || 0}/${t.total_layer_num || 0}`;
      const name = (t.subtask_name || '').slice(0, 24);
      console.log(`${st}\t${d.name}\t${d.dev_id}\t${d.dev_model || '?'}\t${pct}\t${layer}\t${t.wifi_signal || '-'}\t${name}`);
    }
    // 温度详情（打印中的）
    console.log('\n【打印中设备温度】');
    for (const d of running) {
      const t = d.report_status || {};
      console.log(`  ${d.name}: 喷嘴 ${(t.nozzle_temper || 0).toFixed(1)}°C/${t.nozzle_target_temper || 0}°C  热床 ${(t.bed_temper || 0).toFixed(1)}°C/${t.bed_target_temper || 0}°C  剩余 ${t.mc_remaining_time || 0}min`);
    }
  } else {
    console.log('\n【设备】获取失败:', dev.status);
  }

  // 3) 任务队列
  const tk = await getJson('/task?task_state=ongoing&limit=1000&offset=0&task_print_model=0&use_lite=true');
  if (tk.status === 200 && tk.json && tk.json.hits) {
    const tasks = tk.json.hits;
    console.log(`\n【任务队列】共 ${tk.json.total} 个任务\n`);
    for (const t of tasks) {
      const wd = (t.working_devices || []).length;
      const st = (t.sub_tasks || []).length;
      console.log(`  #${t.id} ${t.task_name}`);
      console.log(`     状态: ${t.state} | 文件: ${(t.f3mf_info || {}).f3mf_name || '?'} | 工作中设备: ${wd} | 子任务: ${st} | 剩余次数: ${t.remaining_count} | 创建: ${fmtTime(t.create_time)}`);
    }
  } else {
    console.log('\n【任务队列】获取失败:', tk.status);
  }
  process.exit(0);
})().catch(e => { console.error('ERROR:', e); process.exit(1); });
