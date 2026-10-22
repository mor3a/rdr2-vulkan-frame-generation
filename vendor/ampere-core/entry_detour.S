.intel_syntax noprefix
.text
.globl MfgUnlockForwardDispatch
.seh_proc MfgUnlockForwardDispatch
MfgUnlockForwardDispatch:
    sub rsp, 0x88
    .seh_stackalloc 0x88
    .seh_endprologue
    mov QWORD PTR [rsp + 0x40], rcx
    mov QWORD PTR [rsp + 0x48], rdx
    mov QWORD PTR [rsp + 0x50], r8
    mov QWORD PTR [rsp + 0x58], r9
    mov rax, QWORD PTR [rsp + 0x0B0]
    mov QWORD PTR [rsp + 0x20], rax
    mov rax, QWORD PTR [rsp + 0x0B8]
    mov QWORD PTR [rsp + 0x28], rax
    mov QWORD PTR [rsp + 0x30], r10
    mov rax, QWORD PTR [rsp + 0x88]
    mov QWORD PTR [rsp + 0x38], rax
    call MfgUnlockDispatchForwarding
    mov r11, rax
    mov rcx, QWORD PTR [rsp + 0x40]
    mov rdx, QWORD PTR [rsp + 0x48]
    mov r8, QWORD PTR [rsp + 0x50]
    mov r9, QWORD PTR [rsp + 0x58]
    add rsp, 0x88
    jmp r11

.seh_endproc
