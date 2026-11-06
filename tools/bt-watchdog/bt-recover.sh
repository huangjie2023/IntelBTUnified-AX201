#!/bin/sh
# 故障 A（APFS 校验失败 SIGBUS）连续崩溃时，launchd 会把 bluetoothd 打入节流，
# 表现为"蓝牙整个消失、必须重启"。这个脚本在节流窗口过后把它拉回来。
# 由 com.local.btrecover.plist 每 20 秒调用一次；正常时只做一次 pgrep 就退出。
LOG=/var/log/bt-recover.log

pgrep -x bluetoothd >/dev/null 2>&1 && exit 0

echo "$(date '+%F %T') bluetoothd missing -> kickstart" >>"$LOG"
launchctl kickstart -k system/com.apple.bluetoothd >>"$LOG" 2>&1
sleep 6

pgrep -x bluetoothd >/dev/null 2>&1 && exit 0

# 第二次：节流较深时第一次 kickstart 会被拒，再踢一次。
echo "$(date '+%F %T') still missing -> retry kickstart" >>"$LOG"
launchctl kickstart -k system/com.apple.bluetoothd >>"$LOG" 2>&1
sleep 6

pgrep -x bluetoothd >/dev/null 2>&1 || echo "$(date '+%F %T') gave up this round" >>"$LOG"
exit 0
