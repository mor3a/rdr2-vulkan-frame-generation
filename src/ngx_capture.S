.text
.macro CAPTURE_WRAPPER name, target, kind
.global \name
.seh_proc \name
\name:
  subq $200, %rsp
  .seh_stackalloc 200
  .seh_endprologue
  movq %rcx, 32(%rsp)
  movq %rdx, 40(%rsp)
  movq %r8, 48(%rsp)
  movq %r9, 56(%rsp)
  movq %r10, 64(%rsp)
  movq %r11, 72(%rsp)
  movq %rax, 80(%rsp)
  movdqu %xmm0, 96(%rsp)
  movdqu %xmm1, 112(%rsp)
  movdqu %xmm2, 128(%rsp)
  movdqu %xmm3, 144(%rsp)
  movdqu %xmm4, 160(%rsp)
  movdqu %xmm5, 176(%rsp)
  movq 200(%rsp), %rdx
  movl $\kind, %r8d
  call rdr_record_appid
  movq 32(%rsp), %rcx
  movq 40(%rsp), %rdx
  movq 48(%rsp), %r8
  movq 56(%rsp), %r9
  movq 64(%rsp), %r10
  movq 72(%rsp), %r11
  movq 80(%rsp), %rax
  movdqu 96(%rsp), %xmm0
  movdqu 112(%rsp), %xmm1
  movdqu 128(%rsp), %xmm2
  movdqu 144(%rsp), %xmm3
  movdqu 160(%rsp), %xmm4
  movdqu 176(%rsp), %xmm5
  addq $200, %rsp
  jmp *\target(%rip)
.seh_endproc
.endm
CAPTURE_WRAPPER rdr_capture_init, rdr_original_init, 1
CAPTURE_WRAPPER rdr_capture_ext, rdr_original_ext, 2
CAPTURE_WRAPPER rdr_capture_ext2, rdr_original_ext2, 3
