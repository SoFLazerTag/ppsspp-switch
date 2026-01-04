// Copyright (c) 2013- PPSSPP Project.
// [License Header...]

#include <algorithm>

#include "Common/Serialize/SerializeFuncs.h"
#include "Core/Config.h"
#include "Core/Debugger/MemBlockInfo.h"
#include "Core/HLE/FunctionWrappers.h"
#include "Core/HW/SimpleAudioDec.h"
#include "Core/HW/MediaEngine.h"
#include "Core/HW/BufferQueue.h"

#ifdef USE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include "libavutil/samplefmt.h"
}
#endif  // USE_FFMPEG

// ============================================================================
// 1. GLOBAL SCOPE HELPERS (Linker needs these visible)
// ============================================================================

void AudioClose(SimpleAudio **dec) {
	if (*dec) {
		delete *dec;
		*dec = nullptr;
	}
}

// These must match the 'const' in your header exactly
int SimpleAudio::GetSourcePos() const { return srcPos; }
int SimpleAudio::GetOutSamples() const { return outSamples; }

int SimpleAudio::GetAudioCodecID(int audioType) {
#ifdef USE_FFMPEG
	switch (audioType) {
	case PSP_CODEC_AAC: return AV_CODEC_ID_AAC;
	case PSP_CODEC_AT3: return AV_CODEC_ID_ATRAC3;
	case PSP_CODEC_AT3PLUS: return AV_CODEC_ID_ATRAC3P;
	case PSP_CODEC_MP3: return AV_CODEC_ID_MP3;
	default: return AV_CODEC_ID_NONE;
	}
#else
	return 0;
#endif
}

SimpleAudio::SimpleAudio(int audioType, int sample_rate, int channels)
	: ctxPtr(0xFFFFFFFF), audioType(audioType), sample_rate_(sample_rate), channels_(channels),
	  outSamples(0), srcPos(0), wanted_resample_freq(44100), frame_(0), codec_(0), codecCtx_(0), swrCtx_(0),
	  codecOpen_(false) {
	Init();
}

void SimpleAudio::Init() {
#ifdef USE_FFMPEG
	InitFFmpeg();
	frame_ = av_frame_alloc();
	int audioCodecId = GetAudioCodecID(audioType);
	if (!audioCodecId) return;
	const AVCodec *codec = avcodec_find_decoder((AVCodecID)audioCodecId);
	codec_ = (AVCodec *)codec;
	if (!codec_) return;
	codecCtx_ = avcodec_alloc_context3(codec_);
	if (!codecCtx_) return;
	av_channel_layout_default(&codecCtx_->ch_layout, (channels_ == 2) ? 2 : 1);
	codecOpen_ = false;
#endif
}

bool SimpleAudio::OpenCodec(int block_align) {
#ifdef USE_FFMPEG
	if (codecCtx_->block_align == 0) codecCtx_->block_align = block_align;
	AVDictionary *opts = 0;
	int retval = avcodec_open2(codecCtx_, codec_, &opts);
	av_dict_free(&opts);
	codecOpen_ = true;
	return retval >= 0;
#else
	return false;
#endif
}

void SimpleAudio::SetExtraData(u8 *data, int size, int wav_bytes_per_packet) {
#ifdef USE_FFMPEG
	if (codecCtx_) {
		codecCtx_->extradata = (uint8_t *)av_mallocz(size);
		codecCtx_->extradata_size = size;
		codecCtx_->block_align = wav_bytes_per_packet;
		codecOpen_ = false;
		if (data) memcpy(codecCtx_->extradata, data, size);
	}
#endif
}

void SimpleAudio::SetChannels(int channels) {
#ifdef USE_FFMPEG
	channels_ = channels;
	if (codecCtx_) av_channel_layout_default(&codecCtx_->ch_layout, (channels == 2) ? 2 : 1);
#endif
}

SimpleAudio::~SimpleAudio() {
#ifdef USE_FFMPEG
	swr_free(&swrCtx_);
	av_frame_free(&frame_);
	if (codecCtx_) avcodec_free_context(&codecCtx_);
#endif
}

bool SimpleAudio::IsOK() const {
#ifdef USE_FFMPEG
	return codec_ != 0;
#else
	return false;
#endif
}

bool SimpleAudio::Decode(void *inbuf, int inbytes, uint8_t *outbuf, int *outbytes) {
#ifdef USE_FFMPEG
	if (!codecOpen_) OpenCodec(inbytes);
	AVPacket packet{};
//	av_init_packet(&packet);
	packet.data = static_cast<uint8_t *>(inbuf);
	packet.size = inbytes;
	av_frame_unref(frame_);
	*outbytes = 0;
	srcPos = inbytes;

	if (inbytes != 0) {
		if (avcodec_send_packet(codecCtx_, &packet) < 0) {
			av_packet_unref(&packet);
			return false;
		}
	}
	int err = avcodec_receive_frame(codecCtx_, frame_);
	av_packet_unref(&packet);
	if (err < 0) return true;

	if (!swrCtx_) {
		AVChannelLayout wanted_ch_layout;
		av_channel_layout_default(&wanted_ch_layout, 2);
		swr_alloc_set_opts2(&swrCtx_, &wanted_ch_layout, AV_SAMPLE_FMT_S16, 44100,
		                    &frame_->ch_layout, (AVSampleFormat)frame_->format, frame_->sample_rate, 0, NULL);
		swr_init(swrCtx_);
	}
	int swrRet = swr_convert(swrCtx_, &outbuf, frame_->nb_samples, (const u8 **)frame_->extended_data, frame_->nb_samples);
	outSamples = swrRet * 2;
	*outbytes = outSamples * 2;
	return true;
#else
	*outbytes = 0;
	return true;
#endif
}

static const char *const codecNames[4] = { "AT3+", "AT3", "MP3", "AAC" };
const char *GetCodecName(int codec) {
	if (codec >= PSP_CODEC_AT3PLUS && codec <= PSP_CODEC_AAC) return codecNames[codec - PSP_CODEC_AT3PLUS];
	return "(unk)";
}
bool IsValidCodec(int codec) { return (codec >= PSP_CODEC_AT3PLUS && codec <= PSP_CODEC_AAC); }

// ============================================================================
// 2. AUCTX IMPLEMENTATIONS (Global Scope)
// ============================================================================

AuCtx::AuCtx() : decoder(nullptr) {}
AuCtx::~AuCtx() { if (decoder) AudioClose(&decoder); }

size_t AuCtx::FindNextMp3Sync() {
	if (audioType != PSP_CODEC_MP3 || sourcebuff.size() < 2) return 0;
	for (size_t i = 0; i < sourcebuff.size() - 2; ++i) {
		if ((sourcebuff[i] & 0xFF) == 0xFF && (sourcebuff[i + 1] & 0xC0) == 0xC0) return i;
	}
	return 0;
}

u32 AuCtx::AuDecode(u32 pcmAddr) {
	u32 outptr = PCMBuf + nextOutputHalf * PCMBufSize / 2;
	auto outbuf = Memory::GetPointer(outptr);
	int outpcmbufsize = 0;
	if (pcmAddr) Memory::Write_U32(outptr, pcmAddr);

	if (!sourcebuff.empty()) {
		int nextSync = (int)FindNextMp3Sync();
		decoder->Decode(&sourcebuff[nextSync], (int)sourcebuff.size() - nextSync, outbuf, &outpcmbufsize);
		SumDecodedSamples += decoder->GetOutSamples() / 2;
		int consumed = decoder->GetSourcePos() + nextSync;
		if (consumed > 0) sourcebuff.erase(sourcebuff.begin(), sourcebuff.begin() + consumed);
		AuBufAvailable -= consumed;
	}
	nextOutputHalf ^= 1;
	return outpcmbufsize;
}

u32 AuCtx::AuGetLoopNum() { return LoopNum; }
u32 AuCtx::AuSetLoopNum(int loop) { LoopNum = loop; return 0; }
int AuCtx::AuCheckStreamDataNeeded() { return AuStreamBytesNeeded() > 0 ? 1 : 0; }
int AuCtx::AuStreamBytesNeeded() { return std::min((int)AuBufSize - AuBufAvailable, (int)endPos - (int)readPos); }
int AuCtx::AuStreamWorkareaSize() { return (audioType == PSP_CODEC_MP3) ? 0x05c0 : 0; }

u32 AuCtx::AuNotifyAddStreamData(int size) {
	readPos += size;
	AuBufAvailable += size;
	sourcebuff.resize(sourcebuff.size() + size);
	Memory::MemcpyUnchecked(&sourcebuff[sourcebuff.size() - size], AuBuf + AuStreamWorkareaSize(), size);
	return 0;
}

u32 AuCtx::AuGetInfoToAddStreamData(u32 bufPtr, u32 sizePtr, u32 srcPosPtr) {
	int readsize = AuStreamBytesNeeded();
	if (Memory::IsValidAddress(bufPtr)) Memory::Write_U32(AuBuf + AuStreamWorkareaSize(), bufPtr);
	if (Memory::IsValidAddress(sizePtr)) Memory::Write_U32(readsize, sizePtr);
	if (Memory::IsValidAddress(srcPosPtr)) Memory::Write_U32(readPos, srcPosPtr);
	return 0;
}

u32 AuCtx::AuResetPlayPositionByFrame(int frame) {
	readPos = startPos + (frame * ((MaxOutputSample / 8) * BitRate * 1000)) / SamplingRate;
	SumDecodedSamples = frame * MaxOutputSample;
	AuBufAvailable = 0;
	sourcebuff.clear();
	return 0;
}

u32 AuCtx::AuResetPlayPosition() {
	readPos = startPos;
	SumDecodedSamples = 0;
	AuBufAvailable = 0;
	sourcebuff.clear();
	return 0;
}

void AuCtx::DoState(PointerWrap &p) {
	auto s = p.Section("AuContext", 0, 2);
	if (!s) return;
	Do(p, startPos); Do(p, endPos); Do(p, AuBuf); Do(p, AuBufSize);
	Do(p, PCMBuf); Do(p, PCMBufSize); Do(p, freq); Do(p, SumDecodedSamples);
	Do(p, LoopNum); Do(p, Channels); Do(p, MaxOutputSample); Do(p, readPos);
	Do(p, audioType); Do(p, BitRate); Do(p, SamplingRate); Do(p, askedReadSize);
	int dummy = 0; Do(p, dummy); Do(p, FrameNum);
	Do(p, Version); Do(p, AuBufAvailable); Do(p, sourcebuff); Do(p, nextOutputHalf);
	if (p.mode == p.MODE_READ) decoder = new SimpleAudio(audioType);
}

extern "C" {
    int avpriv_adts_header_parse(const uint8_t *buf, uint32_t *samples, uint8_t *frames) {
        return -1; 
    }
    int av_adts_header_parse(const uint8_t *buf, uint32_t *samples, uint8_t *frames) {
        return -1;
    }
}
