#pragma once

#include <map>
#include <string>
#include <vector>
#include <nlohmann/json.h>
#include "../models/Snowflake.hpp"

// Everything the voice engine needs to connect to a voice server, collected from the
// VOICE_STATE_UPDATE and VOICE_SERVER_UPDATE gateway events.
struct VoiceConnectParams
{
	Snowflake m_guildId = 0;   // zero for calls in DMs and group DMs
	Snowflake m_channelId = 0;
	Snowflake m_userId = 0;
	std::string m_sessionId;
	std::string m_token;
	std::string m_endpoint;

	// The voice server identifies the session by the guild ID, or by the channel ID in calls.
	Snowflake GetServerId() const {
		return m_guildId ? m_guildId : m_channelId;
	}
};

// State of a single user that is connected to a voice channel, or to a call in a DM or group DM.
struct VoiceState
{
	Snowflake m_userId = 0;
	Snowflake m_guildId = 0;   // zero for calls in DMs and group DMs
	Snowflake m_channelId = 0; // zero if the user is not in voice at all
	bool m_bSelfMute = false;
	bool m_bSelfDeaf = false;
	bool m_bServerMute = false;
	bool m_bServerDeaf = false;
	bool m_bSuppressed = false;

	bool IsMuted() const {
		return m_bSelfMute || m_bServerMute || m_bSuppressed;
	}

	bool IsDeafened() const {
		return m_bSelfDeaf || m_bServerDeaf;
	}

	// Loads the state from a voice state object.  guildHint is used if the object
	// has no guild_id of its own (as in the voice_states arrays inside guild objects).
	void Load(nlohmann::json& j, Snowflake guildHint = 0);
};

// Keeps track of who is sitting in which voice channel.  Not thread safe, only to be
// used from the thread that handles gateway messages.
class VoiceStateManager
{
public:
	// Applies a state.  A state with channel zero means the user has left voice.
	// Returns the channel the user was in before, or zero if they weren't in one.
	Snowflake Update(const VoiceState& state);

	const VoiceState* Get(Snowflake user) const;

	// Gets everyone in a channel, ordered by user ID.
	std::vector<Snowflake> GetUsersInChannel(Snowflake channel) const;

	int GetUserCountInChannel(Snowflake channel) const;

	// Forgets states related to a guild (when leaving it), a channel (when it is
	// deleted or a call ends), or everything (when logging off).
	void RemoveGuild(Snowflake guild);
	void RemoveChannel(Snowflake channel);
	void Clear();

private:
	std::map<Snowflake, VoiceState> m_states;
};
