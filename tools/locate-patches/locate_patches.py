#!/usr/bin/env python3
"""locate_patches.py —— 在磁盘上定位 IntelBTUnified 会改写的那几段原始字节。

为什么需要它：kext 在 cs_validate_page 里按字节 pattern 原地打补丁，pattern 在
文件里的位置就决定了「哪一页被改」。而 dmesg 只够存约 8 分钟，开机时打的
`patched /usr/sbin/bluetoothd (page 0x...)` 早就被 DC6/IGFB 刷掉了。

这个脚本直接读 /usr/sbin/bluetoothd（APFS 透明解压，读到的就是逻辑字节，偏移
与崩溃报告里的 imageOffset 同一坐标系），把每个 pattern 的文件偏移、4K 页号算
出来，再和崩溃栈里的偏移比对。

判读：
  某个崩溃偏移所在的 4K 页 == 某个 pattern 所在的 4K 页
      -> 崩溃的这段代码就是被我们改过的，问题可能出在补丁本身
  完全不相交
      -> 递归与补丁无关（假事件 / bluetoothd 自身逻辑）

用法（需要 root，因为 /usr/sbin 在密封系统卷上）：
    sudo python3 locate_patches.py
    sudo python3 locate_patches.py /usr/sbin/BlueTool
    sudo python3 locate_patches.py /usr/sbin/bluetoothd 0x17F508 0x184AF8 0x5168A7
"""

import sys

PAGE = 0x1000
MAX_HITS = 6

# (名字, 原始字节, 掩码或 None)；与 src/IntelBTUnified.cpp 里的 k*Original 一一对应
PATTERNS = [
    ("vendor check",            bytes([0x81, 0xFA, 0x5C, 0x0A, 0x00, 0x00, 0x74]), None),
    ("chipset range (classic)", bytes([0x81, 0xF9, 0xCF, 0x07, 0x00, 0x00, 0x72]), None),
    ("chipset range 13.3",      bytes([0x81, 0xF9, 0x9E, 0x0F, 0x00, 0x00, 0x77, 0x1A]), None),
    ("chipset range 15.4",      bytes([0x81, 0xF9, 0x6F, 0x17, 0x00, 0x00, 0x77, 0x30]), None),
    ("nvram check",             bytes([0x41, 0x80, 0x00, 0x01, 0x75, 0x00, 0x84, 0xDB, 0x75, 0x00]),
                                bytes([0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0x00])),
    ("same-address check",      bytes([0x48, 0x89, 0xF3, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x85, 0xC0, 0x74, 0x1D]),
                                bytes([0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF])),
    ("board-id (Mac-F60...) ",  b"Mac-F60DEB81FF30ACF6", None),
]

# 2026-11-05 17:40 与 2026-11-06 15:17 两次 SIGBUS 的崩溃地址，减去 base 后
# 完全相同的 0x339B50（APFS 16KiB 压缩验证组校验失败，Kernel Triage 有
# "compressed data hash mismatch"）。这里按“失败 16KiB 组”比对，而不是按单页。
DEFAULT_CRASH = [
    ("SIGBUS 11-05 21:29", 0x339B50),
    ("SIGBUS 11-06 15:17", 0x339B50),
]
GROUP = 0x4000


def find_masked(data, pat, mask):
    """返回所有匹配偏移。mask 为 None 时按整段精确比较。"""
    n = len(pat)
    if mask is None:
        hits, start = [], 0
        while len(hits) < MAX_HITS:
            i = data.find(pat, start)
            if i < 0:
                break
            hits.append(i)
            start = i + 1
        return hits
    # 掩码搜索：先用第一个全掩码字节做粗筛
    anchor = 0
    while mask[anchor] != 0xFF:
        anchor += 1
        if anchor >= n:
            return []
    hits, start = [], 0
    while len(hits) < MAX_HITS:
        i = data.find(pat[anchor:anchor + 1], start)
        if i < 0:
            break
        base = i - anchor
        if base >= 0 and base + n <= len(data):
            if all(m == 0 or data[base + k] == pat[k] for k, m in enumerate(mask)):
                hits.append(base)
        start = i + 1
    return hits


def main():
    path = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("0x") else "/usr/sbin/bluetoothd"
    crash = DEFAULT_CRASH
    extra = [a for a in sys.argv[1:] if a.startswith("0x")]
    if extra:
        crash = [("自定义", int(a, 16)) for a in extra]

    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        print("读不了 %s: %s" % (path, e))
        return 1

    print("文件: %s" % path)
    print("大小: %d 字节 (0x%X)" % (len(data), len(data)))
    print()

    patched_pages = {}
    print("== 会被改写的原始字节位置")
    print("%-24s %-12s %-12s %s" % ("补丁", "文件偏移", "4K 页", "命中数"))
    for name, pat, mask in PATTERNS:
        hits = find_masked(data, pat, mask)
        if not hits:
            print("%-24s %-12s %-12s %s" % (name, "-", "-", "0 (此系统版本不含 / 已被打过)"))
            continue
        for i, off in enumerate(hits):
            page = off & ~(PAGE - 1)
            patched_pages.setdefault(page, []).append(name)
            print("%-24s 0x%-10X 0x%-10X %s" % (name if i == 0 else "", off, page,
                                               ("共 %d 处" % len(hits)) if i == 0 else ""))
    print()

    print("== 崩溃偏移 vs 16KiB 校验组")
    print("%-20s %-11s %-24s %s" % ("", "4K 页", "16KiB 校验组", "组内是否有我们改过的页"))
    overlap = False
    for label, off in crash:
        page = off & ~(PAGE - 1)
        grp = off & ~(GROUP - 1)
        inside = [pg for pg in sorted(patched_pages) if grp <= pg < grp + GROUP]
        if inside:
            overlap = True
            note = "是 -> " + ", ".join("0x%X" % pg for pg in inside)
        else:
            note = "否"
        print("%-20s 0x%-9X 0x%-9X-0x%-9X %s" % (label, page, grp, grp + GROUP - 1, note))
    print()

    print("== 被改过的页清单 (按偏移排序)")
    if patched_pages:
        for page in sorted(patched_pages):
            print("  0x%-8X 组 0x%-8X  %s" % (page, page & ~(GROUP - 1),
                                              ", ".join(sorted(set(patched_pages[page])))))
    else:
        print("  (无。可能该系统版本 pattern 已变化，或文件不是预期版本)")
    print()

    if overlap:
        print("结论: 失败的那个 16KiB 组里有我们改过的页 -> 典型“混合内容”哈希不符，")
        print("      1.9.3 的判定会命中并吞掉，属于补丁的预期副作用，方向正确。")
    else:
        print("结论: 失败的那个 16KiB 组里【一个我们改过的页都没有】-> 与补丁无关，")
        print("      是这一组压缩数据本身读出来/解压出来不对（介质或读路径），")
        print("      1.9.3 会拒吞并打 real data corruption，这属于硬件侧问题。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
