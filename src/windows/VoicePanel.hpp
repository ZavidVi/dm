#pragma once

#include "models/Snowflake.hpp"

// The small window that shows who is in the voice channel or call you are in, who is talking,
// and has the buttons to mute, deafen and disconnect.
namespace VoicePanel
{
	void Initialize();

	// Creates the panel, or brings it forward if it's there.
	void Show();
	void Hide();

	// Brings what's on the panel up to date.  Cheap to call often.
	void Refresh();

	bool IsShown();
}

// Entry points for the menus, the channel list, and the panel itself.
void VoiceUIJoinChannel(Snowflake guild, Snowflake channel);
void VoiceUIStartCall(Snowflake channel);
void VoiceUIHangUp();
void VoiceUIToggleMute();
void VoiceUIToggleDeafen();
