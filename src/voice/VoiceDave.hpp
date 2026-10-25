#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "models/Snowflake.hpp"
#include "VoiceSync.hpp"

// DAVE, Discord's end-to-end encryption of voice, as seen from the voice gateway.
//
// This class drives libdave (which does the MLS and the frame encryption) according to the messages
// that arrive on the voice gateway, and says what has to be sent back.  See the DAVE protocol
// whitepaper (github.com/discord/dave-protocol) for the message flow.
class VoiceDave
{
public:
	// Messages that have to be sent to the voice gateway as the result of an event.
	struct Output
	{
		// Binary messages, the opcode byte is already in place.
		std::vector<std::vector<uint8_t> > m_binary;

		// JSON messages: dave_protocol_ready_for_transition (23) ...
		std::vector<int> m_readyForTransition;

		// ... and dave_mls_invalid_commit_welcome (31).
		std::vector<int> m_invalidCommitWelcome;
	};

	// Voice gateway opcodes of the DAVE protocol.
	enum
	{
		OP_PREPARE_TRANSITION         = 21,
		OP_EXECUTE_TRANSITION         = 22,
		OP_READY_FOR_TRANSITION       = 23,
		OP_PREPARE_EPOCH              = 24,
		OP_MLS_EXTERNAL_SENDER        = 25,
		OP_MLS_KEY_PACKAGE            = 26,
		OP_MLS_PROPOSALS              = 27,
		OP_MLS_COMMIT_WELCOME         = 28,
		OP_MLS_ANNOUNCE_COMMIT        = 29,
		OP_MLS_WELCOME                = 30,
		OP_MLS_INVALID_COMMIT_WELCOME = 31,
	};

public:
	VoiceDave();
	~VoiceDave();

	// The highest protocol version this build supports, sent in the Identify message.
	static int GetMaxProtocolVersion();

	// Start of a voice session.
	void Init(Snowflake selfUser, Snowflake channel, uint32_t selfSsrc);

	// Users that are in the call, ourselves included.  Needed to vet the members that get added.
	void SetUsers(const std::set<Snowflake>& users);
	void AddUser(Snowflake user);
	void RemoveUser(Snowflake user);

	// Events from the voice gateway.
	void OnSessionDescription(int protocolVersion, Output& out);
	void OnPrepareTransition(int protocolVersion, int transitionId, Output& out);
	void OnExecuteTransition(int transitionId, Output& out);
	void OnPrepareEpoch(int protocolVersion, int epoch, Output& out);
	void OnExternalSender(const uint8_t* data, size_t size);
	void OnProposals(const uint8_t* data, size_t size, Output& out);
	void OnAnnounceCommit(int transitionId, const uint8_t* data, size_t size, Output& out);
	void OnWelcome(int transitionId, const uint8_t* data, size_t size, Output& out);

	// Media encryption.  Safe to call from the media threads.  Decrypt takes the user that sent the
	// frame, since each sender has their own keys.
	bool Encrypt(uint32_t ssrc, const uint8_t* frame, size_t size, std::vector<uint8_t>& out);
	bool Decrypt(Snowflake user, const uint8_t* frame, size_t size, std::vector<uint8_t>& out);

	// Whether we are able to send media right now.  False while a group is being negotiated.
	bool CanEncrypt() const;

	int GetProtocolVersion() const { return m_protocolVersion; }

private:
	struct Decryptor
	{
		void* m_handle; // DAVEDecryptorHandle
		Decryptor() : m_handle(nullptr) {}
	};

	void ReInit(Output& out);
	void SendKeyPackage(Output& out);
	void ApplyKeysToEncryptor();
	void ApplyKeysToDecryptor(Snowflake user, void* decryptorHandle);
	void ApplyKeysToAllDecryptors();
	void SetAllDecryptorsPassthrough(bool passthrough);
	void StartTransition(int protocolVersion, int transitionId, Output& out);
	void GetRecognizedUsers(std::vector<std::string>& strings, std::vector<const char*>& pointers);
	void* GetDecryptor(Snowflake user);

	static void MlsFailureCallback(const char* source, const char* reason, void* userData);

private:
	void* m_session;    // DAVESessionHandle
	void* m_encryptor;  // DAVEEncryptorHandle

	VoiceLock m_decryptorLock;
	std::map<Snowflake, Decryptor> m_decryptors;

	VoiceLock m_usersLock;
	std::set<Snowflake> m_users;

	Snowflake m_selfUser;
	Snowflake m_channel;
	std::string m_selfUserString;

	int m_protocolVersion;      // the version that is in force
	bool m_bHavePending;
	int m_pendingVersion;
	int m_pendingTransition;
	bool m_bMlsFailed;          // set by the failure callback of libdave
};
