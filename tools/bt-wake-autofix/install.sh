#!/bin/sh
# install.sh —— 安装 bt-wake-autofix 守护
#   sudo sh install.sh
#   sudo sh install.sh uninstall
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "请用 sudo 运行: sudo sh $0"
    exit 1
fi

SRC=$(cd "$(dirname "$0")" && pwd)
PREFIX=/usr/local/lib/bt-autofix
PLIST=/Library/LaunchDaemons/local.bt-wake-autofix.plist
LABEL=local.bt-wake-autofix

unload() {
    launchctl bootout "system/$LABEL" 2>/dev/null || true
}

if [ "${1:-}" = "uninstall" ]; then
    unload
    rm -f "$PLIST"
    rm -rf "$PREFIX"
    rm -f /var/tmp/bt-autofix.pid
    echo "已卸载。日志保留在 /var/log/bt-autofix.log"
    exit 0
fi

echo "== 1/4 检查源文件"
for f in bt-wake-autofix.sh bt-wake-autofix.js local.bt-wake-autofix.plist; do
    [ -f "$SRC/$f" ] || { echo "缺少 $SRC/$f"; exit 1; }
done

echo "== 2/4 安装到 $PREFIX"
mkdir -p "$PREFIX"
install -m 755 "$SRC/bt-wake-autofix.sh" "$PREFIX/bt-wake-autofix.sh"
install -m 644 "$SRC/bt-wake-autofix.js" "$PREFIX/bt-wake-autofix.js"

echo "== 3/4 检查 IOBluetooth 私有接口（这一步决定整套是否可用）"
if ! /usr/bin/osascript -l JavaScript "$PREFIX/bt-wake-autofix.js" --check > /tmp/bt-autofix-check.txt 2>&1; then
    echo "自检执行失败，输出如下："
    cat /tmp/bt-autofix-check.txt
    echo
    echo " → 这套方案依赖 IOBluetooth 私有接口，当前系统可能已移除。不要继续安装。"
    exit 2
fi
cat /tmp/bt-autofix-check.txt
if grep -q '^ERROR' /tmp/bt-autofix-check.txt; then
    echo
    echo " → 接口缺失，放弃安装（不会影响现有蓝牙配置）。"
    exit 2
fi

echo "== 4/4 注册 LaunchDaemon"
unload
install -m 644 "$SRC/local.bt-wake-autofix.plist" "$PLIST"
chown root:wheel "$PLIST"
chmod 644 "$PLIST"
launchctl bootstrap system "$PLIST"
launchctl enable "system/$LABEL" 2>/dev/null || true

echo
echo "安装完成。"
echo "  查看状态: sudo launchctl print system/$LABEL | head -20"
echo "  查看日志: tail -20 /var/log/bt-autofix.log"
echo "  立即试一次: sudo sh $PREFIX/bt-wake-autofix.sh --now"
echo "  卸载: sudo sh $SRC/install.sh uninstall"
