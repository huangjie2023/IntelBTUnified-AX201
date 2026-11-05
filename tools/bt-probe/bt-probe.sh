#!/bin/sh
# bt-probe.sh —— AX201 蓝牙状态探针
#
#   默认行为：只读。不 kickstart、不重启、不改任何系统状态。
#   --autorecover：额外在“USB 设备还在、电源开着、bluetoothd 不在”时尝试拉起 bluetoothd。
#                  这一条是实验性的，见 README。
#
# 记录的状态三元组：
#   usb  = USB 总线上还有没有 "Bluetooth USB Host Controller"
#   btd  = bluetoothd 进程在不在
#   pw   = com.apple.Bluetooth 里的 ControllerPowerState（1 表示用户希望它开着）
#
# 只在状态发生变化时写日志；另外每 30 分钟写一行 heartbeat 证明探针还活着。
set -u

STATE_DIR=/var/tmp/bt-probe
LOG=/var/log/bt-probe.log
STATE="$STATE_DIR/state"
HEART="$STATE_DIR/heart"
MAXLOG=2097152

AUTORECOVER=0
for a in "$@"; do
    case "$a" in
        --autorecover) AUTORECOVER=1 ;;
    esac
done

mkdir -p "$STATE_DIR" 2>/dev/null || exit 1

now()   { date "+%Y-%m-%d %H:%M:%S"; }
epoch() { date +%s; }
logline() { printf '%s %s\n' "$(now)" "$*" >> "$LOG"; }

# 轮转：超过 2MB 改名为 .1，只保留一份旧日志
if [ -f "$LOG" ]; then
    sz=$(wc -c < "$LOG" 2>/dev/null | tr -d ' ')
    if [ -n "${sz:-}" ] && [ "$sz" -gt "$MAXLOG" ] 2>/dev/null; then
        mv -f "$LOG" "$LOG.1"
    fi
fi

# ---------------- 采集 ----------------
usb=0
ioreg -p IOUSB -w0 2>/dev/null | grep -qi "Bluetooth USB Host Controller" && usb=1

btd=0
btdpid=$(pgrep -x bluetoothd 2>/dev/null | head -1)
[ -n "${btdpid:-}" ] && btd=1

pw=$(defaults read /Library/Preferences/com.apple.Bluetooth ControllerPowerState 2>/dev/null || echo -1)

cur="usb=$usb btd=$btd pw=$pw"
prev=$(cat "$STATE" 2>/dev/null || echo "")
printf '%s\n' "$cur" > "$STATE"

uptime_str=$(uptime 2>/dev/null | sed 's/^ *//')

# ---------------- 上下文转储 ----------------
dump_context() {
    logline "---- context begin ----"
    logline "  uptime: $uptime_str"
    logline "  usb bt nodes: $(ioreg -p IOUSB -w0 2>/dev/null | grep -i 'bluetooth' | head -3 | tr '\n' ' ')"
    logline "  xport obj: $(ioreg -r -c IOBluetoothHostControllerUSBTransport -w0 2>/dev/null | head -2 | tr '\n' ' ')"
    logline "  btd procs: $(pgrep -lf bluetoothd 2>/dev/null | head -5 | tr '\n' '|')"
    logline "  ---- kernel log (last 3m) ----"
    log show --last 3m --predicate 'process == "kernel"' --info 2>/dev/null \
        | grep -iE "bluetooth|apfs|usb" | tail -20 | while IFS= read -r l; do
            logline "    $l"
        done
    logline "  ---- recent crash reports ----"
    for d in /Library/Logs/DiagnosticReports "$HOME/Library/Logs/DiagnosticReports"; do
        [ -d "$d" ] || continue
        find "$d" -maxdepth 1 -name 'bluetoothd*' -mtime -1 2>/dev/null | tail -5 | while IFS= read -r f; do
            logline "    $f"
        done
    done
    logline "---- context end ----"
}

# ---------------- 主流程 ----------------
if [ -z "$prev" ]; then
    logline "probe started | $cur | pid=${btdpid:-none} | $uptime_str"
    exit 0
fi

if [ "$cur" = "$prev" ]; then
    th=$(cat "$HEART" 2>/dev/null || echo 0)
    case "$th" in ''|*[!0-9]*) th=0 ;; esac
    tn=$(epoch)
    if [ $((tn - th)) -ge 1800 ]; then
        logline "heartbeat | $cur | $uptime_str"
        printf '%s\n' "$tn" > "$HEART"
    fi
    exit 0
fi

logline "TRANSITION | $prev -> $cur"
dump_context

# ---------------- 实验性自愈 ----------------
# 2026-11-05 实测验证：purge + kickstart 组合成功恢复“蓝牙在但连不上”的状态。
# 机制：IntelBTUnified 的 patched_cs_validate_page 钩子会在内存里改 bluetoothd 的代码页；
# 内存压力下该页（或其所在 APFS 16KB 压缩验证组的相邻页）被逐出，再 pagein 时
# 组内哈希校验失败 → SIGBUS / 加载到坏页。purge 把整个 page cache 冲掉，
# 让 bluetoothd 下次从盘上重新加载“干净原始页”，kickstart 重新拉起进程。
# 单独 kickstart 在部分场景（坏页仍在 UBC 里）不够，必须先 purge。
if [ "$AUTORECOVER" = 1 ] && [ "$usb" = 1 ] && [ "$pw" = 1 ] && [ "$btd" = 0 ]; then
    logline "AUTORECOVER: device on bus, power on, bluetoothd missing -> purge + kickstart"
    /usr/sbin/purge >> "$LOG" 2>&1 || true
    launchctl kickstart -k system/com.apple.bluetoothd >> "$LOG" 2>&1 || true
    sleep 3
    if pgrep -x bluetoothd >/dev/null 2>&1; then
        logline "AUTORECOVER: ok, bluetoothd is back"
    else
        # 第二次尝试：有些场景 purge 后第一次 kickstart 仍会撞上校验，隔 10s 再来一次
        sleep 10
        /usr/sbin/purge >> "$LOG" 2>&1 || true
        launchctl kickstart -k system/com.apple.bluetoothd >> "$LOG" 2>&1 || true
        sleep 3
        if pgrep -x bluetoothd >/dev/null 2>&1; then
            logline "AUTORECOVER: ok on 2nd attempt, bluetoothd is back"
        else
            logline "AUTORECOVER: failed, still no bluetoothd"
        fi
    fi
fi

exit 0
