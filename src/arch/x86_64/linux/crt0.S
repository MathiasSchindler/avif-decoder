.global _start
#ifndef NEWOS_DISABLE_STACK_GUARD_INIT
.extern __newos_stack_guard_init
#endif
.text
_start:
    mov (%rsp), %rax
    lea 16(%rsp,%rax,8), %rcx
.Lfind_auxv:
    cmpq $0, (%rcx)
    je .Lauxv
    add $8, %rcx
    jmp .Lfind_auxv
.Lauxv:
    add $8, %rcx
    xor %r8d, %r8d
    xor %r9d, %r9d
.Lread_auxv:
    mov (%rcx), %rax
    test %rax, %rax
    je .Lfind_base
    cmp $3, %rax
    jne .Lcheck_phnum
    mov 8(%rcx), %r8
    jmp .Lnext_auxv
.Lcheck_phnum:
    cmp $5, %rax
    jne .Lnext_auxv
    mov 8(%rcx), %r9
.Lnext_auxv:
    add $16, %rcx
    jmp .Lread_auxv
.Lfind_base:
    test %r8, %r8
    je .Lrelocation_failure
    mov %r8, %rbx
    and $-4096, %rbx
    cmpl $0x464c457f, (%rbx)
    jne .Lrelocation_failure
    lea _DYNAMIC(%rip), %r10
    xor %r11d, %r11d
    xor %r12d, %r12d
.Lread_dynamic:
    mov (%r10), %rax
    test %rax, %rax
    je .Lapply_relocations
    cmp $7, %rax
    jne .Lcheck_relasz
    mov 8(%r10), %r11
    add %rbx, %r11
    jmp .Lnext_dynamic
.Lcheck_relasz:
    cmp $8, %rax
    jne .Lnext_dynamic
    mov 8(%r10), %r12
.Lnext_dynamic:
    add $16, %r10
    jmp .Lread_dynamic
.Lapply_relocations:
    test %r12, %r12
    je .Lrelocations_done
    test %r11, %r11
    je .Lrelocation_failure
.Lrelocation_loop:
    cmpq $8, 8(%r11)
    jne .Lrelocation_failure
    mov (%r11), %rax
    add %rbx, %rax
    mov 16(%r11), %rdx
    add %rbx, %rdx
    mov %rdx, (%rax)
    add $24, %r11
    sub $24, %r12
    jne .Lrelocation_loop
.Lrelocations_done:
#ifndef NEWOS_DISABLE_STACK_GUARD_INIT
    mov (%rsp), %rdi
    lea 8(%rsp), %rsi
    call __newos_stack_guard_init
#endif
    mov (%rsp), %rdi
    lea 8(%rsp), %rsi
    call main
    mov %eax, %edi
    mov $231, %eax
    syscall
.Lrelocation_failure:
    mov $127, %edi
    mov $231, %eax
    syscall