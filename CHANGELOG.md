# Changelog

## 0.1.0 (2026-11-08)

First release.

- `LEC()` dialplan function: attaches WebRTC AEC2 to a trunk channel (on, aggressive, conservative or off), and reads back per-call metrics.
- Pairs samples by position, so jitter does not misalign the reference.
- Resamples 16 / 48 kHz audiohook audio to 8 kHz for the canceller and back.
- Self-test at load; holds a module reference while in use; logs one NOTICE line of metrics per call.
- `lec_replay` offline test tool: pcap replay, with an optional mixed-in far end for double-talk tests.
- Examples for plain Asterisk and the FreePBX / IssabelPBX predial hook, with AstDB switches and an `IFMODULE` guard.
