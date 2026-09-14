# Synthetic audio fixtures

## A/V timeline offsets

Both Matroska files contain synthetic 16x16 FFV1 video and 48 kHz mono FLAC
audio. The audio contains exactly 23040 samples with the signed 16-bit value
16384. `positive-delay.mkv` starts audio 240 ms after video; LsmasNative reports
11520 delay samples and 34560 total samples. `negative-delay.mkv` starts video
240 ms after audio; it reports -11520 delay samples and 11520 total samples.

Regenerate from the repository root with FFmpeg:

```powershell
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "color=s=16x16:r=25:d=0.8" -itsoffset 0.24 -f lavfi -i "aevalsrc=0.5:s=48000:d=0.48" -map 0:v -map 1:a -c:v ffv1 -c:a flac -sample_fmt s16 -fflags +bitexact -flags:v +bitexact -flags:a +bitexact -map_metadata -1 tests/fixtures/audio/positive-delay.mkv
ffmpeg -hide_banner -loglevel error -y -itsoffset 0.24 -f lavfi -i "color=s=16x16:r=25:d=0.8" -f lavfi -i "aevalsrc=0.5:s=48000:d=0.48" -map 0:v -map 1:a -c:v ffv1 -c:a flac -sample_fmt s16 -fflags +bitexact -flags:v +bitexact -flags:a +bitexact -map_metadata -1 tests/fixtures/audio/negative-delay.mkv
```

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

## AAC/MOV offsets and fresh seeks

The `aac-av-{zero,positive,negative}.mp4` fixtures contain synthetic 16x16 MPEG-4
video and a repeating mono PCM ramp encoded as 48 kHz AAC with perceptual noise
substitution disabled. They cover first-read priming, negative A/V alignment,
and the MOV tail-probe/rewind discard regression through the real provider and
both RAM and disk caches. Every cached sample is compared exactly with the
initial continuous decode, including reads after reaching EOF.

| File | Delay samples | Total samples with A/V sync |
| --- | ---: | ---: |
| `aac-av-zero.mp4` | 0 | 57600 |
| `aac-av-positive.mp4` | 10464 | 57568 |
| `aac-av-negative.mp4` | -11520 | 46080 |

The positive fixture reproduces a false 224-sample discard after tail probing
in unpatched FFmpeg. Its actual muxed offset differs from the requested 240 ms;
the expected values above describe the committed media. The generator used
FFmpeg `git-2022-08-12-f6a36c7cf`; newer muxers may produce different edit lists.
The decoder baseline for assertions is L-SMASH-Works r1310 and its pinned
FFmpeg, independently of the tool used to generate the media.

Regenerate from the repository root:

```powershell
python tests/fixtures/audio/generate_av_fixtures.py
```
