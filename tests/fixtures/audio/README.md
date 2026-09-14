# Synthetic audio fixtures

## Compressed audio delay and padding

The three `synthetic-tone` files encode a generated 997 Hz sine at 44.1 kHz,
1.013 seconds, and two channels. MP3, AAC, and Opus carry different codec delay
and end-padding information; a complete read must account for those samples
instead of mistaking them for decoder failures or adding successful silence.

| File | Codec | Decoded sample rate | Decoded frames per channel |
| --- | --- | ---: | ---: |
| `synthetic-tone.mp3` | MP3 | 44100 | 44673 |
| `synthetic-tone.m4a` | AAC | 44100 | 44673 |
| `synthetic-tone.opus` | Opus | 48000 | 48624 |

These frame counts exclude declared codec delay and end padding. The r1310
FFmpeg baseline trims the AAC container's final partial packet to its declared
44673 samples; older FFmpeg versions could expose 45056 decoded samples instead.
Opus encodes at 48 kHz. Tests compare complete and random reads through both cache modes
against an independent uncached provider, including legal head/EOF silence.
The sequential decode supplies the exact PCM reference for both caches. AAC
perceptual noise substitution and Opus seek/preroll can differ from a full
sequential decode. Their uncached random reads check complete writes, legal
padding, and the presence of positive and negative signal over intervals of at
least two sine periods when those polarities exist in the reference. All cached
samples remain subject to exact comparison with the initial sequential PCM.
Independent Opus experiments compared four ranges with FFmpeg using actual
seeks and 80 ms preroll; the resulting PCM hashes matched exactly. Sequential
PCM therefore serves as the cache oracle rather than the Opus seek oracle.

Regenerate from the repository root with FFmpeg:

```powershell
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "sine=frequency=997:duration=1.013:sample_rate=44100" -ac 2 -c:a libmp3lame tests/fixtures/audio/synthetic-tone.mp3
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "sine=frequency=997:duration=1.013:sample_rate=44100" -ac 2 -c:a aac tests/fixtures/audio/synthetic-tone.m4a
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "sine=frequency=997:duration=1.013:sample_rate=44100" -ac 2 -c:a libopus tests/fixtures/audio/synthetic-tone.opus
```
