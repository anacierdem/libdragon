#include <ucode.S>

.text

llv $v01, 0, %lo(label1)
llv $v01, 0, %lo(label2)
llv $v01, 0, %lo(neg(label3))

.dmem

label1:
.noreloc .word 0xDEADBEEF
.noreloc .word 0xDEADBEEF
.noreloc .word 0xDEADBEEF
.noreloc .word 0xDEADBEEF
label2:
.noreloc .word 0xDEADBEEF
.noreloc .word 0xDEADBEEF
.noreloc .word 0xDEADBEEF
.noreloc .word 0xDEADBEEF

.dmemoffset 0x1000-16

label3:
.noreloc .word 0xDEADBEEF

.endmem


