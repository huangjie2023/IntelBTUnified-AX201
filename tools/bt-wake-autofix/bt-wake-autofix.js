// bt-wake-autofix.js
// bluetoothd 在唤醒后主动退出（EXC_GUARD / Application Triggered Fault）时，
// 负责把它留下的残局收拾干净：控制器上电 + 重连已配对设备。
//
// 用法:
//   osascript -l JavaScript bt-wake-autofix.js --check   只报告能力与状态，不动作
//   osascript -l JavaScript bt-wake-autofix.js           确保上电 + 重连
//
// 依赖: macOS 内置 osascript 的 ObjC bridge + IOBluetooth 私有 C 接口。

ObjC.import('IOBluetooth');

function str(v) {
    try {
        if (v === undefined || v === null) return '';
        var u = ObjC.unwrap(v);
        if (u === undefined || u === null) return '';
        return String(u);
    } catch (e) {
        return '';
    }
}

function has(fn) {
    try { return typeof fn === 'function'; } catch (e) { return false; }
}

function run(argv) {
    var checkOnly = argv.indexOf('--check') >= 0;
    var out = [];

    // 私有接口存在性。macOS 若移除，这里会直接说清楚，而不是静默失败。
    if (!has($.IOBluetoothPreferenceGetControllerPowerState)) {
        out.push('ERROR 此系统没有 IOBluetoothPreferenceGetControllerPowerState');
        return out.join('\n');
    }
    if (!has($.IOBluetoothPreferenceSetControllerPowerState)) {
        out.push('ERROR 此系统没有 IOBluetoothPreferenceSetControllerPowerState');
        return out.join('\n');
    }

    var power = $.IOBluetoothPreferenceGetControllerPowerState();
    out.push('controllerPower=' + power);

    if (power === 0) {
        if (checkOnly) {
            out.push('action: 会执行上电（--check 模式下不动作）');
        } else {
            var rc = $.IOBluetoothPreferenceSetControllerPowerState(1);
            out.push('powered_on rc=' + rc + ' now=' + $.IOBluetoothPreferenceGetControllerPowerState());
        }
    }

    var paired = null;
    try { paired = $.IOBluetoothPairedDevices(); } catch (e) { paired = null; }
    if (!paired || paired.isNil()) {
        out.push('paired=n/a（IOBluetoothPairedDevices 不可用）');
        return out.join('\n');
    }

    var n = paired.count;
    out.push('paired=' + n);
    if (n === 0) return out.join('\n');

    var pending = 0;
    for (var i = 0; i < n; i++) {
        var d;
        try { d = paired.objectAtIndex(i); } catch (e) { continue; }

        var name = str(d.name) || '(unnamed)';
        var addr = str(d.addressString) || '?';

        var connected = false;
        try { connected = d.isConnected() ? true : false; } catch (e) {}

        var trusted = false;
        try { trusted = d.isTrusted() ? true : false; } catch (e) {}

        if (connected) {
            out.push('  ' + addr + '  ' + name + '  connected');
            continue;
        }

        out.push('  ' + addr + '  ' + name + '  disconnected trusted=' + trusted);
        if (checkOnly) { pending++; continue; }

        // 只对 trusted 的设备动手，避免把已删除/临时配对的一起拉回来。
        if (!trusted) continue;
        try {
            var r = d.openConnection();
            out.push('    openConnection rc=' + r);
        } catch (e) {
            out.push('    openConnection 异常: ' + e);
        }
    }
    if (checkOnly && pending) out.push('action: ' + pending + ' 台设备待重连（--check 模式下不动作）');

    return out.join('\n');
}
