/*
  Faun macOS backend using CoreAudio (default output AudioUnit).

  Faun pushes mixed audio with blocking writes, while CoreAudio pulls it
  from a render callback.  A single-producer/single-consumer ring buffer
  connects the two.  sysaudio_write() blocks until the ring has room, which
  paces the Faun audio thread the same way the PulseAudio backend does.
*/

#include <AudioToolbox/AudioToolbox.h>
#include <dispatch/dispatch.h>

typedef struct {
    AudioComponentInstance unit;
    dispatch_semaphore_t space;     // Signaled when the callback consumes data.
    uint8_t* ring;
    uint32_t ringMask;              // Ring byte size - 1 (size is power of 2).
    uint32_t limit;                 // Maximum number of bytes to queue.
    _Atomic uint32_t readPos;       // Free-running byte counters.
    _Atomic uint32_t writePos;
    int running;
}
CoreAudioSession;

static CoreAudioSession caSession;

static OSStatus _caRender(void* ref, AudioUnitRenderActionFlags* flags,
                          const AudioTimeStamp* stamp, UInt32 bus,
                          UInt32 frameCount, AudioBufferList* abl)
{
    CoreAudioSession* cs = (CoreAudioSession*) ref;
    uint8_t* out = (uint8_t*) abl->mBuffers[0].mData;
    uint32_t want = abl->mBuffers[0].mDataByteSize;
    uint32_t r = atomic_load_explicit(&cs->readPos, memory_order_relaxed);
    uint32_t w = atomic_load_explicit(&cs->writePos, memory_order_acquire);
    uint32_t n = w - r;
    (void) flags;
    (void) stamp;
    (void) bus;
    (void) frameCount;

    if (n > want)
        n = want;
    if (n) {
        uint32_t pos  = r & cs->ringMask;
        uint32_t part = cs->ringMask + 1 - pos;
        if (part > n)
            part = n;
        memcpy(out, cs->ring + pos, part);
        if (part < n)
            memcpy(out + part, cs->ring, n - part);
        atomic_store_explicit(&cs->readPos, r + n, memory_order_release);
        dispatch_semaphore_signal(cs->space);
    }
    if (n < want)
        memset(out + n, 0, want - n);   // Underrun; output silence.
    return noErr;
}

static const char* sysaudio_open(const char* appName)
{
    (void) appName;
    memset(&caSession, 0, sizeof(caSession));
    return NULL;
}

static void sysaudio_close()
{
}

static const char* sysaudio_allocVoice(FaunVoice* voice, int updateHz,
                                       const char* appName)
{
    CoreAudioSession* cs = &caSession;
    AudioComponentDescription desc;
    AudioComponent comp;
    AudioStreamBasicDescription asbd;
    AURenderCallbackStruct cb;
    uint32_t frameBytes, ringSize;
    int chan = faun_channelCount(voice->mix.chanLayout);
    (void) updateHz;
    (void) appName;

    voice->backend = NULL;

    if (voice->mix.format != FAUN_F32)
        return "CoreAudio backend requires float samples";

    frameBytes = chan * sizeof(float);

    // Queue up to three mix updates (about 60 ms at the default 48 Hz).
    cs->limit = voice->mix.used * frameBytes * 3;
    for (ringSize = 1024; ringSize < cs->limit; ringSize <<= 1)
        ;
    cs->ring = (uint8_t*) malloc(ringSize);
    if (! cs->ring)
        return "No memory for CoreAudio ring buffer";
    cs->ringMask = ringSize - 1;
    atomic_init(&cs->readPos, 0);
    atomic_init(&cs->writePos, 0);

    cs->space = dispatch_semaphore_create(0);

    desc.componentType         = kAudioUnitType_Output;
    desc.componentSubType      = kAudioUnitSubType_DefaultOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    desc.componentFlags        = 0;
    desc.componentFlagsMask    = 0;

    comp = AudioComponentFindNext(NULL, &desc);
    if (! comp)
        goto fail;
    if (AudioComponentInstanceNew(comp, &cs->unit) != noErr) {
        cs->unit = NULL;
        goto fail;
    }

    // Interleaved float samples; the output unit converts to the device rate.
    memset(&asbd, 0, sizeof(asbd));
    asbd.mSampleRate       = voice->mix.rate;
    asbd.mFormatID         = kAudioFormatLinearPCM;
    asbd.mFormatFlags      = kAudioFormatFlagIsFloat |
                             kAudioFormatFlagIsPacked;
    asbd.mBytesPerPacket   = frameBytes;
    asbd.mFramesPerPacket  = 1;
    asbd.mBytesPerFrame    = frameBytes;
    asbd.mChannelsPerFrame = chan;
    asbd.mBitsPerChannel   = 32;

    if (AudioUnitSetProperty(cs->unit, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Input, 0,
                             &asbd, sizeof(asbd)) != noErr)
        goto fail;

    cb.inputProc       = _caRender;
    cb.inputProcRefCon = cs;
    if (AudioUnitSetProperty(cs->unit, kAudioUnitProperty_SetRenderCallback,
                             kAudioUnitScope_Input, 0,
                             &cb, sizeof(cb)) != noErr)
        goto fail;

    if (AudioUnitInitialize(cs->unit) != noErr)
        goto fail;

    if (AudioOutputUnitStart(cs->unit) != noErr) {
        AudioUnitUninitialize(cs->unit);
        goto fail;
    }
    cs->running = 1;

    voice->backend = cs;
    return NULL;

fail:
    if (cs->unit) {
        AudioComponentInstanceDispose(cs->unit);
        cs->unit = NULL;
    }
    dispatch_release(cs->space);
    free(cs->ring);
    cs->ring = NULL;
    return "Cannot open CoreAudio default output";
}

#define CS  ((CoreAudioSession*) voice->backend)

static void sysaudio_freeVoice(FaunVoice* voice)
{
    CoreAudioSession* cs = CS;
    if (cs) {
        AudioOutputUnitStop(cs->unit);
        AudioUnitUninitialize(cs->unit);
        AudioComponentInstanceDispose(cs->unit);
        cs->unit = NULL;
        dispatch_release(cs->space);
        free(cs->ring);
        cs->ring = NULL;
        voice->backend = NULL;
    }
}

static const char* sysaudio_write(FaunVoice* voice, const void* data,
                                  uint32_t len)
{
    CoreAudioSession* cs = CS;
    const uint8_t* src = (const uint8_t*) data;

    while (len) {
        uint32_t w = atomic_load_explicit(&cs->writePos, memory_order_relaxed);
        uint32_t r = atomic_load_explicit(&cs->readPos, memory_order_acquire);
        uint32_t used = w - r;
        uint32_t n, pos, part;

        if (used >= cs->limit) {
            // Wait for the render callback to consume some data.  If the
            // device stops pulling (e.g. it was unplugged), drop the rest
            // rather than hang the audio thread.
            if (dispatch_semaphore_wait(cs->space,
                    dispatch_time(DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC)))
                return NULL;
            continue;
        }

        n = cs->limit - used;
        if (n > len)
            n = len;
        pos  = w & cs->ringMask;
        part = cs->ringMask + 1 - pos;
        if (part > n)
            part = n;
        memcpy(cs->ring + pos, src, part);
        if (part < n)
            memcpy(cs->ring, src + part, n - part);
        atomic_store_explicit(&cs->writePos, w + n, memory_order_release);

        src += n;
        len -= n;
    }
    return NULL;
}

static int sysaudio_startVoice(FaunVoice* voice)
{
    CoreAudioSession* cs = CS;
    if (! cs->running) {
        if (AudioOutputUnitStart(cs->unit) != noErr)
            return 0;
        cs->running = 1;
    }
    return 1;
}

static int sysaudio_stopVoice(FaunVoice* voice)
{
    CoreAudioSession* cs = CS;
    if (cs->running) {
        if (AudioOutputUnitStop(cs->unit) != noErr)
            return 0;
        cs->running = 0;

        // Discard queued audio so stale samples don't play on resume.
        atomic_store(&cs->readPos, atomic_load(&cs->writePos));
    }
    return 1;
}
