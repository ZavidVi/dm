// Checks the transport encryption of the voice engine against published test vectors, and that
// packets survive a round trip.  Build with -DDM_VOICE_TESTS=ON, see voice-deps/CMakeLists.txt.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "voice/VoiceCrypto.hpp"

static int g_failures = 0;

static std::vector<uint8_t> FromHex(const char* hex)
{
	std::vector<uint8_t> bytes;
	for (size_t i = 0; hex[i] && hex[i + 1]; i += 2)
	{
		unsigned value = 0;
		sscanf(hex + i, "%2x", &value);
		bytes.push_back(uint8_t(value));
	}
	return bytes;
}

static std::string ToHex(const std::vector<uint8_t>& bytes)
{
	std::string out;
	char buf[3];
	for (uint8_t b : bytes) {
		snprintf(buf, sizeof buf, "%02x", b);
		out += buf;
	}
	return out;
}

static void Check(bool ok, const char* what)
{
	printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		g_failures++;
}

static void CheckEqual(const std::vector<uint8_t>& got, const std::vector<uint8_t>& expected, const char* what)
{
	bool ok = got == expected;
	Check(ok, what);
	if (!ok) {
		printf("  expected: %s\n", ToHex(expected).c_str());
		printf("  got:      %s\n", ToHex(got).c_str());
	}
}

// draft-irtf-cfrg-xchacha, section 2.2.1
static void TestHChaCha20()
{
	std::vector<uint8_t> key = FromHex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
	std::vector<uint8_t> nonce = FromHex("000000090000004a0000000031415927");

	std::vector<uint8_t> subkey(32);
	VoiceCrypto::HChaCha20(key.data(), nonce.data(), subkey.data());

	CheckEqual(subkey, FromHex("82413b4227b27bfed30e42508a877d73a0f9e4d58a74a853c12ec41326d3ecdc"), "HChaCha20 test vector");
}

// draft-irtf-cfrg-xchacha, appendix A.3.1
static void TestXChaCha20Poly1305()
{
	std::vector<uint8_t> plain = FromHex(
		"4c616469657320616e642047656e746c656d656e206f662074686520636c6173"
		"73206f66202739393a204966204920636f756c64206f6666657220796f75206f"
		"6e6c79206f6e652074697020666f7220746865206675747572652c2073756e73"
		"637265656e20776f756c642062652069742e");
	std::vector<uint8_t> aad = FromHex("50515253c0c1c2c3c4c5c6c7");
	std::vector<uint8_t> key = FromHex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
	std::vector<uint8_t> nonce = FromHex("404142434445464748494a4b4c4d4e4f5051525354555657");

	std::vector<uint8_t> expected = FromHex(
		"bd6d179d3e83d43b9576579493c0e939572a1700252bfaccbed2902c21396cbb"
		"731c7f1b0b4aa6440bf3a82f4eda7e39ae64c6708c54c216cb96b72e1213b452"
		"2f8c9ba40db5d945b11b69b982c1bb9e3f3fac2bc369488f76b2383565d3fff9"
		"21f9664c97637da9768812f615c68b13b52e"
		"c0875924c1c7987947deafd8780acf49");

	std::vector<uint8_t> out;
	bool ok = VoiceCrypto::SealWithNonce(
		VoiceCrypto::MODE_XCHACHA20_POLY1305_RTPSIZE, key.data(), nonce.data(),
		aad.data(), aad.size(), plain.data(), plain.size(), out);

	Check(ok, "XChaCha20-Poly1305 seals");
	CheckEqual(out, expected, "XChaCha20-Poly1305 test vector");
}

// McGrew and Viega, GCM test case 13
static void TestAesGcm()
{
	std::vector<uint8_t> key(32, 0);
	std::vector<uint8_t> nonce(24, 0);
	std::vector<uint8_t> plain(16, 0);

	std::vector<uint8_t> out;
	bool ok = VoiceCrypto::SealWithNonce(
		VoiceCrypto::MODE_AES256_GCM_RTPSIZE, key.data(), nonce.data(),
		nullptr, 0, plain.data(), plain.size(), out);

	Check(ok, "AES-256-GCM seals");
	CheckEqual(out, FromHex("cea7403d4d606b6e074ec5d3baf39d18d0d1c8a799996bf0265b98b5d48ab919"), "AES-256-GCM test vector");
}

static void TestRoundTrip(VoiceCrypto::eMode mode, const char* name)
{
	uint8_t key[VoiceCrypto::KEY_SIZE];
	for (size_t i = 0; i < sizeof key; i++)
		key[i] = uint8_t(i * 7 + 1);

	uint8_t header[VoiceCrypto::RTP_HEADER_SIZE] = { 0x80, 120, 0x12, 0x34, 0, 0, 0x03, 0xc0, 0xde, 0xad, 0xbe, 0xef };
	std::vector<uint8_t> payload = FromHex("f8fffe0102030405060708090a0b0c0d");

	std::string label(name);

	std::vector<uint8_t> packet;
	Check(VoiceCrypto::EncryptPacket(mode, key, 0x01020304, header, payload.data(), payload.size(), packet), (label + ": encrypts").c_str());
	Check(packet.size() == VoiceCrypto::RTP_HEADER_SIZE + payload.size() + VoiceCrypto::PACKET_OVERHEAD, (label + ": packet size").c_str());
	Check(memcmp(packet.data(), header, sizeof header) == 0, (label + ": header stays in the clear").c_str());
	Check(memcmp(packet.data() + packet.size() - 4, "\x01\x02\x03\x04", 4) == 0, (label + ": nonce counter at the end").c_str());

	std::vector<uint8_t> decrypted;
	Check(VoiceCrypto::DecryptPacket(mode, key, packet.data(), packet.size(), decrypted), (label + ": decrypts").c_str());
	CheckEqual(decrypted, payload, (label + ": round trip").c_str());

	// A flipped bit anywhere has to be noticed, in the header as well.
	for (size_t position : { size_t(3), size_t(14), packet.size() - 6, packet.size() - 1 })
	{
		std::vector<uint8_t> tampered = packet;
		tampered[position] ^= 0x01;
		std::vector<uint8_t> ignored;
		Check(!VoiceCrypto::DecryptPacket(mode, key, tampered.data(), tampered.size(), ignored),
			(label + ": rejects tampering at " + std::to_string(position)).c_str());
	}

	// A packet the way the server forwards it, with a header extension: the preamble is part of the
	// authenticated header, the body of the extension is encrypted along with the payload.
	uint8_t extHeader[VoiceCrypto::RTP_HEADER_SIZE + 4] = {
		0x90, 120, 0x00, 0x07, 0, 0, 0x0b, 0xb8, 0x11, 0x22, 0x33, 0x44,
		0xbe, 0xde, 0x00, 0x01
	};
	std::vector<uint8_t> extBody = FromHex("10aabbcc");

	std::vector<uint8_t> plainWithExt = extBody;
	plainWithExt.insert(plainWithExt.end(), payload.begin(), payload.end());

	uint8_t nonce24[24] = { 0x00, 0x00, 0x00, 0x2a };
	std::vector<uint8_t> sealed;
	Check(VoiceCrypto::SealWithNonce(mode, key, nonce24, extHeader, sizeof extHeader, plainWithExt.data(), plainWithExt.size(), sealed),
		(label + ": seals a packet with extension").c_str());

	std::vector<uint8_t> extPacket(extHeader, extHeader + sizeof extHeader);
	extPacket.insert(extPacket.end(), sealed.begin(), sealed.end());
	extPacket.insert(extPacket.end(), nonce24, nonce24 + 4);

	decrypted.clear();
	Check(VoiceCrypto::DecryptPacket(mode, key, extPacket.data(), extPacket.size(), decrypted), (label + ": decrypts with extension").c_str());
	CheckEqual(decrypted, payload, (label + ": extension body is dropped").c_str());
}

int main()
{
	TestHChaCha20();
	TestXChaCha20Poly1305();
	TestAesGcm();
	TestRoundTrip(VoiceCrypto::MODE_AES256_GCM_RTPSIZE, "AES-256-GCM");
	TestRoundTrip(VoiceCrypto::MODE_XCHACHA20_POLY1305_RTPSIZE, "XChaCha20-Poly1305");

	printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
	return g_failures ? 1 : 0;
}
