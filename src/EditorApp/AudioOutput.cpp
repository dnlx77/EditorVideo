#include "EditorApp/AudioOutput.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cstring>
#include <vector>

#pragma comment(lib, "winmm.lib")

namespace editor::app {

namespace {

// 8 blocchi da 40 ms: circa un terzo di secondo di audio in coda, abbastanza per non avere buchi.
constexpr int kBufferCount = 8;
constexpr int kBufferMilliseconds = 40;

struct Buffer
{
    WAVEHDR header = {};
    std::vector<char> data;
    bool submitted = false;
};

}  // namespace

struct AudioOutput::Impl
{
    HWAVEOUT device = nullptr;
    HANDLE event = nullptr;
    Buffer buffers[kBufferCount];
    int current = 0;
    std::size_t fill = 0;  // byte già scritti nel blocco corrente
    int rate = 0;
    int channels = 0;
    // Campioni per canale mandati alla scheda audio dall'ultimo reset: la posizione non può superarli.
    std::atomic<long long> submittedFrames{0};

    bool isFree(Buffer& buffer)
    {
        if (buffer.submitted && (buffer.header.dwFlags & WHDR_DONE) == 0)
            return false;
        if (buffer.submitted) {
            waveOutUnprepareHeader(device, &buffer.header, sizeof(WAVEHDR));
            buffer.submitted = false;
        }
        return true;
    }

    void submit(Buffer& buffer)
    {
        buffer.header = {};
        buffer.header.lpData = buffer.data.data();
        buffer.header.dwBufferLength = static_cast<DWORD>(fill);
        waveOutPrepareHeader(device, &buffer.header, sizeof(WAVEHDR));
        buffer.submitted = true;
        waveOutWrite(device, &buffer.header, sizeof(WAVEHDR));
        submittedFrames += static_cast<long long>(fill / (channels * 2));
        current = (current + 1) % kBufferCount;
        fill = 0;
    }
};

AudioOutput::AudioOutput()
    : impl_(std::make_unique<Impl>())
{
}

AudioOutput::~AudioOutput()
{
    close();
}

bool AudioOutput::open(int sampleRate, int channels)
{
    close();
    Impl& d = *impl_;

    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = static_cast<WORD>(channels);
    format.nSamplesPerSec = static_cast<DWORD>(sampleRate);
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(channels * 2);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    d.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (d.event == nullptr)
        return false;
    if (waveOutOpen(&d.device, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(d.event), 0, CALLBACK_EVENT)
        != MMSYSERR_NOERROR) {
        d.device = nullptr;
        CloseHandle(d.event);
        d.event = nullptr;
        return false;
    }

    d.rate = sampleRate;
    d.channels = channels;
    const std::size_t bytes = static_cast<std::size_t>(sampleRate) * kBufferMilliseconds / 1000 * format.nBlockAlign;
    for (Buffer& buffer : d.buffers) {
        buffer.data.assign(bytes, 0);
        buffer.submitted = false;
    }
    d.current = 0;
    d.fill = 0;
    return true;
}

void AudioOutput::close()
{
    Impl& d = *impl_;
    if (d.device != nullptr) {
        reset();
        waveOutClose(d.device);
        d.device = nullptr;
    }
    if (d.event != nullptr) {
        CloseHandle(d.event);
        d.event = nullptr;
    }
}

bool AudioOutput::isOpen() const
{
    return impl_->device != nullptr;
}

int AudioOutput::sampleRate() const
{
    return impl_->rate;
}

bool AudioOutput::write(const std::int16_t* samples, std::size_t count, const std::atomic<bool>& stop)
{
    Impl& d = *impl_;
    const char* src = reinterpret_cast<const char*>(samples);
    std::size_t remaining = count * sizeof(std::int16_t);

    while (remaining > 0) {
        Buffer& buffer = d.buffers[d.current];
        // Aspettiamo che la scheda audio liberi il blocco, controllando spesso se bisogna fermarsi.
        while (!d.isFree(buffer)) {
            if (stop.load())
                return false;
            WaitForSingleObject(d.event, 20);
        }
        if (stop.load())
            return false;

        const std::size_t chunk = std::min(remaining, buffer.data.size() - d.fill);
        std::memcpy(buffer.data.data() + d.fill, src, chunk);
        d.fill += chunk;
        src += chunk;
        remaining -= chunk;
        if (d.fill == buffer.data.size())
            d.submit(buffer);
    }
    return true;
}

void AudioOutput::flush()
{
    Impl& d = *impl_;
    if (d.device != nullptr && d.fill > 0 && d.isFree(d.buffers[d.current]))
        d.submit(d.buffers[d.current]);
}

long long AudioOutput::playedFrames() const
{
    const Impl& d = *impl_;
    if (d.device == nullptr)
        return 0;
    MMTIME time = {};
    time.wType = TIME_SAMPLES;
    if (waveOutGetPosition(d.device, &time, sizeof(time)) != MMSYSERR_NOERROR)
        return 0;
    long long frames = 0;
    if (time.wType == TIME_SAMPLES)
        frames = time.u.sample;
    else if (time.wType == TIME_BYTES)
        frames = time.u.cb / (d.channels * 2);
    else
        return 0;  // il driver non sa dire la posizione in campioni o byte
    // Alcuni driver, subito dopo un reset, riportano per un attimo una posizione vecchia: non si può aver suonato
    // più di quanto è stato mandato.
    return std::min(frames, d.submittedFrames.load());
}

bool AudioOutput::drained() const
{
    const Impl& d = *impl_;
    if (d.fill > 0)
        return false;
    return std::all_of(std::begin(d.buffers), std::end(d.buffers), [](const Buffer& buffer) {
        return !buffer.submitted || (buffer.header.dwFlags & WHDR_DONE) != 0;
    });
}

void AudioOutput::reset()
{
    Impl& d = *impl_;
    if (d.device == nullptr)
        return;
    waveOutReset(d.device);  // segna tutti i blocchi come suonati e azzera la posizione
    d.submittedFrames = 0;
    for (Buffer& buffer : d.buffers)
        d.isFree(buffer);
    d.current = 0;
    d.fill = 0;
}

}  // namespace editor::app
