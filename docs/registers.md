# Register and mailbox reference

Addresses are as seen by the WF MCU. "Host" gives the BAR0 route where one exists.
"Source" says where the knowledge comes from: **measured** on the MT7922 here, or taken from
**BirriaSDR** (MT7961), **w1700k** (MT7996), or the **vendor** firmware's own behaviour.

## Mailbox (`0x00407c00`, host BAR0 `0x87c00`)

Offsets are generated into `sdr_fw.h` from the `.equ MB_*` lines in `sdr_fw.S`, so host and
program cannot disagree.

| Offset | Name | Written by | Contents |
|:--|:--|:--|:--|
| `+0x000` | `MB_MAGIC` | program | `0x41495242` ("BRIA") once ready |
| `+0x004` | `MB_NONCE` | host | Random per boot |
| `+0x008` | `MB_RESPONSE` | program | `nonce ^ 0xb1771a5d` |
| `+0x01c` | `MB_STATE` | program | `0x10` during PHY power-on, `1` ready |
| `+0x034` | `MB_LOOPS` | program | Idle-loop counter |
| `+0x03c` | `MB_CMD` | host | Command; cleared by the program on pickup |
| `+0x04c` | `MB_POWER_ON` | program | ROM PHY power-on return value (host presets `0xa5a5a5a5`) |
| `+0x054` | `MB_DONE` | program | Completed command count |
| `+0x0bc` | `MB_CANARY` | host | `0x5a17c0de`; never written by the program |
| `+0x100` | `MB_OPS` | both | Up to 20 ops of 20 bytes: `addr, clear, set` (host), `after, before` (program); `addr = 0` ends the list |
| `+0x2a0` | `MB_PHASE` | program | Index of the op being applied; `0xfd` while streaming |
| `+0x2b8` | `MB_HOOKS` | host | 1: call the ROM grant hooks around grant changes |
| `+0x320` | `MB_LIMIT` | host | Stream packet limit, 0 = until stopped |
| `+0x324` | `MB_GUARD` | host | Stream poll guard, microseconds |
| `+0x340` | `MB_RESULT` | program | Eight stream result words ([streaming.md](streaming.md#stream-results-0x340)) |

An op with `clear = set = 0` is a pure read: the program never writes the address. An op with
`clear = ~0` is a plain write of `set`.

## ICAP (`0x80021090`), source: BirriaSDR, w1700k, measured

| Register | Bits | Meaning |
|:--|:--|:--|
| CR0 `0x80021090` | 0 | EN |
| | 1 | CAPTURE (running) |
| | 10 | SW_RST_B |
| | 13 | RING_BUF mode |
| | 17 | WRAP (w1700k; not used) |
| | 22:20 | SRC_SEL (w1700k; not used) |
| | 23 | SWITCH (w1700k; never set here) |
| CR2 `0x80021098` | | Ring start, 4-byte units |
| CR3 `0x8002109c` | | Ring end, 4-byte units (`0xfffc` = 256 KiB) |
| CR9 `0x800210b4` | 15:0 | Write cursor, 4-byte units |
| CR10 `0x800210b8` | 16..19 | Lane selectors and packers; all 0 = one sample per 32-bit word |

CR0 values used: `0x4f1` armed and cursor reset, `0x24f3` capture, `0x24f1` stop with EN
kept. On the MT7922 CR0 reads back only `0x401` or `0x403`: the bits in `0xf0` and `0x2000`
are write-only.

CR10 bits 16, 17, 18 and 19 select the first two words, the first word, the low 24 bits, and
the first three words respectively (measured; matches w1700k's MT7996 description). None of
them gives an 8-bit or otherwise denser sample format.

## Grant and ROM hooks, source: vendor

| Address | Meaning |
|:--|:--|
| `0x88009004` bits 27:24 | `0xf` gives the SYSRAM bus to ICAP; SYSRAM is unreadable meanwhile |
| `*(0x00821b4c)`, `*(0x00821b50)` | Called with the enable state before each grant change (WM `0x96c5b0`) |
| `*(0x00821b7c)` | ICAP status, polled until 0 before a release |
| `0x00836eae` | ROM: WF PHY power-on |

## Capture source and rate, source: BirriaSDR, measured

| Address | Bits | Meaning |
|:--|:--|:--|
| `0x830ad440` | 7:0 | Source mode; only `0x13` (dense ADC) works in this flow |
| `0x830ad448` | 11:8 | Tap; 4 = chain 0 |
| | 19:18 | Divider: 3, 2, 1, 0 = 40, 80, 160, 320 MSps |
| `0x83080004/8`, `0x83090004/8` | 13:11, 6 | PHY clock gates, chain 0 / chain 1 (`0x3840`) |
| `0x83081000`, `0x83091000` | 27:24 | Digital filter class `w` |
| | 28 | 5 GHz band |
| `0x820e3000` | 9:8 | MAC clock mode (`01` receive only) |

## A-die (SPI bridge ports), source: measured

| Register | Port | Bits | Meaning |
|:--|:--|:--|:--|
| `0x02c` | 5 (TOP) | | Die ID, `0x79228a00` |
| `0x160` | 0 and 1 | 31:30 | Analog filter width `w` (0..3 = 20/40/80/160 MHz) |
| | | 29 | 5 GHz band |
| `0x184` | 1 | | LO frequency, kHz |
| `0x258..0x268` | 0 | | AGC state (do not replay) |
| `0x768` | 1 | 31, 29:27, 26:23 | Gain clamp: enable, LNA, LPF (BirriaSDR) |
| `0xc04`, `0xc08` | 1 | 31:21 | VCO band code, ~linear per channel |
| `0xc28` | 1 | | `0x40000000 | word` |
| `0xc68` | 1 | 31; 30:0 | LOAD (self-clearing); synthesizer word `f * 2^21 / 60 MHz` |
| `0xc6c` | 1 | | `f / 125 kHz - 2` |
| `0xd7c` | 1 | 21 | VCO calibration DONE (sticky: reads 1 before a start) |
| `0xe1c` | 1 | 9:0 | Crystal selection: `0xd4` = 40 MHz, `0x113` = 52 MHz |

## Gain (CSD block `0x83000000 + (chain << 16)`), source: w1700k, measured

| Offset | Bits | Meaning |
|:--|:--|:--|
| `+0x0c4` | 1:0 | Force A and B |
| | 10:8, 18:16 | LNA for A, B (0..4) |
| | 15:12, 23:20 | LPF for A, B (0..9) |
| `+0x0d0` | 27:24 | Coarse AGC force (left cleared) |
| `+0x108` | 29:24 | Digital gain, signed 6-bit, about 1 dB per step |
| `+0x150` | 31; 22:20 | Force D; LNA |

## MCU DMA0 (`0x54000000`, host BAR0 `0x02000`)

| Address | Use |
|:--|:--|
| `0x54000110` | DMA error status (reported in stream results) |
| `0x54000120` | `MT_WFDMA_DUMMY_CR`: stop flag, `0x53544f50` "STOP" |
| `0x54000204` | Interrupt mask (program writes 0) |
| `0x54000208` | GLO_CFG: bit 0 TX enable, bit 2 RX enable, bits 1/3 busy |
| `0x5400020c` | Reset DMA index (bit 0 = TX0) |
| `0x54000300..0c` | TX0: base, count, CPU index, DMA index |
| `0x54000808` | PDA download status: `0x25` after a complete image |
