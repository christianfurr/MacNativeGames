# Positional audio voice updates

Verified on 11 November 2026, Apple M4, native OoT update 131072.

The title soundtrack's front-left voice was being erased by later unused voice
records. The renderer used each record's metadata ID as the destination slot.
Inactive records can retain ID zero: a live trace showed slot zero arriving
active at 48 kHz, then 90 later inactive ID-zero records leaving it unused.
The other five soundtrack voices remained active. This dropped front-left
before effects and stereo downmixing.

Voice updates now use the record's position in the bounded input table. Channel
resource tables are also copied by position, so inactive ID-zero metadata cannot
overwrite the first active resource. This preserves the source mix; no balance
offset, normalization, or forced mono is applied.

## Verification

- Complete REV15 and REV16 requests exercise two independent active voices,
  later inactive ID-zero records, a subsequent gain change, and freeing one
  active slot without clearing the other. The full audio probe passes 49,493
  checks. The overwrite regression fails with the original handling.
- Before: a 30-second title capture measured left -42.47 dBFS and right -31.38
  dBFS, a difference of 11.08 dB. A channel-resource-only change did not fix it.
- After the voice fix: a continuous 16-second title segment measured left
  -29.227 dBFS and right -29.198 dBFS, a difference of 0.029 dB. Both channels
  correlate above 0.9999999 with the matching original-source segment, with
  gain approximately 1.0. RMS sample error is about 0.0000153, consistent with
  the comparison WAV's PCM16 rounding. The source was decoded independently
  from the owner's local six-channel soundtrack and downmixed without gain
  correction.
- Temporary tracing was removed from the final build. Local source audio,
  recordings, and private game data are not part of this change.
- Final installed-app check: a continuous 30-second title recording matches
  the original source on both channels above 0.9999999 correlation, at unity
  gain. Left measures -28.275 dBFS and right -28.398 dBFS; their 0.124 dB
  difference matches the source's natural balance for that segment. No
  hardware underruns, overruns, missing frames, or backlog trims were reported
  during this capture. The installed executable's code section matches the
  tested runtime, and the full application passes signature verification.

The longer diagnostic capture also contains silent intervals. This change
verifies restored channel balance during playback; it does not establish
uninterrupted movie audio or corrected cutscene synchronization.
