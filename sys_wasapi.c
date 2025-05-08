/*
  Faun WASAPI backend
*/

#define COBJMACROS
#include <mmdeviceapi.h>
#include <audioclient.h>

const CLSID CLSID_MMDeviceEnumerator = {
    0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}
};
const IID   IID_IMMDeviceEnumerator = {
    0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}
};
const IID   IID_IAudioClient = {
    0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}
};
const IID   IID_IAudioRenderClient = {
    0xf294acfc, 0x3146, 0x4483, {0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2}
};


#define LAG_ADJUST

struct {
    IMMDevice* mmdev;
    IAudioClient* client;
    IAudioRenderClient* render;
    UINT32 rbufSize;            // Frame count.
#ifdef LAG_ADJUST
    uint16_t dpMultiple;
    int16_t  fullWrite;
#endif
    WAVEFORMATEX format;
    char errorMsg[80];
}
waSession;


static const char* waError(const char* msg, HRESULT hr)
{
    sprintf(waSession.errorMsg, "%s (0x%08lx)", msg, hr);
    return waSession.errorMsg;
}

static void sysaudio_close();

static const char* sysaudio_open(const char* appName)
{
    IMMDeviceEnumerator* denum;
    void* ptr;
    //WAVEFORMATEX* closest = NULL;
    WAVEFORMATEX* fmt;
    HRESULT hr;
    (void) appName;


    waSession.mmdev  = NULL;
    waSession.client = NULL;
    waSession.render = NULL;
#ifdef LAG_ADJUST
    waSession.dpMultiple = 3;
    waSession.fullWrite = 0;
#endif

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr))
        return "COM initialize failed";

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL,
                          CLSCTX_INPROC_SERVER, &IID_IMMDeviceEnumerator, &ptr);
    if (FAILED(hr))
        return "Create MMDeviceEnumerator failed";

    denum = ptr;
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(denum, eRender,
                                                eMultimedia, &waSession.mmdev);
    IMMDeviceEnumerator_Release(denum);
    if (FAILED(hr))
        return "GetDefaultAudioEndpoint failed";

    hr = IMMDevice_Activate(waSession.mmdev, &IID_IAudioClient,
                            CLSCTX_INPROC_SERVER, NULL, &ptr);
    if(FAILED(hr)) {
        IMMDevice_Release(waSession.mmdev);
        waSession.mmdev = NULL;
        return "IMMDevice_Activate failed";
    }

    waSession.client = ptr;
    //get_device_name_and_guid(waSession.mmdev, &deviceName, NULL);

    fmt = &waSession.format;
    fmt->wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    fmt->nChannels       = 2;
    fmt->nSamplesPerSec  = 44100;
    fmt->nAvgBytesPerSec = 2 * 44100 * sizeof(float);
    fmt->wBitsPerSample  = 32;
    fmt->nBlockAlign     = (fmt->nChannels * fmt->wBitsPerSample) / 8;
    fmt->cbSize          = 0;

#if 0
    hr = IAudioClient_IsFormatSupported(waSession.client,
                                        AUDCLNT_SHAREMODE_SHARED,
                                        fmt, &closest);
    if (hr != S_OK) {
        CoTaskMemFree(closest);
        sysaudio_close();
        return "Audio device does not support float sample format";
    }
#endif

    return NULL;
}

static void sysaudio_close()
{
    if (waSession.client) {
        IAudioClient_Release(waSession.client);
        waSession.client = NULL;
    }

    if (waSession.mmdev) {
        IMMDevice_Release(waSession.mmdev);
        waSession.mmdev = NULL;
    }

    CoUninitialize();
}

static const char* sysaudio_allocVoice(FaunVoice* voice, int updateHz,
                                       const char* appName)
{
    const REFERENCE_TIME oneSecondR = 10000000;
    REFERENCE_TIME bufTime;
    REFERENCE_TIME defPeriod;
    const char* env;
    BYTE* rbuf;
    void* ptr;
    UINT32 frameCount;
    HRESULT hr;
    int32_t mixFrames;
    (void) updateHz;
    (void) appName;


    hr = IAudioClient_GetDevicePeriod(waSession.client, &defPeriod, NULL);
    if (FAILED(hr))
        return waError("GetDevicePeriod failed", hr);

    env = getenv("FAUN_BTIME");
    if (env) {
        int bt, hz;
        sscanf(env, "%d:%d", &bt, &hz);
        printf("Faun bufTime: %d updateHz: %d\n", bt, hz);
        bufTime = bt;
        voice->updateHz = hz;
#ifdef LAG_ADJUST
        waSession.fullWrite = -1;   // Disable lag adjustment.
#endif
    } else {
#ifdef LAG_ADJUST
        bufTime = defPeriod * waSession.dpMultiple;
#else
        bufTime = defPeriod * 3;
#endif

        /* Use the default 48 Hz update.  The update for 100000 * 3 should be
           33.33 Hz, but audio breaks up unless it is 44-48.
        */
        //voice->updateHz = (uint32_t) (oneSecondR / bufTime);
    }

    {
    const DWORD quality = AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    DWORD streamFlags = AUDCLNT_STREAMFLAGS_NOPERSIST |
                        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        quality;
retry:
    hr = IAudioClient_Initialize(waSession.client, AUDCLNT_SHAREMODE_SHARED,
                                 streamFlags,
                                 bufTime, 0, &waSession.format, NULL);
    if (hr == (HRESULT) 0x80070057 && (streamFlags & quality)) {
        // Windows Vista returns E_INVALIDARG with *_SRC_DEFAULT_QUALITY.
        streamFlags &= ~quality;
        goto retry;
    }
    }
    if (FAILED(hr))
        return waError("AudioClient Initialize failed", hr);

    hr = IAudioClient_GetBufferSize(waSession.client, &frameCount);
    if (FAILED(hr))
        return waError("GetBufferSize failed", hr);
    waSession.rbufSize = frameCount;

    // Always mix a multiple of defPeriod (441 frames) even if the client
    // buffer size doesn't have that alignment.
    mixFrames = (int32_t) (bufTime * voice->mix.rate / oneSecondR);
    if ((int) frameCount < mixFrames)
        return "AudioClient buffer frames < mix frames";

#ifdef REPORT
    printf("write_test %s %s\n"
           "IAC bufTime: %I64lld defPeriod: %I64lld\n"
           "IAC bufSize: %d frames\n",
           __DATE__, __TIME__,
           bufTime, defPeriod, frameCount);

    env = getenv("FAUN_MIX");
    if (env) {
        sscanf(env, "%d", &mixFrames);
    }
    printf("Mixer frameCount: %d updateHz: %d\n", mixFrames, voice->updateHz);
#endif

    faun_reserve(&voice->mix, mixFrames);
    voice->mix.used = mixFrames;

    hr = IAudioClient_GetService(waSession.client, &IID_IAudioRenderClient,
                                 &ptr);
    if (FAILED(hr))
        return waError("Get AudioRenderClient failed", hr);
    waSession.render = ptr;

    // Fill buffer with silence.
    hr = IAudioRenderClient_GetBuffer(waSession.render, frameCount, &rbuf);
    if (FAILED(hr))
        return waError("GetBuffer failed", hr);
    IAudioRenderClient_ReleaseBuffer(waSession.render, frameCount,
                                     AUDCLNT_BUFFERFLAGS_SILENT);

    hr = IAudioClient_Start(waSession.client);
    if (FAILED(hr))
        return waError("AudioClient start failed", hr);

    return NULL;
}

static void sysaudio_freeVoice(FaunVoice *voice)
{
    (void) voice;

    if (waSession.render) {
        IAudioRenderClient_Release(waSession.render);
        waSession.render = NULL;
    }
    //voice->backend = NULL;
}

#ifdef LAG_ADJUST
static const char* waSession_adjustBuffer(FaunVoice* voice)
{
    void* ptr;
    HRESULT hr;

    sysaudio_freeVoice(voice);
    IAudioClient_Release(waSession.client);
    waSession.client = NULL;

    // The adjustment is a one-time increase of dpMultiple from 3 to 4.
    waSession.dpMultiple = 4;

    hr = IMMDevice_Activate(waSession.mmdev, &IID_IAudioClient,
                            CLSCTX_INPROC_SERVER, NULL, &ptr);
    if (FAILED(hr))
        return waError("IMMDevice_Activate failed", hr);

    waSession.client = ptr;
    return sysaudio_allocVoice(voice, voice->updateHz, NULL);
}
#endif

#ifdef REPORT
static int writeCycle = 0;
#endif

static const char* sysaudio_write(FaunVoice* voice, const void* data,
                                  uint32_t len)
{
    BYTE* dataPos = (BYTE*) data;
    BYTE* rbuf;
    HRESULT hr;
    UINT32 padding;
    UINT32 favail;
    UINT32 flen = len / waSession.format.nBlockAlign;
    int slept = 0;
#ifdef REPORT
    int32_t stat[22];
    int statUsed = 0;
#endif
    (void) voice;


    while (flen) {
        hr = IAudioClient_GetCurrentPadding(waSession.client, &padding);
        if (FAILED(hr))
            return waError("GetCurrentPadding failed", hr);

        favail = waSession.rbufSize - padding;
#ifdef REPORT
        stat[statUsed++] = favail;
#endif
        if (favail > 0) {
            if (favail > flen) {
#ifdef REPORT
                stat[statUsed++] = (int32_t) flen - favail;
#endif
                favail = flen;
            }
            len = favail * waSession.format.nBlockAlign;

            hr = IAudioRenderClient_GetBuffer(waSession.render, favail, &rbuf);
            if (FAILED(hr))
                return waError("GetBuffer failed", hr);
            memcpy(rbuf, dataPos, len);
            IAudioRenderClient_ReleaseBuffer(waSession.render, favail, 0);

            dataPos += len;
            flen -= favail;
        } else {
            Sleep(2);
            ++slept;
        }
    }

#ifdef REPORT
    printf("write %d [", writeCycle++);
    for (int i = 0; i < statUsed; ++i)
        printf("%d ", stat[i]);
    printf("]\n");
#endif

#ifdef LAG_ADJUST
    // We know audio is being delivered on schedule only if a Sleep is
    // required.  Therefore, the client buffer is increased if no Sleep occurs
    // within 5 sequential sysaudio_write calls.

    // if (writeCycle == 90) { waSession.fullWrite = 4; slept = 0; }
    if (waSession.fullWrite >= 0) {
        if (slept) {
            waSession.fullWrite = 0;
        } else {
            if (++waSession.fullWrite == 5) {
                waSession.fullWrite = -1;   // Disable lag adjustment.
                fprintf(_errStream, "Faun sysaudio_write buffer adjusted\n");
                return waSession_adjustBuffer(voice);
            }
        }
    }
#endif

    return NULL;
}

static int sysaudio_startVoice(FaunVoice *voice)
{
    (void) voice;
    IAudioClient_Start(waSession.client);
    return 1;
}

static int sysaudio_stopVoice(FaunVoice *voice)
{
    (void) voice;
    IAudioClient_Stop(waSession.client);
    return 0;
}
