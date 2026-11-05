#!/bin/sh
# install.sh —— 安装 1.9.1 验证看门狗（单文件自解压，不依赖仓库其它文件）
#
#   sudo sh install.sh          安装并启动
#   sudo sh install.sh status   看已抓到的记录
#   sudo sh install.sh uninstall
#
# 为什么需要它：本机 dmesg 缓冲区只够存约 8 分钟（DC6/IGFB 刷屏），
# 而 "swallowed verify mismatch" 是 1.9.1 修复命中的唯一证据，
# 不抓就永远看不到。看门狗每 60 秒扫一次 dmesg，把新增的命中追加到
# /var/log/bt-verify.log，只记录增量，不重复。
set -eu

LABEL=local.bt-verify
DIR=/usr/local/lib/bt-verify
PLIST=/Library/LaunchDaemons/$LABEL.plist
SINK=/var/log/bt-verify.log
STATE=/var/tmp/bt-verify

if [ "$(id -u)" -ne 0 ]; then
    echo "请用 sudo 运行: sudo sh $0"
    exit 1
fi

if [ "${1:-}" = "status" ]; then
    echo "== kext 版本"
    kmutil showloaded 2>/dev/null | grep -i intelbt || echo "  (未加载)"
    echo "== 抓到的命中记录"
    if [ -f "$SINK" ]; then cat "$SINK"; else echo "  (还没有。说明至今没触发过故障条件，或修复没生效——两者都需继续观察)"; fi
    echo "== 看门狗状态"
    launchctl print "system/$LABEL" 2>/dev/null | grep -E "state|last exit code|runs" || echo "  (未安装)"
    exit 0
fi

if [ "${1:-}" = "uninstall" ]; then
    launchctl bootout "system/$LABEL" 2>/dev/null || true
    rm -f "$PLIST"
    rm -rf "$DIR" "$STATE"
    echo "已卸载。记录保留在 $SINK"
    exit 0
fi

echo "== 1/4 写看门狗脚本"
mkdir -p "$DIR" "$STATE"
cat > "$DIR/verify-watch.sh" <<'WATCH'
#!/bin/sh
# 每 60 秒扫 dmesg，抓 1.9.1 的 VNOP_VERIFY 吞错记录（只记增量）
set -u
SINK=/var/log/bt-verify.log
CNT=/var/tmp/bt-verify/count

now=$(date "+%Y-%m-%d %H:%M:%S")
cur=$(dmesg 2>/dev/null | grep -c "swallowed verify mismatch" 2>/dev/null || true)
case "${cur:-}" in ''|*[!0-9]*) cur=0 ;; esac
prev=$(cat "$CNT" 2>/dev/null || echo 0)
case "${prev:-}" in ''|*[!0-9]*) prev=0 ;; esac

if [ "$cur" -gt "$prev" ]; then
    dmesg 2>/dev/null | grep "swallowed verify mismatch" | tail -n $((cur - prev)) | while IFS= read -r l; do
        printf '%s VERIFY-BYPASS | %s\n' "$now" "$l" >> "$SINK"
    done
elif [ "$cur" -lt "$prev" ]; then
    # 缓冲区滚动：计数变小，之前的命中可能已丢失
    printf '%s NOTE | dmesg rotated (count %s -> %s)\n' "$now" "$prev" "$cur" >> "$SINK"
fi
printf '%s\n' "$cur" > "$CNT"
WATCH
chmod 755 "$DIR/verify-watch.sh"

echo "== 2/4 写 LaunchDaemon"
cat > "$PLIST" <<PLISTEOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>$LABEL</string>
    <key>ProgramArguments</key>
    <array>
        <string>/bin/sh</string>
        <string>$DIR/verify-watch.sh</string>
    </array>
    <key>StartInterval</key>
    <integer>60</integer>
    <key>RunAtLoad</key>
    <true/>
    <key>StandardErrorPath</key>
    <string>/var/log/bt-verify.err</string>
</dict>
</plist>
PLISTEOF
chown root:wheel "$PLIST"
chmod 644 "$PLIST"

echo "== 3/4 初始化记录"
: > "$SINK"
chmod 644 "$SINK"

echo "== 4/4 注册并启动"
launchctl bootout "system/$LABEL" 2>/dev/null || true
launchctl bootstrap system "$PLIST"
launchctl enable "system/$LABEL" 2>/dev/null || true
launchctl kickstart "system/$LABEL" 2>/dev/null || true
sleep 1

echo
echo "安装完成。"
echo "  看证据:   sudo sh $0 status"
echo "  实时观察: sudo tail -f $SINK"
echo "  卸载:     sudo sh $0 uninstall"
