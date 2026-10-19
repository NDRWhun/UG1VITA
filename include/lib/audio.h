#ifndef LIB_AUDIO_H_
#define LIB_AUDIO_H_
#include <lib/winapp.h>
#include <lib/memmap.h>
#include <platform/sync.h>
#include <atomic>
#include <vector>

namespace win32
{

class AudioBuffer;

// PCM sample layout: 1 or 2 channels, 8-bit unsigned or 16-bit signed samples.
struct AudioFormat
{
    x86::reg32 channels;
    x86::reg32 bitsPerSample;
    x86::reg32 sampleRate;

    x86::reg32 frameBytes() const { return channels * bitsPerSample / 8; }
};

// SceAudioOut port fed by a mixer thread on the audio core, 16-bit stereo. The port runs at the primary buffer's
// rate (SetFormat) when SceAudioOut supports it, else at 48 kHz; buffers at other rates are resampled. Each output
// block is about 20 ms; the port queues one block while the next is mixed, so the hardware is never starved as
// long as the game keeps its buffers filled.
class AudioDevice : public GenericResource
{
    friend class AudioBuffer;
public:
    static constexpr x86::reg32 s_channelCount = 2;
    static constexpr x86::reg32 s_defaultRate = 22050;
    static constexpr x86::reg32 s_maxGrain = 1024;          // frames per output block, at most
    static constexpr x86::sreg32 s_unityGain = 1 << 15;     // Q15 gains
    static constexpr AudioFormat s_outputFormat = { s_channelCount, 16, s_defaultRate };

    AudioDevice();
    ~AudioDevice();

    void play(AudioBuffer* buffer, bool looping);
    void stop(AudioBuffer* buffer);
    pf::Mutex& mutex() { return m_mutex; }
    // Output rate for the primary buffer's format; applied before the next block.
    void setOutputRate(x86::reg32 sampleRate);

private:
    static int threadEntry(void* self);
    void run();
    // Port rate for a requested rate, and frames per block at a rate.
    static x86::reg32 portRate(x86::reg32 sampleRate);
    static x86::reg32 grainFrames(x86::reg32 sampleRate);
    bool configurePort(x86::reg32 rate);

private:
    int                         m_port;
    pf::Mutex                   m_mutex;
    std::vector<AudioBuffer*>   m_playingBuffers;
    x86::sreg32                 m_masterGain[s_channelCount];   // primary buffer volume and pan
    x86::reg32                  m_rate;                         // port rate (audio thread, and m_mutex)
    x86::reg32                  m_grain;                        // frames per block (audio thread)
    std::atomic<x86::reg32>     m_requestedRate;
    std::atomic<bool>           m_running;
    pf::Thread*                 m_thread;
    x86::sreg32                 m_accumulator[s_maxGrain * s_channelCount];
    x86::sreg16                 m_output[s_maxGrain * s_channelCount];
};

// DirectSound-style ring buffer in guest memory. While playing, the mixer reads it from the play
// cursor at the buffer's frequency, resampled to the output rate: a looping buffer wraps until it is
// stopped, any other buffer stops at its end and rewinds. A primary buffer not written for a whole
// lap plays silence. Volume and pan are in hundredths of a decibel.
class AudioBuffer : public GenericResource
{
    friend class AudioDevice;
public:
    static constexpr x86::reg32 s_primaryBufferBytes = 4096 * AudioDevice::s_channelCount * sizeof(x86::sreg16);

    AudioBuffer(AudioDevice* device, WinApplication* app, x86::reg32 bufferSize, const AudioFormat& format, bool primary);
    ~AudioBuffer();

    x86::reg32 bufferSize() const       { return m_bufferSize; }
    bool primary() const                { return m_primary; }
    const AudioFormat& format() const   { return m_format; }
    x86::sreg32 volume() const          { return m_volume; }
    x86::sreg32 pan() const             { return m_pan; }
    x86::reg32 frequency() const        { return m_frequency ? m_frequency : m_format.sampleRate; }
    bool looping() const                { return m_looping; }

    void play(bool looping);
    void stop();
    bool playing() const;
    // Guest address of the buffer memory.
    x86::reg32 lock() const;
    // The game has written new data.
    void unlock();
    // Byte offset of the next frame the mixer reads; everything before it has been mixed.
    x86::reg32 position() const;
    void setPosition(x86::reg32 offset);

    // A primary buffer's format also sets the output rate.
    void setFormat(const AudioFormat& format);
    void setVolume(x86::sreg32 volume);
    void setPan(x86::sreg32 pan);
    // 0 selects the format's sample rate.
    void setFrequency(x86::reg32 frequency);

private:
    // Adds `frames` output frames at outputRate into the stereo accumulator; returns false once a
    // non-looping buffer has reached its end.
    bool mixInto(x86::sreg32* accumulator, x86::reg32 frames, x86::reg32 outputRate, const x86::sreg32* masterGain);
    void updateGain();

private:
    AudioDevice*        m_device;
    x86::reg32          m_bufferSize;
    MemMap*             m_memmap;
    const x86::reg8*    m_data;
    AudioFormat         m_format;
    x86::reg32          m_position;     // play cursor in bytes, frame aligned
    x86::reg32          m_staleFrames;  // frames mixed since the last unlock, up to one lap
    x86::reg32          m_phase;        // fraction of the current source frame, 16 bits
    x86::reg32          m_frequency;
    x86::sreg32         m_volume;
    x86::sreg32         m_pan;
    x86::sreg32         m_gain[AudioDevice::s_channelCount];
    bool                m_playing;
    bool                m_looping;
    bool                m_primary;
};

}

#endif /*!LIB_AUDIO_H_*/
