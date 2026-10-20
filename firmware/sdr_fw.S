/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 unstablerobotgf */
/* Portions Copyright (c) 2026 Luigi C. (BirriaSDR) */
/* MT7922 SDR program: runs on the WF MCU (NDS32) in place of the vendor firmware.
 * Downloaded to ILM 0x0091b000 by the unpatched ROM; talks to the host through a SYSRAM mailbox.
 *
 * Commands (host writes MB_CMD, waits for MB_DONE to increment):
 *   1 OPS     apply up to 20 {addr, clear, set} read-modify-writes; clear == set == 0 reads only
 *   2 STREAM  run ICAP circularly and DMA 8 KiB of samples per MCU event until 'STOP'
 */
.equ MAILBOX, 0x00407c00      /* SYSRAM; host BAR0 + 0x87c00 */
.equ BOOT_MAGIC, 0x41495242
.equ RESPONSE_XOR, 0xb1771a5d
.equ PHY_POWER_ON, 0x00836eae /* ROM: WF PHY power-on, no arguments */
.equ TIMER, 0x89020068        /* free-running 1 MHz counter */
.equ ICAP_GRANT, 0x88009004   /* [27:24] = 0xf hands SYSRAM's bus to ICAP */
.equ ICAP_CR0, 0x80021090
.equ ICAP_CR9, 0x800210b4     /* producer cursor, ring units (4 B) */
.equ RING_MCU, 0xe0200000     /* ICAP ring, 256 KiB, MCU view */
.equ ICAP_HOOK0, 0x00821b4c   /* ROM function pointers the vendor calls around a grant change */
.equ ICAP_HOOK1, 0x00821b50
.equ ICAP_BUSY, 0x00821b7c    /* ROM ICAP status function: 0 = idle */
.equ DMA_ERR, 0x54000110      /* MCU DMA0 (device side of WFDMA PCIE0) */
.equ DMA_INT_MASK, 0x54000204
.equ DMA_GLO_CFG, 0x54000208
.equ DMA_RST_IDX, 0x5400020c
.equ DMA_TX0, 0x54000300      /* +0 base, +4 count, +8 CPU index, +12 DMA index */
.equ STOPREG, 0x54000120      /* MT_WFDMA_DUMMY_CR: host-writable, readable under the grant */
.equ STOPMAGIC, 0x53544f50    /* 'STOP' */
.equ DRING, 0x02042000        /* TX0 descriptors, data RAM (SYSRAM is gone under the grant) */
.equ DHDR, 0x02042100         /* one 64-byte event header per descriptor */
.equ SVARS, 0x02043000        /* stream variables, see below */
.equ DCOUNT, 4
.equ PKT, 0x2000              /* 8 KiB payload = 2048 ring units */

/* Mailbox offsets (driver/sdr_internal.h keeps the same names) */
.equ MB_MAGIC, 0x00
.equ MB_NONCE, 0x04
.equ MB_RESPONSE, 0x08
.equ MB_STATE, 0x1c           /* 0x10 powering on, 1 ready */
.equ MB_LOOPS, 0x34
.equ MB_CMD, 0x3c
.equ MB_POWER_ON, 0x4c
.equ MB_DONE, 0x54
.equ MB_CANARY, 0xbc          /* host-only; the program never touches it */
.equ MB_OPS, 0x100            /* 20 B each: addr, clear, set, after, before; addr 0 ends */
.equ MB_PHASE, 0x2a0          /* op index being applied, 0xfd while streaming */
.equ MB_HOOKS, 0x2b8
.equ MB_LIMIT, 0x320
.equ MB_GUARD, 0x324
.equ MB_RESULT, 0x340

/* SVARS: +0 produced units, +4 last CR9, +8 consumed units, +12 last poll time, +16 max poll
 * gap us, +20 max backlog, +24 gaps, +28 limit, +32 guard us, +36 nonce, +40 end state,
 * +44 epoch seq, +48 start time, +52 release poll start, +56 grant held, +60 saved seq, +64 hooks */

    .section .text.entry,"ax"
    .global _start
    .type _start, @function
    .macro load32 reg,value
    sethi \reg, (\value >> 12)
    ori \reg, \reg, (\value & 0xfff)
    .endm

    /* Vendor grant wrapper (WM 0x96c5b0): *(0x821b4c)(en), *(0x821b50)(en). Clobbers r0-r5. */
    .macro vendor_hooks en
    lwi $r0, [$r8 + 64]
    beqz $r0, .Lskip\@
    load32 $r2, ICAP_HOOK0
    lwi $r2, [$r2]
    movi $r0, \en
    jral5 $r2
    load32 $r2, ICAP_HOOK1
    lwi $r2, [$r2]
    movi $r0, \en
    jral5 $r2
.Lskip\@:
    .endm

    /* Credit CR9 progress (mod 65536) to produced units, track the poll gap and the backlog
     * against consumed units; jump to stream_gap if either would let ICAP lap the DMA. r0, r1, r3. */
    .macro observe_source
    lwi $r0, [$r10]
    lwi $r1, [$r8 + 12]
    swi $r0, [$r8 + 12]
    sub $r1, $r0, $r1
    lwi $r3, [$r8 + 16]
    slt $r3, $r3, $r1
    beqz $r3, .Lo1\@
    swi $r1, [$r8 + 16]
.Lo1\@:
    lwi $r3, [$r8 + 32]
    slt $r3, $r1, $r3
    beqz $r3, stream_gap
    load32 $r0, ICAP_CR9
    lwi $r0, [$r0]
    zeh33 $r0, $r0
    lwi $r1, [$r8 + 4]
    swi $r0, [$r8 + 4]
    sub $r1, $r0, $r1
    zeh33 $r1, $r1
    lwi $r0, [$r8 + 0]
    add $r0, $r0, $r1
    swi $r0, [$r8 + 0]
    lwi $r1, [$r8 + 8]
    sub $r0, $r0, $r1
    lwi $r3, [$r8 + 20]
    slt $r3, $r3, $r0
    beqz $r3, .Lo2\@
    swi $r0, [$r8 + 20]
.Lo2\@:
    movi $r3, 61440           /* 60 Ki units: 4 Ki short of the 64 Ki-unit ring */
    slt $r3, $r0, $r3
    beqz $r3, stream_gap
    .endm

_start:
    load32 $fp, MAILBOX
    lwi $r0, [$fp + MB_NONCE]
    load32 $r1, RESPONSE_XOR
    xor $r0, $r0, $r1
    swi $r0, [$fp + MB_RESPONSE]
    movi $r0, 0x10
    swi $r0, [$fp + MB_STATE]
    dsb
    load32 $r2, PHY_POWER_ON
    jral5 $r2
    swi $r0, [$fp + MB_POWER_ON]
    movi $r0, 1
    swi $r0, [$fp + MB_STATE]
    dsb
    load32 $r0, BOOT_MAGIC
    swi $r0, [$fp + MB_MAGIC]
    dsb
    movi $r3, 0
wait_cmd:
    addi $r3, $r3, 1
    swi $r3, [$fp + MB_LOOPS]
    lwi $r0, [$fp + MB_CMD]
    beqz $r0, wait_cmd
    movi $r1, 0
    swi $r1, [$fp + MB_CMD]
    movi $r1, 1
    beq $r0, $r1, ops
    movi $r1, 2
    beq $r0, $r1, stream
    j wait_cmd

command_done:
    lwi $r0, [$fp + MB_DONE]
    addi $r0, $r0, 1
    dsb
    swi $r0, [$fp + MB_DONE]
    j wait_cmd

    /* Command 1: each op is announced in MB_PHASE first, so a bus hang names its address. */
ops:
    addi $r1, $fp, MB_OPS
    movi $r4, 0
1:
    lwi $r2, [$r1 + 0]
    beqz $r2, command_done
    swi $r4, [$fp + MB_PHASE]
    dsb
    lwi $r3, [$r2]
    swi $r3, [$r1 + 16]       /* before */
    lwi $r0, [$r1 + 4]
    lwi $r5, [$r1 + 8]
    or $r10, $r0, $r5
    beqz $r10, 2f             /* pure read */
    bitc $r3, $r3, $r0
    or $r3, $r3, $r5
    swi $r3, [$r2]
    dsb
2:
    lwi $r3, [$r2]
    swi $r3, [$r1 + 12]       /* after */
    addi $r1, $r1, 20
    addi $r4, $r4, 1
    j 1b

    /* Command 2: one grant for the whole stream. ICAP runs circularly; one 8 KiB descriptor at a
     * time DMAs straight from the ring to the host as an 0xed/0xf5 event (header +8 gaps,
     * +12 epoch, +36 seq, +40 nonce). Ends on 'STOP' in STOPREG or at the packet limit. */
stream:
    load32 $r8, SVARS
    load32 $r6, DMA_TX0
    load32 $r9, DRING
    load32 $r10, TIMER
    lwi $r0, [$fp + MB_LIMIT]
    swi $r0, [$r8 + 28]
    lwi $r0, [$fp + MB_GUARD]
    swi $r0, [$r8 + 32]
    lwi $r0, [$fp + MB_NONCE]
    swi $r0, [$r8 + 36]
    lwi $r0, [$fp + MB_HOOKS]
    swi $r0, [$r8 + 64]
    movi $r0, 0
    swi $r0, [$r8 + 16]
    swi $r0, [$r8 + 20]
    swi $r0, [$r8 + 24]
    swi $r0, [$r8 + 40]
    swi $r0, [$r8 + 56]
    movi $r4, 0
    movi $r5, 0
    movi $r0, 0xfd
    swi $r0, [$fp + MB_PHASE]
    dsb
    /* Take TX0 only if the engine is idle (GLO_CFG busy bits 1, 3); TX on, RX off, no IRQs. */
    lwi $r0, [$r6 + (DMA_GLO_CFG - DMA_TX0)]
    andi $r1, $r0, 10
    beqz $r1, 40f
    movi $r0, 6
    swi $r0, [$r8 + 40]
    j stream_publish
40:
    bitci $r0, $r0, 5
    bclr $r0, $r0, 30
    swi $r0, [$r6 + (DMA_GLO_CFG - DMA_TX0)]
    movi $r0, 0
    swi $r0, [$r6 + (DMA_INT_MASK - DMA_TX0)]
    addi $r1, $r9, 0
    movi $r2, (DCOUNT * 4)
41:
    swi.bi $r0, [$r1], 4
    addi $r2, $r2, -1
    bnez $r2, 41b
    addi $r1, $r9, (DHDR - DRING)
    movi $r2, (DCOUNT * 16)
42:
    swi.bi $r0, [$r1], 4
    addi $r2, $r2, -1
    bnez $r2, 42b
    /* Event headers: connac2 MCU RXD fields the host's MCU event path accepts. */
    addi $r1, $r9, (DHDR - DRING)
    movi $r2, DCOUNT
43:
    load32 $r0, (0x3800002c + PKT)
    swi $r0, [$r1 + 0]
    load32 $r0, (0x38000014 + PKT)
    swi $r0, [$r1 + 24]
    movi $r0, 0xed
    swi $r0, [$r1 + 28]
    movi $r0, 0xf5
    swi $r0, [$r1 + 32]
    lwi $r0, [$r8 + 36]
    swi $r0, [$r1 + 40]
    addi $r1, $r1, 64
    addi $r2, $r2, -1
    bnez $r2, 43b
    dsb
    swi $r9, [$r6 + 0]
    movi $r0, DCOUNT
    swi $r0, [$r6 + 4]
    movi $r0, 1
    swi $r0, [$r6 + (DMA_RST_IDX - DMA_TX0)]
    movi $r0, 0
    swi $r0, [$r6 + 8]
    lwi $r0, [$r6 + (DMA_GLO_CFG - DMA_TX0)]
    ori $r0, $r0, 1
    swi $r0, [$r6 + (DMA_GLO_CFG - DMA_TX0)]
    dsb
    /* The grant, once. Nothing below touches SYSRAM until it is released. */
    vendor_hooks 1
    load32 $r1, ICAP_GRANT
    lwi $r0, [$r1]
    sethi $r2, 0xf000         /* 0x0f000000 */
    or $r0, $r0, $r2
    swi $r0, [$r1]
    dsb
    movi $r0, 1
    swi $r0, [$r8 + 56]
    lwi $r0, [$r10]
    swi $r0, [$r8 + 48]
    movi $r4, 0               /* sequence */
    movi $r5, 0               /* descriptor index */
stream_epoch:
    swi $r4, [$r8 + 44]
    load32 $r1, ICAP_CR0
    movi $r0, 0x4f1           /* armed, cursor reset */
    swi $r0, [$r1]
    dsb
    movi $r0, 0x24f3          /* capture */
    swi $r0, [$r1]
    /* CR9 holds the previous end until the writer restarts: wait for it to come back low. */
    lwi $r3, [$r10]
44:
    load32 $r1, ICAP_CR9
    lwi $r0, [$r1]
    zeh33 $r0, $r0
    movi $r2, 0x1000
    slt $r2, $r0, $r2
    bnez $r2, 45f
    lwi $r1, [$r10]
    sub $r1, $r1, $r3
    movi $r2, 10000
    slt $r1, $r1, $r2
    bnez $r1, 44b
    movi $r0, 10
    swi $r0, [$r8 + 40]
    j stream_drain
45:
    swi $r0, [$r8 + 4]
    swi $r0, [$r8 + 0]
    movi $r1, 0
    swi $r1, [$r8 + 8]
    lwi $r1, [$r10]
    swi $r1, [$r8 + 12]
stream_packet:
    load32 $r2, 100000000
46:
    observe_source
    lwi $r0, [$r6 + 12]
    beq $r0, $r5, 47f         /* previous descriptor done */
    addi $r2, $r2, -1
    bnez $r2, 46b
    movi $r0, 4
    swi $r0, [$r8 + 40]
    j stream_release
47:
    lwi $r0, [$r8 + 44]
    sub $r0, $r4, $r0
    slli $r0, $r0, 11
    swi $r0, [$r8 + 8]
    load32 $r1, STOPREG
    lwi $r0, [$r1]
    load32 $r1, STOPMAGIC
    bne $r0, $r1, 48f
    movi $r0, 1
    swi $r0, [$r8 + 40]
    j stream_drain
48:
    lwi $r0, [$r8 + 28]
    beqz $r0, 49f
    bne $r4, $r0, 49f
    movi $r0, 2
    swi $r0, [$r8 + 40]
    j stream_drain
49:
    load32 $r2, 10000000
50:
    observe_source            /* leaves the backlog in r0 */
    movi $r3, 2048
    slt $r3, $r0, $r3
    beqz $r3, 51f             /* a full packet is ready */
    addi $r2, $r2, -1
    bnez $r2, 50b
    movi $r0, 10
    swi $r0, [$r8 + 40]
    j stream_drain
51:
    slli $r0, $r5, 6
    addi $r1, $r9, (DHDR - DRING)
    add $r1, $r1, $r0
    swi $r4, [$r1 + 36]
    lwi $r0, [$r8 + 24]
    swi $r0, [$r1 + 8]
    lwi $r0, [$r8 + 44]
    swi $r0, [$r1 + 12]
    sub $r0, $r4, $r0
    andi $r0, $r0, 31
    slli $r0, $r0, 13
    load32 $r3, RING_MCU
    add $r3, $r3, $r0         /* payload: 8 KiB block (seq - epoch) % 32 of the ring */
    slli $r0, $r5, 4
    add $r0, $r9, $r0
    swi $r1, [$r0 + 0]
    swi $r3, [$r0 + 8]
    movi $r2, 0
    swi $r2, [$r0 + 12]
    load32 $r2, ((44 << 16) | (1 << 14) | (PKT + 4))   /* SDL0 44, LS1, SDL1 PKT + 4 */
    swi $r2, [$r0 + 4]
    dsb
    addi $r5, $r5, 1
    andi $r5, $r5, (DCOUNT - 1)
    swi $r5, [$r6 + 8]
    addi $r4, $r4, 1
    j stream_packet
stream_gap:
    /* Overrun or a too-long poll gap: drain, count it, restart the epoch at ring offset 0. */
    load32 $r2, 100000000
52:
    lwi $r0, [$r6 + 12]
    beq $r0, $r5, 53f
    addi $r2, $r2, -1
    bnez $r2, 52b
    movi $r0, 4
    swi $r0, [$r8 + 40]
    j stream_release
53:
    lwi $r0, [$r8 + 24]
    addi $r0, $r0, 1
    swi $r0, [$r8 + 24]
    j stream_epoch
stream_drain:
    load32 $r2, 100000000
54:
    lwi $r0, [$r6 + 12]
    beq $r0, $r5, stream_release
    addi $r2, $r2, -1
    bnez $r2, 54b
    movi $r0, 4
    swi $r0, [$r8 + 40]
stream_release:
    swi $r4, [$r8 + 60]       /* the ROM calls below clobber r0-r5 */
    load32 $r1, ICAP_CR0
    movi $r0, 0x4f1           /* vendor leaves ICAP at 0x401 (EN kept) when it releases */
    swi $r0, [$r1]
    dsb
    /* Release like the vendor: ROM status idle (up to 5 ms), hooks, then the grant. */
    lwi $r0, [$r8 + 64]
    beqz $r0, 56f
    lwi $r0, [$r10]
    swi $r0, [$r8 + 52]
55:
    load32 $r2, ICAP_BUSY
    lwi $r2, [$r2]
    jral5 $r2
    beqz $r0, 56f
    lwi $r1, [$r10]
    lwi $r2, [$r8 + 52]
    sub $r1, $r1, $r2
    movi $r2, 5000
    slt $r1, $r1, $r2
    bnez $r1, 55b
56:
    vendor_hooks 0
    load32 $r1, ICAP_GRANT
    lwi $r0, [$r1]
    sethi $r2, 0xf0fff
    ori $r2, $r2, 0xfff       /* 0xf0ffffff */
    and $r0, $r0, $r2
    swi $r0, [$r1]
    dsb
    lwi $r4, [$r8 + 60]
stream_publish:
    /* SYSRAM is back: seq, gaps, max backlog, max poll us, end state, DMA errors, us, units. */
    swi $r4, [$fp + MB_RESULT + 0]
    lwi $r0, [$r8 + 24]
    swi $r0, [$fp + MB_RESULT + 4]
    lwi $r0, [$r8 + 20]
    swi $r0, [$fp + MB_RESULT + 8]
    lwi $r0, [$r8 + 16]
    swi $r0, [$fp + MB_RESULT + 12]
    lwi $r0, [$r8 + 40]
    swi $r0, [$fp + MB_RESULT + 16]
    lwi $r0, [$r6 + (DMA_ERR - DMA_TX0)]
    swi $r0, [$fp + MB_RESULT + 20]
    lwi $r0, [$r10]
    lwi $r1, [$r8 + 48]
    sub $r0, $r0, $r1
    swi $r0, [$fp + MB_RESULT + 24]
    lwi $r0, [$r8 + 0]
    swi $r0, [$fp + MB_RESULT + 28]
    j command_done
    .size _start, .-_start
