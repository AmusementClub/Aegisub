// Copyright (c) 2008-2009, Karl Blomster
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

/// @file video_provider_ffmpegsource.cpp
/// @brief FFmpegSource2-based video provider
/// @ingroup video_input ffms
///

#ifdef WITH_FFMS2
#include "ffms_color_metadata.h"
#include "ffms_chroma_location.h"
#include "ffms_native_format_info.h"
#include "ffmpegsource_common.h"
#include "include/aegisub/video_provider.h"

#include "options.h"
#include "utils.h"
#include "video_frame.h"

#include <array>
#include <cstring>

#include <libaegisub/fs.h>
#include <libaegisub/make_unique.h>

namespace {
typedef enum AGI_ColorSpaces {
	AGI_CS_RGB = 0,
	AGI_CS_BT709 = 1,
	AGI_CS_UNSPECIFIED = 2,
	AGI_CS_FCC = 4,
	AGI_CS_BT470BG = 5,
	AGI_CS_SMPTE170M = 6,
	AGI_CS_SMPTE240M = 7,
	AGI_CS_YCOCG = 8,
	AGI_CS_BT2020_NCL = 9,
	AGI_CS_BT2020_CL = 10,
	AGI_CS_SMPTE2085 = 11,
	AGI_CS_CHROMATICITY_DERIVED_NCL = 12,
	AGI_CS_CHROMATICITY_DERIVED_CL = 13,
	AGI_CS_ICTCP = 14
} AGI_ColorSpaces;

bool IsQuarterTurn(int rotation);
bool IsHalfTurn(int rotation);
bool IsClockwiseQuarterTurn(int rotation);
bool IsCounterClockwiseQuarterTurn(int rotation);

/// @class FFmpegSourceVideoProvider
/// @brief Implements video loading through the FFMS library.
class FFmpegSourceVideoProvider final : public VideoProvider, FFmpegSourceProvider {
	/// video source object
	agi::scoped_holder<FFMS_VideoSource*, void (FFMS_CC*)(FFMS_VideoSource*)> VideoSource;
	const FFMS_VideoProperties *VideoInfo = nullptr; ///< video properties

	int Width = -1;                 ///< width in pixels
	int Height = -1;                ///< height in pixels
	int CS = -1;                    ///< Reported colorspace of first frame
	int CR = -1;                    ///< Reported colorrange of first frame
	int CP = -1;                    ///< Reported color primaries of first frame
	int TC = -1;                    ///< Reported transfer characteristics of first frame
	int RealCS = -1;                ///< Original colorspace before any matrix override
	int RealCR = -1;                ///< Original colorrange before conversion to RGB
	int RealCP = -1;                ///< Original color primaries before any override
	int RealTC = -1;                ///< Original transfer characteristics before any override
	int NativePixelFormat = -1;     ///< Original FFmpeg AVPixelFormat reported by FFMS2
	int Rotation = 0;               ///< Rotation metadata from FFMS2 when runtime/header support it
	int Flip = 0;                   ///< Flip metadata from FFMS2 when runtime/header support it
	SourceFrameOutputMode OutputMode = SourceFrameOutputMode::Bgra8;
	bool NativeOutputSupported = false;
	std::vector<int> KeyFramesList; ///< list of keyframes
	agi::vfr::Framerate Timecodes;  ///< vfr object
	std::string ColorSpace;         ///< Colorspace name
	std::string RealColorSpace;     ///< Colorspace name

	char FFMSErrMsg[1024];          ///< FFMS error message
	FFMS_ErrorInfo ErrInfo;         ///< FFMS error codes/messages
	bool has_audio = false;

	bool ConfigureOutputMode(SourceFrameOutputMode mode);
	SourceFrameGeometry GetUnbakedFrameGeometry() const;
	void LoadVideo(agi::fs::path const& filename, std::string const& colormatrix);

public:
	FFmpegSourceVideoProvider(agi::fs::path const& filename, std::string const& colormatrix, agi::BackgroundRunner *br);

	void GetFrame(int n, VideoFrame &out) override;
	bool GetNativeFrame(int n, SourceFrame& frame, std::shared_ptr<void>& owner) override;

	void SetColorSpace(std::string const& matrix) override {
#if FFMS_VERSION >= ((2 << 24) | (17 << 16) | (1 << 8) | 0)
		if (matrix == ColorSpace) return;
		if (matrix == RealColorSpace)
			ffms::SetInputFormatV(VideoSource, CS, CR, ffms::GetPixFmt(""), nullptr);
		else if (matrix == "TV.601")
			ffms::SetInputFormatV(VideoSource, AGI_CS_BT470BG, CR, ffms::GetPixFmt(""), nullptr);
		else
			return;
		ColorSpace = matrix;
#endif
	}

	int GetFrameCount() const override             { return VideoInfo->NumFrames; }
	int GetWidth() const override;
	int GetHeight() const override;
	double GetDAR() const override;

	agi::vfr::Framerate GetFPS() const override    { return Timecodes; }
	std::string GetColorSpace() const override     { return ColorSpace; }
	std::string GetRealColorSpace() const override { return RealColorSpace; }
	SourceFrameColorMetadata GetColorMetadata() const override;
	SourceFrameColorMetadata GetRealColorMetadata() const override;
	SourceFrameGeometry GetFrameGeometry() const override;
	SourceFrameNativeFormatIdentity GetNativeFormatIdentity() const override;
	std::vector<SourceFrameOutputMode> GetAvailableSourceModes() const override;
	bool SetOutputMode(SourceFrameOutputMode mode) override { return ConfigureOutputMode(mode); }
	std::vector<int> GetKeyFrames() const override { return KeyFramesList; };
	std::string GetDecoderName() const override    { return "FFmpegSource"; }
	bool WantsCaching() const override             { return true; }
	bool HasAudio() const override                 { return has_audio; }
};

std::string colormatrix_description(int cs, int cr) {
	// Assuming TV for unspecified
	std::string str = cr == FFMS_CR_JPEG ? "PC" : "TV";

	switch (cs) {
		case AGI_CS_RGB:
			return "None";
		case AGI_CS_BT709:
			return str + ".709";
		case AGI_CS_FCC:
			return str + ".FCC";
		case AGI_CS_BT470BG:
		case AGI_CS_SMPTE170M:
			return str + ".601";
		case AGI_CS_SMPTE240M:
			return str + ".240M";
		default:
			throw VideoOpenError("Unknown video color space");
	}
}

SourceFrameColorMetadata ffms_color_metadata(int cs, int cr, int cp, int tc, std::string const& matrix) {
	return ffms::MapColorMetadata(cs, cr, cp, tc, matrix);
}

FFMSNativeFormatIds ResolveFFMSNativeFormatIds() {
	return {
		ffms::GetPixFmt("nv12"),
		ffms::GetPixFmt("p010le"),
		ffms::GetPixFmt("yuv420p"),
		ffms::GetPixFmt("yuv420p10le"),
		ffms::GetPixFmt("yuv422p"),
		ffms::GetPixFmt("yuv422p10le"),
		ffms::GetPixFmt("yuv444p"),
		ffms::GetPixFmt("yuv444p10le")
	};
}

FFMSNativeFormatIds const& GetFFMSNativeFormatIds() {
	static FFMSNativeFormatIds const ids = ResolveFFMSNativeFormatIds();
	return ids;
}

bool IsQuarterTurn(int rotation) {
	return rotation % 180 == 90 || rotation % 180 == -90;
}

bool IsHalfTurn(int rotation) {
	return rotation % 360 == 180 || rotation % 360 == -180;
}

bool IsClockwiseQuarterTurn(int rotation) {
	return rotation % 360 == 90 || rotation % 360 == -270;
}

bool IsCounterClockwiseQuarterTurn(int rotation) {
	return rotation % 360 == 270 || rotation % 360 == -90;
}

FFmpegSourceVideoProvider::FFmpegSourceVideoProvider(agi::fs::path const& filename, std::string const& colormatrix, agi::BackgroundRunner *br) try
: FFmpegSourceProvider(br)
, VideoSource(nullptr, ffms::DestroyVideoSource)
{
	ErrInfo.Buffer		= FFMSErrMsg;
	ErrInfo.BufferSize	= sizeof(FFMSErrMsg);
	ErrInfo.ErrorType	= FFMS_ERROR_SUCCESS;
	ErrInfo.SubType		= FFMS_ERROR_SUCCESS;

	SetLogLevel();

	LoadVideo(filename, colormatrix);
}
catch (agi::EnvironmentError const& err) {
	throw VideoOpenError(err.GetMessage());
}

void FFmpegSourceVideoProvider::LoadVideo(agi::fs::path const& filename, std::string const& colormatrix) {
	FFMS_Indexer *Indexer = ffms::CreateIndexer(filename.string().c_str(), &ErrInfo);
	if (!Indexer) {
		if (ErrInfo.SubType == FFMS_ERROR_FILE_READ)
			throw agi::fs::FileNotFound(std::string(ErrInfo.Buffer));
		else
			throw VideoNotSupported(ErrInfo.Buffer);
	}

	std::map<int, std::string> TrackList = GetTracksOfType(Indexer, FFMS_TYPE_VIDEO);
	if (TrackList.size() <= 0)
		throw VideoNotSupported("no video tracks found");

	int TrackNumber = -1;
	if (TrackList.size() > 1) {
		auto Selection = AskForTrackSelection(TrackList, FFMS_TYPE_VIDEO);
		if (Selection == TrackSelection::None)
			throw agi::UserCancelException("video loading cancelled by user");
		TrackNumber = static_cast<int>(Selection);
	}

	// generate a name for the cache file
	auto CacheName = GetCacheFilename(filename);

	// try to read index
	agi::scoped_holder<FFMS_Index*, void (FFMS_CC*)(FFMS_Index*)>
		Index(ffms::ReadIndex(CacheName.string().c_str(), &ErrInfo), ffms::DestroyIndex);

	if (Index && ffms::IndexBelongsToFile(Index, filename.string().c_str(), &ErrInfo))
		Index = nullptr;

	// time to examine the index and check if the track we want is indexed
	// technically this isn't really needed since all video tracks should always be indexed,
	// but a bit of sanity checking never hurt anyone
	if (Index && TrackNumber >= 0) {
		FFMS_Track *TempTrackData = ffms::GetTrackFromIndex(Index, TrackNumber);
		if (ffms::GetNumFrames(TempTrackData) <= 0)
			Index = nullptr;
	}

	// moment of truth
	if (!Index) {
		auto TrackMask = TrackSelection::None;
		if (OPT_GET("Provider/FFmpegSource/Index All Tracks")->GetBool() || OPT_GET("Video/Open Audio")->GetBool())
			TrackMask = TrackSelection::All;
		Index = DoIndexing(Indexer, CacheName, TrackMask, GetErrorHandlingMode());
	}
	else {
		ffms::CancelIndexing(Indexer);
	}

	// update access time of index file so it won't get cleaned away
	agi::fs::Touch(CacheName);

	// we have now read the index and may proceed with cleaning the index cache
	CleanCache();

	// track number still not set?
	if (TrackNumber < 0) {
		// just grab the first track
		TrackNumber = ffms::GetFirstIndexedTrackOfType(Index, FFMS_TYPE_VIDEO, &ErrInfo);
		if (TrackNumber < 0)
			throw VideoNotSupported(std::string("Couldn't find any video tracks: ") + ErrInfo.Buffer);
	}

	// Check if there's an audio track
	has_audio = ffms::GetFirstTrackOfType(Index, FFMS_TYPE_AUDIO, nullptr) != -1;

	// set thread count
	int Threads = OPT_GET("Provider/Video/FFmpegSource/Decoding Threads")->GetInt();
#if FFMS_VERSION < ((2 << 24) | (17 << 16) | (2 << 8) | 1)
	if (ffms::GetSourceType(Index) == FFMS_SOURCE_LAVF)
		Threads = 1;
#endif

	// set seekmode
	// TODO: give this its own option?
	int SeekMode;
	if (OPT_GET("Provider/Video/FFmpegSource/Unsafe Seeking")->GetBool())
		SeekMode = FFMS_SEEK_UNSAFE;
	else
		SeekMode = FFMS_SEEK_NORMAL;

	VideoSource = ffms::CreateVideoSource(filename.string().c_str(), TrackNumber, Index, Threads, SeekMode, &ErrInfo);
	if (!VideoSource)
		throw VideoOpenError(std::string("Failed to open video track: ") + ErrInfo.Buffer);

	// load video properties
	VideoInfo = ffms::GetVideoProperties(VideoSource);

	const FFMS_Frame *TempFrame = ffms::GetFrame(VideoSource, 0, &ErrInfo);
	if (!TempFrame)
		throw VideoOpenError(std::string("Failed to decode first frame: ") + ErrInfo.Buffer);

	Width  = TempFrame->EncodedWidth;
	Height = TempFrame->EncodedHeight;
	Rotation = ffms::GetVideoRotation(VideoInfo);
	Flip = ffms::GetVideoFlip(VideoInfo);

	int VideoCS = CS = TempFrame->ColorSpace;
	CR = TempFrame->ColorRange;
	CP = ffms::GetFrameColorPrimaries(TempFrame);
	TC = ffms::GetFrameTransferCharacteristics(TempFrame);
	RealCS = VideoCS;
	RealCR = CR;
	RealCP = CP;
	RealTC = TC;
	NativePixelFormat = TempFrame->EncodedPixelFormat >= 0
		? TempFrame->EncodedPixelFormat
		: TempFrame->ConvertedPixelFormat;
	SourceFrameFormatInfo native_format_info;
	NativeOutputSupported = TryGetFFMSNativeSourceFrameFormatInfo(
		NativePixelFormat,
		GetFFMSNativeFormatIds(),
		native_format_info);

	if (CS == AGI_CS_UNSPECIFIED)
		CS = Width > 1024 || Height >= 600 ? AGI_CS_BT709 : AGI_CS_BT470BG;
	RealColorSpace = ColorSpace = colormatrix_description(CS, CR);

#if FFMS_VERSION >= ((2 << 24) | (17 << 16) | (1 << 8) | 0)
	if (CS != AGI_CS_RGB && CS != AGI_CS_BT470BG && ColorSpace != colormatrix && colormatrix == "TV.601") {
		CS = AGI_CS_BT470BG;
		ColorSpace = colormatrix_description(AGI_CS_BT470BG, CR);
	}

	if (CS != VideoCS) {
		if (ffms::SetInputFormatV(VideoSource, CS, CR, ffms::GetPixFmt(""), &ErrInfo))
			throw VideoOpenError(std::string("Failed to set input format: ") + ErrInfo.Buffer);
	}
#endif

	if (!ConfigureOutputMode(SourceFrameOutputMode::Bgra8))
		throw VideoOpenError(std::string("Failed to set output format: ") + ErrInfo.Buffer);

	// get frame info data
	FFMS_Track *FrameData = ffms::GetTrackFromVideo(VideoSource);
	if (FrameData == nullptr)
		throw VideoOpenError("failed to get frame data");
	const FFMS_TrackTimeBase *TimeBase = ffms::GetTimeBase(FrameData);
	if (TimeBase == nullptr)
		throw VideoOpenError("failed to get track time base");

	// build list of keyframes and timecodes
	std::vector<int> TimecodesVector;
	for (int CurFrameNum = 0; CurFrameNum < VideoInfo->NumFrames; CurFrameNum++) {
		const FFMS_FrameInfo *CurFrameData = ffms::GetFrameInfo(FrameData, CurFrameNum);
		if (!CurFrameData)
			throw VideoOpenError("Couldn't get info about frame " + std::to_string(CurFrameNum));

		// keyframe?
		if (CurFrameData->KeyFrame)
			KeyFramesList.push_back(CurFrameNum);

		// calculate timestamp and add to timecodes vector
		int Timestamp = (int)((CurFrameData->PTS * TimeBase->Num) / TimeBase->Den);
		TimecodesVector.push_back(Timestamp);
	}
	if (TimecodesVector.size() < 2)
		Timecodes = 25.0;
	else
		Timecodes = agi::vfr::Framerate(TimecodesVector);
}

bool FFmpegSourceVideoProvider::ConfigureOutputMode(SourceFrameOutputMode mode) {
	int target_format = -1;
	if (mode == SourceFrameOutputMode::Native) {
		if (!NativeOutputSupported || NativePixelFormat < 0)
			return false;
		target_format = NativePixelFormat;
	}
	else {
		target_format = ffms::GetPixFmt("bgra");
		if (target_format < 0)
			return false;
	}

	const int target_formats[] = { target_format, -1 };
	if (ffms::SetOutputFormatV2(VideoSource, target_formats, Width, Height, FFMS_RESIZER_BICUBIC, &ErrInfo))
		return false;

	OutputMode = mode;
	return true;
}

SourceFrameGeometry FFmpegSourceVideoProvider::GetUnbakedFrameGeometry() const {
	auto geometry = MakeDefaultSourceFrameGeometry(Width, Height);
	geometry.visible_rect = ffms::GetVideoVisibleRect(VideoInfo, Width, Height);
	geometry.rotation = Rotation;
	geometry.display_vflip = Flip < 0;
	if (VideoInfo && VideoInfo->SARNum > 0 && VideoInfo->SARDen > 0)
		geometry.pixel_aspect_ratio = static_cast<double>(VideoInfo->SARNum) / VideoInfo->SARDen;
	return geometry;
}

SourceFrameGeometry FFmpegSourceVideoProvider::GetFrameGeometry() const {
	auto geometry = GetUnbakedFrameGeometry();
	if (OutputMode == SourceFrameOutputMode::Native)
		return geometry;
	return BakeSourceFrameGeometry(geometry);
}

int FFmpegSourceVideoProvider::GetWidth() const {
	auto const geometry = BakeSourceFrameGeometry(GetUnbakedFrameGeometry());
	return geometry.visible_rect.IsValid() ? geometry.visible_rect.width : geometry.storage_width;
}

int FFmpegSourceVideoProvider::GetHeight() const {
	auto const geometry = BakeSourceFrameGeometry(GetUnbakedFrameGeometry());
	return geometry.visible_rect.IsValid() ? geometry.visible_rect.height : geometry.storage_height;
}

double FFmpegSourceVideoProvider::GetDAR() const {
	auto const geometry = BakeSourceFrameGeometry(GetUnbakedFrameGeometry());
	auto const visible = geometry.visible_rect.IsValid()
		? geometry.visible_rect
		: SourceFrameRect{ 0, 0, geometry.storage_width, geometry.storage_height };
	if (visible.width <= 0 || visible.height <= 0)
		return 0.0;
	return static_cast<double>(visible.width) * geometry.pixel_aspect_ratio / visible.height;
}

void FFmpegSourceVideoProvider::GetFrame(int n, VideoFrame &out) {
	n = mid(0, n, GetFrameCount() - 1);

	auto frame = ffms::GetFrame(VideoSource, n, &ErrInfo);
	if (!frame)
		throw VideoDecodeError(std::string("Failed to retrieve frame: ") +  ErrInfo.Buffer);

	out.data.assign(frame->Data[0], frame->Data[0] + frame->Linesize[0] * Height);
	out.flipped = false;
	out.width = Width;
	out.height = Height;
	out.pitch = frame->Linesize[0];
	// Handle flip
	if (Flip > 0)
		for (int x = 0; x < Height; ++x)
			for (int y = 0; y < Width / 2; ++y)
				for (int ch = 0; ch < 4; ++ch)
					std::swap(out.data[frame->Linesize[0] * x + 4 * y + ch], out.data[frame->Linesize[0] * x + 4 * (Width - 1 - y) + ch]);

	else if (Flip < 0)
		for (int x = 0; x < Height / 2; ++x)
			for (int y = 0; y < Width; ++y)
				for (int ch = 0; ch < 4; ++ch)
					std::swap(out.data[frame->Linesize[0] * x + 4 * y + ch], out.data[frame->Linesize[0] * (Height - 1 - x) + 4 * y + ch]);

	// Handle rotation
	if (IsHalfTurn(Rotation)) {
		std::vector<unsigned char> data(std::move(out.data));
		out.data.resize(Width * Height * 4);
		for (int x = 0; x < Height; ++x)
			for (int y = 0; y < Width; ++y)
				for (int ch = 0; ch < 4; ++ch)
					out.data[4 * (Width * x + y) + ch] = data[frame->Linesize[0] * (Height - 1 - x) + 4 * (Width - 1 - y) + ch];
		out.pitch = 4 * Width;
	}
	else if (IsClockwiseQuarterTurn(Rotation)) {
		std::vector<unsigned char> data(std::move(out.data));
		out.data.resize(Width * Height * 4);
		for (int x = 0; x < Width; ++x)
			for (int y = 0; y < Height; ++y)
				for (int ch = 0; ch < 4; ++ch)
					out.data[4 * (Height * x + y) + ch] = data[frame->Linesize[0] * y + 4 * (Width - 1 - x) + ch];
		out.width = Height;
		out.height = Width;
		out.pitch = 4 * Height;
	}
	else if (IsCounterClockwiseQuarterTurn(Rotation)) {
		std::vector<unsigned char> data(std::move(out.data));
		out.data.resize(Width * Height * 4);
		for (int x = 0; x < Width; ++x)
			for (int y = 0; y < Height; ++y)
				for (int ch = 0; ch < 4; ++ch)
					out.data[4 * (Height * x + y) + ch] = data[frame->Linesize[0] * (Height - 1 - y) + 4 * x + ch];
		out.width = Height;
		out.height = Width;
		out.pitch = 4 * Height;
	}
}

bool FFmpegSourceVideoProvider::GetNativeFrame(int n, SourceFrame& out, std::shared_ptr<void>& owner) {
	if (OutputMode != SourceFrameOutputMode::Native)
		return false;

	n = mid(0, n, GetFrameCount() - 1);

	auto frame = ffms::GetFrame(VideoSource, n, &ErrInfo);
	if (!frame)
		throw VideoDecodeError(std::string("Failed to retrieve frame: ") + ErrInfo.Buffer);

	int output_pixfmt = frame->ConvertedPixelFormat >= 0
		? frame->ConvertedPixelFormat
		: frame->EncodedPixelFormat;
	SourceFrameFormatInfo format_info;
	if (!TryGetFFMSNativeSourceFrameFormatInfo(output_pixfmt, GetFFMSNativeFormatIds(), format_info))
		return false;

	struct NativeFrameStorage {
		std::array<std::vector<unsigned char>, 4> planes;
	};

	auto storage = std::make_shared<NativeFrameStorage>();
	int frame_width = frame->ScaledWidth > 0 ? frame->ScaledWidth : Width;
	int frame_height = frame->ScaledHeight > 0 ? frame->ScaledHeight : Height;

	out = { };
	out.output_mode = SourceFrameOutputMode::Native;
	out.native_format = { SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, output_pixfmt };
	out.format_info = format_info;
	out.width = frame_width;
	out.height = frame_height;
	out.flipped = false;
	out.plane_count = format_info.plane_count;
	out.geometry = GetFrameGeometry();
	out.color = ffms_color_metadata(
		frame->ColorSpace >= 0 ? frame->ColorSpace : CS,
		frame->ColorRange >= 0 ? frame->ColorRange : CR,
		ffms::GetFrameColorPrimaries(frame, CP),
		ffms::GetFrameTransferCharacteristics(frame, TC),
		ColorSpace);
	out.chroma_location = ffms::MapChromaLocation(ffms::GetFrameChromaLocation(frame));

	for (int i = 0; i < out.plane_count; ++i) {
		int plane_width = GetSourceFramePlaneWidth(format_info, frame_width, i);
		int plane_height = GetSourceFramePlaneHeight(format_info, frame_height, i);
		auto const& plane_info = format_info.planes[static_cast<size_t>(i)];
		size_t row_bytes = static_cast<size_t>(plane_width) * plane_info.bytes_per_sample;
		storage->planes[static_cast<size_t>(i)].resize(row_bytes * plane_height);

		auto* src = frame->Data[i];
		ptrdiff_t src_stride = frame->Linesize[i];
		if (!src || src_stride == 0)
			return false;
		if (src_stride < 0)
			src += static_cast<ptrdiff_t>(plane_height - 1) * (-src_stride);

		for (int y = 0; y < plane_height; ++y) {
			std::memcpy(
				storage->planes[static_cast<size_t>(i)].data() + row_bytes * y,
				src + static_cast<ptrdiff_t>(y) * src_stride,
				row_bytes);
		}

		out.planes[static_cast<size_t>(i)] = {
			storage->planes[static_cast<size_t>(i)].data(),
			static_cast<ptrdiff_t>(row_bytes),
			plane_width,
			plane_height
		};
	}

	owner = storage;
	return true;
}

SourceFrameColorMetadata FFmpegSourceVideoProvider::GetColorMetadata() const {
	auto color = ffms_color_metadata(CS, CR, CP, TC, ColorSpace);
	if (OutputMode == SourceFrameOutputMode::Bgra8)
		color.range = SourceFrameColorRange::Full;
	return color;
}

SourceFrameColorMetadata FFmpegSourceVideoProvider::GetRealColorMetadata() const {
	auto color = ffms_color_metadata(RealCS, RealCR, RealCP, RealTC, RealColorSpace);
	if (OutputMode == SourceFrameOutputMode::Bgra8)
		color.range = SourceFrameColorRange::Full;
	return color;
}

SourceFrameNativeFormatIdentity FFmpegSourceVideoProvider::GetNativeFormatIdentity() const {
	if (NativePixelFormat < 0)
		return { };
	return {
		SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat,
		NativePixelFormat
	};
}

std::vector<SourceFrameOutputMode> FFmpegSourceVideoProvider::GetAvailableSourceModes() const {
	if (NativeOutputSupported)
		return { SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 };
	return { SourceFrameOutputMode::Bgra8 };
}
}

std::unique_ptr<VideoProvider> CreateFFmpegSourceVideoProvider(agi::fs::path const& path, std::string const& colormatrix, agi::BackgroundRunner *br) {
	return agi::make_unique<FFmpegSourceVideoProvider>(path, colormatrix, br);
}

#endif /* WITH_FFMS2 */
