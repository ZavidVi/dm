#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

// Transport encryption of the RTP packets exchanged with a Discord voice server.
//
// Both supported modes are the "rtpsize" AEAD flavours: the RTP header (including the CSRC list
// and the four byte extension preamble, if present) is sent in the clear but authenticated, the
// rest of the packet is encrypted and followed by the 16 byte tag and a 4 byte nonce counter.
namespace VoiceCrypto
{
	enum eMode
	{
		MODE_NONE,
		MODE_AES256_GCM_RTPSIZE,
		MODE_XCHACHA20_POLY1305_RTPSIZE,
	};

	const size_t KEY_SIZE = 32;
	const size_t RTP_HEADER_SIZE = 12;
	const size_t TAG_SIZE = 16;
	const size_t NONCE_COUNTER_SIZE = 4;

	// Overhead added to an RTP packet by the encryption.
	const size_t PACKET_OVERHEAD = TAG_SIZE + NONCE_COUNTER_SIZE;

	eMode ModeFromString(const std::string& mode);
	const char* ModeToString(eMode mode);

	// Picks the best mode out of the ones the server offers.  Returns MODE_NONE if none is usable.
	eMode ChooseMode(const std::vector<std::string>& offered);

	// Builds an encrypted packet out of a 12 byte RTP header and a payload.
	bool EncryptPacket(
		eMode mode,
		const uint8_t* key,
		uint32_t nonceCounter,
		const uint8_t* rtpHeader,
		const uint8_t* payload,
		size_t payloadSize,
		std::vector<uint8_t>& packetOut
	);

	// Decrypts a received packet.  The decrypted payload (with any RTP header extension and
	// padding already removed) is placed in payloadOut.
	bool DecryptPacket(
		eMode mode,
		const uint8_t* key,
		const uint8_t* packet,
		size_t packetSize,
		std::vector<uint8_t>& payloadOut
	);

	// The building blocks, public so that they can be tested against published test vectors.
	// HChaCha20 turns a key and the first 16 bytes of an XChaCha20 nonce into a subkey.
	void HChaCha20(const uint8_t* key, const uint8_t* nonce16, uint8_t* subkey);

	// Encrypts with an explicit nonce (24 bytes, AES-GCM uses the first 12).  The output is the
	// ciphertext followed by the tag.
	bool SealWithNonce(
		eMode mode,
		const uint8_t* key,
		const uint8_t* nonce24,
		const uint8_t* aad,
		size_t aadSize,
		const uint8_t* plain,
		size_t plainSize,
		std::vector<uint8_t>& out
	);
}
