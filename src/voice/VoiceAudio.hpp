#pragma once

#include <cstdint>
#include "VoiceSync.hpp"
#include <mmsystem.h>

// Audio capture and playback for voice chat, on top of the waveIn / waveOut APIs of winmm, which
// exist on every version of Windows this program runs on.
//
// Everything is 48 kHz, 16 bit, mono, in blocks of 20 ms (one Opus frame).  The wave mapper takes
// care of converting from and to whatever the sound hardware speaks.
class VoiceAudioIO
{
public:
	static const int SAMPLE_RATE = 48000;
	static const int FRAME_SAMPLES = 960;   // 20 ms

	class Listener
	{
	public:
		virtual ~Listener() {}

		// A block of microphone input is available.  Called on the audio thread.
		virtual void OnCapturedFrame(const int16_t* samples) = 0;

		// Fill a block of audio for the speakers.  Called on the audio thread.
		virtual void OnPlaybackFrame(int16_t* samples) = 0;
	};

public:
	VoiceAudioIO();
	~VoiceAudioIO();

	// Opens the devices and starts the audio thread.  Fails only if neither a microphone nor
	// speakers could be opened.
	bool Start(Listener* pListener);
	void Stop();

	bool HasCapture() const  { return m_hWaveIn != NULL; }
	bool HasPlayback() const { return m_hWaveOut != NULL; }

private:
	static const int CAPTURE_BUFFERS  = 4;
	static const int PLAYBACK_BUFFERS = 6;

	bool OpenCapture();
	bool OpenPlayback();
	void Close();
	void ThreadProc();
	void ServiceCapture();
	void ServicePlayback();

private:
	Listener* m_pListener;
	HWAVEIN m_hWaveIn;
	HWAVEOUT m_hWaveOut;
	HANDLE m_hCaptureEvent;
	HANDLE m_hPlaybackEvent;
	HANDLE m_hStopEvent;
	VoiceThread<VoiceAudioIO> m_thread;

	WAVEHDR m_captureHeaders[CAPTURE_BUFFERS];
	WAVEHDR m_playbackHeaders[PLAYBACK_BUFFERS];
	int16_t m_captureData[CAPTURE_BUFFERS][FRAME_SAMPLES];
	int16_t m_playbackData[PLAYBACK_BUFFERS][FRAME_SAMPLES];
};
