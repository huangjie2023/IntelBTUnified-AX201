#!/bin/sh
# collect.sh —— 一次采齐「这次崩溃到底是谁的锅」所需的全部证据，输出压到 60 行内。
#
#   curl -fsSL .../collect.sh -o /tmp/cc.sh && sudo sh /tmp/cc.sh
#
# 为什么需要它：dmesg 只够存 ~8 分钟（DC6/IGFB 刷屏），而 log show --last boot
# 一次几千行根本没法贴。这里只取每个问题需要的那几行，并把结果同时写进
# ~/Desktop/bt-report.txt。
#
# 采集项：
#   1. kext 版本 / 开机时长 / msgbuf 容量
#   2. 最近几次 bluetoothd 崩溃报告的时间戳
#   3. 最新一份崩溃报告的关键签名（异常类型、故障地址、递归层数、有无 APFS 字样）
#   4. 内核日志里 btlfx/ibtp 的行（含 patched ... page 0x...）与吞错计数
#   5. 崩溃前 40 秒 bluetoothd 自己的统一日志（它在反复报什么错）
#   6. 补丁页定位：崩溃栈偏移是否落在我们改过的 4K 页里
#   7. 内存压力
set -u

UHOME=$(eval echo "~${SUDO_USER:-root}")
OUT="$UHOME/Desktop/bt-report.txt"
KLOG=/tmp/bt-kernel.log

run() {
    {
        echo "================ bt-report $(date '+%Y-%m-%d %H:%M:%S') ================"

        echo
        echo "---- 1. kext / 开机"
        kmutil showloaded 2>/dev/null | grep -i intelbt || echo "  IntelBTUnified 未加载"
        printf '  boottime: '; sysctl -n kern.boottime 2>/dev/null
        printf '  msgbuf 行数: '; dmesg 2>/dev/null | wc -l

        echo
        echo "---- 2. 最近 bluetoothd 崩溃报告"
        REPORTS=$(ls -t "$UHOME"/Library/Logs/DiagnosticReports/bluetoothd* /Library/Logs/DiagnosticReports/bluetoothd* 2>/dev/null | head -5)
        if [ -z "$REPORTS" ]; then
            echo "  (没有崩溃报告)"
        else
            for f in $REPORTS; do
                printf '  %s  %s\n' "$(stat -f '%Sm' -t '%Y-%m-%d %H:%M:%S' "$f")" "$(basename "$f")"
            done
        fi

        echo
        echo "---- 3. 最新一份报告的签名"
        NEW=$(ls -t "$UHOME"/Library/Logs/DiagnosticReports/bluetoothd* /Library/Logs/DiagnosticReports/bluetoothd* 2>/dev/null | head -1)
        if [ -n "$NEW" ]; then
            grep -oE '"exception" : \{[^}]*\}' "$NEW" 2>/dev/null | head -1
            grep -c '"recursionInfoArray"' "$NEW" 2>/dev/null | sed 's/^/  递归标记出现次数: /'
            grep -cE 'compressed data hash mismatch|Pagein of compressed inode' "$NEW" 2>/dev/null | sed 's/^/  APFS 校验失败字样: /'
            grep -oE '"depth" : [0-9]+' "$NEW" 2>/dev/null | head -2 | sed 's/^/  /'
        fi

        echo
        echo "---- 4. 内核日志里的 btlfx / ibtp"
        log show --last boot --style compact --predicate 'process == "kernel"' >"$KLOG" 2>/dev/null
        printf '  kernel 日志总行数: '; wc -l <"$KLOG"
        grep -iE "btlfx|ibtp:|intelbt" "$KLOG" | head -20 || echo "  (无)"
        printf '  swallowed verify mismatch 命中: '; grep -c "swallowed verify mismatch" "$KLOG"
        printf '  failed to route VNOP_VERIFY : '; grep -c "failed to route" "$KLOG"

        echo
        echo "---- 5. 崩溃前 40 秒 bluetoothd 日志（末尾 30 行）"
        if [ -n "$NEW" ]; then
            T=$(stat -f '%Sm' -t '%Y-%m-%d %H:%M:%S' "$NEW")
            S=$(date -j -v-40S -f '%Y-%m-%d %H:%M:%S' "$T" '+%Y-%m-%d %H:%M:%S' 2>/dev/null)
            E=$(date -j -v+10S -f '%Y-%m-%d %H:%M:%S' "$T" '+%Y-%m-%d %H:%M:%S' 2>/dev/null)
            echo "  窗口: $S .. $E"
            log show --start "$S" --end "$E" --info --debug --style compact \
                --predicate 'processImagePath CONTAINS "bluetoothd"' 2>/dev/null | tail -30
        fi

        echo
        echo "---- 6. 补丁页 vs 崩溃栈"
        if command -v python3 >/dev/null 2>&1; then
            cat >/tmp/bt-locate.py <<'PY'
import sys
PAGE=0x1000
PATS=[("vendor check",bytes([0x81,0xFA,0x5C,0x0A,0x00,0x00,0x74]),None),
      ("chipset classic",bytes([0x81,0xF9,0xCF,0x07,0x00,0x00,0x72]),None),
      ("chipset 13.3",bytes([0x81,0xF9,0x9E,0x0F,0x00,0x00,0x77,0x1A]),None),
      ("chipset 15.4",bytes([0x81,0xF9,0x6F,0x17,0x00,0x00,0x77,0x30]),None),
      ("nvram check",bytes([0x41,0x80,0x00,0x01,0x75,0x00,0x84,0xDB,0x75,0x00]),
                     bytes([0xFF,0xFF,0x00,0xFF,0xFF,0x00,0xFF,0xFF,0xFF,0x00])),
      ("same-address",bytes([0x48,0x89,0xF3,0xE8,0,0,0,0,0x85,0xC0,0x74,0x1D]),
                      bytes([0xFF,0xFF,0xFF,0xFF,0,0,0,0,0xFF,0xFF,0xFF,0xFF])),
      ("board-id",b"Mac-F60DEB81FF30ACF6",None)]
CRASH=[("recursion entry",1592056),("log call",1591558),("frame",1570056),("frame",5335207)]
try:
    d=open("/usr/sbin/bluetoothd","rb").read()
except OSError as e:
    print("  读不到 bluetoothd: %s"%e); raise SystemExit
pages={}
for name,pat,mask in PATS:
    hits=[];start=0
    while len(hits)<4:
        if mask is None:
            i=d.find(pat,start)
        else:
            i=-1
            while True:
                j=d.find(pat[0:1],start)
                if j<0: break
                if j+len(pat)<=len(d) and all(m==0 or d[j+k]==pat[k] for k,m in enumerate(mask)):
                    i=j; break
                start=j+1
        if i<0: break
        hits.append(i); start=i+1
    for o in hits:
        pages.setdefault(o&~(PAGE-1),[]).append("%s@0x%X"%(name,o))
print("  改写的页:")
if not pages: print("    (一个都没命中：系统版本 pattern 可能已变)")
for p in sorted(pages): print("    0x%X  %s"%(p,", ".join(pages[p])))
print("  崩溃栈比对:")
hit=False
for label,off in CRASH:
    p=off&~(PAGE-1); n=pages.get(p)
    if n: hit=True
    print("    0x%-7X (页 0x%-7X) %s"%(off,p,("重叠 -> "+",".join(n)) if n else "不重叠"))
print("  结论: " + ("补丁页与崩溃栈【重叠】，先排除补丁" if hit else "补丁页与崩溃栈【不重叠】，问题在别处"))
PY
            python3 /tmp/bt-locate.py 2>&1 | head -20
        else
            echo "  (没有 python3，跳过)"
        fi

        echo
        echo "---- 7. 内存压力"
        sysctl -n vm.swapusage 2>/dev/null | sed 's/^/  /'
        vm_stat 2>/dev/null | sed -n '3,5p' | sed 's/^/  /'

        echo
        echo "================ 完 ================"
    } 2>&1 | tee "$OUT"
}

if [ "$(id -u)" -ne 0 ]; then
    echo "请用 sudo 运行: sudo sh $0"
    exit 1
fi

run
echo
echo "已保存到: $OUT"
