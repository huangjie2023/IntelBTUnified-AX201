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

# 2026-11-07：仅靠“bluetoothd 不在”抓不到崩溃后的坏页状态。launchd 会在
# 10 秒 throttle 后自动把 bluetoothd 拉起来，探针 60 秒一轮，看到的永远是
# “进程在、设备在、电源开”= 正常三元组，于是 autorecover 一次都不会触发。
# 新增触发源：诊断报告目录里出现新的 bluetoothd / ExcUserFault 报告。
CRASH_DIR=/Library/Logs/DiagnosticReports
CRASHMARK="$STATE_DIR/crashmark"
RECOVERMARK="$STATE_DIR/recover-at"
RECOVER_COOLDOWN=120

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

# ---------------- 新崩溃报告 ----------------
# 崩溃报告落盘比崩溃本身滞后约 13 秒，60 秒轮询足够。
latest_crash=$(ls -t "$CRASH_DIR"/bluetoothd*.ips "$CRASH_DIR"/ExcUserFault*.ips 2>/dev/null | head -1)
crm=0
if [ -n "${latest_crash:-}" ]; then
    crm=$(stat -f %m "$latest_crash" 2>/dev/null || echo 0)
    case "$crm" in ''|*[!0-9]*) crm=0 ;; esac
fi
prev_crm=$(cat "$CRASHMARK" 2>/dev/null || echo 0)
case "$prev_crm" in ''|*[!0-9]*) prev_crm=0 ;; esac

new_crash=0
if [ -f "$CRASHMARK" ]; then
    [ "$crm" -gt "$prev_crm" ] && new_crash=1
else
    # 首次只建基线，不触发。注意这里必须无条件落盘（crm 可能是 0）：
    # 若首跑时目录里还没有任何报告就不写文件，那么第一份真崩溃会被
    # 误当成“首次”吃掉，自愈永远晚一份报告。
    printf '%s\n' "$crm" > "$CRASHMARK"
fi

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

if [ "$cur" = "$prev" ] && [ "$new_crash" = 0 ]; then
    th=$(cat "$HEART" 2>/dev/null || echo 0)
    case "$th" in ''|*[!0-9]*) th=0 ;; esac
    tn=$(epoch)
    if [ $((tn - th)) -ge 1800 ]; then
        logline "heartbeat | $cur | $uptime_str"
        printf '%s\n' "$tn" > "$HEART"
    fi
    exit 0
fi

if [ "$cur" != "$prev" ]; then
    logline "TRANSITION | $prev -> $cur"
    dump_context
fi
if [ "$new_crash" = 1 ]; then
    logline "NEW-CRASH | $(basename "$latest_crash") (btd=$btd usb=$usb pw=$pw)"
fi

# ---------------- 自愈 ----------------
# 两个触发源，任一满足即可：
#   1. 设备在总线上、电源开、但 bluetoothd 进程不在（老的判据）
#   2. 出现新的崩溃报告 —— 这条才是主力。launchd 抢在探针前面把 bluetoothd
#      拉起来了，进程活着但代码页是崩溃前那一份混合态，表现为“蓝牙能开、
#      连不上设备、一连就自动开关”。老的判据看不到这种情况。
dorecover() {
    r="$1"
    logline "AUTORECOVER($r): purge + kickstart"
    t0=$(epoch)
    /usr/sbin/purge >> "$LOG" 2>&1 || true
    logline "AUTORECOVER: purge finished in $(( $(epoch) - t0 ))s"
    sleep 1
    launchctl kickstart -k system/com.apple.bluetoothd >> "$LOG" 2>&1 || true
    sleep 4
    if pgrep -x bluetoothd >/dev/null 2>&1; then
        logline "AUTORECOVER: ok, bluetoothd pid $(pgrep -x bluetoothd | head -1)"
        return 0
    fi
    # 有些场景 purge 后第一次 kickstart 仍会撞上校验，隔 10s 再来一轮
    logline "AUTORECOVER: 1st attempt failed, retrying in 10s"
    sleep 10
    /usr/sbin/purge >> "$LOG" 2>&1 || true
    launchctl kickstart -k system/com.apple.bluetoothd >> "$LOG" 2>&1 || true
    sleep 4
    if pgrep -x bluetoothd >/dev/null 2>&1; then
        logline "AUTORECOVER: ok on 2nd attempt, bluetoothd pid $(pgrep -x bluetoothd | head -1)"
    else
        logline "AUTORECOVER: failed, still no bluetoothd"
    fi
}

if [ "$AUTORECOVER" = 1 ] && [ "$usb" = 1 ] && [ "$pw" = 1 ]; then
    need=0
    reason=""
    [ "$btd" = 0 ] && { need=1; reason="bluetoothd missing"; }
    if [ "$new_crash" = 1 ]; then
        need=1
        if [ -z "$reason" ]; then reason="new crash report"; else reason="$reason; new crash report"; fi
    fi
    if [ "$need" = 1 ]; then
        tn=$(epoch)
        last=$(cat "$RECOVERMARK" 2>/dev/null || echo 0)
        case "$last" in ''|*[!0-9]*) last=0 ;; esac
        if [ $((tn - last)) -lt "$RECOVER_COOLDOWN" ]; then
            logline "AUTORECOVER: skipped ($reason), $((RECOVER_COOLDOWN - (tn - last)))s cooldown left"
        else
            dorecover "$reason"
            printf '%s\n' "$tn" > "$RECOVERMARK"
        fi
    fi
fi

# 同一份报告只触发一次：无论是否真的恢复，都把基线推平
[ "$crm" -gt 0 ] && printf '%s\n' "$crm" > "$CRASHMARK"

exit 0
