/**
 * @file
 *
 * @author feliwir
 *
 * @brief Class for caching loaded audio samples to reduce file IO.
 *
 * @copyright Thyme is free software: you can redistribute it and/or
 *            modify it under the terms of the GNU General Public License
 *            as published by the Free Software Foundation, either version
 *            2 of the License, or (at your option) any later version.
 *            A full copy of the GNU General Public License can be found in
 *            LICENSE
 */

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "audioeventrts.h"
#include "audiomanager.h"
#include "ffmpegaudiofilecache.h"
#include "filesystem.h"
#include <captainslog.h>
#include <list>

namespace Thyme
{
struct WavHeader
{
    uint8_t riff_id[4] = { 'R', 'I', 'F', 'F' };
    uint32_t chunk_size;
    uint8_t wave_id[4] = { 'W', 'A', 'V', 'E' };
    /* "fmt" sub-chunk */
    uint8_t fmt_id[4] = { 'f', 'm', 't', ' ' }; // FMT header
    uint32_t subchunk1_size = 16; // Size of the fmt chunk
    uint16_t audio_format = 1; // Audio format 1=PCM
    uint16_t channels = 1; // Number of channels 1=Mono 2=Sterio
    uint32_t samples_per_sec = 16000; // Sampling Frequency in Hz
    uint32_t bytes_per_sec = 16000 * 2; // bytes per second
    uint16_t block_align = 2; // 2=16-bit mono, 4=16-bit stereo
    uint16_t bits_per_sample = 16; // Number of bits per sample
    /* "data" sub-chunk */
    uint8_t subchunk2_id[4] = { 'd', 'a', 't', 'a' }; // "data"  string
    uint32_t subchunk2_size; // Sampled data length
};

FFmpegAudioFileCache::~FFmpegAudioFileCache()
{
    ScopedMutexClass lock(&m_mutex);

    for (auto it = m_cacheMap.begin(); it != m_cacheMap.end(); ++it) {
        Release_Open_Audio(&it->second);
    }
}

/**
 * Read an FFmpeg packet from file
 */
int FFmpegAudioFileCache::Read_FFmpeg_Packet(void *opaque, uint8_t *buf, int buf_size)
{
    File *file = static_cast<File *>(opaque);
    int read = file->Read(buf, buf_size);

    // Streaming protocol requires us to return real errors - when we read less equal 0 we're at EOF
    if (read <= 0)
        return AVERROR_EOF;

    return read;
}

/**
 * Open all the required FFmpeg handles for a required file.
 */
bool FFmpegAudioFileCache::Open_FFmpeg_Contexts(FFmpegOpenAudioFile *open_audio, File *file)
{
#if LOGGING_LEVEL != LOGLEVEL_NONE
    av_log_set_level(AV_LOG_INFO);
#endif

// This is required for FFmpeg older than 4.0 -> deprecated afterwards though
#if LIBAVFORMAT_VERSION_MAJOR < 58
    av_register_all();
#endif

    // FFmpeg setup
    open_audio->fmt_ctx = avformat_alloc_context();
    if (!open_audio->fmt_ctx) {
        captainslog_error("Failed to alloc AVFormatContext");
        return false;
    }

    size_t avio_ctx_buffer_size = 0x10000;
    uint8_t *buffer = static_cast<uint8_t *>(av_malloc(avio_ctx_buffer_size));
    if (!buffer) {
        captainslog_error("Failed to alloc AVIOContextBuffer");
        Close_FFmpeg_Contexts(open_audio);
        return false;
    }

    open_audio->avio_ctx = avio_alloc_context(buffer, avio_ctx_buffer_size, 0, file, &Read_FFmpeg_Packet, nullptr, nullptr);
    if (!open_audio->avio_ctx) {
        captainslog_error("Failed to alloc AVIOContext");
        Close_FFmpeg_Contexts(open_audio);
        return false;
    }

    open_audio->fmt_ctx->pb = open_audio->avio_ctx;
    open_audio->fmt_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;

    int result = 0;
    result = avformat_open_input(&open_audio->fmt_ctx, nullptr, nullptr, nullptr);
    if (result < 0) {
        char error_buffer[1024];
        av_strerror(result, error_buffer, sizeof(error_buffer));
        captainslog_error("Failed 'avformat_open_input': %s", error_buffer);
        Close_FFmpeg_Contexts(open_audio);
        return false;
    }

    result = avformat_find_stream_info(open_audio->fmt_ctx, NULL);
    if (result < 0) {
        char error_buffer[1024];
        av_strerror(result, error_buffer, sizeof(error_buffer));
        captainslog_error("Failed 'avformat_find_stream_info': %s", error_buffer);
        Close_FFmpeg_Contexts(open_audio);
        return false;
    }

    if (open_audio->fmt_ctx->nb_streams != 1) {
        captainslog_error("Expected exactly one audio stream per file");
        Close_FFmpeg_Contexts(open_audio);
        return false;
    }

    AVCodec *input_codec = avcodec_find_decoder(open_audio->fmt_ctx->streams[0]->codecpar->codec_id);
    if (!input_codec) {
        captainslog_error("Audio codec not supported: '%u'", open_audio->fmt_ctx->streams[0]->codecpar->codec_tag);
        Close_FFmpeg_Contexts(open_audio);
        return false;
    }

    open_audio->codec_ctx = avcodec_alloc_context3(input_codec);
    if (!open_audio->codec_ctx) {
        captainslog_error("Could not allocate codec context");
        Close_FFmpeg_Contexts(open_audio);
        return false;
    }

    result = avcodec_parameters_to_context(open_audio->codec_ctx, open_audio->fmt_ctx->streams[0]->codecpar);
    if (result < 0) {
        char error_buffer[1024];
        av_strerror(result, error_buffer, sizeof(error_buffer));
        captainslog_error("Failed 'avcodec_parameters_to_context': %s", error_buffer);
        Close_FFmpeg_Contexts(open_audio);
        return false;
    }

    result = avcodec_open2(open_audio->codec_ctx, input_codec, NULL);
    if (result < 0) {
        char error_buffer[1024];
        av_strerror(result, error_buffer, sizeof(error_buffer));
        captainslog_error("Failed 'avcodec_open2': %s", error_buffer);
        Close_FFmpeg_Contexts(open_audio);
        return false;
    }

    return true;
}

/**
 * Decode the input data and append it to our wave data stream
 */
bool FFmpegAudioFileCache::Decode_FFmpeg(FFmpegOpenAudioFile *file)
{
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    int result = 0;

    // Read all packets inside the file
    while (av_read_frame(file->fmt_ctx, packet) >= 0) {
        result = avcodec_send_packet(file->codec_ctx, packet);
        if (result < 0) {
            char error_buffer[1024];
            av_strerror(result, error_buffer, sizeof(error_buffer));
            captainslog_error("Failed 'avcodec_send_packet': %s", error_buffer);
            return false;
        }
        // Decode all frames contained inside the packet
        while (result >= 0) {
            result = avcodec_receive_frame(file->codec_ctx, frame);
            // Check if we need more data
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
                break;
            else if (result < 0) {
                char error_buffer[1024];
                av_strerror(result, error_buffer, sizeof(error_buffer));
                captainslog_error("Failed 'avcodec_receive_frame': %s", error_buffer);
                return false;
            }

            int frame_data_size = av_samples_get_buffer_size(
                NULL, file->codec_ctx->channels, frame->nb_samples, file->codec_ctx->sample_fmt, 1);
            file->wave_data = static_cast<uint8_t *>(av_realloc(file->wave_data, file->data_size + frame_data_size));
            memcpy(file->wave_data + file->data_size, frame->data[0], frame_data_size);
            file->data_size += frame_data_size;
        }

        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    av_frame_free(&frame);

    return true;
}

/**
 * Close all the open FFmpeg handles for an open file.
 */
void FFmpegAudioFileCache::Close_FFmpeg_Contexts(FFmpegOpenAudioFile *open_audio)
{
    if (open_audio->fmt_ctx) {
        avformat_close_input(&open_audio->fmt_ctx);
    }

    if (open_audio->codec_ctx) {
        avcodec_free_context(&open_audio->codec_ctx);
    }

    if (open_audio->avio_ctx->buffer) {
        av_freep(&open_audio->avio_ctx->buffer);
    }

    if (open_audio->avio_ctx) {
        avio_context_free(&open_audio->avio_ctx);
    }
}

void FFmpegAudioFileCache::Fill_Wave_Data(FFmpegOpenAudioFile *open_audio)
{
    WavHeader wav;
    wav.chunk_size = open_audio->data_size - (offsetof(WavHeader, chunk_size) + sizeof(uint32_t));
    wav.subchunk2_size = open_audio->data_size - (offsetof(WavHeader, subchunk2_size) + sizeof(uint32_t));
    wav.channels = open_audio->codec_ctx->channels;
    wav.bits_per_sample = av_get_bytes_per_sample(open_audio->codec_ctx->sample_fmt) * 8;
    wav.samples_per_sec = open_audio->codec_ctx->sample_rate;
    wav.bytes_per_sec = open_audio->codec_ctx->sample_rate * open_audio->codec_ctx->channels * wav.bits_per_sample / 8;
    wav.block_align = open_audio->codec_ctx->channels * wav.bits_per_sample / 8;
    memcpy(open_audio->wave_data, &wav, sizeof(WavHeader));
}

/**
 * Opens an audio file. Reads from the cache if available or loads from file if not.
 */
AudioDataHandle FFmpegAudioFileCache::Open_File(const Utf8String &filename)
{
    ScopedMutexClass lock(&m_mutex);

    captainslog_trace("FFmpegAudioFileCache: opening file %s", filename.Str());

    // Try to find existing data for this file to avoid loading it if unneeded.
    auto it = m_cacheMap.find(filename);

    if (it != m_cacheMap.end()) {
        ++(it->second.ref_count);

        return static_cast<AudioDataHandle>(it->second.wave_data);
    }

    // Load the file from disk
    File *file = g_theFileSystem->Open_File(filename.Str(), File::READ | File::BINARY | File::BUFFERED);

    if (file == nullptr) {
        if (filename.Is_Not_Empty()) {
            captainslog_warn("Missing audio file '%s', could not cache.", filename.Str());
        }

        return nullptr;
    }

    FFmpegOpenAudioFile open_audio;
    open_audio.wave_data = static_cast<uint8_t *>(av_malloc(sizeof(WavHeader)));
    open_audio.data_size = sizeof(WavHeader);

    if (!Open_FFmpeg_Contexts(&open_audio, file)) {
        captainslog_warn("Failed to load audio file '%s', could not cache.", filename.Str());
        Release_Open_Audio(&open_audio);
        file->Close();
        return nullptr;
    }

    if (!Decode_FFmpeg(&open_audio)) {
        captainslog_warn("Failed to decode audio file '%s', could not cache.", filename.Str());
        Close_FFmpeg_Contexts(&open_audio);
        Release_Open_Audio(&open_audio);
        file->Close();
        return nullptr;
    }

    Fill_Wave_Data(&open_audio);
    Close_FFmpeg_Contexts(&open_audio);
    file->Close();

    open_audio.ref_count = 1;
    m_currentSize += open_audio.data_size;

    // m_maxSize prevents using overly large amounts of memory, so if we are over it, unload some other samples.
    if (m_currentSize > m_maxSize && !Free_Space_For_Sample(open_audio)) {
        captainslog_warn("Cannot play audio file since cache is full: %s", filename.Str());
        m_currentSize -= open_audio.data_size;
        Release_Open_Audio(&open_audio);

        return nullptr;
    }

    m_cacheMap[filename] = open_audio;

    return static_cast<AudioDataHandle>(open_audio.wave_data);
}
/**
 * Opens an audio file for an event. Reads from the cache if available or loads from file if not.
 */
AudioDataHandle FFmpegAudioFileCache::Open_File(AudioEventRTS *audio_event)
{
    ScopedMutexClass lock(&m_mutex);
    Utf8String filename;

    // What part of an event are we playing?
    switch (audio_event->Get_Next_Play_Portion()) {
        case 0:
            filename = audio_event->Get_Attack_Name();
            break;
        case 1:
            filename = audio_event->Get_File_Name();
            break;
        case 2:
            filename = audio_event->Get_Decay_Name();
            break;
        case 3:
            return nullptr;
        default:
            break;
    }

    captainslog_trace("FFmpegAudioFileCache: opening file %s", filename.Str());

    // Try to find existing data for this file to avoid loading it if unneeded.
    auto it = m_cacheMap.find(filename);

    if (it != m_cacheMap.end()) {
        ++(it->second.ref_count);

        return static_cast<AudioDataHandle>(it->second.wave_data);
    }

    // Load the file from disk
    File *file = g_theFileSystem->Open_File(filename.Str(), File::READ | File::BINARY | File::BUFFERED);

    if (file == nullptr) {
        if (!filename.Is_Empty()) {
            captainslog_warn("Missing audio file '%s', could not cache.", filename.Str());
        }

        return nullptr;
    }

    FFmpegOpenAudioFile open_audio;
    open_audio.wave_data = static_cast<uint8_t *>(av_malloc(sizeof(WavHeader)));
    open_audio.data_size = sizeof(WavHeader);
    open_audio.audio_event_info = audio_event->Get_Event_Info();

    if (!Open_FFmpeg_Contexts(&open_audio, file)) {
        captainslog_warn("Failed to load audio file '%s', could not cache.", filename.Str());
        return nullptr;
    }

    if (audio_event->Is_Positional_Audio() && open_audio.codec_ctx->channels > 1) {
        captainslog_error("Audio marked as positional audio cannot have more than one channel.");
        return nullptr;
    }

    if (!Decode_FFmpeg(&open_audio)) {
        captainslog_warn("Failed to decode audio file '%s', could not cache.", filename.Str());
        Close_FFmpeg_Contexts(&open_audio);
        return nullptr;
    }

    Fill_Wave_Data(&open_audio);
    Close_FFmpeg_Contexts(&open_audio);

    open_audio.ref_count = 1;
    m_currentSize += open_audio.data_size;

    // m_maxSize prevents using overly large amounts of memory, so if we are over it, unload some other samples.
    if (m_currentSize > m_maxSize && !Free_Space_For_Sample(open_audio)) {
        captainslog_warn("Cannot play audio file since cache is full: %s", filename.Str());
        m_currentSize -= open_audio.data_size;
        Release_Open_Audio(&open_audio);

        return nullptr;
    }

    m_cacheMap[filename] = open_audio;

    return static_cast<AudioDataHandle>(open_audio.wave_data);
}

/**
 * Closes a file, reducing the references to it. Does not actually free the cache.
 */
void FFmpegAudioFileCache::Close_File(AudioDataHandle file)
{
    if (file == nullptr) {
        return;
    }

    ScopedMutexClass lock(&m_mutex);

    for (auto it = m_cacheMap.begin(); it != m_cacheMap.end(); ++it) {
        if (static_cast<AudioDataHandle>(it->second.wave_data) == file) {
            --(it->second.ref_count);

            break;
        }
    }
}

/**
 * Sets the maximum amount of memory in bytes that the cache should use.
 */
void FFmpegAudioFileCache::Set_Max_Size(unsigned size)
{
    ScopedMutexClass lock(&m_mutex);
    m_maxSize = size;
}

/**
 * Attempts to free space by releasing files with no references
 */
unsigned FFmpegAudioFileCache::Free_Space(unsigned required)
{
    std::list<Utf8String> to_free;
    unsigned freed = 0;

    // First check for samples that don't have any references.
    for (const auto &cached : m_cacheMap) {
        if (cached.second.ref_count == 0) {
            to_free.push_back(cached.first);
            freed += cached.second.data_size;

            // If required is "0" we free as much as possible
            if (required && freed >= required) {
                break;
            }
        }
    }

    for (const auto &file : to_free) {
        auto to_remove = m_cacheMap.find(file);

        if (to_remove != m_cacheMap.end()) {
            Release_Open_Audio(&to_remove->second);
            m_currentSize -= to_remove->second.data_size;
            m_cacheMap.erase(to_remove);
        }
    }

    return freed;
}

/**
 * Attempts to free space for a file by releasing files with no references and lower priority sounds.
 */
bool FFmpegAudioFileCache::Free_Space_For_Sample(const FFmpegOpenAudioFile &file)
{
    captainslog_assert(m_currentSize >= m_maxSize); // Assumed to be called only when we need more than allowed.
    std::list<Utf8String> to_free;
    unsigned required = m_currentSize - m_maxSize;
    unsigned freed = 0;

    // First check for samples that don't have any references.
    freed = Free_Space(required);

    // If we still don't have enough potential space freed up, look for lower priority sounds to remove.
    if (freed < required) {
        for (const auto &cached : m_cacheMap) {
            if (cached.second.ref_count != 0
                && cached.second.audio_event_info->Get_Priority() < file.audio_event_info->Get_Priority()) {
                to_free.push_back(cached.first);
                freed += cached.second.data_size;

                if (freed >= required) {
                    break;
                }
            }
        }
    }

    // If we have enough space to free, do the actual freeing, otherwise we didn't succeed, no point bothering.
    if (freed < required) {
        return false;
    }

    for (const auto &file : to_free) {
        auto to_remove = m_cacheMap.find(file);

        if (to_remove != m_cacheMap.end()) {
            Release_Open_Audio(&to_remove->second);
            m_currentSize -= to_remove->second.data_size;
            m_cacheMap.erase(to_remove);
        }
    }

    return true;
}

/**
 * Closes any playing instances of an audio file and then frees the memory for it.
 */
void FFmpegAudioFileCache::Release_Open_Audio(FFmpegOpenAudioFile *open_audio)
{
    // Close any playing samples that use this data.
    if (open_audio->ref_count != 0 && g_theAudio) {
        g_theAudio->Close_Any_Sample_Using_File(open_audio->wave_data);
    }

    // Deallocate the data buffer depending on how it was allocated.
    if (open_audio->wave_data != nullptr) {
        av_freep(&open_audio->wave_data);
        open_audio->audio_event_info = nullptr;
    }
}
} // namespace Thyme
