'use strict';
const u6 = {timer: 0, busy: false, polling: false, epoch: 0};
const u6By = id => document.getElementById(id);
const u6Api = '/cgi-bin/u6diag.cgi?action=';
const u6Phases = ['等待 CarPlay 路引会话', '已声明路线引导能力', '识别通过', '已提交导航订阅',
    '已收到路引', '手机拒绝识别', '按原厂方式识别', '订阅提交失败'];

async function u6Request(action, method = 'GET') {
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 4500);
    const payload = method === 'POST' ? {headers: {'Content-Type': 'application/json'}, body: '{}'} : {};
    try {
        const request = await fetch(u6Api + action, {method, cache: 'no-store', signal: controller.signal, ...payload});
        if (!request.ok) throw new Error('HTTP ' + request.status);
        return await (action === 'preview' ? request.text() : request.json());
    } finally { clearTimeout(timeout); }
}

function u6Draw(d) {
    const chip = u6By('u6-connection');
    chip.textContent = d.carplay ? 'CarPlay 已连接' : '等待 CarPlay';
    chip.dataset.state = 'online';
    let phase = u6Phases[d.phase] || '读取会话状态';
    if (d.phase === 2) {
        if (!d.auth_complete) phase = '等待原厂认证完成';
        else if (!d.callback_ready) phase = '等待 iAP2 发送通道';
    }
    if (d.phase === 3 && d.carplay_returns && d.carplay_return === 0) phase = '订阅已发送，等待导航';
    u6By('u6-phase').textContent = d.ok ? phase : '诊断未就绪';
    for (const kind of ['route', 'turn', 'lane']) u6By('u6-' + kind).textContent = d[kind] || 0;
    u6By('u6-handshake').textContent = '本次开机累计 · 声明 ' + (d.identify || 0) +
        ' · 通过 ' + (d.accepted || 0) + ' · 拒绝 ' + (d.rejected || 0) +
        ' · 订阅 ' + (d.subscriptions || 0);
    let transport = '认证完成后自动订阅导航。';
    if (d.control_tx) transport = '订阅已交给 iAP2 发送队列。';
    if (d.phase === 7) transport = '订阅提交失败，详情见日志。';
    if (d.carplay_skipped) transport = '发送时没有可用的 CarPlay 会话。';
    if (d.carplay_calls) transport = d.carplay_returns
        ? 'CarPlay 发送接口返回 ' + d.carplay_return + '。'
        : '已调用 CarPlay 发送接口，等待返回。';
    u6By('u6-transport').textContent = d.ok ? transport : '';
    u6By('u6-log-switch').checked = Boolean(d.logging);
    u6By('u6-log-switch').disabled = !d.ok || u6.busy;
    const clear = u6By('u6-clear');
    clear.disabled = u6.busy;
    clear.textContent = d.ok ? '清空日志' : '重试初始化';
    clear.dataset.action = d.ok ? 'clear' : 'init';
    const names = {
        ram_directory:'创建内存目录', ram_filesystem:'检查内存文件系统', ram_mount:'挂载内存目录',
        ram_backing_unconfirmed:'确认内存存储', state_open:'打开日志文件', state_lock:'锁定日志文件',
        state_seek:'检查日志长度', state_size:'核对日志大小', state_header:'读取日志文件头',
        state_allocate:'分配日志内存', state_map:'读取日志内存',
    };
    const reason = d.errno ? '。' + (names[d.error] || '初始化') + '失败，错误 ' + d.errno : '';
    u6By('u6-log-status').textContent = d.ok
        ? (d.logging ? '正在记录。' : '记录已暂停。') + '当前 ' + d.recorded + '/' + d.capacity +
          ' 条，已覆盖 ' + d.overwritten + ' 条。日志只存内存，盒子断电后清除。'
        : '日志未就绪' + reason + '。可重试初始化或导出诊断。';
}

async function u6Poll() {
    if (!u6By('u6-connection') || document.hidden || u6.busy || u6.polling) return;
    u6.polling = true;
    const epoch = u6.epoch;
    try {
        const state = await u6Request('status');
        if (epoch === u6.epoch) u6Draw(state);
    } catch {
        if (epoch !== u6.epoch || !u6By('u6-connection')) return;
        const chip = u6By('u6-connection');
        chip.textContent = '状态暂不可用';
        chip.dataset.state = 'error';
    } finally { u6.polling = false; }
}

async function u6Preview() {
    try { u6By('u6-log-text').textContent = await u6Request('preview'); }
    catch { u6By('u6-log-text').textContent = '读取失败，请确认手机仍连接盒子。'; }
}

async function u6Action(action) {
    if (u6.busy) return;
    u6.busy = true;
    ++u6.epoch;
    const toggle = u6By('u6-log-switch');
    const controls = [toggle, u6By('u6-clear')];
    const disabled = controls.map(control => control.disabled);
    for (const control of controls) control.disabled = true;
    try {
        const state = await u6Request(action, 'POST');
        u6.busy = false;
        u6Draw(state);
        if (u6By('u6-preview').open) await u6Preview();
    } catch {
        if (action === 'enable' || action === 'disable') toggle.checked = action === 'disable';
        u6By('u6-log-status').textContent = '操作未完成，请确认当前会话状态后重试。';
        controls.forEach((control,index) => control.disabled = disabled[index]);
    } finally { u6.busy = false; }
}

function u6Host(j) {
    u6By('u6-name').textContent = j.name || '无线 CarPlay 盒子';
    const version = j.sys?.appver || '';
    u6By('u6-version').textContent = version || '版本暂不可用';
}

function u6Boot() {
    for (const button of document.querySelectorAll('[data-u6-tab]')) {
        button.addEventListener('click', () => {
            for (const tab of document.querySelectorAll('[data-u6-tab]')) tab.setAttribute('aria-pressed', String(tab === button));
            for (const pane of document.querySelectorAll('[data-u6-pane]')) pane.hidden = pane.dataset.u6Pane !== button.dataset.u6Tab;
            if (button.dataset.u6Tab === 'nav') hudPoll();
        });
    }
    const toggle = u6By('u6-log-switch');
    toggle.addEventListener('change', () => u6Action(toggle.checked ? 'enable' : 'disable'));
    const clear = u6By('u6-clear');
    clear.addEventListener('click', () => u6Action(clear.dataset.action || 'init'));
    u6By('u6-preview').addEventListener('toggle', event => { if (event.target.open) u6Preview(); });
    clearInterval(u6.timer);
    u6Poll();
    u6.timer = setInterval(u6Poll, 4000);
    hudBoot();
}
