#pragma once

#include <set>
#include <string>
#include "state/VoiceState.hpp"

// The voice engine as the rest of the program sees it: one voice connection at a time, driven
// from the main thread.
//
// Voice needs Opus, DAVE (libdave), and OpenSSL, and is only part of the build if ENABLE_VOICE is
// defined.  Without it, everything here exists but does nothing, and IsAvailable returns false.
namespace VoiceEngine
{
	enum eState
	{
		STATE_IDLE,
		STATE_CONNECTING,
		STATE_CONNECTED,
		STATE_RECONNECTING,
		STATE_ERROR,
	};

	bool IsAvailable();

	// Starts connecting to a voice server, dropping the connection there was before.
	void Connect(const VoiceConnectParams& params, const std::set<Snowflake>& usersInChannel);

	// Hangs up.
	void Disconnect();

	void SetMuted(bool muted);
	void SetDeafened(bool deafened);

	eState GetState();
	bool HasMicrophone();

	// The reason the last connection failed, if it did.
	std::string GetErrorText();

	// Gets the users who are talking right now.
	void GetSpeakingUsers(std::set<Snowflake>& users);

	// Called on the main thread when the main window gets WM_VOICEENGINESTOPPED.  Returns
	// false if the message is about a connection that is gone already.
	bool OnStopped(unsigned generation, bool bFailed);
}
