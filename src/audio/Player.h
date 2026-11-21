#pragma once
#include "audio/Wwise.h"
#include <memory>

// Minimal PCM playback through the Windows wave mapper (WinMM): one clip at a time, pause / seek / volume.
class AudioPlayer {
public:
    ~AudioPlayer();
    bool Play(std::shared_ptr<const AudioPcm> pcm, double fromSeconds = 0);
    void Pause();
    void Resume();
    void Stop();
    void Seek(double seconds);
    void SetVolume(float v);   // 0..1
    float Volume() const { return m_volume; }

    bool Active() const { return m_out != nullptr; }   // playing or paused
    bool Paused() const { return m_paused; }
    bool Finished() const;     // reached the end of the clip
    double Position() const;   // seconds
    const AudioPcm* Clip() const { return m_pcm.get(); }

private:
    void Close();
    void* m_out = nullptr;      // HWAVEOUT
    void* m_header = nullptr;   // WAVEHDR*
    std::shared_ptr<const AudioPcm> m_pcm;
    size_t m_startFrame = 0;
    bool m_paused = false;
    float m_volume = 0.8f;
};
