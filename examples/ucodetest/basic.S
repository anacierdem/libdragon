#include <ucode.S>
.text

llv $v01, 0, %lo(label1), $zero
llv $v01, 0, %lo(label2), $zero
llv $v01, 0, %lo(label3-_gp), $gp
llv $v01, 0, -80, $0
llv $v01, 0, %lo(label_neg), $zero

#include "dmem.S"
.dmem

label1:
.word 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF
.int 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF
label2:
.int 0xDEADBEEF
.int 0xDEADBEEF
.int 0xDEADBEEF
.int 0xDEADBEEF

.skip 10, 0x33

.dmemoffset 0x800-32
label3:
.int 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF

.dmemoffset 0x800
_gp:

.dmemoffset 0x800+16
label4:
.int 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF

.dmemoffset 0x1000-32

label_neg:
.int 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF

.endmem
