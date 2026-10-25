#include "VoiceAudio.hpp"

#include <cstring>
#include "utils/Util.hpp"

VoiceAudioIO::VoiceAudioIO() :
	m_pListener(nullptr),
	m_hWaveIn(NULL),
	m_hWaveOut(NULL),
	m_hCaptureEvent(NULL),
	m_hPlaybackEvent(NULL),
	m_hStopEvent(NULL)
{
	memset(m_captureHeaders, 0, sizeof m_captureHeaders);
	memset(m_playbackHeaders, 0, sizeof m_playbackHeaders);
	memset(m_captureData, 0, sizeof m_captureData);
	memset(m_playbackData, 0, sizeof m_playbackData);
}

VoiceAudioIO::~VoiceAudioIO()
{
	Stop();
}

static void FillWaveFormat(WAVEFORMATEX& wfx)
{
	memset(&wfx, 0, sizeof wfx);
	wfx.wFormatTag      = WAVE_FORMAT_PCM;
	wfx.nChannels       = 1;
	wfx.nSamplesPerSec  = VoiceAudioIO::SAMPLE_RATE;
	wfx.wBitsPerSample  = 16;
	wfx.nBlockAlign     = wfx.nChannels * wfx.wBitsPerSample / 8;
	wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
	wfx.cbSize          = 0;
}

bool VoiceAudioIO::OpenCapture()
{
	WAVEFORMATEX wfx;
	FillWaveFormat(wfx);

	MMRESULT mr = waveInOpen(&m_hWaveIn, WAVE_MAPPER, &wfx, (DWORD_PTR)m_hCaptureEvent, 0, CALLBACK_EVENT);
	if (mr != MMSYSERR_NOERROR) {
		DbgPrintF("VoiceAudioIO: waveInOpen failed with %d", int(mr));
		m_hWaveIn = NULL;
		return false;
	}

	for (int i = 0; i < CAPTURE_BUFFERS; i++)
	{
		WAVEHDR& hdr = m_captureHeaders[i];
		memset(&hdr, 0, sizeof hdr);
		hdr.lpData = (LPSTR)m_captureData[i];
		hdr.dwBufferLength = sizeof m_captureData[i];

		if (waveInPrepareHeader(m_hWaveIn, &hdr, sizeof hdr) != MMSYSERR_NOERROR ||
			waveInAddBuffer(m_hWaveIn, &hdr, sizeof hdr) != MMSYSERR_NOERROR)
		{
			DbgPrintF("VoiceAudioIO: couldn't prepare capture buffer %d", i);
			waveInClose(m_hWaveIn);
			m_hWaveIn = NULL;
			return false;
		}
	}

	return waveInStart(m_hWaveIn) == MMSYSERR_NOERROR;
}

bool VoiceAudioIO::OpenPlayback()
{
	WAVEFORMATEX wfx;
	FillWaveFormat(wfx);

	MMRESULT mr = waveOutOpen(&m_hWaveOut, WAVE_MAPPER, &wfx, (DWORD_PTR)m_hPlaybackEvent, 0, CALLBACK_EVENT);
	if (mr != MMSYSERR_NOERROR) {
		DbgPrintF("VoiceAudioIO: waveOutOpen failed with %d", int(mr));
		m_hWaveOut = NULL;
		return false;
	}

	// Start with silence in every buffer; they are refilled as they finish.
	for (int i = 0; i < PLAYBACK_BUFFERS; i++)
	{
		WAVEHDR& hdr = m_playbackHeaders[i];
		memset(&hdr, 0, sizeof hdr);
		memset(m_playbackData[i], 0, sizeof m_playbackData[i]);
		hdr.lpData = (LPSTR)m_playbackData[i];
		hdr.dwBufferLength = sizeof m_playbackData[i];

		if (waveOutPrepareHeader(m_hWaveOut, &hdr, sizeof hdr) != MMSYSERR_NOERROR ||
			waveOutWrite(m_hWaveOut, &hdr, sizeof hdr) != MMSYSERR_NOERROR)
		{
			DbgPrintF("VoiceAudioIO: couldn't prepare playback buffer %d", i);
			waveOutReset(m_hWaveOut);
			waveOutClose(m_hWaveOut);
			m_hWaveOut = NULL;
			return false;
		}
	}

	return true;
}

bool VoiceAudioIO::Start(Listener* pListener)
{
	if (m_thread.IsRunning())
		return true;

	m_pListener = pListener;
	m_hCaptureEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	m_hPlaybackEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	m_hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

	bool bCapture = OpenCapture();
	bool bPlayback = OpenPlayback();

	if (!bCapture && !bPlayback) {
		Close();
		return false;
	}

	if (!m_thread.Start(this, &VoiceAudioIO::ThreadProc)) {
		Close();
		return false;
	}

	return true;
}

void VoiceAudioIO::Stop()
{
	if (m_hStopEvent)
		SetEvent(m_hStopEvent);

	m_thread.Join();
	Close();
}

void VoiceAudioIO::Close()
{
	if (m_hWaveIn)
	{
		waveInStop(m_hWaveIn);
		waveInReset(m_hWaveIn);
		for (int i = 0; i < CAPTURE_BUFFERS; i++)
			waveInUnprepareHeader(m_hWaveIn, &m_captureHeaders[i], sizeof(WAVEHDR));
		waveInClose(m_hWaveIn);
		m_hWaveIn = NULL;
	}

	if (m_hWaveOut)
	{
		waveOutReset(m_hWaveOut);
		for (int i = 0; i < PLAYBACK_BUFFERS; i++)
			waveOutUnprepareHeader(m_hWaveOut, &m_playbackHeaders[i], sizeof(WAVEHDR));
		waveOutClose(m_hWaveOut);
		m_hWaveOut = NULL;
	}

	if (m_hCaptureEvent)  { CloseHandle(m_hCaptureEvent);  m_hCaptureEvent = NULL; }
	if (m_hPlaybackEvent) { CloseHandle(m_hPlaybackEvent); m_hPlaybackEvent = NULL; }
	if (m_hStopEvent)     { CloseHandle(m_hStopEvent);     m_hStopEvent = NULL; }
}

void VoiceAudioIO::ServiceCapture()
{
	for (int i = 0; i < CAPTURE_BUFFERS; i++)
	{
		WAVEHDR& hdr = m_captureHeaders[i];
		if (!(hdr.dwFlags & WHDR_DONE))
			continue;

		if (hdr.dwBytesRecorded == sizeof m_captureData[i])
			m_pListener->OnCapturedFrame(m_captureData[i]);

		hdr.dwFlags &= ~WHDR_DONE;
		hdr.dwBytesRecorded = 0;
		waveInAddBuffer(m_hWaveIn, &hdr, sizeof hdr);
	}
}

void VoiceAudioIO::ServicePlayback()
{
	for (int i = 0; i < PLAYBACK_BUFFERS; i++)
	{
		WAVEHDR& hdr = m_playbackHeaders[i];
		if (!(hdr.dwFlags & WHDR_DONE))
			continue;

		m_pListener->OnPlaybackFrame(m_playbackData[i]);

		hdr.dwFlags &= ~WHDR_DONE;
		waveOutWrite(m_hWaveOut, &hdr, sizeof hdr);
	}
}

void VoiceAudioIO::ThreadProc()
{
	// Voice should be handled promptly, or it'll stutter.
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

	HANDLE handles[3];
	int count = 0;
	handles[count++] = m_hStopEvent;
	int captureIndex = -1, playbackIndex = -1;
	if (m_hWaveIn)  { captureIndex  = count; handles[count++] = m_hCaptureEvent; }
	if (m_hWaveOut) { playbackIndex = count; handles[count++] = m_hPlaybackEvent; }

	for (;;)
	{
		// The timeout is a safety net: events from the wave devices are auto-reset and could be
		// coalesced, so check everything periodically.
		DWORD result = WaitForMultipleObjects(count, handles, FALSE, 20);
		if (result == WAIT_OBJECT_0)
			break;

		if (m_hWaveIn)
			ServiceCapture();

		if (m_hWaveOut)
			ServicePlayback();

		(void)captureIndex;
		(void)playbackIndex;
	}
}
