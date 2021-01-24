#include <ucode.S>

.text

llv $v01, 0, %lo(label1)
llv $v01, 0, %lo(label2)
llv $v01, 0, %lo(neg(label_neg))

.dmem

label1:
.noreloc .word 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF
label2:
.noreloc .word 0xDEADBEEF
.noreloc .word 0xDEADBEEF
.noreloc .word 0xDEADBEEF
.noreloc .word 0xDEADBEEF
label3:
.noreloc .word 0xBEEFF00D
.noreloc .word 0xBEEFF00D
.noreloc .word 0xBEEFF00D
.noreloc .word 0xBEEFF00D

.dmemoffset 0x1000-16

label_neg:
.noreloc .word 0xDEADBEEF

.endmem


