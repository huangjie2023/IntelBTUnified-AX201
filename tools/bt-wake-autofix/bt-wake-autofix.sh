#!/bin/sh
# bt-wake-autofix.sh
# 平时什么都不做，只在发现 bluetoothd 换过 PID（= 被 launchd 重新拉起）之后，
# 等 20 秒让系统安静下来，再调 bt-wake-autofix.js 把控制器上电、把已配对设备连回去。
#
# 手动用法:
#   sudo sh bt-wake-autofix.sh --check   能力自检，不动作
#   sudo sh bt-wake-autofix.sh --now     立刻执行一次恢复
#   sudo sh bt-wake-autofix.sh           守护模式（由 LaunchDaemon 每 30s 调一次）
set -u

PREFIX="${BT_AUTOFIX_PREFIX:-/usr/local/lib/bt-autofix}"
JS="$PREFIX/bt-wake-autofix.js"
STATE="${BT_AUTOFIX_STATE:-/var/tmp/bt-autofix.pid}"
LOG="${BT_AUTOFIX_LOG:-/var/log/bt-autofix.log}"
DELAY="${BT_AUTOFIX_DELAY:-20}"

say() {
    printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >>"$LOG" 2>/dev/null
}

runjs() {
    /usr/bin/osascript -l JavaScript "$JS" "$@" 2>&1
}

case "${1:-}" in
    --check)
        echo "=== bt-wake-autofix 能力自检 ==="
        echo "js: $JS"
        [ -f "$JS" ] || { echo "缺少 $JS"; exit 1; }
        runjs --check
        exit $?
        ;;
    --now)
        echo "=== 立即执行一次 ==="
        runjs
        exit $?
        ;;
    --help|-h)
        sed -n '2,12p' "$0"
        exit 0
        ;;
esac

[ -f "$JS" ] || exit 0

pid=$(/usr/bin/pgrep -x bluetoothd 2>/dev/null | /usr/bin/head -1)
[ -z "$pid" ] && exit 0

prev=$(cat "$STATE" 2>/dev/null || printf '')
printf '%s\n' "$pid" >"$STATE" 2>/dev/null

# 首次运行只登记，不动作。
[ -z "$prev" ] && exit 0
[ "$pid" = "$prev" ] && exit 0

say "bluetoothd 重启 $prev -> $pid，等待 ${DELAY}s 后执行恢复"
sleep "$DELAY"

result=$(runjs)
say "恢复结果: $(printf '%s' "$result" | tr '\n' ';')"
