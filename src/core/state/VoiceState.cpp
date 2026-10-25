#include "VoiceState.hpp"
#include "../utils/Util.hpp"

#ifdef _WIN32
#include <windows.h>
#include <ctime>
#include <cstdio>

static bool VoiceDebugEnabled(std::string& logPath)
{
	static int enabled = -1;
	static std::string path;

	if (enabled < 0)
	{
		char exe[MAX_PATH];
		DWORD len = GetModuleFileNameA(NULL, exe, MAX_PATH);
		std::string dir(exe, len);
		size_t slash = dir.find_last_of("\\/");
		dir = slash == std::string::npos ? std::string() : dir.substr(0, slash + 1);

		enabled = GetFileAttributesA((dir + "voice-debug.enabled").c_str()) != INVALID_FILE_ATTRIBUTES ? 1 : 0;
		path = dir + "voice-debug.log";
	}

	logPath = path;
	return enabled == 1;
}

void VoiceDebugLogPayload(const std::string& code, const std::string& payload)
{
	std::string path;
	if (!VoiceDebugEnabled(path))
		return;

	std::string text;
	bool bVoiceEvent = code.compare(0, 6, "VOICE_") == 0 || code.compare(0, 5, "CALL_") == 0;

	if (bVoiceEvent)
	{
		text = payload.substr(0, 3000);
	}
	else
	{
		// Other events are of interest only for the voice states they might carry, and where they are.
		size_t pos = 0;
		int found = 0;
		while ((pos = payload.find("voice_states", pos)) != std::string::npos && found < 6)
		{
			size_t from = pos > 120 ? pos - 120 : 0;
			text += "\n    ..." + payload.substr(from, 600) + "...";
			pos += 12;
			found++;
		}

		if (found == 0)
			return;
	}

	FILE* f = fopen(path.c_str(), "ab");
	if (!f)
		return;

	time_t now = time(NULL);
	char stamp[32];
	strftime(stamp, sizeof stamp, "%H:%M:%S", localtime(&now));
	fprintf(f, "[%s] %s (%zu bytes) %s\n", stamp, code.c_str(), payload.size(), text.c_str());
	fclose(f);
}
#else
void VoiceDebugLogPayload(const std::string&, const std::string&) {}
#endif

void VoiceState::Load(nlohmann::json& j, Snowflake guildHint)
{
	m_userId    = GetSnowflake(j, "user_id");
	m_guildId   = j.contains("guild_id") ? GetSnowflake(j, "guild_id") : guildHint;
	m_channelId = GetSnowflake(j, "channel_id");

	m_bSelfMute   = GetFieldSafeBool(j, "self_mute", false);
	m_bSelfDeaf   = GetFieldSafeBool(j, "self_deaf", false);
	m_bServerMute = GetFieldSafeBool(j, "mute", false);
	m_bServerDeaf = GetFieldSafeBool(j, "deaf", false);
	m_bSuppressed = GetFieldSafeBool(j, "suppress", false);
}

Snowflake VoiceStateManager::Update(const VoiceState& state)
{
	Snowflake oldChannel = 0;

	auto iter = m_states.find(state.m_userId);
	if (iter != m_states.end())
		oldChannel = iter->second.m_channelId;

	if (state.m_channelId == 0) {
		if (iter != m_states.end())
			m_states.erase(iter);
	}
	else {
		m_states[state.m_userId] = state;
	}

	return oldChannel;
}

const VoiceState* VoiceStateManager::Get(Snowflake user) const
{
	auto iter = m_states.find(user);
	if (iter == m_states.end())
		return nullptr;

	return &iter->second;
}

std::vector<Snowflake> VoiceStateManager::GetUsersInChannel(Snowflake channel) const
{
	std::vector<Snowflake> users;
	for (auto& pair : m_states) {
		if (pair.second.m_channelId == channel)
			users.push_back(pair.first);
	}
	return users;
}

int VoiceStateManager::GetUserCountInChannel(Snowflake channel) const
{
	int count = 0;
	for (auto& pair : m_states) {
		if (pair.second.m_channelId == channel)
			count++;
	}
	return count;
}

std::set<Snowflake> VoiceStateManager::GetChannelsOfGuild(Snowflake guild) const
{
	std::set<Snowflake> channels;
	for (auto& pair : m_states) {
		if (pair.second.m_guildId == guild)
			channels.insert(pair.second.m_channelId);
	}
	return channels;
}

void VoiceStateManager::RemoveGuild(Snowflake guild)
{
	for (auto iter = m_states.begin(); iter != m_states.end();) {
		if (iter->second.m_guildId == guild)
			iter = m_states.erase(iter);
		else
			++iter;
	}
}

void VoiceStateManager::RemoveChannel(Snowflake channel)
{
	for (auto iter = m_states.begin(); iter != m_states.end();) {
		if (iter->second.m_channelId == channel)
			iter = m_states.erase(iter);
		else
			++iter;
	}
}

void VoiceStateManager::Clear()
{
	m_states.clear();
}
