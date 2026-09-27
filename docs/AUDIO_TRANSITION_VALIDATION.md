# PCM transitions and device validation

Lyra now starts a second decoder while the current track is still playing. Each
decoder writes native-rate stereo PCM to its own bounded FIFO. One mixer consumes
those FIFOs, converts the successor to the active I2S rate when necessary, and
keeps a single DMA/output task running across automatic transitions. Gapless
mode concatenates the PCM frames. Crossfade mode mixes both decoders with
complementary linear gains for up to the configured 1, 2, 4, or 6 seconds. Manual
next/previous with crossfade enabled requests an immediate overlap.

The first track sets the I2S sample rate for the session. Successive tracks
with a different rate use linear interpolation. The log records both source
formats and the retained I2S rate at each handoff. A format change *within one
file* is rejected, because that decoder's FIFO has a fixed native rate.

The FIFO handoff removes the GUI-poll and I2S-restart gaps. It does not remove
silence actually decoded from a compressed file: some MP3/AAC encoders add
priming or padding, and the current decoder API may emit those samples. WAV,
AIFF, and FLAC provide the clearest sample-exact boundary tests. A file with
an unknown or inaccurate duration may start its automatic crossfade late;
the mixer still concatenates its decoded PCM when it ends.

## On-device measurement (JC3248W535EN)

The device owner builds, flashes, and monitors the board. Use a DAC or speaker
loopback capture when checking audible continuity; a quiet transition alone
cannot distinguish intentional silence in a file from an output underrun.

1. Put two sample-continuous WAV files on MicroSD, followed by 44.1 kHz FLAC,
   48 kHz WAV, stereo/mono variants, and representative MP3/AAC files. Include
   a very short file and a corrupt successor. Queue them and play with
   **Gapless playback** on and **Crossfade** off. Listen or capture the DAC
   across every boundary; check that `PCM handoff` occurs and that the I2S
   output rate in that line remains fixed.
2. Repeat with 1, 2, 4, and 6 second crossfades, including manual Next and
   Previous. Capture both file signals to verify they coexist for the selected
   interval, rather than fading one before starting the other. Also try pause,
   seek, stop, repeat-song, shuffle, and disabling gapless playback.
3. Watch `PCM stats` every 30 seconds, at each handoff, and when playback
   stops. Record
   `source-underruns`, `pcm-fifo-underruns`, `late`, and `preload-fail` before
   and after the boundary. The PCM FIFO count measures empty software FIFO
   episodes after startup; hardware DMA may continue playing its queued data
   for a short time. `late` means the successor was not ready at the PCM
   boundary. Audible gaps should have a corresponding trace or be investigated
   as encoded silence.
4. Record `internal-min`, `psram-min`, and `largest-internal-min` during a
   crossfade, along with `mix-max` in microseconds. Confirm that internal heap
   headroom remains healthy while both decoder workers and the output task
   are allocated. `scratch-internal` reports whether any decoder PCM or stereo
   scratch allocation fell back to internal RAM; `mixer-stack-spare` reports
   unused mixer stack at its lowest point. `sd-read-max` measures the file
   read only, while `sd-lock-max` measures waiting for the shared SD gate.
   `mix-max` excludes the wait for space in the output FIFO.
   `format` and `resampled` count format/rate changes; verify
   these match the queued files. `output` is the session I2S rate.

`lyra::audio::diagnostics()` exposes the same counters for a debugger or a
future diagnostics screen. These measurements must be made on the physical
board; source inspection cannot establish them.

## Reported FLAC boundary and retest

One on-device log before the decoder-stack change showed `preload-fail=1`,
`gapless=0`, `pcm-fifo-underruns=2`, and `largest-internal-min=15360` bytes.
The successor worker previously requested a 24 KiB internal stack, which is
larger than that observed block. Decoder workers now request PSRAM-backed stacks;
the log reports an exact failure stage if the successor still cannot start,
plus each worker’s stack high-water mark on completion. `pcm-fifo-underruns`
counts software FIFO empty episodes and does not by itself prove audible DAC
silence, because I2S DMA may still contain samples.

For the same two FLAC files, capture the `FLAC seek` line if a seek was used,
the `playing` lines, one `PCM handoff` line, and the `PCM stats` lines on both
sides of the boundary. A successful preloaded boundary should increase
`gapless` with `preload-fail=0` and no new `late` count. Compare
`pcm-fifo-underruns` before and after the boundary, and record the lowest
`internal-min`, `psram-min`, and `largest-internal-min` while both decoders
are active. If a failure persists, the new error log distinguishes FIFO
allocation, task creation, and decoder failure. Test both uninterrupted
playback and a seek near the end; the seek log now gives the selected frame’s
actual base sample and discarded PCM bytes.

The reported follow-up log confirmed one gapless FLAC handoff with
`preload-fail=0`, but also showed a failed LCD flush when DMA free memory was
4875 bytes and the largest block was 3328 bytes. Decoder scratch buffers now
prefer PSRAM, the I2S staging chunk uses 8 KiB rather than 16 KiB of DMA
memory, and preloading starts 20 seconds before the listed end of a
track with known duration. FLAC SD reads are limited to 16 KiB per
transaction; same-rate PCM bypasses interpolation. Recheck marquee smoothness,
LCD flush errors, `pcm-fifo-underruns`, and the new memory fields together.
