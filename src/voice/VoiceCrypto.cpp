#include "VoiceCrypto.hpp"

#include <cstring>
#include <openssl/evp.h>

namespace VoiceCrypto
{

eMode ModeFromString(const std::string& mode)
{
	if (mode == "aead_aes256_gcm_rtpsize")        return MODE_AES256_GCM_RTPSIZE;
	if (mode == "aead_xchacha20_poly1305_rtpsize") return MODE_XCHACHA20_POLY1305_RTPSIZE;
	return MODE_NONE;
}

const char* ModeToString(eMode mode)
{
	switch (mode) {
		case MODE_AES256_GCM_RTPSIZE:         return "aead_aes256_gcm_rtpsize";
		case MODE_XCHACHA20_POLY1305_RTPSIZE: return "aead_xchacha20_poly1305_rtpsize";
		default:                              return "";
	}
}

eMode ChooseMode(const std::vector<std::string>& offered)
{
	// AES-GCM is preferred when it is offered, as the servers ask for.
	eMode best = MODE_NONE;
	for (auto& name : offered) {
		eMode mode = ModeFromString(name);
		if (mode == MODE_AES256_GCM_RTPSIZE)
			return mode;
		if (mode != MODE_NONE)
			best = mode;
	}
	return best;
}

// ---- XChaCha20 ----

static inline uint32_t Rotl32(uint32_t v, int n)
{
	return (v << n) | (v >> (32 - n));
}

static inline uint32_t Load32LE(const uint8_t* p)
{
	return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

static inline void Store32LE(uint8_t* p, uint32_t v)
{
	p[0] = uint8_t(v);
	p[1] = uint8_t(v >> 8);
	p[2] = uint8_t(v >> 16);
	p[3] = uint8_t(v >> 24);
}

#define QUARTER_ROUND(a, b, c, d) \
	a += b; d ^= a; d = Rotl32(d, 16); \
	c += d; b ^= c; b = Rotl32(b, 12); \
	a += b; d ^= a; d = Rotl32(d, 8);  \
	c += d; b ^= c; b = Rotl32(b, 7);

// Derives the subkey of XChaCha20 from the key and the first 16 bytes of the nonce.
void HChaCha20(const uint8_t* key, const uint8_t* nonce16, uint8_t* subkey)
{
	uint32_t x[16];
	x[0] = 0x61707865;
	x[1] = 0x3320646e;
	x[2] = 0x79622d32;
	x[3] = 0x6b206574;
	for (int i = 0; i < 8; i++)
		x[4 + i] = Load32LE(key + i * 4);
	for (int i = 0; i < 4; i++)
		x[12 + i] = Load32LE(nonce16 + i * 4);

	for (int i = 0; i < 10; i++)
	{
		QUARTER_ROUND(x[0], x[4], x[8],  x[12]);
		QUARTER_ROUND(x[1], x[5], x[9],  x[13]);
		QUARTER_ROUND(x[2], x[6], x[10], x[14]);
		QUARTER_ROUND(x[3], x[7], x[11], x[15]);
		QUARTER_ROUND(x[0], x[5], x[10], x[15]);
		QUARTER_ROUND(x[1], x[6], x[11], x[12]);
		QUARTER_ROUND(x[2], x[7], x[8],  x[13]);
		QUARTER_ROUND(x[3], x[4], x[9],  x[14]);
	}

	for (int i = 0; i < 4; i++)
		Store32LE(subkey + i * 4, x[i]);
	for (int i = 0; i < 4; i++)
		Store32LE(subkey + 16 + i * 4, x[12 + i]);
}

#undef QUARTER_ROUND

// ---- AEAD ----

class CipherContext
{
public:
	CipherContext() : m_ctx(EVP_CIPHER_CTX_new()) {}
	~CipherContext() { if (m_ctx) EVP_CIPHER_CTX_free(m_ctx); }
	EVP_CIPHER_CTX* Get() { return m_ctx; }

private:
	CipherContext(const CipherContext&);
	CipherContext& operator=(const CipherContext&);

	EVP_CIPHER_CTX* m_ctx;
};

// Makes the full nonce out of the four byte counter that goes with each packet: the counter,
// followed by zeros.  AES-GCM uses the first 12 bytes of it, XChaCha20 all 24.
static void MakeNonce(const uint8_t* counter, uint8_t* nonce24)
{
	memset(nonce24, 0, 24);
	memcpy(nonce24, counter, NONCE_COUNTER_SIZE);
}

// Prepares the cipher for a given mode, key and nonce.
static bool InitCipher(
	CipherContext& cc, bool encrypt, eMode mode, const uint8_t* key, const uint8_t* nonce24)
{
	EVP_CIPHER_CTX* ctx = cc.Get();
	if (!ctx)
		return false;

	uint8_t iv[12];
	uint8_t subkey[32];
	const uint8_t* useKey = key;
	const EVP_CIPHER* cipher = NULL;

	memset(iv, 0, sizeof iv);

	if (mode == MODE_AES256_GCM_RTPSIZE)
	{
		cipher = EVP_aes_256_gcm();
		memcpy(iv, nonce24, sizeof iv);
	}
	else if (mode == MODE_XCHACHA20_POLY1305_RTPSIZE)
	{
		HChaCha20(key, nonce24, subkey);
		useKey = subkey;

		// Then it's ChaCha20-Poly1305 with 4 zero bytes followed by the last 8 bytes of the nonce.
		memcpy(iv + 4, nonce24 + 16, 8);
		cipher = EVP_chacha20_poly1305();
	}
	else
	{
		return false;
	}

	int ivLen = int(sizeof iv);
	bool ok =
		EVP_CipherInit_ex(ctx, cipher, NULL, NULL, NULL, encrypt ? 1 : 0) == 1 &&
		EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, ivLen, NULL) == 1 &&
		EVP_CipherInit_ex(ctx, NULL, NULL, useKey, iv, encrypt ? 1 : 0) == 1;

	memset(subkey, 0, sizeof subkey);
	return ok;
}

bool EncryptPacket(
	eMode mode,
	const uint8_t* key,
	uint32_t nonceCounter,
	const uint8_t* rtpHeader,
	const uint8_t* payload,
	size_t payloadSize,
	std::vector<uint8_t>& packetOut)
{
	uint8_t counter[NONCE_COUNTER_SIZE];
	counter[0] = uint8_t(nonceCounter >> 24);
	counter[1] = uint8_t(nonceCounter >> 16);
	counter[2] = uint8_t(nonceCounter >> 8);
	counter[3] = uint8_t(nonceCounter);

	uint8_t nonce[24];
	MakeNonce(counter, nonce);

	CipherContext cc;
	if (!InitCipher(cc, true, mode, key, nonce))
		return false;

	EVP_CIPHER_CTX* ctx = cc.Get();

	packetOut.resize(RTP_HEADER_SIZE + payloadSize + PACKET_OVERHEAD);
	memcpy(packetOut.data(), rtpHeader, RTP_HEADER_SIZE);

	int len = 0;
	if (EVP_CipherUpdate(ctx, NULL, &len, rtpHeader, int(RTP_HEADER_SIZE)) != 1)
		return false;

	uint8_t* cipherText = packetOut.data() + RTP_HEADER_SIZE;
	int cipherLen = 0;
	if (payloadSize != 0)
	{
		if (EVP_CipherUpdate(ctx, cipherText, &len, payload, int(payloadSize)) != 1)
			return false;
		cipherLen = len;
	}

	if (EVP_CipherFinal_ex(ctx, cipherText + cipherLen, &len) != 1)
		return false;
	cipherLen += len;

	if (size_t(cipherLen) != payloadSize)
		return false;

	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, int(TAG_SIZE), cipherText + cipherLen) != 1)
		return false;

	memcpy(cipherText + cipherLen + TAG_SIZE, counter, NONCE_COUNTER_SIZE);
	return true;
}

bool DecryptPacket(
	eMode mode,
	const uint8_t* key,
	const uint8_t* packet,
	size_t packetSize,
	std::vector<uint8_t>& payloadOut)
{
	if (packetSize < RTP_HEADER_SIZE + PACKET_OVERHEAD)
		return false;

	uint8_t first = packet[0];
	if ((first >> 6) != 2)
		return false; // RTP version isn't 2

	size_t headerSize = RTP_HEADER_SIZE + size_t(first & 0x0F) * 4; // fixed header plus CSRCs
	bool hasExtension = (first & 0x10) != 0;
	bool hasPadding   = (first & 0x20) != 0;

	// The extension preamble (profile and length) is part of the authenticated header, but the
	// body of the extension is encrypted.
	size_t extensionWords = 0;
	size_t aadSize = headerSize;
	if (hasExtension)
	{
		if (packetSize < headerSize + 4 + PACKET_OVERHEAD)
			return false;

		extensionWords = (size_t(packet[headerSize + 2]) << 8) | packet[headerSize + 3];
		aadSize += 4;
	}

	if (packetSize < aadSize + PACKET_OVERHEAD)
		return false;

	size_t cipherSize = packetSize - aadSize - PACKET_OVERHEAD;
	const uint8_t* cipherText = packet + aadSize;
	const uint8_t* tag = cipherText + cipherSize;
	const uint8_t* counter = tag + TAG_SIZE;

	uint8_t nonce[24];
	MakeNonce(counter, nonce);

	CipherContext cc;
	if (!InitCipher(cc, false, mode, key, nonce))
		return false;

	EVP_CIPHER_CTX* ctx = cc.Get();

	int len = 0;
	if (EVP_CipherUpdate(ctx, NULL, &len, packet, int(aadSize)) != 1)
		return false;

	std::vector<uint8_t> plain(cipherSize);
	int plainLen = 0;
	if (cipherSize != 0)
	{
		if (EVP_CipherUpdate(ctx, plain.data(), &len, cipherText, int(cipherSize)) != 1)
			return false;
		plainLen = len;
	}

	// The tag has to be set before finishing, and OpenSSL wants a non-const pointer for it.
	uint8_t tagCopy[TAG_SIZE];
	memcpy(tagCopy, tag, TAG_SIZE);
	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, int(TAG_SIZE), tagCopy) != 1)
		return false;

	if (EVP_CipherFinal_ex(ctx, plain.data() + plainLen, &len) != 1)
		return false; // authentication failed

	size_t start = extensionWords * 4;
	size_t end = size_t(plainLen);
	if (start > end)
		return false;

	if (hasPadding)
	{
		if (end <= start)
			return false;

		size_t padding = plain[end - 1];
		if (padding == 0 || padding > end - start)
			return false;

		end -= padding;
	}

	payloadOut.assign(plain.begin() + start, plain.begin() + end);
	return true;
}

bool SealWithNonce(
	eMode mode,
	const uint8_t* key,
	const uint8_t* nonce24,
	const uint8_t* aad,
	size_t aadSize,
	const uint8_t* plain,
	size_t plainSize,
	std::vector<uint8_t>& out)
{
	CipherContext cc;
	if (!InitCipher(cc, true, mode, key, nonce24))
		return false;

	EVP_CIPHER_CTX* ctx = cc.Get();

	int len = 0;
	if (aadSize != 0 && EVP_CipherUpdate(ctx, NULL, &len, aad, int(aadSize)) != 1)
		return false;

	out.resize(plainSize + TAG_SIZE);
	int cipherLen = 0;
	if (plainSize != 0)
	{
		if (EVP_CipherUpdate(ctx, out.data(), &len, plain, int(plainSize)) != 1)
			return false;
		cipherLen = len;
	}

	if (EVP_CipherFinal_ex(ctx, out.data() + cipherLen, &len) != 1)
		return false;

	return EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, int(TAG_SIZE), out.data() + plainSize) == 1;
}

}
