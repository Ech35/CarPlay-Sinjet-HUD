const hud = {state:null, posting:false, polling:false, timer:0, revision:0, statusFailed:false};
const hudGateMessages = {
    waiting_session:'等待 CarPlay 会话。', waiting_producer:'等待 CarPlay 服务就绪。',
    waiting_subscription:'等待导航订阅完成。', waiting_auth:'等待 CarPlay 认证完成。',
    waiting_callback:'等待 CarPlay 发送通道。', waiting_carplay_marker:'等待 CarPlay 连接确认。',
    session_changed:'CarPlay 会话正在切换，稍后重试。'
};

function hudGateMessage(s) {
    if (!s?.gate_reason) return '等待 CarPlay 连接。';
    return hudGateMessages[s.gate_reason] || 'HUD 会话核验未通过（'+s.gate_reason+'），请导出日志。';
}
const hudMessages = {
    phone_bluetooth_busy:'手机配对蓝牙仍在使用，稍后会重试。',
    le_unsupported:'控制器未报告低功耗蓝牙支持，请导出日志。',
    le_disabled:'原厂低功耗蓝牙尚未就绪，请导出日志。',
    controller_off:'原厂蓝牙尚未开启，请等待 CarPlay 连接。',
    scan_start:'正在搜索附近的 HUD…', scan_complete:'搜索完成，请选择自己的 HUD。',
    connect_start:'正在连接 HUD…', discover_start:'正在确认 HUD 的写入接口…',
    connected:'HUD 已连接。', worker_ready:'HUD 服务已就绪。',
    settings_restored:'已恢复保存的 HUD 设置。', settings_wait:'等待保存的 HUD 配置；首次使用请搜索并连接。',
    test_start:'正在发送测试，约 3 秒后清屏。', test_written:'测试已写入，请查看 HUD。',
    test_complete:'测试与清屏已发送，请核对 HUD 的实际显示。',
    test_failed:'测试未完成，请导出日志。', disconnected:'HUD 已断开。',
    relay_enabled:'自动转发已开启。', relay_disabled:'自动转发已关闭并断开 HUD。',
    relay_ready:'实时导航已接入 HUD。', relay_wait_carplay:'等待 CarPlay 连接。',
    relay_connect_failed:'连接未完成，稍后自动重试。', relay_send_retry:'发送中断，稍后自动重连。',
    relay_freshness_expired:'两分钟没有有效导航更新，等待新数据恢复显示。',
    relay_input_gap:'数据出现缺口，已清除旧显示并等待更新。',
    relay_parse_failed:'有导航消息未能解析，请导出日志。',
    connection_guard:'CarPlay 或手机配对状态已变化，等待恢复连接。'
};

async function hudRequest(action, body) {
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 4500);
    const options = body === undefined ? {} : {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(body)};
    try {
        let reply;
        try { reply = await fetch('/cgi-bin/u6hud_ble.cgi?action='+action, {...options, cache:'no-store', signal:controller.signal}); }
        catch (error) { throw new Error(error.name === 'AbortError' ? 'HUD 接口请求超时，请重试。' : 'HUD 接口无法访问，请检查与盒子的连接。'); }
        if (!reply.ok) throw new Error('HUD 接口返回 HTTP '+reply.status+'，请导出日志。');
        let result;
        try { result = await reply.json(); }
        catch { throw new Error('HUD 接口返回格式异常，请导出日志。'); }
        if (!result || typeof result !== 'object' || typeof result.ok !== 'boolean') throw new Error('HUD 接口返回数据不完整，请导出日志。');
        if (!result.ok) {
            const errors = {carplay_not_ready:'请先连接 CarPlay。',target_not_selected:'请先选择并连接自己的 HUD。',
                operation_busy:'当前操作正在进行，请稍候。',request_busy:'请求正在提交，请稍候。',
                relay_active:'请先关闭自动转发，再使用手动测试。',test_icons_disabled:'请先开启完整转向图标。',
                settings_write_failed:'设置保存失败，请导出日志。',ram_state_unavailable:'内存服务尚未就绪，请导出日志。'};
            throw new Error(errors[result.error] || '操作未完成，请导出日志。');
        }
        if (action === 'status' && ['carplay_ready','busy','connected','relay_enabled'].some(key=>typeof result[key] !== 'boolean')) {
            throw new Error('HUD 接口状态字段不完整，请导出日志。');
        }
        return result;
    } finally { clearTimeout(timeout); }
}

function hudControls() {
    const s = hud.state;
    if (!s) {
        for (const id of ['hud-scan','hud-device','hud-connect','hud-disconnect','hud-relay','hud-icons','hud-test-kind','hud-test-icon','hud-test']) {
            const control=u6By(id);if(control)control.disabled=true;
        }
        return;
    }
    const busy = hud.posting || s.busy;
    u6By('hud-scan').disabled = busy || !s.carplay_ready || s.connected || s.relay_enabled;
    u6By('hud-device').disabled = busy || s.connected || s.relay_enabled;
    u6By('hud-connect').disabled = busy || !s.carplay_ready || s.connected || !u6By('hud-device').value || s.relay_enabled;
    u6By('hud-disconnect').disabled = hud.posting || !s || (!s.connected && !s.busy && !s.relay_enabled);
    u6By('hud-relay').disabled = hud.posting || !s || (!s.relay_enabled && (busy || !s.selected?.type));
    u6By('hud-icons').disabled = busy || !s.icon_target_ready;
    u6By('hud-test-kind').disabled = busy || !s.connected || s.relay_enabled;
    u6By('hud-test-icon').disabled = busy || !s.expanded_icons || !s.connected || s.relay_enabled;
    const icons = u6By('hud-test-kind').value === 'icons';
    u6By('hud-test-icon-field').hidden = !icons;
    u6By('hud-test').disabled = busy || !s.carplay_ready || !s.connected || s.relay_enabled || (icons && !s.expanded_icons);
    u6By('hud-test-preview').textContent = icons
        ? (s?.expanded_icons ? '显示选定转向图标和 100 米。' : '请先确认 HUD 已补齐图标，并开启完整转向图标。')
        : u6By('hud-test-kind').value === 'lanes' ? '显示六条车道及推荐方向。' : '显示左转和 100 米距离。';
}

function hudDraw(s) {
    hud.state=s;
    const select=u6By('hud-device'), previous=select.value;
    const peers=[];
    for (const p of [...(s.devices||[]),s.selected]) {
        if (!p || ![1,2].includes(p.type) || p.connectable===false) continue;
        if (!peers.some(v=>v.address===p.address && v.type===p.type)) peers.push(p);
    }
    const placeholder=document.createElement('option');
    placeholder.value='';placeholder.textContent=peers.length?'请选择 HUD':'搜索后选择 HUD';
    select.replaceChildren(placeholder);
    for (const p of peers) {
        const option=document.createElement('option');option.value=JSON.stringify({address:p.address,type:p.type});
        option.textContent=(p.name||'未命名设备')+' · '+p.address;select.append(option);
    }
    const saved=s.selected?.type?JSON.stringify({address:s.selected.address,type:s.selected.type}):'';
    if ([...select.options].some(p=>p.value===previous)) select.value=previous;
    if (!select.value && saved) select.value=saved;
    u6By('hud-relay').checked=Boolean(s.relay_enabled);
    u6By('hud-icons').checked=Boolean(s.expanded_icons);
    u6By('hud-state').textContent=s.connected?'HUD 已连接':s.busy?'正在处理':s.phase===6?'连接未完成':'未连接';
    u6By('hud-selected').textContent=s.selected?.type?(s.selected.name||s.selected.address):'选择设备后连接';
    u6By('hud-result').textContent=!s.carplay_ready ? hudGateMessage(s) : (hudMessages[s.stage] || 'CarPlay 已就绪，可以连接 HUD。');
    u6By('hud-result').dataset.error=String(s.phase===6);
    u6By('hud-relay-result').textContent=!s.relay_enabled?'自动转发已关闭。':!s.running?'HUD 服务已退出，请导出日志。':
        !s.carplay_ready?hudGateMessage(s):s.worker_relay_enabled===false?'正在恢复保存的 HUD 设置…':!s.connected?'正在等待或连接已保存的 HUD。':
        s.relay_freshness_expired?'两分钟没有有效导航更新，收到新数据后恢复。':
        s.relay_clear_sent?'已发送清屏，等待新的导航。':s.relay_core_phase!==5?'HUD 已连接，等待导航。':
        '正在转发'+(s.relay_source?' · '+s.relay_source:'')+'，已发送 '+(s.relay_frames||0)+' 条显示指令。';
    u6By('hud-test-result').textContent=s.test_count?'本次开机已发送 '+s.test_count+' 次测试。'+
        (s.clear_sent?'最近一次清屏已发送。':'请核对清屏情况。'):'';
    hudControls();
}

async function hudPoll() {
    if (hud.polling || hud.posting || document.hidden || !u6By('hud-device')) return;
    hud.polling=true;
    const revision=hud.revision;
    try {
        const state=await hudRequest('status');
        if (revision===hud.revision && !hud.posting) {
            try { hudDraw(state); }
            catch (error) { throw new Error('HUD 页面更新失败：'+String(error.message).slice(0,100)); }
            if (hud.statusFailed) u6By('hud-message').textContent='';
            hud.statusFailed=false;
        }
    }
    catch (error) {
        if (revision===hud.revision && !hud.posting) {
            hud.statusFailed=true;
            hud.state=null;hudControls();
            u6By('hud-message').textContent=error.message || 'HUD 状态读取失败，请重试。';
        }
    }
    finally {
        hud.polling=false;clearTimeout(hud.timer);
        hud.timer=setTimeout(hudPoll,document.querySelector('[data-u6-pane="nav"]').hidden?4000:1000);
    }
}

async function hudAction(action, body={}) {
    if (hud.posting) return;
    if (action==='connect') {
        try { body=JSON.parse(u6By('hud-device').value); } catch { return; }
    }
    if (action==='test') {
        const kind=u6By('hud-test-kind').value;
        if (kind==='lanes') body={lanes:true};
        if (kind==='icons') body={icon:Number(u6By('hud-test-icon').value)};
    }
    hud.posting=true;hud.revision++;hud.statusFailed=false;hudControls();
    u6By('hud-message').textContent='正在提交操作…';
    try {
        await hudRequest(action,body);
        u6By('hud-message').textContent='操作已提交。';
    } catch (error) { u6By('hud-message').textContent=error.message; }
    finally { hud.posting=false;await hudPoll();hudControls(); }
}

function hudBoot() {
    for (const action of ['scan','connect','test','disconnect']) u6By('hud-'+action).addEventListener('click',()=>hudAction(action));
    u6By('hud-relay').addEventListener('change',()=>hudAction('relay',{enabled:u6By('hud-relay').checked}));
    u6By('hud-icons').addEventListener('change',()=>hudAction('icons',{enabled:u6By('hud-icons').checked}));
    u6By('hud-device').addEventListener('change',hudControls);
    u6By('hud-test-kind').addEventListener('change',hudControls);
    hudPoll();
}

document.addEventListener('visibilitychange',()=>{if(!document.hidden)hudPoll();});
