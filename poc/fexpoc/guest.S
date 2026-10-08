// the x86-64 guest for the FexPoc proof of concept (docs/native-arm64-backend.md, section 4).
//
// linked at 0x8_0000_0000, where SelfLoader puts a PS5 main image, and loaded there by Program.cs
// as one flat blob. the same bytes run under both execution models:
//
//   fex  -- native arm64 .NET process, libsharpfex runs this through FEXCore. import slots become
//           `mov [rsp-8],rcx; mov [rsp-16],r11; syscall; ret` (sharpfex.h).
//   x64  -- today's model: an x86-64 .NET process runs this natively (under the host layer on a
//           phone, so FEX translates everything). import slots become `movabs rax, tramp; jmp rax`
//           to the x64_tramp_* below, which build SharpEmu's arg pack and call C# on the host stack.
//
// every guest function is SysV and returns 0 (or a value the host checks) on success.

  .intel_syntax noprefix
  .text
  .globl _start

  .set NSLOTS, 8
  .set STRIDE, 16
  .set SLOT_ARGS, 0
  .set SLOT_CALLBACK, 1
  .set SLOT_ADD, 2
  .set SLOT_YIELD, 3
  .set SLOT_HASH, 4
  .set SLOT_TRANSFER, 5

// --- header: magic, slot geometry, then offsets from the base in a fixed order -----------------
_start:
hdr:
  .ascii "SFXPOC01"
  .long NSLOTS, STRIDE
  .quad slots - hdr            //  0
  .quad cbret - hdr            //  1
  .quad f_test_args - hdr      //  2
  .quad f_test_regs - hdr      //  3
  .quad f_test_callback - hdr  //  4
  .quad guest_cb_plain - hdr   //  5
  .quad f_test_transfer - hdr  //  6
  .quad f_bench_hle - hdr      //  7
  .quad f_bench_hash - hdr     //  8
  .quad f_bench_cb - hdr       //  9
  .quad f_bench_compute - hdr  // 10
  .quad f_worker_yield - hdr   // 11
  .quad f_unaligned - hdr      // 12
  .quad x64_enter - hdr        // 13
  .quad x64_gateway_ctx - hdr  // 14
  .quad x64_gateway_fn - hdr   // 15
  .quad x64_host_rsp - hdr     // 16
  .quad x64_tramps - hdr       // 17
  .quad str_hello - hdr        // 18
  .quad hash_buf - hdr         // 19
  .quad code_end - hdr         // 20
  .quad blob_end - hdr         // 21
  .quad scratch - hdr          // 22

// --- import slots, patched by the loader ------------------------------------------------------
  .balign 64
slots:
  .fill NSLOTS * STRIDE, 1, 0xCC
  .set slot_args, slots + SLOT_ARGS * STRIDE
  .set slot_callback, slots + SLOT_CALLBACK * STRIDE
  .set slot_add, slots + SLOT_ADD * STRIDE
  .set slot_yield, slots + SLOT_YIELD * STRIDE
  .set slot_hash, slots + SLOT_HASH * STRIDE
  .set slot_transfer, slots + SLOT_TRANSFER * STRIDE

// where a host->guest call returns to: CALLBACKRET (fex model only).
  .balign 16
cbret:
  .byte 0x0f, 0x3e
  .fill 14, 1, 0xCC

// --- tests --------------------------------------------------------------------------------------

// six integer arguments, a stack argument, a double in xmm0 and a pointer to a guest string, which
// the host reads by dereferencing it. the host returns
//   rax  = 1+2+3+4+5+6 + strlen*1000 + (long)(xmm0*100) = 21 + 16000 + 250 = 16271
//   xmm0 = xmm0 * 2 = 5.0
// result: 0, or bit 0 = wrong rax, bit 1 = wrong xmm0.
  .balign 16
f_test_args:
  push rbx
  sub rsp, 16
  mov qword ptr [rsp], 6
  lea rdi, [rip + str_hello]
  mov esi, 1
  mov edx, 2
  mov ecx, 3
  mov r8d, 4
  mov r9d, 5
  movsd xmm0, qword ptr [rip + c_2_5]
  call slot_args
  xor ebx, ebx
  cmp rax, 16271
  je 1f
  or ebx, 1
1:
  ucomisd xmm0, qword ptr [rip + c_5_0]
  jne 2f
  jp 2f
  jmp 3f
2:
  or ebx, 2
3:
  mov eax, ebx
  add rsp, 16
  pop rbx
  ret

// an import must hand back every register the guest owns except rax/rdx. checked: the six
// callee-saved ones, and rcx, r10, r11, which `syscall` (rcx, r11) and today's trampoline (all
// three) would otherwise clobber. result: 0, or one bit per register that came back wrong.
  .balign 16
f_test_regs:
  push rbx
  push rbp
  push r12
  push r13
  push r14
  push r15
  sub rsp, 8
  movabs rbx, 0x1111111111111111
  movabs rbp, 0x2222222222222222
  movabs r12, 0x3333333333333333
  movabs r13, 0x4444444444444444
  movabs r14, 0x5555555555555555
  movabs r15, 0x6666666666666666
  movabs rcx, 0x7777777777777777
  movabs r11, 0x8888888888888888
  movabs r10, 0x9999999999999999
  mov edi, 40
  mov esi, 2
  call slot_add
  xor edx, edx
  cmp rax, 42
  je 1f
  or edx, 1
1:
  movabs r8, 0x1111111111111111
  cmp rbx, r8
  je 1f
  or edx, 2
1:
  movabs r8, 0x2222222222222222
  cmp rbp, r8
  je 1f
  or edx, 4
1:
  movabs r8, 0x3333333333333333
  cmp r12, r8
  je 1f
  or edx, 8
1:
  movabs r8, 0x4444444444444444
  cmp r13, r8
  je 1f
  or edx, 16
1:
  movabs r8, 0x5555555555555555
  cmp r14, r8
  je 1f
  or edx, 32
1:
  movabs r8, 0x6666666666666666
  cmp r15, r8
  je 1f
  or edx, 64
1:
  movabs r8, 0x7777777777777777
  cmp rcx, r8
  je 1f
  or edx, 128
1:
  movabs r8, 0x8888888888888888
  cmp r11, r8
  je 1f
  or edx, 256
1:
  movabs r8, 0x9999999999999999
  cmp r10, r8
  je 1f
  or edx, 512
1:
  mov eax, edx
  add rsp, 8
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbp
  pop rbx
  ret

// guest -> HLE -> guest -> HLE -> guest: the host calls guest_cb_nested(x, 7), which itself makes
// an import call, and adds 1. returns x*7 + 2.
  .balign 16
f_test_callback:
  sub rsp, 8
  mov rsi, rdi
  lea rdi, [rip + guest_cb_nested]
  call slot_callback
  add rsp, 8
  ret

  .balign 16
guest_cb_nested:
  push rbx
  mov rbx, rdi
  imul rbx, rsi
  mov rdi, rbx
  mov esi, 1
  call slot_add
  pop rbx
  ret

  .balign 16
guest_cb_plain:
  mov rax, rdi
  imul rax, rsi
  add rax, 1
  ret

// a context transfer (longjmp, fiber switch): the host rewrites rip and drops the return address,
// so the call never returns to its call site. returns 0x77, or 0xBAD if the call came back.
  .balign 16
f_test_transfer:
  sub rsp, 8
  lea rdi, [rip + 1f]
  call slot_transfer
  add rsp, 8
  mov eax, 0xBAD
  ret
1:
  add rsp, 8
  mov eax, 0x77
  ret

// --- benches ------------------------------------------------------------------------------------

// n import round trips: returns sum(i + 1) = n(n+1)/2.
  .balign 16
f_bench_hle:
  push rbx
  push r12
  push r13
  mov rbx, rdi
  xor r12d, r12d
  xor r13d, r13d
  test rbx, rbx
  jz 2f
1:
  mov rdi, r12
  mov esi, 1
  call slot_add
  add r13, rax
  inc r12
  cmp r12, rbx
  jb 1b
2:
  mov rax, r13
  pop r13
  pop r12
  pop rbx
  ret

// n imports that do real work on guest memory (FNV-1a over len bytes): returns n * hash.
  .balign 16
f_bench_hash:
  push rbx
  push r12
  push r13
  push r14
  push r15
  mov rbx, rdi
  mov r14, rsi
  mov r15, rdx
  xor r12d, r12d
  xor r13d, r13d
  test rbx, rbx
  jz 2f
1:
  mov rdi, r14
  mov rsi, r15
  call slot_hash
  add r13, rax
  inc r12
  cmp r12, rbx
  jb 1b
2:
  mov rax, r13
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

// n guest -> HLE -> guest callback round trips: returns sum(i*7 + 2).
  .balign 16
f_bench_cb:
  push rbx
  push r12
  push r13
  mov rbx, rdi
  xor r12d, r12d
  xor r13d, r13d
  test rbx, rbx
  jz 2f
1:
  lea rdi, [rip + guest_cb_plain]
  mov rsi, r12
  call slot_callback
  add r13, rax
  inc r12
  cmp r12, rbx
  jb 1b
2:
  mov rax, r13
  pop r13
  pop r12
  pop rbx
  ret

// pure guest code, no imports: xorshift64 plus a load and a store per step into a 256-byte table.
// identical work in both models (FEX translates it either way), so this is the control.
  .balign 16
f_bench_compute:
  movabs rax, 0x9E3779B97F4A7C15
  mov rcx, rdi
  lea rdx, [rip + scratch]
  test rcx, rcx
  jz 2f
1:
  mov r8, rax
  shl r8, 13
  xor rax, r8
  mov r8, rax
  shr r8, 7
  xor rax, r8
  mov r8, rax
  shl r8, 17
  xor rax, r8
  mov r9, rax
  and r9d, 0xF8
  add rax, qword ptr [rdx + r9]
  mov qword ptr [rdx + r9], rax
  dec rcx
  jnz 1b
2:
  ret

// a guest thread that blocks four times in an import and is resumed each time, then halts with
// rax = 0x600D. run with sfx_run (fex model only).
  .balign 16
f_worker_yield:
  push rbx
  xor ebx, ebx
1:
  mov edi, ebx
  call slot_yield
  inc ebx
  cmp ebx, 4
  jb 1b
  pop rbx
  mov eax, 0x600D
  hlt

// a locked add on a misaligned address that straddles a 16-byte boundary: arm64 faults on it and
// FEX backpatches the block. returns the new value.
  .balign 16
f_unaligned:
  lock add qword ptr [rdi], 1
  mov rax, qword ptr [rdi]
  ret

// --- today's model: entry and import trampolines (x64 only) -------------------------------------

// x64_enter(target, guest_rsp, a0, a1, a2) -> rax: switch to the guest stack and call target,
// keeping the host rsp in a slot the trampolines switch back to (SharpEmu keeps it in TLS).
  .balign 16
x64_enter:
  push rbx
  push rbp
  push r12
  push r13
  push r14
  push r15
  push qword ptr [rip + x64_host_rsp]
  sub rsp, 8
  mov qword ptr [rip + x64_host_rsp], rsp
  mov rax, rdi
  mov rsp, rsi
  mov rdi, rdx
  mov rsi, rcx
  mov rdx, r8
  call rax
  mov rsp, qword ptr [rip + x64_host_rsp]
  add rsp, 8
  pop qword ptr [rip + x64_host_rsp]
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbp
  pop rbx
  ret

// one per import, as CreateImportHandlerTrampoline(idx) emits: the arg pack is the 12 argument
// and callee-saved GPRs pushed onto the guest stack, rdi first, return address at +0x60.
.macro TRAMP idx
  .balign 16
x64_tramp_\idx:
  push r15
  push r14
  push r13
  push r12
  push rbp
  push rbx
  push r9
  push r8
  push rcx
  push rdx
  push rsi
  push rdi
  mov eax, \idx
  jmp x64_tramp_common
.endm

  TRAMP 0
  TRAMP 1
  TRAMP 2
  TRAMP 3
  TRAMP 4
  TRAMP 5
  TRAMP 6
  TRAMP 7

// below the pack: xmm0-7 at +0x00, r10 +0x80, r11 +0x88, mxcsr +0x90, x87 cw +0x94, rax in/out
// +0x98; the pack at +0xA0; the return address at +0x100. the gateway gets that frame pointer.
  .balign 16
x64_tramp_common:
  sub rsp, 0xA0
  movdqu xmmword ptr [rsp + 0x00], xmm0
  movdqu xmmword ptr [rsp + 0x10], xmm1
  movdqu xmmword ptr [rsp + 0x20], xmm2
  movdqu xmmword ptr [rsp + 0x30], xmm3
  movdqu xmmword ptr [rsp + 0x40], xmm4
  movdqu xmmword ptr [rsp + 0x50], xmm5
  movdqu xmmword ptr [rsp + 0x60], xmm6
  movdqu xmmword ptr [rsp + 0x70], xmm7
  mov qword ptr [rsp + 0x80], r10
  mov qword ptr [rsp + 0x88], r11
  stmxcsr dword ptr [rsp + 0x90]
  fnstcw word ptr [rsp + 0x94]
  mov qword ptr [rsp + 0x98], 0
  mov rbx, rsp
  mov rsp, qword ptr [rip + x64_host_rsp]
  and rsp, -16
  mov rdi, qword ptr [rip + x64_gateway_ctx]
  mov esi, eax
  mov rdx, rbx
  call qword ptr [rip + x64_gateway_fn]
  mov rsp, rbx
  movdqu xmm0, xmmword ptr [rsp + 0x00]
  movdqu xmm1, xmmword ptr [rsp + 0x10]
  movdqu xmm2, xmmword ptr [rsp + 0x20]
  movdqu xmm3, xmmword ptr [rsp + 0x30]
  movdqu xmm4, xmmword ptr [rsp + 0x40]
  movdqu xmm5, xmmword ptr [rsp + 0x50]
  movdqu xmm6, xmmword ptr [rsp + 0x60]
  movdqu xmm7, xmmword ptr [rsp + 0x70]
  mov r10, qword ptr [rsp + 0x80]
  mov r11, qword ptr [rsp + 0x88]
  ldmxcsr dword ptr [rsp + 0x90]
  fldcw word ptr [rsp + 0x94]
  mov rax, qword ptr [rsp + 0x98]
  add rsp, 0xA0
  pop rdi
  pop rsi
  pop rdx
  pop rcx
  pop r8
  pop r9
  pop rbx
  pop rbp
  pop r12
  pop r13
  pop r14
  pop r15
  ret

  .balign 16
c_2_5:
  .double 2.5
c_5_0:
  .double 5.0

code_end:

// --- data: its own pages, never declared executable ---------------------------------------------
  .balign 4096
x64_gateway_ctx:
  .quad 0
x64_gateway_fn:
  .quad 0
x64_host_rsp:
  .quad 0
  .balign 16
x64_tramps:
  .quad x64_tramp_0, x64_tramp_1, x64_tramp_2, x64_tramp_3
  .quad x64_tramp_4, x64_tramp_5, x64_tramp_6, x64_tramp_7
str_hello:
  .asciz "hello from guest"
  .balign 64
hash_buf:
  .rept 64
  .byte 0x00, 0x11, 0x22, 0x33
  .endr
  .balign 64
scratch:
  .fill 512, 1, 0
  .balign 4096
blob_end:
