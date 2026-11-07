#!/usr/bin/perl
#
# 定位 bluetoothd 里 "port failed to appear ... seconds" 那句日志的引用点。
#
# 背景：睡眠唤醒后 bluetoothd 等 /dev/cu.BLTH 15 秒，超时就 os_crash（故障 B）。
# 串口真正就绪要 20~26 秒，所以每次唤醒必崩。把那个 15 秒放宽到 90 秒即可。
#
# 用法：
#   cp /usr/sbin/bluetoothd /tmp/btd
#   perl find-uart-timeout.pl /tmp/btd
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
my $p     = $base + 32;
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
# 日志原文是 "port failed to appear after 15 seconds"，但格式串可能拆成两段，
# 所以按具体到宽泛依次试。
my @needles = ('port failed to appear', 'failed to appear after', '/dev/cu.BLTH');
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
    last if $n;
}
die "no candidate string found\n" unless @hits;

my %tgt;
for my $h (@hits) {
    next unless $h >= $fileoff && $h < $fileoff + $filesize;
    my $va = $vmaddr + ($h - $fileoff);
    printf "  hit file=%d va=0x%x\n", $h, $va;
    my $s = substr($b, $h, 96);
    $s =~ s/[^\x20-\x7e]/./g;
    print "    text: $s\n";
    $tgt{$va} = 1;
}
die "string only outside __TEXT\n" unless %tgt;
# ---- 扫 RIP-relative lea 引用 ----
# lea r64, [rip+disp32] = 48 8D /r，modrm 的 mod=00 rm=101，reg 任意
my $tend = $fileoff + $filesize;
my @refs;
while ($b =~ /\x48\x8D([\x05\x0D\x15\x1D\x25\x2D\x35\x3D])/g) {
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
    my $start = $i - 48;
    $start = $fileoff if $start < $fileoff;
    my $hex = unpack('H*', substr($b, $start, 144));
    $hex =~ s/(..)/$1 /g;
    printf "  from 0x%x:\n  %s\n", $vmaddr + ($start - $fileoff), $hex;
}

print "\ndone\n";
