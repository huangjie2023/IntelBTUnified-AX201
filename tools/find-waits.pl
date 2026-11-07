#!/usr/bin/perl
#
# find-waits.pl：扫描 bluetoothd 里所有「mov edi,imm32; call usleep_ms」
# 等待调用点，并自动回扫每个调用点前面的循环边界常量（B 类根治第三步）。
#
# 依据（find-callers.pl 17:58 实测）：
#   usleep_ms thunk 形态：55 48 89 e5 69 ff e8 03 00 00 5d [e9|c3]
#     = push rbp; mov rbp,rsp; imul edi,edi,1000; pop rbp; jmp/call usleep
#   UART 等待循环形态：mov edi,0x64(100ms); call usleep_ms; ... cmp r15d,0x95(149)
#     ≈ 149x100ms ≈ 15s
#   USB 路径若复用同一 thunk，其循环常量就在调用点前面不远处。
#
# 用法：
#   perl find-waits.pl /tmp/btd
#
# 无 @ 符号、无反斜杠转义，可安全从聊天窗口复制。只用 perl 核心模块。

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
}
die "bad mach magic\n" unless u32($base) == 0xfeedfacf;

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
my $tstart = $base + $fileoff;
my $tend   = $tstart + $filesize;
printf "TEXT vmaddr=0x%x abs=[%d,%d)\n", $vmaddr, $tstart, $tend;

# ---- 1) 找 usleep_ms thunk：55 48 89 e5 69 ff e8 03 00 00 5d [e9|c3] ----
my $thunkPat = "\x55\x48\x89\xE5\x69\xFF\xE8\x03\x00\x00\x5D";
my %thunks;
{
    my $pos = $tstart;
    while (($pos = index($buf, $thunkPat, $pos)) >= 0 && $pos < $tend - 12) {
        my $va = $vmaddr + ($pos - $tstart);
        my $tail = substr($buf, $pos + 11, 1);
        $thunks{$va} = ($tail eq "\xE9") ? 'jmp' : ($tail eq "\xC3") ? 'ret' : '?';
        $pos += 1;
    }
}
printf "usleep_ms thunks: %d\n", scalar(keys %thunks);
for my $va (sort { $a <=> $b } keys %thunks) {
    printf "  0x%x (tail %s)\n", $va, $thunks{$va};
}
die "no thunk found\n" unless %thunks;

# ---- 2) 回扫循环边界常量：cmp r32, imm 形态 ----
# 返回 [ (距离, 描述, 值), ... ]，最多回看 160 字节
sub scan_bounds {
    my $off  = shift;
    my $lo   = $off - 160;
    $lo = $tstart if $lo < $tstart;
    my @out;
    for (my $q = $off - 2; $q >= $lo; $q--) {
        my $d = $off - $q;
        # 41 81 F8..FF imm32   cmp r8d..r15d, imm32
        if ($d >= 7 && substr($buf, $q, 2) eq "\x41\x81") {
            my $m = ord(substr($buf, $q + 2, 1));
            if ($m >= 0xF8) {
                my $imm = u32($q + 3);
                push @out, [$d, sprintf("cmp r%d, %u (0x%x)", 8 + ($m - 0xF8), $imm, $imm)];
            }
        }
        # 81 F8..FF imm32      cmp eax..edi, imm32
        if ($d >= 6 && ord(substr($buf, $q, 1)) == 0x81) {
            my $m = ord(substr($buf, $q + 1, 1));
            if ($m >= 0xF8 && $m <= 0xFF) {
                my $imm = u32($q + 2);
                push @out, [$d, sprintf("cmp r%dd, %u (0x%x)", $m - 0xF8, $imm, $imm)];
            }
        }
        # 41 83 F8..FF imm8
        if ($d >= 4 && substr($buf, $q, 2) eq "\x41\x83") {
            my $m = ord(substr($buf, $q + 2, 1));
            if ($m >= 0xF8 && $m <= 0xFF) {
                my $imm = unpack('c', substr($buf, $q + 3, 1));
                push @out, [$d, sprintf("cmp r%d, %d", 8 + ($m - 0xF8), $imm)] if $imm > 2;
            }
        }
    }
    return @out;
}

# ---- 3) 扫全部 BF imm32 E8（mov edi,imm; call），目标为 thunk 的输出 ----
printf "\nwait sites (mov edi,imm; call usleep_ms):\n";
my $hits = 0;
for (my $off = $tstart; $off < $tend - 10; $off++) {
    next unless ord(substr($buf, $off, 1)) == 0xBF && ord(substr($buf, $off + 5, 1)) == 0xE8;
    my $imm    = u32($off + 1);
    next unless $imm >= 1 && $imm <= 10000;
    my $disp   = unpack('l<', substr($buf, $off + 6, 4));
    my $callVA = $vmaddr + ($off + 5 - $tstart);
    my $target = $callVA + 5 + $disp;
    next unless $thunks{$target};
    $hits++;
    my $siteVA = $vmaddr + ($off - $tstart);
    printf "\nsite va=0x%x  usleep_ms(%u ms)\n", $siteVA, $imm;
    my @bounds = scan_bounds($off);
    if (@bounds) {
        printf "  loop-bound candidates:\n";
        my %desc;
        for my $b (@bounds) {
            my ($d, $txt) = @$b;
            next if $desc{$txt}++;
            printf "    -%3d B: %s\n", $d, $txt;
        }
    }
    else {
        print "  (no cmp constant within 160B — maybe non-loop call)\n";
    }
}
printf "\ntotal wait sites: %d\ndone\n", $hits;
