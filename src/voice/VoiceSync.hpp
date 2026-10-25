#pragma once

// Small synchronization helpers for the voice engine.
//
// The rest of the program avoids std::thread and std::mutex so that it can still be built with
// the toolchains for ancient Windows versions, so the voice engine sticks to the Win32 primitives
// as well.

#include <winsock2.h>
#include <windows.h>

class VoiceLock
{
public:
	VoiceLock()  { InitializeCriticalSection(&m_cs); }
	~VoiceLock() { DeleteCriticalSection(&m_cs); }

	void Lock()   { EnterCriticalSection(&m_cs); }
	void Unlock() { LeaveCriticalSection(&m_cs); }

private:
	VoiceLock(const VoiceLock&);
	VoiceLock& operator=(const VoiceLock&);

	CRITICAL_SECTION m_cs;
};

class VoiceAutoLock
{
public:
	explicit VoiceAutoLock(VoiceLock& lock) : m_lock(lock) { m_lock.Lock(); }
	~VoiceAutoLock() { m_lock.Unlock(); }

private:
	VoiceAutoLock(const VoiceAutoLock&);
	VoiceAutoLock& operator=(const VoiceAutoLock&);

	VoiceLock& m_lock;
};

// A thread that runs a member function of T.
template <class T>
class VoiceThread
{
public:
	VoiceThread() : m_hThread(NULL), m_pObject(nullptr), m_pFunc(nullptr) {}
	~VoiceThread() { Join(); }

	bool Start(T* pObject, void (T::*pFunc)())
	{
		if (m_hThread)
			return false;

		m_pObject = pObject;
		m_pFunc = pFunc;

		DWORD id = 0;
		m_hThread = CreateThread(NULL, 0, &VoiceThread::ThreadProc, this, 0, &id);
		return m_hThread != NULL;
	}

	bool IsRunning() const { return m_hThread != NULL; }

	// Waits for the thread to end.  The thread has to be told to stop by other means.
	void Join()
	{
		if (!m_hThread)
			return;

		WaitForSingleObject(m_hThread, INFINITE);
		CloseHandle(m_hThread);
		m_hThread = NULL;
	}

private:
	static DWORD WINAPI ThreadProc(LPVOID param)
	{
		VoiceThread* pThis = (VoiceThread*)param;
		(pThis->m_pObject->*(pThis->m_pFunc))();
		return 0;
	}

	HANDLE m_hThread;
	T* m_pObject;
	void (T::*m_pFunc)();
};
