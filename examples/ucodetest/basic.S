#include <ucode.S>

.text

llv $v01, 0, label1
llv $v01, 0, label2
llv $v01, 0, 4, $0
llv $v01, 0, label_neg

lw $1, %lo(label2)($0)

.dmem
#define word noreloc .word
label1:
.word 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF
label2:
.word 0xDEADBEEF
.word 0xDEADBEEF
.word 0xDEADBEEF
.word 0xDEADBEEF

.dmemoffset 0x1000-16

label_neg:
.word 0xDEADBEEF
#undef word
.endmem
