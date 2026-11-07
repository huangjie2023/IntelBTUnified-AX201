#!/usr/bin/perl
#
# 定位 bluetoothd 里 transport 初始化超时日志的引用点（故障 B 根治第一步）。
#
# 背景：睡眠唤醒后控制器就绪 >15 秒，bluetoothd 等 15 秒超时后 os_crash
# （EXC_GUARD）。实测崩溃路径是 USB transport："Failed to initialize transport"
# （×4 处）日志紧邻崩溃。目标：找到引用这些字符串的代码，从上下文字节里
# 解出 15 秒常量（15 / 15000 / 15e9 ns / 任意形态），设计字节补丁。
#
# 用法：
#   cp /usr/sbin/bluetoothd /tmp/btd
#   perl find-timeout.pl /tmp/btd
#
# 只用 perl 自带模块，不需要 Xcode 命令行工具。
# 注意：全文用 $buf 而非 $b —— $b 会遮蔽 sort 的全局变量。

use strict;
use warnings;

my $path = $ARGV[0] || '/tmp/btd';
open(my $fh, '<:raw', $path) or die "open $path: $!\n";
local $/;
my $buf = <$fh>;
close $fh;
my $size = length($buf);
printf "file=%s size=%d\n", $path, $size;

sub u32 { unpack('V', substr($buf, $_[0], 4)) }
sub u64 { unpack('Q<', substr($buf, $_[0], 8)) }

# ---- fat / thin ----
# fat 魔数 0xcafebabe/0xcafebabf 是大端存储，'V'（小端）读出来是
# 0xbebafeca / 0xbfbafeca，两种都要认。
my $base  = 0;
my $magic = u32(0);
if ($magic == 0xbebafeca || $magic == 0xbfbafeca) {
    my $cnt = unpack('N', substr($buf, 4, 4));
    printf "fat slices=%d\n", $cnt;
    for my $i (0 .. $cnt - 1) {
        my $p = 8 + $i * 20;
        my ($cpu, $sub, $off, $sz) = unpack('NNNN', substr($buf, $p, 16));
        printf "  slice%d cpu=0x%08x off=%d size=%d\n", $i, $cpu, $off, $sz;
        $base = $off if $cpu == 0x01000007 && $base == 0;    # x86_64
    }
    printf "picked x86_64 slice at %d\n", $base;
}

my $m = u32($base);
die sprintf("bad mach magic 0x%08x at %d（没找到 x86_64 切片？）\n", $m, $base)
    unless $m == 0xfeedfacf;
# load commands 起始偏移：传统 mach_header_64 共 28 字节，commands 在 +28。
# Tahoe 实测（17:38 真机 bluetoothd）：头部多 4 字节——+28 处为 0x00000000，
# +32 处才是第一条 LC_SEGMENT_64(0x19)。自适应判定：+28 处 cmd=0 而 +32 处
# 像合法命令（cmd≠0 且 size 合理）→ 用 +32。
sub cmd_start {
    my $off = 28;
    if (u32($base + 28) == 0) {
        my ($c, $s) = (u32($base + 32), u32($base + 36));
        $off = 32 if $c != 0 && $s >= 8 && $s <= 0x100000;
    }
    return $off;
}

my $ncmds = u32($base + 16);
my $p     = $base + cmd_start();
my ($vmaddr, $fileoff, $filesize) = (0, 0, 0);

for (1 .. $ncmds) {
    my $cmd     = u32($p);
    my $cmdsize = u32($p + 4);
    last if $cmdsize < 8;
    if ($cmd == 0x19) {    # LC_SEGMENT_64
        my $name = substr($buf, $p + 8, 16);
        $name =~ s/\0.*//s;
        my $va = u64($p + 24);
        my $fo = u64($p + 40);
        my $fs = u64($p + 48);
        printf "SEG %-16s vmaddr=0x%x fileoff=%d filesize=%d\n", $name, $va, $fo, $fs;
        ($vmaddr, $fileoff, $filesize) = ($va, $fo, $fs) if $name eq '__TEXT';
    }
    $p += $cmdsize;
}
unless ($filesize) {
    print "!! __TEXT 未找到，dump load commands：\n";
    dump_commands();
    die "no __TEXT segment\n";
}
printf "TEXT vmaddr=0x%x fileoff=%d(abs %d) size=%d\n", $vmaddr, $fileoff, $base + $fileoff, $filesize;

# 诊断：__TEXT 没找到时，把文件类型和 load command 流 dump 出来。
sub dump_commands {
    my $ft        = u32($base + 12);
    my $sizeofcmds = u32($base + 20);
    printf "mach filetype=%d (2=EXECUTE 6=DYLIB 13=FILESET) ncmds=%d sizeofcmds=%d cmd_start=+%d\n",
        $ft, $ncmds, $sizeofcmds, cmd_start();
    my $q = $base + cmd_start();
    my $end = $base + 28 + $sizeofcmds;
    $end = $base + $size if $end > $base + $size;
    my $shown = 0;
    while ($q < $end && $shown < 40) {
        my ($c, $sz) = (u32($q), u32($q + 4));
        my $extra = '';
        if ($c == 0x19) {
            my $nm = substr($buf, $q + 8, 16);
            $nm =~ s/\0.*//s;
            $extra = " seg=$nm";
        } elsif ($c == 0x80000034) {
            my $nm = substr($buf, $q + 8, 16);
            $nm =~ s/\0.*//s;
            $extra = " fileset_entry=$nm";
        }
        printf "  cmd=0x%08x size=%u%s\n", $c, $sz, $extra;
        last if $sz < 8;
        $q += $sz;
        $shown++;
    }
}

# ---- 找字符串 ----
# 实测（17:11）：USB 路径 "Failed to initialize transport" ×4；UART 时代的
# "port failed to appear" 可能也还在。所有 needle 全扫，全部报告。
my @needles = (
    'Failed to initialize transport',
    'port failed to appear',
    'failed to appear after',
    '/dev/cu.BLTH',
);

my @hits;
for my $needle (@needles) {
    my $pos = 0;
    my $n   = 0;
    while (($pos = index($buf, $needle, $pos)) >= 0) {
        push @hits, $pos;
        $pos += 1;
        $n++;
    }
    printf "needle '%s' hits=%d\n", $needle, $n;
}
# 同一字符串可能被多个 needle 命中（前缀重叠），按位置去重。
my %seen;
@hits = grep { !$seen{$_}++ } @hits;
@hits = sort { $a <=> $b } @hits;

# 关键：段表里的 fileoff 是切片内相对偏移；hits 是绝对文件位置。
# __TEXT 数据的绝对区间 = [base+fileoff, base+fileoff+filesize)。
my $tstart = $base + $fileoff;
my $tend   = $tstart + $filesize;

my %tgt;
for my $h (@hits) {
    next unless $h >= $tstart && $h < $tend;
    my $va = $vmaddr + ($h - $tstart);
    printf "  hit file=%d va=0x%x\n", $h, $va;
    my $s = substr($buf, $h, 128);
    $s =~ s/[^\x20-\x7e]/./g;
    print "    text: $s\n";
    $tgt{$va} = 1;
}
die "string only outside __TEXT\n" unless %tgt;

# ---- 扫 RIP-relative lea 引用 ----
# lea r64, [rip+disp32]：REX.W 前缀 48（r8~r15 用 4C），modrm mod=00 rm=101，
# reg 任意 → 05 0D 15 1D 25 2D 35 3D。
my @refs;
while ($buf =~ /([\x48\x4C])\x8D([\x05\x0D\x15\x1D\x25\x2D\x35\x3D])/g) {
    my $i = pos($buf) - 3;
    next if $i < $tstart;
    next if $i + 7 > $tend;
    my $disp = unpack('l<', substr($buf, $i + 3, 4));
    my $ia   = $vmaddr + ($i - $tstart);
    my $t    = $ia + 7 + $disp;
    push @refs, [$i, $ia] if $tgt{$t};
}

printf "refs=%d\n", scalar(@refs);
for my $r (@refs) {
    my ($i, $ia) = @$r;
    printf "\nref va=0x%x file=%d\n", $ia, $i;
    # 前后各留足上下文：引用点前 64 字节（常见 mov esi,imm32 / cmp /
    # dispatch_after 参数都在 lea 之后几个字节内），后 160 字节。
    my $start = $i - 64;
    $start = $tstart if $start < $tstart;
    my $len = $i - $start + 160;
    $len = $tend - $start if $start + $len > $tend;
    my $hex = unpack('H*', substr($buf, $start, $len));
    $hex =~ s/(..)/$1 /g;
    printf "  from 0x%x (ref at +%d):\n  %s\n", $vmaddr + ($start - $tstart), $i - $start, $hex;
}

print "\ndone\n";
