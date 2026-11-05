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

# 2026-11-05 17:40:55 崩溃报告里 bluetoothd 的 imageOffset（= 文件偏移）
DEFAULT_CRASH = [
    ("递归函数入口", 1592056),   # 0x184AF8
    ("日志调用点",   1591558),   # 0x184906
    ("递归函数",     1570056),   # 0x17F508
    ("递归函数",     5335207),   # 0x5168A7
]


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

    print("== 崩溃栈偏移比对")
    print("%-14s %-12s %-12s %s" % ("", "文件偏移", "4K 页", "该页是否被我们改过"))
    overlap = False
    for label, off in crash:
        page = off & ~(PAGE - 1)
        names = patched_pages.get(page)
        if names:
            overlap = True
            note = "是 -> " + ", ".join(sorted(set(names)))
        else:
            note = "否"
        print("%-14s 0x%-10X 0x%-10X %s" % (label, off, page, note))
    print()

    print("== 被改过的页清单 (按偏移排序)")
    if patched_pages:
        for page in sorted(patched_pages):
            print("  0x%X  %s" % (page, ", ".join(sorted(set(patched_pages[page])))))
    else:
        print("  (无。可能该系统版本 pattern 已变化，或文件不是预期版本)")
    print()

    if overlap:
        print("结论: 崩溃偏移与被改写的页【重叠】——补丁所在的那段代码就在递归环里，")
        print("      需要先排除补丁本身（收紧 pattern / 换等价改法）再谈别的。")
    else:
        print("结论: 崩溃偏移与被改写的页【完全不重叠】——递归环不在我们改过的代码里，")
        print("      问题指向伪造的 HCI 事件或 bluetoothd 自身逻辑。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
