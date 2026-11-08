# Asterisk SIP Echo Cancellation (func_lec)

**Echo cancellation for Asterisk SIP / PJSIP trunk calls.** If your users hear their own voice coming back on outside calls through a SIP trunk, and the carrier won't fix it, this module removes the echo inside Asterisk. It uses WebRTC's echo canceller (AEC2), works with FreePBX, IssabelPBX and plain Asterisk, and needs no DAHDI hardware.

> Asterisk has no built-in echo canceller for SIP channels: `echocancel` and `CHANNEL(echocan_mode)` only work on DAHDI. `func_lec` fills that gap for SIP trunk legs.

## Is this your problem?

- You hear **your own voice delayed** (about 100–250 ms) on calls through a **SIP trunk**, often to mobile numbers.
- Internal calls between extensions are fine.
- The echo is still there when the person at the other end **mutes their phone**. In that case the echo is created inside the carrier's network, typically at a 2-wire/4-wire hybrid, behind an echo canceller that is missing or too short. It is not acoustic echo from a handset.
- Phones' acoustic echo cancellers can't help: the echo arrives already mixed into the incoming audio.

## How it works

- `func_lec` attaches an audiohook to the **trunk channel**. Audio sent to the carrier is the reference; audio coming back from the carrier is cleaned before it reaches the phone.
- **Canceller:** WebRTC AEC2 with the extended filter and delay-agnostic mode, so it finds the echo delay by itself and keeps re-estimating it during the call. Its non-linear processor removes the residual echo.
- **Alignment:** samples are paired by position, not by arrival time, so network jitter doesn't misalign the reference.
- **Any codec on the phone side:** the audiohook audio follows the call's highest rate (16 kHz with G.722, 48 kHz with Opus). It is resampled to 8 kHz for the canceller and the cleaned audio is resampled back (speexdsp).
- **Bounded per call:** fixed memory, and about 0.4% of one CPU core per call (Xeon Gold 6154).
- **One log line per call:** a NOTICE with the measured echo reduction (ERLE), the detected delay and counters. No audio is stored.

## Results

On a live carrier trunk with network echo at 140–180 ms and an echo return loss of 25–38 dB:

| Test | Result |
|---|---|
| Offline replay, far end muted | echo from 30.6 dB to about 0 dB, below the 18 dB line noise |
| Offline double talk, far-end speech at 0 / −6 dB vs. our side | far-end level kept (median change 0 / −1 dB) |
| Offline double talk, very quiet far end (−18 dB) | some syllables dip (median −3.6 dB) |
| Live call, G.722 desk phone, far end muted | user heard no echo; log `erle_db=19.2 delay_ms=128` |
| Live call, both sides talking | no clipping reported |

Alternatives tested on the same recordings and rejected:
- **spandsp `echo_can` 0.0.6:** made the echo worse.
- **speexdsp MDF:** about 4 dB of reduction.
- **A plain level-based echo suppressor:** clipped the far end during double talk.
- **DAHDI OSLEC:** its tail is too short for a 175 ms echo, and it needs DAHDI channels anyway.

## Requirements

- Asterisk with development headers. Built and run on **Asterisk 18**; other versions with the same audiohook API (13+) should work but are untested.
- **webrtc-audio-processing 0.3.x** (`webrtc-audio-processing-devel` on RHEL/Rocky 8 PowerTools; `libwebrtc-audio-processing-dev` on Debian/Ubuntu releases that still ship 0.3). The 1.x series removed the AEC2 C API, so it won't link.
- **speexdsp** development package (`speexdsp-devel` / `libspeexdsp-dev`).
- gcc and make. No C++ compiler is needed.

## Build and install

```bash
make check-deps
make
sudo make install
sudo asterisk -rx 'module load func_lec.so'
sudo asterisk -rx 'module show like func_lec'
```

If only the runtime library is installed and there is no devel symlink, point the build at it:

```bash
make WEBRTC_LIBS=/usr/lib64/libwebrtc_audio_processing.so.1
```

At load time the module creates and frees one canceller as a self-test, and refuses to load if that fails. While a call uses it, it can't be unloaded.

## Usage

```
Set(LEC()=on)            ; attach, moderate suppression
Set(LEC()=aggressive)    ; stronger suppression
Set(LEC()=conservative)  ; weaker suppression
Set(LEC()=off)           ; detach
${LEC()}                 ; current metrics, or "off"
```

Run it **on the trunk channel**. For outgoing calls, that means a `Dial()` pre-dial handler, `b(lec-attach^s^1)`. See [examples/extensions_custom.conf.sample](examples/extensions_custom.conf.sample) for plain Asterisk and for the FreePBX / IssabelPBX predial hook. Both include:

- AstDB switches: a master switch, all extensions, or a per-extension allow list. They are checked per call, with no reload.
- An `IFMODULE(func_lec.so)` guard, so calls simply skip the canceller if the module isn't loaded.

Turn it off for every new call:

```bash
sudo asterisk -rx 'database put LEC enabled 0'
```

## Reading the log

One line per call, at hangup:

```
NOTICE func_lec.c: LEC PJSIP/my-carrier-0000098b: nlp=1 rx_s=43 tx_s=43 gaps=0 underrun=4960 overflow=0
  echo_frames=1223 erle_db=19.2 delay_ms=128 delay_std=80 seq=0 noseq=2181 badfmt=0 rate=16000 resampled=1966
```

| Field | Meaning |
|---|---|
| `erle_db` | Echo reduction on frames where only our side talked |
| `delay_ms` | Echo delay found by the canceller |
| `rate`, `resampled` | Audiohook rate and how many frames were resampled |
| `badfmt` | Frames skipped (should be 0) |
| `underrun` | Reference samples missing, mostly ringback before answer (normal) |

## Testing offline

`lec_replay` runs the same canceller code on a recorded call. Capture the trunk RTP on the PBX:

```bash
sudo tcpdump -i <trunk-if> -w call.pcap 'udp and host <carrier-media-ip>'
```

Then replay it:

```bash
./lec_replay call.pcap <carrier-media-ip> 0 0 before.raw   # pass-through baseline
./lec_replay call.pcap <carrier-media-ip> 0 2 after.raw    # moderate suppression
```

Both outputs are signed 16-bit, 8 kHz mono. An optional `mix.raw` argument adds far-end speech, to test double talk. Recordings contain call audio, so handle and delete them according to your privacy rules.

## Limitations

- **Outgoing calls are the tested path.** For incoming calls, attach after the call reaches an extension, so IVR prompts and in-band DTMF aren't processed.
- **No packet-loss alignment in Asterisk:** frames reaching the audiohook carry no RTP sequence number (`seq=0`), so the gap handling isn't active. After a loss, the canceller re-adapts within a second or two.
- **The far end's background noise is suppressed while the near end talks.** No comfort noise is added yet.
- **Quiet far-end speech during double talk** can dip by a few dB.
- **Fax over G.711 isn't handled.** Don't attach the canceller to fax calls.
- **Rebuild after every Asterisk upgrade.**

## Troubleshooting

| Symptom | Check |
|---|---|
| `module load` fails | `ldd func_lec.so` (missing webrtc or speexdsp library), and the log for the self-test message |
| `badfmt` greater than 0 | Frames that aren't signed linear; report them with your Asterisk version |
| `erle_db` near 0 and `echo_frames` 0 | No echo on that call, or the canceller is on the wrong channel (it must be the trunk channel) |
| Echo remains | Try `Set(LEC()=aggressive)`, and check `delay_ms`. Echo delays above about 250 ms are untested |

## License

GPL-2.0-only, the same as Asterisk modules. See [LICENSE](LICENSE). The WebRTC AEC declarations in `src/lec_core_webrtc.c` come from webrtc-audio-processing 0.3 (BSD-3-Clause, the WebRTC project authors).

## Keywords

Asterisk echo cancellation, Asterisk echo canceller SIP, PJSIP echo, SIP trunk echo, network echo, line echo, hybrid echo, hear my own voice on calls, delayed echo VoIP, FreePBX echo, IssabelPBX echo, VitalPBX echo, WebRTC AEC Asterisk, G.168.
