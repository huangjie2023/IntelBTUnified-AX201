#!/usr/bin/perl
#
# find-callers.pl：给定关键 VA，反扫 __TEXT 里所有直接 call/jmp 引用，
# dump 调用点上下文（B 类根治第二步）。
#
# 目标（来自 find-timeout.pl 的 17:43 实测输出）：
#   1. 0x10014bdc9 —— 引用 "Failed to initialize transport %s" 的报错函数内部，
#      先自动向上找函数起点（55 48 89 e5 prologue），再找它的全部调用者；
#   2. 0x1000F23EE —— UART 等待循环的轮询助手（mov edi,100; call X; cmp 0x95），
#      USB 路径如果复用它，调用点旁边就是超时常量。
#
# 用法：
#   perl find-callers.pl /tmp/btd
#
# 不含 @ 符号与反斜杠转义，可安全从聊天窗口复制。只用 perl 核心模块。

use strict;
use warnings;

my $path = shift || '/tmp/btd';
open(my $fh, '<:raw', $path) or die "open $path: $!\n";
local $/;
my $buf = <$fh>;
close $fh;
printf "file=%s size=%d\n", $path, length($buf);

sub u32 { unpack('V', substr($buf, $_[0], 4)) }
sub u64 { unpack('Q<', substr($buf, $_[0], 8)) }

my $base  = 0;
my $magic = u32(0);
if ($magic == 0xbebafeca || $magic == 0xbfbafeca) {
    my $cnt = unpack('N', substr($buf, 4, 4));
    for my $i (0 .. $cnt - 1) {
        my $p = 8 + $i * 20;
        my ($cpu, $sub, $off, $sz) = unpack('NNNN', substr($buf, $p, 16));
        $base = $off if $cpu == 0x01000007 && $base == 0;
    }
    printf "x86_64 slice at %d\n", $base;
}
die "bad mach magic\n" unless u32($base) == 0xfeedfacf;

# Tahoe mach 头多 4 字节：+28 为 0、+32 才是第一条命令
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
    if ($cmd == 0x19) {
        my $name = substr($buf, $p + 8, 16);
        $name =~ s/\0.*//s;
        ($vmaddr, $fileoff, $filesize) = (u64($p + 24), u64($p + 40), u64($p + 48))
            if $name eq '__TEXT';
    }
    $p += $cmdsize;
}
die "no __TEXT\n" unless $filesize;
# 目标 VA 可从命令行传（默认：报错函数引用点 + 轮询助手）
my $tstart = $base + $fileoff;
my $tend   = $tstart + $filesize;
printf "TEXT vmaddr=0x%x abs=[%d,%d)\n", $vmaddr, $tstart, $tend;

sub va2off { my $va = shift; return $tstart + ($va - $vmaddr) }

# 向上找函数起点：最近的 55 48 89 e5（push rbp; mov rbp,rsp）
sub find_func_start {
    my $va  = shift;
    my $off = va2off($va);
    my $lo  = $off > 16384 ? $off - 16384 : $tstart;
    for (my $q = $off - 4; $q >= $lo; $q--) {
        next unless substr($buf, $q, 4) eq "\x55\x48\x89\xE5";
        return $vmaddr + ($q - $tstart);
    }
    return 0;
}

# 扫全部 E8/E9 rel32，返回引用了目标 VA 的 [指令va, 类型]
sub find_calls {
    my $target = shift;
    my @out;
    my $pos = $tstart;
    while (($pos = index($buf, "\xE8", $pos)) >= 0 && $pos < $tend - 5) {
        my $disp = unpack('l<', substr($buf, $pos + 1, 4));
        my $ia   = $vmaddr + ($pos - $tstart);
        push @out, [$ia, 'call'] if $ia + 5 + $disp == $target;
        $pos += 1;
    }
    $pos = $tstart;
    while (($pos = index($buf, "\xE9", $pos)) >= 0 && $pos < $tend - 5) {
        my $disp = unpack('l<', substr($buf, $pos + 1, 4));
        my $ia   = $vmaddr + ($pos - $tstart);
        push @out, [$ia, 'jmp'] if $ia + 5 + $disp == $target;
        $pos += 1;
    }
    return @out;
}

sub dump_ctx {
    my ($va, $before, $after) = @_;
    my $off   = va2off($va);
    my $start = $off - $before;
    $start = $tstart if $start < $tstart;
    my $len = ($off - $start) + $after;
    $len = $tend - $start if $start + $len > $tend;
    my $hex = unpack('H*', substr($buf, $start, $len));
    $hex =~ s/(..)/$1 /g;
    printf "  from 0x%x (ref at +%d):\n  %s\n", $vmaddr + ($start - $tstart), $off - $start, $hex;
}

# ---- 主流程：对每个目标，找函数起点 + 全部直接调用者 ----
my @targets = @ARGV ? (map { hex($_) } @ARGV) : (0x10014bdc9, 0x1000F23EE);
for my $t (@targets) {
    printf "\n== target 0x%x ==\n", $t;
    my $fs = find_func_start($t);
    printf "func start: %s (ref +%d)\n",
        $fs ? sprintf("0x%x", $fs) : '(not found within 16KB)',
        $fs ? ($t - $fs) : 0;
    if ($fs) {
        print "  func prologue region:\n";
        dump_ctx($fs + 16, 16, 128);
    }
    my %seenv;
    for my $tv ($fs, $t) {
        next unless $tv;
        next if $seenv{$tv}++;
        for my $r (find_calls($tv)) {
            my ($ia, $kind) = @$r;
            printf "\n  %s -> 0x%x at va=0x%x\n", $kind, $tv, $ia;
            # 前面留 112 字节：等待循环的 cmp/常量通常在 call 之前不远
            dump_ctx($ia, 112, 48);
        }
    }
}

print "\ndone\n";
