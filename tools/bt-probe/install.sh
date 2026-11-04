#!/bin/sh
# install.sh —— 安装 bt-probe 蓝牙状态探针
#
#   sudo sh install.sh              只记录（推荐先用这个）
#   sudo sh install.sh autorecover  记录 + 尝试自愈（实验性）
#   sudo sh install.sh uninstall    卸载（保留日志）
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "请用 sudo 运行: sudo sh $0"
    exit 1
fi

SRC=$(cd "$(dirname "$0")" && pwd)
PREFIX=/usr/local/lib/bt-probe
PLIST=/Library/LaunchDaemons/local.bt-probe.plist
LABEL=local.bt-probe

unload() {
    launchctl bootout "system/$LABEL" 2>/dev/null || true
}

if [ "${1:-}" = "uninstall" ]; then
    unload
    rm -f "$PLIST"
    rm -rf "$PREFIX"
    rm -rf /var/tmp/bt-probe
    echo "已卸载。日志保留在 /var/log/bt-probe.log"
    exit 0
fi

echo "== 1/4 检查源文件"
for f in bt-probe.sh local.bt-probe.plist; do
    [ -f "$SRC/$f" ] || { echo "缺少 $SRC/$f"; exit 1; }
done

echo "== 2/4 安装到 $PREFIX"
mkdir -p "$PREFIX"
install -m 755 "$SRC/bt-probe.sh" "$PREFIX/bt-probe.sh"

echo "== 3/4 写 LaunchDaemon"
install -m 644 "$SRC/local.bt-probe.plist" "$PLIST"
chown root:wheel "$PLIST"
chmod 644 "$PLIST"

if [ "${1:-}" = "autorecover" ]; then
    /usr/libexec/PlistBuddy -c "Add :ProgramArguments:2 string --autorecover" "$PLIST"
    echo "   已启用实验性自愈（--autorecover）"
else
    echo "   纯观测模式（不加 --autorecover）"
fi

: > /var/log/bt-probe.log
chmod 644 /var/log/bt-probe.log

echo "== 4/4 注册并启动"
unload
launchctl bootstrap system "$PLIST"
launchctl enable "system/$LABEL" 2>/dev/null || true
launchctl kickstart "system/$LABEL" 2>/dev/null || true
sleep 2

echo
echo "安装完成。当前状态："
tail -5 /var/log/bt-probe.log 2>/dev/null || echo "  (还没有日志，等一分钟)"
echo
echo "  查看状态: sudo launchctl print system/$LABEL | head -20"
echo "  看日志:   sudo tail -f /var/log/bt-probe.log"
echo "  卸载:     sudo sh $SRC/install.sh uninstall"
