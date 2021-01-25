
.macro .dmem
    .pushsection .data
    .org 0 # make sure we are also at the start of data section
    .offset 0 # Set offset into the absolute section

    .macro .relocAlign type, value1=, value2=, rest:vararg
        .ifb \rest
            .both \type \value1
        .else
            .both \type \value1, \value2, \rest
        .endif
    .endm

    .macro .both do:vararg
        \do
        .section .data
        \do
        .previous
    .endm

    .macro .insertMultipleZeros type, first, values:vararg
        .ifb \values
            \type 0x0
        .else
            \type 0x0
            .insertMultipleZeros \type \values
        .endif
    .endm

    .macro .noreloc type, value1=, value2=, rest:vararg
        .ifnc \type,.double
        .ifnc \type,.float
        .ifnc \type,.single
        .ifnc \type,.word
        .ifnc \type,.byte
        .ifnc \type,.hword
        .ifnc \type,.int
        .ifnc \type,.long
        .ifnc \type,.short
        .ifnc \type,.sleb128
        .ifnc \type,.uleb128
        .ifnc \type,.octa

        .ifnc \type,.fill
        .ifnc \type,.skip
        .ifnc \type,.space
        .ifnc \type,.align
        .ifnc \type,.balign
        .ifnc \type,.incbin
            .error "\type is an unsupported noreloc directive"
            .exitm
        .endif
        .endif
        .endif
        .endif
        .endif
        .endif
        .endif
        .endif
        .endif
        .endif
        .endif
        .endif

        .endif
        .endif
        .endif
        .endif
        .endif
        .endif

        .ifc \type,.fill
            .fill \value1, \value2, 0x0
            .section .data
            .fill \value1, \value2, \rest
            .previous
            .exitm
        .endif

        .ifc \type,.skip
            .skip \value1, 0x0
            .section .data
            .skip \value1, \value2
            .previous
            .exitm
        .endif

        .ifc \type,.incbin
            .both .incbin "\value1", \value2, \rest
            .exitm
        .endif

        .ifc \type,.align
            .relocAlign \type \value1 \value2 \rest
            .exitm
        .endif

        .ifc \type,.balign
            .relocAlign \type \value1 \value2 \rest
            .exitm
        .endif

        .insertMultipleZeros \type \value1 \value2 \rest
        .section .data

        .ifb \rest
            .ifb \value2
                \type \value1
            .else
                \type \value1, \value2
            .endif
        .else
            \type \value1, \value2, \rest
        .endif

        .previous
    .endm

    .macro .dmemoffset offset fill
        .offset \offset
        .section .data
        .org \offset , \fill
        .previous
    .endm
.endm

.macro .endmem
    .purgem .insertMultipleZeros
    .purgem .noreloc
    .purgem .dmemoffset
    .purgem .relocAlign
    .popsection
.endm

#define data error ".data directive is not usable once you include dmem.S"
#define text error ".text directive is not usable once you include dmem.S"
#define include error ".include directive is not usable once you include dmem.S, use preprocessor include instead."

#define offset error ".offset directive is not usable once you include dmem.S"
#define section error ".section directive is not usable once you include dmem.S"
#define struct error ".struct directive is not usable once you include dmem.S"

#define dc dc_not_supported
#define dcb dcb_not_supported
#define ds ds_not_supported

#define ascii noreloc .ascii
#define asciz noreloc .asciz
#define string noreloc .string
#define string8 noreloc .string8
#define string16 noreloc .string16

#define double noreloc .double
#define float noreloc .float
#define single noreloc .single
#define word noreloc .word
#define byte noreloc .byte
#define hword noreloc .hword
#define int noreloc .int
#define long noreloc .long
#define short noreloc .short
#define sleb128 noreloc .sleb128
#define uleb128 noreloc .uleb128
#define octa noreloc .octa

#define zero noreloc .skip
#define skip noreloc .skip
#define space noreloc .skip
#define fill noreloc .fill
#define align noreloc .align
#define balign noreloc .balign
#define incbin noreloc .incbin
#define org dmemoffset
#define vtable noreloc .vtable