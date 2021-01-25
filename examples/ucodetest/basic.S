#include <ucode.S>
.text

llv $v01, 0, %lo(label1), $zero
llv $v01, 0, %lo(label2), $zero
llv $v01, 0, %lo(label3-_gp), $gp
llv $v01, 0, -80, $0
llv $v01, 0, %lo(label_neg), $zero

lw $1, %lo(label2)($0)

.dmem
#define word noreloc .word
label1:
.word 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF
.word 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF
label2:
.word 0xDEADBEEF
.word 0xDEADBEEF
.word 0xDEADBEEF
.word 0xDEADBEEF

.dmemoffset 0x800-32
label3:
.word 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF

.dmemoffset 0x800
_gp:

.dmemoffset 0x800+16
label4:
.word 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF

.dmemoffset 0x1000-32

label_neg:
.word 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF
#undef word
.endmem
