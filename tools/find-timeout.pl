#!/usr/bin/perl
#
# 定位 bluetoothd 里 transport 初始化超时日志的引用点（故障 B 根治第一步）。
#
# 背景：睡眠唤醒后控制器就绪 >15 秒，bluetoothd 等 15 秒超时后 os_crash
# （EXC_GUARD）。实测崩溃路径是 USB transport："Failed to initialize transport"
# （×4 处）日志紧邻崩溃。目标：找到引用这些字符串的代码，从上下文字节里
# 解出 15 秒常量（15 / 15000 / 15e9 ns / 15<<20 任意形态），设计字节补丁。
#
# 用法：
#   cp /usr/sbin/bluetoothd /tmp/btd
#   perl find-timeout.pl /tmp/btd
#
# 只用 perl 自带模块，不需要 Xcode 命令行工具。

use strict;
use warnings;

my $path = $ARGV[0] || '/tmp/btd';
open(my $fh, '<:raw', $path) or die "open $path: $!\n";
local $/;
my $b = <$fh>;
close $fh;
my $size = length($b);
printf "file=%s size=%d\n", $path, $size;

sub u32 { unpack('V', substr($b, $_[0], 4)) }
sub u64 { unpack('Q<', substr($b, $_[0], 8)) }

# ---- fat / thin ----
my $base  = 0;
my $magic = u32(0);
if ($magic == 0xcafebabe || $magic == 0xcafebabf) {
    my $cnt = unpack('N', substr($b, 4, 4));
    printf "fat slices=%d\n", $cnt;
    for my $i (0 .. $cnt - 1) {
        my $p = 8 + $i * 20;
        my ($cpu, $sub, $off, $sz) = unpack('NNNN', substr($b, $p, 16));
        printf "  slice%d cpu=0x%08x off=%d size=%d\n", $i, $cpu, $off, $sz;
        $base = $off if $cpu == 0x01000007 && $base == 0;
    }
}

my $m = u32($base);
die sprintf("bad mach magic 0x%08x at %d\n", $m, $base) unless $m == 0xfeedfacf;
my $ncmds = u32($base + 16);
my $p     = $base + 28;    # mach_header_64 共 28 字节（含 reserved），load commands 紧随其后
my ($vmaddr, $fileoff, $filesize) = (0, 0, 0);

for (1 .. $ncmds) {
    my $cmd     = u32($p);
    my $cmdsize = u32($p + 4);
    last if $cmdsize < 8;
    if ($cmd == 0x19) {    # LC_SEGMENT_64
        my $name = substr($b, $p + 8, 16);
        $name =~ s/\0.*//s;
        my $va = u64($p + 24);
        my $fo = u64($p + 40);
        my $fs = u64($p + 48);
        printf "SEG %-16s vmaddr=0x%x fileoff=%d filesize=%d\n", $name, $va, $fo, $fs;
        ($vmaddr, $fileoff, $filesize) = ($va, $fo, $fs) if $name eq '__TEXT';
    }
    $p += $cmdsize;
}
die "no __TEXT segment\n" unless $filesize;
printf "TEXT vmaddr=0x%x fileoff=%d size=%d\n", $vmaddr, $fileoff, $filesize;

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
    while (($pos = index($b, $needle, $pos)) >= 0) {
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
die "no candidate string found\n" unless @hits;

my %tgt;
for my $h (@hits) {
    next unless $h >= $fileoff && $h < $fileoff + $filesize;
    my $va = $vmaddr + ($h - $fileoff);
    printf "  hit file=%d va=0x%x\n", $h, $va;
    my $s = substr($b, $h, 128);
    $s =~ s/[^\x20-\x7e]/./g;
    print "    text: $s\n";
    $tgt{$va} = 1;
}
die "string only outside __TEXT\n" unless %tgt;

# ---- 扫 RIP-relative lea 引用 ----
# lea r64, [rip+disp32]：REX.W 前缀 48（r8~r15 用 4C），modrm mod=00 rm=101，
# reg 任意 → 05 0D 15 1D 25 2D 35 3D。
my $tend = $fileoff + $filesize;
my @refs;
while ($b =~ /([\x48\x4C])\x8D([\x05\x0D\x15\x1D\x25\x2D\x35\x3D])/g) {
    my $i = pos($b) - 3;
    next if $i < $fileoff;
    next if $i + 7 > $tend;
    my $disp = unpack('l<', substr($b, $i + 3, 4));
    my $ia   = $vmaddr + ($i - $fileoff);
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
    $start = $fileoff if $start < $fileoff;
    my $len = $i - $start + 160;
    $len = $tend - $start if $start + $len > $tend;
    my $hex = unpack('H*', substr($b, $start, $len));
    $hex =~ s/(..)/$1 /g;
    printf "  from 0x%x (ref at +%d):\n  %s\n", $vmaddr + ($start - $fileoff), $i - $start, $hex;
}

print "\ndone\n";
