#include <ucode.S>

.text

llv $v01, 0, %lo(label1)
llv $v01, 0, %lo(label2)
llv $v01, 0, %lo(neg(label_neg))

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
label3:
.word 0xBEEFF00D
.word 0xBEEFF00D
.word 0xBEEFF00D
.word 0xBEEFF00D

.dmemoffset 0x1000-16

label_neg:
.word 0xDEADBEEF
#undef word
.endmem
