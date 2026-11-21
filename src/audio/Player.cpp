#include "audio/Player.h"
#include <windows.h>
#include <mmsystem.h>

AudioPlayer::~AudioPlayer() { Close(); }

void AudioPlayer::Close() {
    if (m_out) {
        HWAVEOUT h = (HWAVEOUT)m_out;
        waveOutReset(h);
        if (m_header) waveOutUnprepareHeader(h, (WAVEHDR*)m_header, sizeof(WAVEHDR));
        waveOutClose(h);
    }
    delete (WAVEHDR*)m_header;
    m_header = nullptr;
    m_out = nullptr;
    m_paused = false;
}

bool AudioPlayer::Play(std::shared_ptr<const AudioPcm> pcm, double from) {
    Close();
    if (!pcm || pcm->channels <= 0 || pcm->sampleRate <= 0 || pcm->Frames() == 0) return false;
    m_pcm = std::move(pcm);
    m_startFrame = std::min(m_pcm->Frames() - 1, (size_t)std::max(0.0, from * m_pcm->sampleRate));
    WAVEFORMATEX fmt = {};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = (WORD)m_pcm->channels;
    fmt.nSamplesPerSec = (DWORD)m_pcm->sampleRate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = (WORD)(fmt.nChannels * 2);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    HWAVEOUT h = nullptr;
    if (waveOutOpen(&h, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) return false;
    m_out = h;
    auto* hdr = new WAVEHDR{};
    hdr->lpData = (LPSTR)(m_pcm->samples.data() + m_startFrame * m_pcm->channels);
    hdr->dwBufferLength = (DWORD)((m_pcm->Frames() - m_startFrame) * fmt.nBlockAlign);
    m_header = hdr;
    SetVolume(m_volume);
    if (waveOutPrepareHeader(h, hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR || waveOutWrite(h, hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
        Close();
        return false;
    }
    return true;
}

void AudioPlayer::Pause() {
    if (m_out && !m_paused) { waveOutPause((HWAVEOUT)m_out); m_paused = true; }
}
void AudioPlayer::Resume() {
    if (m_out && m_paused) { waveOutRestart((HWAVEOUT)m_out); m_paused = false; }
}
void AudioPlayer::Stop() { Close(); }

void AudioPlayer::Seek(double seconds) {
    if (!m_pcm) return;
    bool paused = m_paused;
    auto pcm = m_pcm;
    if (Play(pcm, seconds) && paused) Pause();
}

void AudioPlayer::SetVolume(float v) {
    m_volume = std::clamp(v, 0.f, 1.f);
    if (m_out) {
        DWORD lv = (DWORD)(m_volume * 0xFFFF);
        waveOutSetVolume((HWAVEOUT)m_out, lv | (lv << 16));
    }
}

double AudioPlayer::Position() const {
    if (!m_out || !m_pcm) return 0;
    MMTIME t = {};
    t.wType = TIME_SAMPLES;
    waveOutGetPosition((HWAVEOUT)m_out, &t, sizeof(t));
    size_t frame = m_startFrame + (t.wType == TIME_SAMPLES ? t.u.sample : t.u.cb / (m_pcm->channels * 2));
    return std::min((double)frame, (double)m_pcm->Frames()) / m_pcm->sampleRate;
}

bool AudioPlayer::Finished() const {
    if (!m_out || !m_header) return false;
    return (((WAVEHDR*)m_header)->dwFlags & WHDR_DONE) != 0;
}
