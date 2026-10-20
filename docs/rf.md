# RF: band state, tuning, rate, gain

All analog state lives in the A-die, which the host reaches directly through the SPI bridge
at `0x7c004000`. Digital front-end and gain registers live in the PHY (`0x83xxxxxx`) and go
through the MCU's ops command. Every write, of either kind, records the register's previous
value in the undo log first.

## The SPI bridge (`sdr_bridge()`)

BirriaSDR's bridge, unchanged on the MT7922:

| Offset | Use |
|:--|:--|
| `+0x00` | Status: bit 1 busy (A-die ports), bit 5 busy (TOP) |
| `+0x10` / `+0x50` | Command: `port << 13 | reg`, bit 12 set for a read (A-die / TOP) |
| `+0x14` / `+0x54` | Data; writing it launches the transfer |
| `+0x18` / `+0x58` | Read result |

Ports: **1** is the vendor firmware's bank 0, **0** is bank 1, **5** is TOP. Bank 0 (port 1)
carries the 2.4 GHz receive LO.

## Band state: replaying the vendor firmware

BirriaSDR brings up the analog chain with constants measured on the MT7972 and documents
that they must not be reused on another die. They also cover only part of the band-dependent
state: on the MT7922 every register they do not touch stays at whatever band the vendor
firmware last used, often 5 GHz. Instead, this driver writes the vendor firmware's own
register values for 2.4 GHz.

### Where the tables come from

With the stock driver and vendor firmware loaded, the interface was put in monitor mode and
tuned to each channel. The vendor firmware's `REG_READ` command accepts A-die addresses
encoded as `0x99BB0RRR` (bank `BB`, register `RRR`); every register of both banks
(0x000..0xffc) and the TOP registers of interest were read on each channel. Comparing
captures gives two tables in `driver/sdr_stock_2g.h`:

- `sdr_stock_2g[]`, 459 entries: each register whose 2437 MHz value differs from its
  5180 MHz value, plus every register BirriaSDR's bring-up writes. Written once at bring-up.
- `sdr_stock_ch[]`, 62 registers by 13 channels: each register whose value changes between
  2.4 GHz channels. Channels 1..11 were measured; 12 and 13 were refused by the regulatory
  domain and copy channel 11.

The capture tooling depends on a debugfs hook for issuing raw vendor MCU commands and is not
part of this repository.

### Registers deliberately not replayed per channel

- **0x258..0x268** are AGC state, not channel configuration. The 2412 MHz snapshot
  happened to be taken mid-packet, and replaying it made 2412 MHz behave differently from
  every other channel. The band state's 2437 values are kept for all channels.
- **0xdac (port 1)** is a synthesizer readback.

## Tuning (`sdr_rf_tune()`)

Valid range 2390..2500 MHz. For a frequency `f`:

1. Pick the nearest channel `ch = round((f - 2412) / 5)`, clamped to 1..13, and write its
   per-channel state.
2. For port 1 `0xc04` and `0xc08`, bits 31:21 hold a VCO band code that steps almost
   linearly per channel. Interpolate it for `f` from the two nearest measured channels
   (extrapolating past channels 1 and 11).
3. Write the synthesizer:

| Register (port 1) | Value | Meaning |
|:--|:--|:--|
| `0x184` | `f / 1 kHz` | Frequency in kHz, as the vendor writes it |
| `0xc6c` | `f / 125 kHz - 2` | Calibration count |
| `0xc28` | `0x40000000 | word` | Mirror of the word |
| `0xc68` | `0x80000000 | word` | Synthesizer word; bit 31 (LOAD) self-clears |

with `word = round(f * 2^21 / 60 MHz)`: a 60 MHz reference, the VCO at twice the LO, and 20
fractional bits. At 2437 MHz this gives `0x0513bbbc`, identical to the vendor's.

All of these are bridge writes, so tuning works during a stream. Per-write readback and
logging are suppressed while tuning so a retune takes a few milliseconds.

No VCO calibration is triggered. The calibration's DONE bit (`0xd7c` bit 21) reads set even
before a calibration is started, so polling it proves nothing, and the replayed state
already carries the vendor's calibration results for each channel.

## Sample rate and filter width (`sdr_rf_rate()`)

The ADC runs at 320 MSps. The ICAP tap decimates by `2^div`, where `div` is
`0x830ad448` bits 19:18. Each rate is paired with a filter width code `w` written to three
places:

| Rate | `div` | `w` | Bandwidth |
|:--|:--|:--|:--|
| 40 MSps | 3 | 1 | 40 MHz |
| 80 MSps | 2 | 2 | 80 MHz |
| 160 MSps | 1 | 3 | 160 MHz |
| 320 MSps | 0 | 3 | 160 MHz |

| Register | Bits | Meaning |
|:--|:--|:--|
| A-die port 0 and port 1 `0x160` | 31:30 | Analog low-pass width (0..3 = 20/40/80/160 MHz) |
| `0x83081000`, `0x83091000` | 27:24 | Digital filter class, chain 0 / chain 1 |

For 160 and 320 MSps eleven more digital front-end words are applied, copied from the
vendor firmware's state at 5250 MHz / 160 MHz (`sdr_wide_extras[]`). Their previous values
are saved and put back when the rate drops to 80 or below.

The rates were verified by timing CR9 against the MCU's 1 MHz timer. The widths were
verified by the noise floor's spectral shape at each setting.

## Gain (`sdr_rf_gain()`)

Two receive chains each have a CSD block at `0x83000000 + (chain << 16)`. The layout matches
the MT7996 map published by w1700k-sdr:

| Offset | Field | Manual value |
|:--|:--|:--|
| `+0xc4` | bits 1:0 force enable; LNA A/B at 10:8 and 18:16; LPF A/B at 15:12 and 23:20 | `3`, LNA, LPF |
| `+0x108` | bits 29:24: digital gain, signed 6-bit | `digital` |
| `+0x150` | bit 31 force; bits 22:20 LNA | `0x80000000 | lna << 20` |
| `+0xd0` | bits 27:24: coarse AGC force | cleared |

plus A-die port 1 `0x768` bits 31:23 (BirriaSDR's analog clamp): bit 31 enable, LNA at
29:27, LPF at 26:23.

Order matters. Forcing manual gain sets the analog clamp first, then the PHY words with
`+0xc4` last. Returning to AGC restores each PHY word to the value saved before the first
force, then releases the clamp.

Measured step sizes at a quiet frequency: LNA about 6 dB per step (0..4), LPF about 2 dB per
step (0..9), digital about 1 dB per step (-32..31).

The PHY's AGC re-evaluates continuously and, at 80 MSps in a busy band, moves often enough
to be audible as choppiness in a demodulator. Manual gain is stable. The coarse AGC force
in `+0xd0` turned out not to be needed for that.

Gain writes are PHY writes, so the driver stops and restarts a running stream around them.
