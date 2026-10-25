#include "VoiceState.hpp"
#include "../utils/Util.hpp"

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
