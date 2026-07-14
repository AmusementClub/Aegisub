# Secondary Bitmap Subtitle Decoder

## Scope

The decoder path exists only for the secondary subtitle strip. Primary ASS
editing and rendering continue to use `AssFile` and `SubtitlesProvider`
(libass/CSRI). Text secondary subtitles also continue to be imported into an
`AssFile`.

The first supported bitmap codec is HDMV PGS. Aegisub supports two sources:

- external `.sup` / `.pgs` files, parsed by Aegisub's lightweight SUP reader;
- `S_HDMV/PGS` tracks in Matroska containers, demuxed by Aegisub's existing
  libmatroska backend.

Other containers are not delegated to the decoder plugin and are out of scope.

## Responsibility boundary

Aegisub owns file access, Matroska scanning, track selection, content-encoding
removal, timestamps, source reload, target allocation, and display. It
normalizes both supported sources into `SecondarySubtitlePacketStream` with
nanosecond timestamps and the stable codec id `hdmv-pgs`.

Ragbag owns only the libavcodec decoder session, decoded bitmap timeline, PGS
authored-canvas discovery, random time lookup, palette expansion, scaling, and
rendering into host-owned premultiplied BGRA8 storage. Ragbag deliberately does
not link libavformat and cannot open source files or containers.

## Decoder lifecycle

The v1 dynamic ABI is a preload-and-render contract:

1. discover a decoder that advertises `hdmv-pgs`;
2. create a decoder session;
3. call `begin_stream`, followed by ordered `push_packet` calls;
4. call `end_stream` to finalize the random-access timeline;
5. call `render_at` for arbitrary nanosecond timestamps;
6. destroy the session when the secondary source changes.

Packet and codec-private pointers are borrowed for the duration of their call.
`render_at` fully overwrites the target, including transparent pixels. Its
result describes the half-open time interval for which the rendered bitmap may
be reused.

Each decoder session is single-threaded. Separate sessions may be used
concurrently. ABI structures carry `struct_size`, and the initialization symbol
is versioned as `ragbag_subtitle_decoder_init_v1`.

## Geometry and color

PGS presentation composition segments are authoritative for authored width and
height. The main video size is only a fallback for malformed streams. Ragbag
scales authored bitmap rectangles to the target with nearest-neighbour sampling
and returns premultiplied BGRA8 suitable for the wx secondary subtitle strip.

HDR output, high-bit-depth targets, primary-video composition, dirty upload
protocols, document inputs, and libass adapters are explicit non-goals.

## Compatibility

The former Ragbag v0 `open_file` provider is replaced rather than extended.
Ragbag is no longer registered in the global `SubtitlesProviderFactory`; the
secondary subtitle session reaches it only through the private
`secondary_subtitle_decoder` adapter. Missing decoder plugins therefore affect
only PGS secondary sources and do not change normal ASS provider discovery.
