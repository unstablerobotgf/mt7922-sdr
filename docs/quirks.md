# Quirks and lessons learned

Each entry gives the symptom, the cause, the fix, and the evidence. **Verified** means
confirmed by a direct readback or a test whose outcomes would differ otherwise; **inferred**
means the best explanation consistent with the evidence, not directly proven.

## WF bus death after grant release

**Symptom.** After some captures every WF register read returns `0xdead0003` (shown as
`7853/3` when masked to 14 bits), the mailbox stops answering, and only a reboot recovers the
device. CONN_INFRA (`0x70xxxxxx`) and the SPI bridge (`0x7c00xxxx`) keep working, so it is
the WF bus specifically that is gone. Roughly 6 % of captures died this way.

**Failure signature.** The capture itself completes: the mailbox reports it done. The death
appears during the ring read that follows, consistently a few words in (word 8 in three of
the runs, word 12 in another), with the first words real.

**Cause.** BirriaSDR's per-capture sequence: take the grant, start ICAP (`0x24f3`), stop it
by clearing EN (`0x4f0`), release the grant (`0x88009004[27:24] = 0`), then read the ring at
`0xe0200000` (host `0x40000000`). Reading the ring after a grant cycle can hang from either
side: when the MCU copied the ring for the host instead, the MCU itself hung inside the copy.

**Ruled out**, and what ruled each out:

- The TOP/CONN_INFRA clock handover from BirriaSDR: a run with it skipped died the same way.
- A timer or time-since-boot effect: two 2 s idle holds with no captures (heartbeat and
  canary read every ms) were clean; deaths need captures.
- Bluetooth alone: one run with Bluetooth off passed 48/48, but the next one died. Keeping
  Bluetooth off is still done as a precaution.
- Releasing while ICAP is busy: the ROM's ICAP status (`*(0x821b7c)()`) read idle before
  every release.
- Ring overrun: deaths occurred with capture targets (`0x3f00` units) far below the ring end.

Calling the vendor's grant hooks (`*(0x821b4c)`, `*(0x821b50)`) around each change gave one
run of 248 clean captures, but a later run with hooks still died. They reduce the rate; they
do not remove it.

**Fix.** Hold the grant for the whole stream; stop ICAP the way the vendor does (`0x24f1`,
EN kept); never touch the ring after a release; release once, at the end, after polling the
ROM status and calling the hooks.

**Evidence.** No bus death since the change, across every stream at every rate, including
the stop/start cycles that gain changes cause and interactive SDR++ use. w1700k's `icap.h`
documents a matching MT7996 hazard: ring capture, EN cleared, host read, PCIe completion
timeout. They attribute it to CR0 bit 23 (SWITCH), which is never set here.

**Status.** Fix verified by absence of the failure over many grant cycles. The exact failing
access is inferred.

## SYSRAM reads under the grant return stale data

**Symptom.** While ICAP holds the grant, host reads of the mailbox return plausible but wrong
values instead of faulting.

**Cause.** The grant moves SYSRAM's bus to ICAP; a host read gets whatever the bus last
carried.

**Fix.** The mailbox canary at `+0xbc` is written only by the host. `sdr_cmd()` and
`sdr_stream_stop()` read the words they care about and then the canary; a correct canary
vouches for the reads before it. The program keeps all streaming state in data RAM, and the
host signals stop through a DMA scratch register (`0x54000120`) instead of the mailbox.

**Status.** The stale reads are observed. The canary is a guard derived from that, not
itself a measurement of when the bus is handed back.

## The vendor analog constants are die-specific

**Symptom.** With BirriaSDR's A-die bring-up (MT7972 constants), captures got louder and
burstier but contained no detectable 802.11, and the captured spectrum did not follow the
tuned frequency. The die ID differs (`0x79228a00` here, `0x79728a10` on the MT7972), though
some receiver registers happen to match BirriaSDR's values exactly.

**Fix.** Replay the vendor firmware's own 2.4 GHz state ([rf.md](rf.md)). With it the
spectrum tracks tuning. BirriaSDR's bring-up sequence was later removed entirely, since every
A-die register it writes is overwritten by the replay.

**Evidence.** After the replay, an 802.11b Barker detector finds beacons at the channels of
known nearby access points (3, 4 and 6 from a 2437 MHz LO), and finds them only under the
as-is I/Q convention: the mirrored convention would place them on channels 8 and 9, where
there were none. With the BirriaSDR sequence removed, `tools/sdr_check.py` passes rates,
retune and gain and still finds the same channels.

**Status.** Verified. A weaker hit on channel 9 in some runs is most likely the image of the
strong channel 3 signal (finite I/Q image rejection); inferred, not measured.

## Identity gate

The replayed tables and the synthesizer formula assume this die (`TOP 0x02c = 0x79228a00`)
and a 40 MHz crystal (`1/0xe1c[9:0] = 0xd4`). The MT7922 keeps no crystal word at the
location BirriaSDR reads on the MT7961 (`0x02004600`); the vendor ROM patch commits its
crystal choice to `0xe1c` instead. On any mismatch the driver writes nothing.

## AGC state in the channel snapshots

**Symptom.** At 2412 MHz, captures were full of loud, structureless energy (25 to 28 loud
windows per burst) and no beacons.

**Cause.** `0x258..0x268` are AGC state, not configuration. The 2412 MHz snapshot was taken
while a packet was being received (its filter index read 1), and replaying it forced that
mid-packet gain state.

**Fix.** Never replay those per channel; keep the band snapshot's values.

**Status.** Verified: with the registers excluded, loud windows dropped to 3 or 4 per burst
and beacons appeared on channel 1.

## VCO calibration DONE is sticky

`0xd7c` bit 21 reads set before a calibration has been started, so polling it does not prove a
calibration ran. The driver does not run one. The replayed per-channel state includes the
vendor's calibration results, and the synthesizer word written is bit-identical to the
vendor's. Status: verified (readback before start).

## CR0 reads back partially

CR0 reads back only `0x401` or `0x403`. The bits in `0xf0` and `0x2000` are accepted but
read as zero, so CR0 cannot be used to confirm the configuration. CR9 (cursor) movement is
the only reliable sign that ICAP is running. Status: verified.

## FW_START has no response

The ROM jumps to the program without sending the response the driver would normally wait
for. Waiting for it times out. Send it without waiting and poll the mailbox magic.
Status: verified.

## A resident vendor ROM patch

The vendor ROM patch hooks ROM functions this program calls. The patch semaphore says
whether one is resident; the driver refuses to boot if it is. Status: verified (semaphore
reports "not downloaded" after a WFSYS reset from module reload).

## The host cannot reach the PHY

`0x83xxxxxx` (PHY) and `0x88009004` (grant) have no BAR0 route. The MCU program's ops command
applies them; this is why gain and rate changes need the stream stopped. Status: verified.

## Bridge port numbering

Bridge port 1 is the vendor firmware's bank 0, and port 0 is bank 1. The 2.4 GHz LO is bank
0's primary synthesizer. Status: verified by matching readbacks between the two views.

## AGC hunting sounds like dropouts

**Symptom.** 80 MSps audio in SDR++ was choppy while the stream had no gaps.

**Cause.** The PHY AGC re-evaluates continuously, even on noise: the noise floor moved between
roughly 400 and 1330 counts rms.

**Fix.** Manual gain. Status: verified (choppiness gone with manual gain, same stream).

**Lesson.** An earlier measurement showed "30 dB jumps" with the gain forced, which pointed at
the coarse AGC. They were channel 6 traffic in the measurement window. Measure the noise floor
with a low percentile of short-window power, not the mean.

## Bandwidth limits

The link is PCIe 2.0 x1 (5 GT/s), about 373 MB/s usable. 160 MSps produces 640 MB/s and
streams gapped (about 58 % of samples, in contiguous runs). There is no hardware 8-bit mode:
CR10's packers only select which 32-bit words or 24-bit fields are kept, and only capture
source `0x13` works in this flow.

## Writing the program

- The NDS32 assembler is used with `-mreduced-regs`: only `r0..r10`, `r15`, `fp`, `gp`, `lp`
  and `sp` exist.
- ROM functions clobber `r0..r5`; `r6..r10` survive and hold the stream's base pointers.
- Every op is announced in `MB_PHASE` before its access, so a hang names the register.

## Restoring state

The A-die, TOP and CONN_INFRA are not reset by the WFSYS reset the stock driver performs.
Leaving modified state behind made the stock firmware reload and Bluetooth fail until a
reboot. Hence the undo log ([teardown.md](teardown.md)).

## Build hygiene

A power loss right after a build once left zero-length `.ko` and object files. `build.sh`
runs `sync` at the end.
