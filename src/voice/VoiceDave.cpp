#include "VoiceDave.hpp"

#include <cstring>
#include <dave/dave.h>
#include "utils/Util.hpp"

static DAVEEncryptorHandle ToEncryptor(void* p) { return reinterpret_cast<DAVEEncryptorHandle>(p); }
static DAVEDecryptorHandle ToDecryptor(void* p) { return reinterpret_cast<DAVEDecryptorHandle>(p); }
static DAVESessionHandle   ToSession(void* p)   { return reinterpret_cast<DAVESessionHandle>(p); }

static void DaveLogSink(DAVELoggingSeverity severity, const char* file, int line, const char* message)
{
	if (severity < DAVE_LOGGING_SEVERITY_WARNING)
		return;

	DbgPrintF("[DAVE] %s:%d: %s", file, line, message);
}

VoiceDave::VoiceDave() :
	m_session(nullptr),
	m_encryptor(nullptr),
	m_selfUser(0),
	m_channel(0),
	m_protocolVersion(0),
	m_bHavePending(false),
	m_pendingVersion(0),
	m_pendingTransition(0),
	m_bMlsFailed(false)
{
	daveSetLogSinkCallback(&DaveLogSink);

	m_encryptor = daveEncryptorCreate();
	m_session = daveSessionCreate(nullptr, nullptr, &VoiceDave::MlsFailureCallback, this);
}

VoiceDave::~VoiceDave()
{
	{
		VoiceAutoLock lock(m_decryptorLock);
		for (auto& pair : m_decryptors) {
			if (pair.second.m_handle)
				daveDecryptorDestroy(ToDecryptor(pair.second.m_handle));
		}
		m_decryptors.clear();
	}

	if (m_encryptor)
		daveEncryptorDestroy(ToEncryptor(m_encryptor));

	if (m_session)
		daveSessionDestroy(ToSession(m_session));
}

int VoiceDave::GetMaxProtocolVersion()
{
	return int(daveMaxSupportedProtocolVersion());
}

void VoiceDave::MlsFailureCallback(const char* source, const char* reason, void* userData)
{
	DbgPrintF("[DAVE] MLS failure in %s: %s", source, reason);

	VoiceDave* pThis = (VoiceDave*)userData;
	pThis->m_bMlsFailed = true;
}

void VoiceDave::Init(Snowflake selfUser, Snowflake channel, uint32_t selfSsrc)
{
	m_selfUser = selfUser;
	m_channel = channel;
	m_selfUserString = std::to_string(selfUser);

	daveEncryptorAssignSsrcToCodec(ToEncryptor(m_encryptor), selfSsrc, DAVE_CODEC_OPUS);
}

void VoiceDave::SetUsers(const std::set<Snowflake>& users)
{
	VoiceAutoLock lock(m_usersLock);
	m_users = users;
}

void VoiceDave::AddUser(Snowflake user)
{
	VoiceAutoLock lock(m_usersLock);
	m_users.insert(user);
}

void VoiceDave::RemoveUser(Snowflake user)
{
	{
		VoiceAutoLock lock(m_usersLock);
		m_users.erase(user);
	}

	VoiceAutoLock lock(m_decryptorLock);
	auto iter = m_decryptors.find(user);
	if (iter != m_decryptors.end()) {
		if (iter->second.m_handle)
			daveDecryptorDestroy(ToDecryptor(iter->second.m_handle));
		m_decryptors.erase(iter);
	}
}

void VoiceDave::GetRecognizedUsers(std::vector<std::string>& strings, std::vector<const char*>& pointers)
{
	{
		VoiceAutoLock lock(m_usersLock);
		for (Snowflake user : m_users)
			strings.push_back(std::to_string(user));
	}

	// We have to be recognized too, whether or not the gateway has told us we're there.
	bool foundSelf = false;
	for (auto& s : strings) {
		if (s == m_selfUserString)
			foundSelf = true;
	}
	if (!foundSelf)
		strings.push_back(m_selfUserString);

	// The strings aren't allowed to move anymore after this.
	for (auto& s : strings)
		pointers.push_back(s.c_str());
}

void VoiceDave::SendKeyPackage(Output& out)
{
	uint8_t* keyPackage = nullptr;
	size_t length = 0;
	daveSessionGetMarshalledKeyPackage(ToSession(m_session), &keyPackage, &length);

	if (keyPackage && length)
	{
		std::vector<uint8_t> message(1 + length);
		message[0] = OP_MLS_KEY_PACKAGE;
		memcpy(message.data() + 1, keyPackage, length);
		out.m_binary.push_back(std::move(message));
	}

	daveFree(keyPackage);
}

void VoiceDave::ReInit(Output& out)
{
	m_bHavePending = false;

	if (m_protocolVersion > 0)
	{
		daveSessionInit(ToSession(m_session), uint16_t(m_protocolVersion), m_channel, m_selfUserString.c_str());
		daveEncryptorSetPassthroughMode(ToEncryptor(m_encryptor), false);
		SendKeyPackage(out);
	}
	else
	{
		daveSessionReset(ToSession(m_session));
		daveEncryptorSetPassthroughMode(ToEncryptor(m_encryptor), true);
		SetAllDecryptorsPassthrough(true);
	}
}

void VoiceDave::ApplyKeysToEncryptor()
{
	DAVEKeyRatchetHandle ratchet = daveSessionGetKeyRatchet(ToSession(m_session), m_selfUserString.c_str());
	if (!ratchet)
		return;

	daveEncryptorSetKeyRatchet(ToEncryptor(m_encryptor), ratchet);
	daveKeyRatchetDestroy(ratchet);
}

void VoiceDave::ApplyKeysToDecryptor(Snowflake user, void* decryptorHandle)
{
	std::string userString = std::to_string(user);
	DAVEKeyRatchetHandle ratchet = daveSessionGetKeyRatchet(ToSession(m_session), userString.c_str());
	if (!ratchet)
		return;

	daveDecryptorTransitionToKeyRatchet(ToDecryptor(decryptorHandle), ratchet);
	daveKeyRatchetDestroy(ratchet);
}

void VoiceDave::ApplyKeysToAllDecryptors()
{
	VoiceAutoLock lock(m_decryptorLock);
	for (auto& pair : m_decryptors)
	{
		ApplyKeysToDecryptor(pair.first, pair.second.m_handle);

		// Unencrypted media is still allowed for a short while, for the senders that haven't switched yet.
		daveDecryptorTransitionToPassthroughMode(ToDecryptor(pair.second.m_handle), false);
	}
}

void VoiceDave::SetAllDecryptorsPassthrough(bool passthrough)
{
	VoiceAutoLock lock(m_decryptorLock);
	for (auto& pair : m_decryptors)
		daveDecryptorTransitionToPassthroughMode(ToDecryptor(pair.second.m_handle), passthrough);
}

void* VoiceDave::GetDecryptor(Snowflake user)
{
	// N.B. m_decryptorLock has to be held by the caller.
	auto iter = m_decryptors.find(user);
	if (iter != m_decryptors.end())
		return iter->second.m_handle;

	Decryptor dec;
	dec.m_handle = daveDecryptorCreate();

	if (m_protocolVersion > 0)
	{
		ApplyKeysToDecryptor(user, dec.m_handle);
		daveDecryptorTransitionToPassthroughMode(ToDecryptor(dec.m_handle), false);
	}
	else
	{
		daveDecryptorTransitionToPassthroughMode(ToDecryptor(dec.m_handle), true);
	}

	m_decryptors[user] = dec;
	return dec.m_handle;
}

void VoiceDave::OnSessionDescription(int protocolVersion, Output& out)
{
	m_protocolVersion = protocolVersion;
	ReInit(out);
}

void VoiceDave::StartTransition(int protocolVersion, int transitionId, Output& out)
{
	m_bHavePending = true;
	m_pendingVersion = protocolVersion;
	m_pendingTransition = transitionId;

	if (transitionId == 0)
		// Transition zero means: do it right away, without waiting for the others.
		OnExecuteTransition(0, out);
	else
		out.m_readyForTransition.push_back(transitionId);
}

void VoiceDave::OnPrepareTransition(int protocolVersion, int transitionId, Output& out)
{
	if (transitionId != 0 && protocolVersion == 0)
		// Someone without DAVE support is joining.  What they send isn't encrypted, so let it through.
		SetAllDecryptorsPassthrough(true);

	StartTransition(protocolVersion, transitionId, out);
}

void VoiceDave::OnExecuteTransition(int transitionId, Output& out)
{
	(void)out;

	if (!m_bHavePending || m_pendingTransition != transitionId)
		return;

	m_bHavePending = false;
	m_protocolVersion = m_pendingVersion;

	if (m_protocolVersion == 0)
	{
		// Back to transport encryption only.
		daveEncryptorSetPassthroughMode(ToEncryptor(m_encryptor), true);
		return;
	}

	daveEncryptorSetPassthroughMode(ToEncryptor(m_encryptor), false);
	ApplyKeysToEncryptor();
	ApplyKeysToAllDecryptors();
}

void VoiceDave::OnPrepareEpoch(int protocolVersion, int epoch, Output& out)
{
	if (epoch != 1)
		return; // we'll be told with a commit that the epoch changed

	// A new group is being created.
	m_protocolVersion = protocolVersion;
	ReInit(out);
}

void VoiceDave::OnExternalSender(const uint8_t* data, size_t size)
{
	daveSessionSetExternalSender(ToSession(m_session), data, size);
}

void VoiceDave::OnProposals(const uint8_t* data, size_t size, Output& out)
{
	std::vector<std::string> strings;
	std::vector<const char*> pointers;
	GetRecognizedUsers(strings, pointers);

	m_bMlsFailed = false;

	uint8_t* commitWelcome = nullptr;
	size_t length = 0;
	daveSessionProcessProposals(
		ToSession(m_session),
		data,
		size,
		pointers.data(),
		pointers.size(),
		&commitWelcome,
		&length
	);

	if (commitWelcome && length)
	{
		std::vector<uint8_t> message(1 + length);
		message[0] = OP_MLS_COMMIT_WELCOME;
		memcpy(message.data() + 1, commitWelcome, length);
		out.m_binary.push_back(std::move(message));
	}

	daveFree(commitWelcome);
}

void VoiceDave::OnAnnounceCommit(int transitionId, const uint8_t* data, size_t size, Output& out)
{
	m_bMlsFailed = false;

	DAVECommitResultHandle result = daveSessionProcessCommit(ToSession(m_session), data, size);

	bool failed = !result || daveCommitResultIsFailed(result);
	bool ignored = result && daveCommitResultIsIgnored(result);

	if (result)
		daveCommitResultDestroy(result);

	if (failed)
	{
		// Tell the server, and start over with a fresh key package.
		out.m_invalidCommitWelcome.push_back(transitionId);
		ReInit(out);
		return;
	}

	if (ignored)
		return;

	StartTransition(int(daveSessionGetProtocolVersion(ToSession(m_session))), transitionId, out);
}

void VoiceDave::OnWelcome(int transitionId, const uint8_t* data, size_t size, Output& out)
{
	std::vector<std::string> strings;
	std::vector<const char*> pointers;
	GetRecognizedUsers(strings, pointers);

	m_bMlsFailed = false;

	DAVEWelcomeResultHandle result = daveSessionProcessWelcome(
		ToSession(m_session),
		data,
		size,
		pointers.data(),
		pointers.size()
	);

	if (!result)
	{
		out.m_invalidCommitWelcome.push_back(transitionId);
		ReInit(out);
		return;
	}

	daveWelcomeResultDestroy(result);

	StartTransition(int(daveSessionGetProtocolVersion(ToSession(m_session))), transitionId, out);
}

bool VoiceDave::CanEncrypt() const
{
	DAVEEncryptorHandle enc = ToEncryptor(m_encryptor);
	return daveEncryptorIsPassthroughMode(enc) || daveEncryptorHasKeyRatchet(enc);
}

bool VoiceDave::Encrypt(uint32_t ssrc, const uint8_t* frame, size_t size, std::vector<uint8_t>& out)
{
	DAVEEncryptorHandle enc = ToEncryptor(m_encryptor);

	out.resize(daveEncryptorGetMaxCiphertextByteSize(enc, DAVE_MEDIA_TYPE_AUDIO, size));

	size_t written = 0;
	DAVEEncryptorResultCode result = daveEncryptorEncrypt(
		enc,
		DAVE_MEDIA_TYPE_AUDIO,
		ssrc,
		frame,
		size,
		out.data(),
		out.size(),
		&written
	);

	if (result != DAVE_ENCRYPTOR_RESULT_CODE_SUCCESS)
		return false;

	out.resize(written);
	return true;
}

bool VoiceDave::Decrypt(Snowflake user, const uint8_t* frame, size_t size, std::vector<uint8_t>& out)
{
	VoiceAutoLock lock(m_decryptorLock);

	DAVEDecryptorHandle dec = ToDecryptor(GetDecryptor(user));

	out.resize(daveDecryptorGetMaxPlaintextByteSize(dec, DAVE_MEDIA_TYPE_AUDIO, size));

	size_t written = 0;
	DAVEDecryptorResultCode result = daveDecryptorDecrypt(
		dec,
		DAVE_MEDIA_TYPE_AUDIO,
		frame,
		size,
		out.data(),
		out.size(),
		&written
	);

	if (result != DAVE_DECRYPTOR_RESULT_CODE_SUCCESS)
		return false;

	out.resize(written);
	return true;
}
