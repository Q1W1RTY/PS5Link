#include <ds5bt.h>

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cfgmgr32.h>
#include <hidsdi.h>
#include <setupapi.h>

#include <opus/opus.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{

constexpr uint16_t kSonyVendorId = 0x054C;
constexpr uint16_t kDualSenseProductId = 0x0CE6;
constexpr uint16_t kDualSenseEdgeProductId = 0x0DF2;

constexpr const char *kBluetoothHidServiceUuid = "00001124-0000-1000-8000-00805f9b34fb";
constexpr const char *kDs5LinkControlPipe = "\\\\.\\pipe\\DS5Link.Control.v1";

constexpr uint8_t kBtInputPrefix = 0xA1;
constexpr uint8_t kBtReport31 = 0x31;
constexpr uint8_t kBtReport32 = 0x32;
constexpr uint8_t kBtReport36 = 0x36;

constexpr size_t kBtInputReportSize = 78;
constexpr size_t kBtPrefixedInputSize = 79;
constexpr size_t kBtStateReportSize = 78;
constexpr size_t kBtMicReportSize = 142;
constexpr size_t kBtHapticsReportSize = 398;

constexpr size_t kDsStateSize = 63;
constexpr size_t kBtStatePayloadSize = 47;
constexpr size_t kHapticsBlockSize = 64; // 32 stereo frames @ 3000 Hz, 8-bit signed PCM
constexpr size_t kSpeakerOpusSize = 200; // 10 ms CBR Opus @ 160 kbps
constexpr size_t kMicOpusSize = 71;      // 10 ms mono Opus @ 48 kHz
constexpr int kOpusSampleRate = 48000;
constexpr int kOpusFrameSamples = 480;   // 10 ms @ 48 kHz
constexpr size_t kPcmWindowFrames = 512; // 10.67 ms window @ 48 kHz (matches 32 frames @ 3 kHz)

constexpr uint8_t kSpeakerBlockControllerSpeaker = 0x13;
constexpr uint8_t kSpeakerBlockHeadphones = 0x16;

constexpr uint32_t kOutputCrcSeed = 0xEADA2D49u; // CRC32 of 0xA2 prefix
constexpr float kPi = 3.14159265358979323846f;

constexpr std::array<uint8_t, kBtStatePayloadSize> kInitialSetStateData = {
	0xFD, 0xF7, 0x00, 0x00, 0x7F, 0x64, 0x08, 0x09, 0x00, 0x0F,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x05, 0x00,
	0x00, 0x02, 0x01, 0x00, 0xFF, 0xD7, 0x00
};

constexpr std::array<uint32_t, 256> kCrc32Table = {
	0x00000000u, 0x77073096u, 0xEE0E612Cu, 0x990951BAu, 0x076DC419u, 0x706AF48Fu, 0xE963A535u, 0x9E6495A3u,
	0x0EDB8832u, 0x79DCB8A4u, 0xE0D5E91Eu, 0x97D2D988u, 0x09B64C2Bu, 0x7EB17CBDu, 0xE7B82D07u, 0x90BF1D91u,
	0x1DB71064u, 0x6AB020F2u, 0xF3B97148u, 0x84BE41DEu, 0x1ADAD47Du, 0x6DDDE4EBu, 0xF4D4B551u, 0x83D385C7u,
	0x136C9856u, 0x646BA8C0u, 0xFD62F97Au, 0x8A65C9ECu, 0x14015C4Fu, 0x63066CD9u, 0xFA0F3D63u, 0x8D080DF5u,
	0x3B6E20C8u, 0x4C69105Eu, 0xD56041E4u, 0xA2677172u, 0x3C03E4D1u, 0x4B04D447u, 0xD20D85FDu, 0xA50AB56Bu,
	0x35B5A8FAu, 0x42B2986Cu, 0xDBBBC9D6u, 0xACBCF940u, 0x32D86CE3u, 0x45DF5C75u, 0xDCD60DCFu, 0xABD13D59u,
	0x26D930ACu, 0x51DE003Au, 0xC8D75180u, 0xBFD06116u, 0x21B4F4B5u, 0x56B3C423u, 0xCFBA9599u, 0xB8BDA50Fu,
	0x2802B89Eu, 0x5F058808u, 0xC60CD9B2u, 0xB10BE924u, 0x2F6F7C87u, 0x58684C11u, 0xC1611DABu, 0xB6662D3Du,
	0x76DC4190u, 0x01DB7106u, 0x98D220BCu, 0xEFD5102Au, 0x71B18589u, 0x06B6B51Fu, 0x9FBFE4A5u, 0xE8B8D433u,
	0x7807C9A2u, 0x0F00F934u, 0x9609A88Eu, 0xE10E9818u, 0x7F6A0DBBu, 0x086D3D2Du, 0x91646C97u, 0xE6635C01u,
	0x6B6B51F4u, 0x1C6C6162u, 0x856530D8u, 0xF262004Eu, 0x6C0695EDu, 0x1B01A57Bu, 0x8208F4C1u, 0xF50FC457u,
	0x65B0D9C6u, 0x12B7E950u, 0x8BBEB8EAu, 0xFCB9887Cu, 0x62DD1DDFu, 0x15DA2D49u, 0x8CD37CF3u, 0xFBD44C65u,
	0x4DB26158u, 0x3AB551CEu, 0xA3BC0074u, 0xD4BB30E2u, 0x4ADFA541u, 0x3DD895D7u, 0xA4D1C46Du, 0xD3D6F4FBu,
	0x4369E96Au, 0x346ED9FCu, 0xAD678846u, 0xDA60B8D0u, 0x44042D73u, 0x33031DE5u, 0xAA0A4C5Fu, 0xDD0D7CC9u,
	0x5005713Cu, 0x270241AAu, 0xBE0B1010u, 0xC90C2086u, 0x5768B525u, 0x206F85B3u, 0xB966D409u, 0xCE61E49Fu,
	0x5EDEF90Eu, 0x29D9C998u, 0xB0D09822u, 0xC7D7A8B4u, 0x59B33D17u, 0x2EB40D81u, 0xB7BD5C3Bu, 0xC0BA6CADu,
	0xEDB88320u, 0x9ABFB3B6u, 0x03B6E20Cu, 0x74B1D29Au, 0xEAD54739u, 0x9DD277AFu, 0x04DB2615u, 0x73DC1683u,
	0xE3630B12u, 0x94643B84u, 0x0D6D6A3Eu, 0x7A6A5AA8u, 0xE40ECF0Bu, 0x9309FF9Du, 0x0A00AE27u, 0x7D079EB1u,
	0xF00F9344u, 0x8708A3D2u, 0x1E01F268u, 0x6906C2FEu, 0xF762575Du, 0x806567CBu, 0x196C3671u, 0x6E6B06E7u,
	0xFED41B76u, 0x89D32BE0u, 0x10DA7A5Au, 0x67DD4ACCu, 0xF9B9DF6Fu, 0x8EBEEFF9u, 0x17B7BE43u, 0x60B08ED5u,
	0xD6D6A3E8u, 0xA1D1937Eu, 0x38D8C2C4u, 0x4FDFF252u, 0xD1BB67F1u, 0xA6BC5767u, 0x3FB506DDu, 0x48B2364Bu,
	0xD80D2BDAu, 0xAF0A1B4Cu, 0x36034AF6u, 0x41047A60u, 0xDF60EFC3u, 0xA867DF55u, 0x316E8EEFu, 0x4669BE79u,
	0xCB61B38Cu, 0xBC66831Au, 0x256FD2A0u, 0x5268E236u, 0xCC0C7795u, 0xBB0B4703u, 0x220216B9u, 0x5505262Fu,
	0xC5BA3BBEu, 0xB2BD0B28u, 0x2BB45A92u, 0x5CB36A04u, 0xC2D7FFA7u, 0xB5D0CF31u, 0x2CD99E8Bu, 0x5BDEAE1Du,
	0x9B64C2B0u, 0xEC63F226u, 0x756AA39Cu, 0x026D930Au, 0x9C0906A9u, 0xEB0E363Fu, 0x72076785u, 0x05005713u,
	0x95BF4A82u, 0xE2B87A14u, 0x7BB12BAEu, 0x0CB61B38u, 0x92D28E9Bu, 0xE5D5BE0Du, 0x7CDCEFB7u, 0x0BDBDF21u,
	0x86D3D2D4u, 0xF1D4E242u, 0x68DDB3F8u, 0x1FDA836Eu, 0x81BE16CDu, 0xF6B9265Bu, 0x6FB077E1u, 0x18B74777u,
	0x88085AE6u, 0xFF0F6A70u, 0x66063BCAu, 0x11010B5Cu, 0x8F659EFFu, 0xF862AE69u, 0x616BFFD3u, 0x166CCF45u,
	0xA00AE278u, 0xD70DD2EEu, 0x4E048354u, 0x3903B3C2u, 0xA7672661u, 0xD06016F7u, 0x4969474Du, 0x3E6E77DBu,
	0xAED16A4Au, 0xD9D65ADCu, 0x40DF0B66u, 0x37D83BF0u, 0xA9BCAE53u, 0xDEBB9EC5u, 0x47B2CF7Fu, 0x30B5FFE9u,
	0xBDBDF21Cu, 0xCABAC28Au, 0x53B39330u, 0x24B4A3A6u, 0xBAD03605u, 0xCDD70693u, 0x54DE5729u, 0x23D967BFu,
	0xB3667A2Eu, 0xC4614AB8u, 0x5D681B02u, 0x2A6F2B94u, 0xB40BBE37u, 0xC30C8EA1u, 0x5A05DF1Bu, 0x2D02EF8Du
};

uint32_t crc32Seeded(const uint8_t *data, size_t size, uint32_t seed)
{
	uint32_t crc = ~seed;
	for(size_t i = 0; i < size; ++i)
		crc = kCrc32Table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
	return ~crc;
}

uint32_t readLe32(const uint8_t *p)
{
	return static_cast<uint32_t>(p[0])
		| (static_cast<uint32_t>(p[1]) << 8)
		| (static_cast<uint32_t>(p[2]) << 16)
		| (static_cast<uint32_t>(p[3]) << 24);
}

int16_t readLe16Signed(const uint8_t *p)
{
	const uint16_t v = static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
	return static_cast<int16_t>(v);
}

void writeLe32(uint8_t *p, uint32_t value)
{
	p[0] = static_cast<uint8_t>(value & 0xFFu);
	p[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
	p[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
	p[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

void fillOutputReportChecksum(uint8_t *report, size_t reportSize)
{
	if(reportSize < 5)
		return;
	const uint32_t crc = crc32Seeded(report, reportSize - 4, kOutputCrcSeed);
	writeLe32(report + reportSize - 4, crc);
}

std::string toLowerAscii(std::string s)
{
	for(char &c : s)
	{
		if(c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	}
	return s;
}

bool isHexChar(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

std::string formatMac12(const std::string &hex12)
{
	std::string out;
	out.reserve(17);
	for(size_t i = 0; i < 12; i += 2)
	{
		if(!out.empty())
			out.push_back(':');
		out.push_back(hex12[i]);
		out.push_back(hex12[i + 1]);
	}
	return toLowerAscii(out);
}

std::string extractBluetoothAddressFromText(const std::string &text)
{
	const std::string lower = toLowerAscii(text);
	for(size_t i = 0; i + 12 <= lower.size(); ++i)
	{
		const bool afterBoundary = (i == 0) || (lower[i - 1] == '&') || (lower[i - 1] == '_') || (lower[i - 1] == '#') || (lower[i - 1] == '\\');
		if(!afterBoundary)
			continue;
		bool allHex = true;
		for(size_t j = 0; j < 12; ++j)
		{
			if(!isHexChar(lower[i + j]))
			{
				allHex = false;
				break;
			}
		}
		if(!allHex)
			continue;
		const size_t end = i + 12;
		const bool beforeBoundary = (end == lower.size()) || !isHexChar(lower[end]);
		if(beforeBoundary && lower.compare(i, 12, "000000000000") != 0)
			return formatMac12(lower.substr(i, 12));
	}
	return {};
}

class AudioThreadPriorityGuard
{
public:
	AudioThreadPriorityGuard()
	{
		prev_priority_ = GetThreadPriority(GetCurrentThread());
		priority_set_ = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) != FALSE;
		avrt_ = LoadLibraryA("avrt.dll");
		if(!avrt_)
			return;
		using AvSetProc = HANDLE(WINAPI *)(LPCSTR, LPDWORD);
		av_revert_ = reinterpret_cast<AvRevertProc>(GetProcAddress(avrt_, "AvRevertMmThreadCharacteristics"));
		auto av_set = reinterpret_cast<AvSetProc>(GetProcAddress(avrt_, "AvSetMmThreadCharacteristicsA"));
		if(av_set && av_revert_)
		{
			DWORD taskIndex = 0;
			mmcss_ = av_set("Pro Audio", &taskIndex);
		}
	}

	~AudioThreadPriorityGuard()
	{
		if(mmcss_ && av_revert_)
			av_revert_(mmcss_);
		if(priority_set_ && prev_priority_ != THREAD_PRIORITY_ERROR_RETURN)
			SetThreadPriority(GetCurrentThread(), prev_priority_);
		if(avrt_)
			FreeLibrary(avrt_);
	}

private:
	using AvRevertProc = BOOL(WINAPI *)(HANDLE);
	HMODULE avrt_ = nullptr;
	HANDLE mmcss_ = nullptr;
	AvRevertProc av_revert_ = nullptr;
	int prev_priority_ = THREAD_PRIORITY_ERROR_RETURN;
	bool priority_set_ = false;
};

// Gently coexists with DS5LinkBridge Windows service if it is running and cloaking
// the Bluetooth DualSense via HidHide.
bool sendDs5LinkBridgeCommand(const char *command, std::string *outResponse = nullptr)
{
	HANDLE pipe = CreateFileA(
		kDs5LinkControlPipe,
		GENERIC_READ | GENERIC_WRITE,
		0,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);
	if(pipe == INVALID_HANDLE_VALUE)
		return false;

	std::string req = std::string("{\"protocolVersion\":1,\"id\":\"ps5link\",\"command\":\"") + command + "\"}\n";
	DWORD written = 0;
	const BOOL writeOk = WriteFile(pipe, req.data(), static_cast<DWORD>(req.size()), &written, nullptr);
	if(!writeOk || written != req.size())
	{
		CloseHandle(pipe);
		return false;
	}

	char buf[2048];
	DWORD readBytes = 0;
	if(ReadFile(pipe, buf, sizeof(buf) - 1, &readBytes, nullptr) && readBytes > 0)
	{
		buf[readBytes] = '\0';
		if(outResponse)
			*outResponse = buf;
	}
	CloseHandle(pipe);
	return true;
}

struct ImuCalibration
{
	bool valid = false;
	std::array<int16_t, 3> gyroBias = {0, 0, 0};
	std::array<float, 3> gyroScaleRadPerSec = {
		(1.0f / 16.4f) * (kPi / 180.0f),
		(1.0f / 16.4f) * (kPi / 180.0f),
		(1.0f / 16.4f) * (kPi / 180.0f)
	};
	std::array<int16_t, 3> accelBias = {0, 0, 0};
	std::array<float, 3> accelScaleG = {
		1.0f / 8192.0f,
		1.0f / 8192.0f,
		1.0f / 8192.0f
	};
};

ImuCalibration parseFeature05Calibration(const uint8_t *buf, size_t len)
{
	ImuCalibration cal;
	if(len < 35 || buf[0] != 0x05)
		return cal;

	const int16_t gyroPitchBias = readLe16Signed(&buf[1]);
	const int16_t gyroYawBias = readLe16Signed(&buf[3]);
	const int16_t gyroRollBias = readLe16Signed(&buf[5]);
	const int16_t gyroPitchPlus = readLe16Signed(&buf[7]);
	const int16_t gyroPitchMinus = readLe16Signed(&buf[9]);
	const int16_t gyroYawPlus = readLe16Signed(&buf[11]);
	const int16_t gyroYawMinus = readLe16Signed(&buf[13]);
	const int16_t gyroRollPlus = readLe16Signed(&buf[15]);
	const int16_t gyroRollMinus = readLe16Signed(&buf[17]);
	const int16_t gyroSpeedPlus = readLe16Signed(&buf[19]);
	const int16_t gyroSpeedMinus = readLe16Signed(&buf[21]);

	const int16_t accelXPlus = readLe16Signed(&buf[23]);
	const int16_t accelXMinus = readLe16Signed(&buf[25]);
	const int16_t accelYPlus = readLe16Signed(&buf[27]);
	const int16_t accelYMinus = readLe16Signed(&buf[29]);
	const int16_t accelZPlus = readLe16Signed(&buf[31]);
	const int16_t accelZMinus = readLe16Signed(&buf[33]);

	const int speed2x = static_cast<int>(gyroSpeedPlus) + static_cast<int>(gyroSpeedMinus);
	const int pitchDenom = static_cast<int>(gyroPitchPlus) - static_cast<int>(gyroPitchMinus);
	const int yawDenom = static_cast<int>(gyroYawPlus) - static_cast<int>(gyroYawMinus);
	const int rollDenom = static_cast<int>(gyroRollPlus) - static_cast<int>(gyroRollMinus);

	const int accelXRange = static_cast<int>(accelXPlus) - static_cast<int>(accelXMinus);
	const int accelYRange = static_cast<int>(accelYPlus) - static_cast<int>(accelYMinus);
	const int accelZRange = static_cast<int>(accelZPlus) - static_cast<int>(accelZMinus);

	if(speed2x <= 0 || pitchDenom <= 0 || yawDenom <= 0 || rollDenom <= 0
		|| accelXRange <= 0 || accelYRange <= 0 || accelZRange <= 0)
	{
		return cal;
	}

	const float speedRad2x = static_cast<float>(speed2x) * (kPi / 180.0f);
	cal.gyroBias = {gyroPitchBias, gyroYawBias, gyroRollBias};
	cal.gyroScaleRadPerSec = {
		speedRad2x / static_cast<float>(pitchDenom),
		speedRad2x / static_cast<float>(yawDenom),
		speedRad2x / static_cast<float>(rollDenom)
	};

	cal.accelBias = {
		static_cast<int16_t>((static_cast<int>(accelXPlus) + static_cast<int>(accelXMinus)) / 2),
		static_cast<int16_t>((static_cast<int>(accelYPlus) + static_cast<int>(accelYMinus)) / 2),
		static_cast<int16_t>((static_cast<int>(accelZPlus) + static_cast<int>(accelZMinus)) / 2)
	};
	cal.accelScaleG = {
		2.0f / static_cast<float>(accelXRange),
		2.0f / static_cast<float>(accelYRange),
		2.0f / static_cast<float>(accelZRange)
	};
	cal.valid = true;
	return cal;
}

} // namespace

struct Ds5BtManager::Impl
{
	mutable std::mutex mutex;

	// Discovery & coexistence state
	std::chrono::steady_clock::time_point lastPollTime{};
	bool pausedExternalBridge = false;
	std::atomic<bool> usbDualSensePresent{false};

	// Connected device info
	std::atomic<bool> connected{false};
	std::atomic<uint16_t> pid{0};
	std::string devicePath;
	std::string deviceName = "DualSense Wireless Controller (Bluetooth)";
	std::string btAddress;

	HANDLE inputHandle = INVALID_HANDLE_VALUE;
	HANDLE outputHandle = INVALID_HANDLE_VALUE;
	HANDLE featureHandle = INVALID_HANDLE_VALUE;
	HANDLE stopEvent = nullptr;
	size_t outputReportLength = 0;
	size_t featureReportLength = 0;

	std::thread inputThread;
	std::thread outputThread;
	std::atomic<bool> workerStop{false};

	// Live telemetry
	std::atomic<int> batteryPct{-1};
	std::atomic<bool> charging{false};
	std::atomic<bool> fullCharge{false};
	std::atomic<bool> headphonesPlugged{false};
	std::atomic<bool> headsetMicPlugged{false};
	std::atomic<bool> micMuted{true};
	std::atomic<bool> micStreamActive{false};
	std::atomic<bool> speakerAudioEnabled{false};

	// Input & IMU state
	ImuCalibration imuCal;
	ChiakiOrientationTracker orientationTracker{};
	ChiakiAccelNewZero accelZero{};
	ChiakiAccelNewZero realAccel{};
	ChiakiControllerState controllerState{};
	int8_t fingerTouchId[2] = {-1, -1};
	uint32_t lastRawSensorTimestamp = 0;
	bool hasRawSensorTimestamp = false;
	uint64_t motionTimestampUs = 0;
	bool prevMicButtonDown = false;
	std::atomic<bool> pendingMicButtonPress{false};
	std::atomic<uint64_t> inputSeq{0};

	// Microphone Opus decoder (48 kHz mono -> 480 mono + 480 stereo int16)
	OpusDecoder *micDecoder = nullptr;
	std::mutex micCallbackMutex;
	std::function<void(const int16_t *, const int16_t *, size_t)> micAudioCallback;

	// Speaker / 3.5mm Headphone Opus encoder (48 kHz stereo -> 200-byte Opus)
	OpusEncoder *speakerEncoder = nullptr;
	std::mutex audioHapticsMutex;
	std::vector<int16_t> speakerPcmFifo; // Interleaved L/R 48 kHz samples
	std::deque<std::array<uint8_t, kSpeakerOpusSize>> pendingSpeakerPackets;
	std::vector<int8_t> hapticsPcmFifo;  // Interleaved L/R 3 kHz 8-bit signed samples
	std::deque<std::array<uint8_t, kHapticsBlockSize>> pendingHapticsBlocks;
	std::chrono::steady_clock::time_point lastHapticsPushTime{};
	bool voiceCoilHapticsActive = false;
	int trailingZeroHapticsPackets = 0;

	// DualSense 63-byte state & output packet sequencing
	std::mutex outputStateMutex;
	std::array<uint8_t, kDsStateSize> dsState{};
	uint8_t reportSequence = 0;
	uint8_t packetSequence = 0;
	uint8_t micPacketSequence = 0;
	bool stateReportDirty = false;
	bool micToggleDirty = false;
	bool desiredMicActive = false;
	bool desiredMicMuted = true;
	uint8_t lastRumbleLeft = 0;
	uint8_t lastRumbleRight = 0;
	uint8_t lastIntensity = 0;
	uint8_t ledRed = 0;
	uint8_t ledGreen = 0x50;
	uint8_t ledBlue = 0xFF;
	uint8_t ledBrightness = 0;
	uint8_t playerIndicators = 0x04; // center LED

	Impl()
	{
		stopEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
		chiaki_orientation_tracker_init(&orientationTracker);
		chiaki_accel_new_zero_set_inactive(&accelZero, false);
		chiaki_accel_new_zero_set_inactive(&realAccel, true);
		chiaki_controller_state_set_idle(&controllerState);
		resetDsStateLocked();

		int opusErr = OPUS_OK;
		micDecoder = opus_decoder_create(kOpusSampleRate, 1, &opusErr);
		speakerEncoder = opus_encoder_create(kOpusSampleRate, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &opusErr);
		if(speakerEncoder)
		{
			opus_encoder_ctl(speakerEncoder, OPUS_SET_BITRATE(160000));
			opus_encoder_ctl(speakerEncoder, OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));
			opus_encoder_ctl(speakerEncoder, OPUS_SET_VBR(0));
			opus_encoder_ctl(speakerEncoder, OPUS_SET_COMPLEXITY(0));
			opus_encoder_ctl(speakerEncoder, OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS));
		}
	}

	~Impl()
	{
		shutdown();
		if(micDecoder)
		{
			opus_decoder_destroy(micDecoder);
			micDecoder = nullptr;
		}
		if(speakerEncoder)
		{
			opus_encoder_destroy(speakerEncoder);
			speakerEncoder = nullptr;
		}
		if(stopEvent)
		{
			CloseHandle(stopEvent);
			stopEvent = nullptr;
		}
	}

	void resetDsStateLocked()
	{
		dsState.fill(0);
		std::copy(kInitialSetStateData.begin(), kInitialSetStateData.end(), dsState.begin());
		// Default lightbar color (PS5 blue) + player 1 indicator
		dsState[42] = ledBrightness;
		dsState[43] = playerIndicators;
		dsState[44] = ledRed;
		dsState[45] = ledGreen;
		dsState[46] = ledBlue;
		reportSequence = 0;
		packetSequence = 0;
		micPacketSequence = 0;
	}

	void applyAudioRoutingFlagsLocked()
	{
		const bool hp = headphonesPlugged.load(std::memory_order_relaxed);
		const bool hsMic = headsetMicPlugged.load(std::memory_order_relaxed);
		const bool spkActive = hp || speakerAudioEnabled.load(std::memory_order_relaxed);
		const bool micAct = desiredMicActive && !desiredMicMuted;

		dsState[0] |= 0xF0; // allow headphone/speaker/mic volume + audio_control
		dsState[1] |= 0x83; // allow mute light, audio mute, audio_control2

		// Output path: 0x00 = stereo headphones (L_R_X), 0x30 = controller speaker (X_X_R)
		dsState[7] &= static_cast<uint8_t>(~0x33u);
		if(!hsMic)
			dsState[7] |= 0x01; // internal beamforming mic when no 3.5mm headset mic
		if(!hp)
			dsState[7] |= 0x30; // route output to internal speaker when no headphones plugged

		// Volumes
		dsState[4] = 0x7F; // headphone volume
		dsState[5] = 0x64; // speaker volume (100%)
		dsState[6] = micAct ? 0x40 : 0x08;
		dsState[37] = spkActive ? 0x32 : 0x01; // speaker preamp gain

		// Mic mute orange LED
		dsState[8] = desiredMicMuted ? 0x01 : 0x00;

		// Power save & mute bits in dsState[9]:
		// bit 2 (0x04): haptics_power_save
		// bit 3 (0x08): audio_power_save
		// bit 4 (0x10): mute_mic
		// bit 5 (0x20): mute_speaker
		// bit 6 (0x40): mute_headphones
		// bit 7 (0x80): mute_haptics
		uint8_t powerSave = 0x00;
		if(!micAct)
			powerSave |= 0x10; // mute_mic
		if(spkActive)
		{
			if(hp)
				powerSave |= 0x20; // mute speaker, unmute headphones
			else
				powerSave |= 0x40; // mute headphones, unmute speaker
		}
		else
		{
			powerSave |= 0x60; // mute both speaker and headphones
			if(!micAct)
				powerSave |= 0x08; // audio_power_save when neither mic nor speaker is active
		}

		if(voiceCoilHapticsActive || spkActive)
		{
			// Enable 3 kHz voice-coil haptics (disable legacy rumble emulation)
			dsState[0] &= static_cast<uint8_t>(~0x03u);
			dsState[38] &= static_cast<uint8_t>(~0x04u);
			dsState[2] = 0;
			dsState[3] = 0;
		}
		else if(lastRumbleLeft > 0 || lastRumbleRight > 0)
		{
			// Legacy motor rumble via firmware >= 0x0224 improved rumble emulation
			dsState[0] &= static_cast<uint8_t>(~0x01u);
			dsState[0] |= 0x02;
			dsState[38] |= 0x04;
			dsState[2] = lastRumbleRight;
			dsState[3] = lastRumbleLeft;
			powerSave &= static_cast<uint8_t>(~0x84u);
		}
		else
		{
			dsState[0] &= static_cast<uint8_t>(~0x03u);
			dsState[38] &= static_cast<uint8_t>(~0x04u);
			dsState[2] = 0;
			dsState[3] = 0;
			powerSave |= 0x84; // mute haptics when idle
		}

		dsState[9] = powerSave;
	}

	std::array<uint8_t, kBtMicReportSize> buildBtInitReport32Locked()
	{
		std::array<uint8_t, kBtMicReportSize> report{};
		report[0] = kBtReport32;
		report[1] = 0x10;
		report[2] = 0x90;
		report[3] = static_cast<uint8_t>(kDsStateSize);
		std::memcpy(&report[4], dsState.data(), kDsStateSize);
		report[4 + 38] |= 0x02; // allow_color_light_fade_animation takeover
		fillOutputReportChecksum(report.data(), report.size());
		return report;
	}

	std::array<uint8_t, kBtStateReportSize> buildBtStateReport31Locked(const std::array<uint8_t, kDsStateSize> &state)
	{
		std::array<uint8_t, kBtStateReportSize> report{};
		report[0] = kBtReport31;
		report[1] = static_cast<uint8_t>((reportSequence++ & 0x0Fu) << 4);
		report[2] = 0x10;
		std::memcpy(&report[3], state.data(), kBtStatePayloadSize);
		fillOutputReportChecksum(report.data(), report.size());
		return report;
	}

	std::array<uint8_t, kBtMicReportSize> buildBtMicReport32Locked(bool active)
	{
		std::array<uint8_t, kBtMicReportSize> report{};
		report[0] = kBtReport32;
		report[1] = static_cast<uint8_t>((micPacketSequence & 0x0Fu) << 4);
		report[2] = 0x91;
		report[3] = 0x07;
		report[4] = active ? 0xFF : 0xFE;
		report[5] = 0x40;
		report[6] = 0x40;
		report[7] = 0x40;
		report[8] = 0x40;
		report[9] = 0x40;
		report[10] = micPacketSequence;
		report[11] = 0x92;
		report[12] = 0x40;
		micPacketSequence = static_cast<uint8_t>((micPacketSequence + 1) & 0x0Fu);
		fillOutputReportChecksum(report.data(), report.size());
		return report;
	}

	std::array<uint8_t, kBtMicReportSize> buildBtWaveHapticsReport32Locked(
		const std::array<uint8_t, kHapticsBlockSize> &hapticsBlock,
		bool micActive)
	{
		std::array<uint8_t, kBtMicReportSize> report{};
		report[0] = kBtReport32;
		report[1] = static_cast<uint8_t>((reportSequence++ & 0x0Fu) << 4);
		report[2] = 0x91;
		report[3] = 0x07;
		report[4] = micActive ? 0xFF : 0xFE;
		if(micActive)
		{
			report[5] = 0x40;
			report[6] = 0x40;
			report[7] = 0x40;
			report[8] = 0x40;
			report[9] = 0x40;
		}
		else
		{
			report[9] = 0xFF;
		}
		report[10] = packetSequence++;
		report[11] = 0x92;
		report[12] = static_cast<uint8_t>(kHapticsBlockSize);
		std::memcpy(&report[13], hapticsBlock.data(), kHapticsBlockSize);
		fillOutputReportChecksum(report.data(), report.size());
		return report;
	}

	std::array<uint8_t, kBtHapticsReportSize> buildBtCombinedReport36Locked(
		const std::array<uint8_t, kHapticsBlockSize> &hapticsBlock,
		const std::array<uint8_t, kSpeakerOpusSize> &speakerOpus,
		const std::array<uint8_t, kDsStateSize> &state,
		bool micActive,
		bool useHeadphonesBlock)
	{
		std::array<uint8_t, kBtHapticsReportSize> packet{};
		packet[0] = kBtReport36;
		packet[1] = static_cast<uint8_t>((reportSequence++ & 0x0Fu) << 4);
		packet[2] = 0x91;
		packet[3] = 0x07;
		packet[4] = micActive ? 0xFF : 0xFE;
		packet[5] = 0x40;
		packet[6] = 0x40;
		packet[7] = 0x40;
		packet[8] = 0x40;
		packet[9] = 0x40;
		packet[10] = packetSequence++;
		packet[11] = 0x90;
		packet[12] = static_cast<uint8_t>(kDsStateSize);
		std::memcpy(&packet[13], state.data(), kDsStateSize);
		packet[76] = 0x92;
		packet[77] = static_cast<uint8_t>(kHapticsBlockSize);
		std::memcpy(&packet[78], hapticsBlock.data(), kHapticsBlockSize);
		const uint8_t speakerBlock = useHeadphonesBlock ? kSpeakerBlockHeadphones : kSpeakerBlockControllerSpeaker;
		packet[142] = static_cast<uint8_t>(speakerBlock | 0x80u);
		packet[143] = static_cast<uint8_t>(kSpeakerOpusSize);
		std::memcpy(&packet[144], speakerOpus.data(), kSpeakerOpusSize);
		fillOutputReportChecksum(packet.data(), packet.size());
		return packet;
	}

	bool writeOutputReportSync(const uint8_t *report, size_t reportSize)
	{
		if(outputHandle == INVALID_HANDLE_VALUE || outputReportLength < reportSize)
			return false;

		std::vector<uint8_t> writeBuf(outputReportLength, 0);
		std::memcpy(writeBuf.data(), report, reportSize);

		OVERLAPPED ov{};
		ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
		if(!ov.hEvent)
			return false;

		DWORD written = 0;
		BOOL ok = WriteFile(outputHandle, writeBuf.data(), static_cast<DWORD>(writeBuf.size()), &written, &ov);
		if(!ok)
		{
			const DWORD err = GetLastError();
			if(err == ERROR_IO_PENDING)
			{
				HANDLE waits[2] = {ov.hEvent, stopEvent};
				const DWORD waitRes = WaitForMultipleObjects(2, waits, FALSE, 250);
				if(waitRes == WAIT_OBJECT_0)
				{
					ok = GetOverlappedResult(outputHandle, &ov, &written, FALSE);
				}
				else
				{
					CancelIoEx(outputHandle, &ov);
					GetOverlappedResult(outputHandle, &ov, &written, TRUE);
					CloseHandle(ov.hEvent);
					return false;
				}
			}
			else
			{
				CloseHandle(ov.hEvent);
				return false;
			}
		}
		CloseHandle(ov.hEvent);
		return ok != FALSE;
	}

	void readFeature05Calibration()
	{
		imuCal = ImuCalibration{};
		if(featureHandle == INVALID_HANDLE_VALUE || featureReportLength < 41)
			return;

		std::vector<uint8_t> featureBuf(std::max<size_t>(featureReportLength, 64), 0);
		featureBuf[0] = 0x05;
		if(HidD_GetFeature(featureHandle, featureBuf.data(), static_cast<ULONG>(featureBuf.size())))
		{
			imuCal = parseFeature05Calibration(featureBuf.data(), featureBuf.size());
			if(imuCal.valid)
				std::fprintf(stdout, "[PS5Link] DualSense Bluetooth IMU calibration (Feature 0x05) loaded.\n");
		}
	}

	void inputThreadLoop()
	{
		OVERLAPPED ov{};
		ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
		if(!ov.hEvent)
			return;

		std::vector<uint8_t> raw(kBtInputReportSize, 0);
		while(!workerStop.load(std::memory_order_relaxed))
		{
			ResetEvent(ov.hEvent);
			DWORD bytesRead = 0;
			BOOL ok = ReadFile(inputHandle, raw.data(), static_cast<DWORD>(raw.size()), &bytesRead, &ov);
			if(!ok)
			{
				const DWORD err = GetLastError();
				if(err == ERROR_IO_PENDING)
				{
					HANDLE waits[2] = {ov.hEvent, stopEvent};
					const DWORD waitRes = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
					if(waitRes != WAIT_OBJECT_0)
					{
						CancelIoEx(inputHandle, &ov);
						GetOverlappedResult(inputHandle, &ov, &bytesRead, TRUE);
						break;
					}
					if(!GetOverlappedResult(inputHandle, &ov, &bytesRead, FALSE))
						break;
				}
				else
				{
					break;
				}
			}

			if(bytesRead == kBtInputReportSize && raw[0] == kBtReport31)
			{
				handleBluetoothInputReport31(raw.data(), bytesRead);
			}
		}

		CloseHandle(ov.hEvent);
		connected.store(false, std::memory_order_release);
	}

	void handleBluetoothInputReport31(const uint8_t *raw, size_t len)
	{
		if(len != kBtInputReportSize || raw[0] != kBtReport31)
			return;

		std::array<uint8_t, kBtPrefixedInputSize> packet{};
		packet[0] = kBtInputPrefix;
		std::memcpy(&packet[1], raw, kBtInputReportSize);

		const uint32_t expectedCrc = readLe32(&packet[kBtPrefixedInputSize - 4]);
		const uint32_t actualCrc = crc32Seeded(packet.data(), kBtPrefixedInputSize - 4, 0);
		if(actualCrc != expectedCrc)
			return;

		const uint8_t payloadType = static_cast<uint8_t>(packet[2] & 0x0Fu);
		if(payloadType == 0x02)
		{
			// Bluetooth Microphone Opus frame (71 bytes at packet[4..74])
			if(!micDecoder)
				return;
			std::array<int16_t, kOpusFrameSamples> monoPcm{};
			const int decoded = opus_decode(
				micDecoder,
				&packet[4],
				static_cast<opus_int32>(kMicOpusSize),
				monoPcm.data(),
				kOpusFrameSamples,
				0);
			if(decoded == kOpusFrameSamples)
			{
				std::array<int16_t, kOpusFrameSamples * 2> stereoPcm{};
				for(int i = 0; i < kOpusFrameSamples; ++i)
				{
					stereoPcm[i * 2] = monoPcm[i];
					stereoPcm[i * 2 + 1] = monoPcm[i];
				}
				std::function<void(const int16_t *, const int16_t *, size_t)> cb;
				{
					std::lock_guard<std::mutex> lock(micCallbackMutex);
					cb = micAudioCallback;
				}
				if(cb)
					cb(monoPcm.data(), stereoPcm.data(), static_cast<size_t>(kOpusFrameSamples));
			}
			return;
		}

		if(payloadType != 0x01)
			return;

		// Build 64-byte USB-aligned input report u[0..63]
		std::array<uint8_t, 64> u{};
		u[0] = 0x01;
		std::memcpy(&u[1], &packet[3], 63);

		// Parse battery & 3.5mm jack telemetry
		const uint8_t rawBat = static_cast<uint8_t>(u[53] & 0x0Fu);
		const uint8_t chargeState = static_cast<uint8_t>((u[53] >> 4) & 0x0Fu);
		const bool isFull = (chargeState == 0x02);
		const bool isChg = (chargeState == 0x01);
		const int pct = isFull ? 100 : std::min(100, static_cast<int>(std::min<uint8_t>(rawBat, 10)) * 10 + 5);
		batteryPct.store(pct, std::memory_order_relaxed);
		charging.store(isChg, std::memory_order_relaxed);
		fullCharge.store(isFull, std::memory_order_relaxed);

		const bool hp = (u[54] & 0x01u) != 0;
		const bool hsMic = (u[54] & 0x02u) != 0;
		const bool prevHp = headphonesPlugged.exchange(hp, std::memory_order_relaxed);
		const bool prevHsMic = headsetMicPlugged.exchange(hsMic, std::memory_order_relaxed);
		if(hp != prevHp || hsMic != prevHsMic)
		{
			std::lock_guard<std::mutex> lock(outputStateMutex);
			applyAudioRoutingFlagsLocked();
			stateReportDirty = true;
		}

		// Hardware Mic Mute button (under PS button: u[10] & 0x04)
		const bool micBtnDown = (u[10] & 0x04u) != 0;
		if(micBtnDown && !prevMicButtonDown)
			pendingMicButtonPress.store(true, std::memory_order_release);
		prevMicButtonDown = micBtnDown;

		// Parse controller buttons, sticks, triggers, touchpad, and calibrated 6-axis IMU
		auto mapStickAxis = [](uint8_t v) -> int16_t {
			const int centered = static_cast<int>(v) - 128;
			if(centered >= 0)
				return static_cast<int16_t>((centered * 32767) / 127);
			return static_cast<int16_t>((centered * 32768) / 128);
		};

		uint32_t buttons = 0;
		const uint8_t hat = static_cast<uint8_t>(u[8] & 0x0Fu);
		switch(hat)
		{
			case 0: buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_UP; break;
			case 1: buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_UP | CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT; break;
			case 2: buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT; break;
			case 3: buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN | CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT; break;
			case 4: buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN; break;
			case 5: buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN | CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT; break;
			case 6: buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT; break;
			case 7: buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_UP | CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT; break;
			default: break;
		}
		if(u[8] & 0x10u) buttons |= CHIAKI_CONTROLLER_BUTTON_BOX;
		if(u[8] & 0x20u) buttons |= CHIAKI_CONTROLLER_BUTTON_CROSS;
		if(u[8] & 0x40u) buttons |= CHIAKI_CONTROLLER_BUTTON_MOON;
		if(u[8] & 0x80u) buttons |= CHIAKI_CONTROLLER_BUTTON_PYRAMID;

		if(u[9] & 0x01u) buttons |= CHIAKI_CONTROLLER_BUTTON_L1;
		if(u[9] & 0x02u) buttons |= CHIAKI_CONTROLLER_BUTTON_R1;
		if(u[9] & 0x10u) buttons |= CHIAKI_CONTROLLER_BUTTON_SHARE;
		if(u[9] & 0x20u) buttons |= CHIAKI_CONTROLLER_BUTTON_OPTIONS;
		if(u[9] & 0x40u) buttons |= CHIAKI_CONTROLLER_BUTTON_L3;
		if(u[9] & 0x80u) buttons |= CHIAKI_CONTROLLER_BUTTON_R3;

		if(u[10] & 0x01u) buttons |= CHIAKI_CONTROLLER_BUTTON_PS;
		if(u[10] & 0x02u) buttons |= CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;

		const int16_t rawGyroX = readLe16Signed(&u[16]);
		const int16_t rawGyroY = readLe16Signed(&u[18]);
		const int16_t rawGyroZ = readLe16Signed(&u[20]);
		const int16_t rawAccelX = readLe16Signed(&u[22]);
		const int16_t rawAccelY = readLe16Signed(&u[24]);
		const int16_t rawAccelZ = readLe16Signed(&u[26]);
		const uint32_t sensorTs = readLe32(&u[28]);

		const float gyroX = static_cast<float>(rawGyroX - imuCal.gyroBias[0]) * imuCal.gyroScaleRadPerSec[0];
		const float gyroY = static_cast<float>(rawGyroY - imuCal.gyroBias[1]) * imuCal.gyroScaleRadPerSec[1];
		const float gyroZ = static_cast<float>(rawGyroZ - imuCal.gyroBias[2]) * imuCal.gyroScaleRadPerSec[2];

		const float accelX = static_cast<float>(rawAccelX - imuCal.accelBias[0]) * imuCal.accelScaleG[0];
		const float accelY = static_cast<float>(rawAccelY - imuCal.accelBias[1]) * imuCal.accelScaleG[1];
		const float accelZ = static_cast<float>(rawAccelZ - imuCal.accelBias[2]) * imuCal.accelScaleG[2];

		{
			std::lock_guard<std::mutex> lock(mutex);
			controllerState.buttons = buttons;
			controllerState.l2_state = u[5];
			controllerState.r2_state = u[6];
			controllerState.left_x = mapStickAxis(u[1]);
			controllerState.left_y = mapStickAxis(u[2]);
			controllerState.right_x = mapStickAxis(u[3]);
			controllerState.right_y = mapStickAxis(u[4]);

			// 2-point capacitive touchpad (u[33..36] and u[37..40])
			for(int f = 0; f < 2; ++f)
			{
				const uint8_t *tp = &u[33 + f * 4];
				const bool active = (tp[0] & 0x80u) == 0;
				if(active)
				{
					uint16_t x = static_cast<uint16_t>(tp[1]) | (static_cast<uint16_t>(tp[2] & 0x0Fu) << 8);
					uint16_t y = static_cast<uint16_t>((tp[2] >> 4) & 0x0Fu) | (static_cast<uint16_t>(tp[3]) << 4);
					x = std::min<uint16_t>(x, 1919);
					y = std::min<uint16_t>(y, 1079);
					if(fingerTouchId[f] < 0)
						fingerTouchId[f] = chiaki_controller_state_start_touch(&controllerState, x, y);
					else
						chiaki_controller_state_set_touch_pos(&controllerState, static_cast<uint8_t>(fingerTouchId[f]), x, y);
				}
				else if(fingerTouchId[f] >= 0)
				{
					chiaki_controller_state_stop_touch(&controllerState, static_cast<uint8_t>(fingerTouchId[f]));
					fingerTouchId[f] = -1;
				}
			}

			// Sensor timestamp is in 0.333 us ticks (3 ticks = 1 microsecond)
			if(!hasRawSensorTimestamp)
			{
				hasRawSensorTimestamp = true;
				lastRawSensorTimestamp = sensorTs;
				motionTimestampUs = 1000;
			}
			else
			{
				const uint32_t deltaTicks = sensorTs - lastRawSensorTimestamp;
				lastRawSensorTimestamp = sensorTs;
				const uint64_t deltaUs = std::clamp<uint64_t>(deltaTicks / 3, 250, 50000);
				motionTimestampUs += deltaUs;
			}

			chiaki_accel_new_zero_set_active(&realAccel, accelX, accelY, accelZ, true);
			chiaki_orientation_tracker_update(
				&orientationTracker,
				gyroX, gyroY, gyroZ,
				accelX, accelY, accelZ,
				&accelZero,
				false,
				motionTimestampUs);
			chiaki_orientation_tracker_update(
				&orientationTracker,
				gyroX, gyroY, gyroZ,
				accelX, accelY, accelZ,
				&accelZero,
				true,
				motionTimestampUs);
			chiaki_orientation_tracker_apply_to_controller_state(&orientationTracker, &controllerState);
			inputSeq.fetch_add(1, std::memory_order_release);
		}
	}

	void outputThreadLoop()
	{
		AudioThreadPriorityGuard priorityGuard;

		HANDLE timer = CreateWaitableTimerExA(
			nullptr,
			nullptr,
			CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
			TIMER_MODIFY_STATE | SYNCHRONIZE);
		if(!timer)
			timer = CreateWaitableTimerExA(nullptr, nullptr, 0, TIMER_MODIFY_STATE | SYNCHRONIZE);

		// Startup handshake:
		// 1. Report 0x32 init report (lightbar takeover)
		// 2. Report 0x31 state report (LED fade animation reset, then target LED color & player LED)
		{
			std::array<uint8_t, kBtMicReportSize> init32{};
			std::array<uint8_t, kBtStateReportSize> reset31{};
			std::array<uint8_t, kBtStateReportSize> color31{};
			{
				std::lock_guard<std::mutex> lock(outputStateMutex);
				applyAudioRoutingFlagsLocked();
				init32 = buildBtInitReport32Locked();

				auto resetState = dsState;
				resetState[38] = 0x02; // allow_color_light_fade_animation
				resetState[41] = 0x02; // fade out default blue flash
				reset31 = buildBtStateReport31Locked(resetState);
			}
			writeOutputReportSync(init32.data(), init32.size());
			writeOutputReportSync(reset31.data(), reset31.size());
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			{
				std::lock_guard<std::mutex> lock(outputStateMutex);
				dsState[1] |= 0x15; // allow mute light, LED color, player indicators
				dsState[38] |= 0x01; // allow brightness change
				color31 = buildBtStateReport31Locked(dsState);
				stateReportDirty = false;
			}
			writeOutputReportSync(color31.data(), color31.size());
		}

		constexpr auto kPacedInterval = std::chrono::microseconds(10667); // 32 frames @ 3000 Hz = 10.667 ms
		auto nextPacedDeadline = std::chrono::steady_clock::now() + kPacedInterval;

		while(!workerStop.load(std::memory_order_relaxed) && connected.load(std::memory_order_acquire))
		{
			const auto now = std::chrono::steady_clock::now();

			// 1. Check for immediate microphone open/close toggle
			bool doMicToggle = false;
			std::array<uint8_t, kBtStateReportSize> micState31{};
			std::array<uint8_t, kBtMicReportSize> micCmd32{};
			{
				std::lock_guard<std::mutex> lock(outputStateMutex);
				if(micToggleDirty)
				{
					micToggleDirty = false;
					doMicToggle = true;
					applyAudioRoutingFlagsLocked();
					const bool streamOn = desiredMicActive && !desiredMicMuted;
					micState31 = buildBtStateReport31Locked(dsState);
					micCmd32 = buildBtMicReport32Locked(streamOn);
					stateReportDirty = false;
				}
			}
			if(doMicToggle)
			{
				writeOutputReportSync(micState31.data(), micState31.size());
				writeOutputReportSync(micCmd32.data(), micCmd32.size());
			}

			// 2. Check if 10.67 ms paced slot is due for Report 0x36 (speaker + haptics) or Report 0x32 (wave haptics)
			if(now >= nextPacedDeadline)
			{
				if(now - nextPacedDeadline > kPacedInterval * 3)
					nextPacedDeadline = now + kPacedInterval;
				else
					nextPacedDeadline += kPacedInterval;

				const bool hp = headphonesPlugged.load(std::memory_order_relaxed);
				const bool spkMode = hp || speakerAudioEnabled.load(std::memory_order_relaxed);

				bool hasSpeakerPacket = false;
				std::array<uint8_t, kSpeakerOpusSize> speakerPacket{};
				bool hasHapticsBlock = false;
				std::array<uint8_t, kHapticsBlockSize> hapticsBlock{};

				{
					std::lock_guard<std::mutex> lock(audioHapticsMutex);
					if(!pendingSpeakerPackets.empty())
					{
						speakerPacket = pendingSpeakerPackets.front();
						pendingSpeakerPackets.pop_front();
						hasSpeakerPacket = true;
					}
					if(!pendingHapticsBlocks.empty())
					{
						hapticsBlock = pendingHapticsBlocks.front();
						pendingHapticsBlocks.pop_front();
						hasHapticsBlock = true;
						trailingZeroHapticsPackets = 2;
					}
					else if(trailingZeroHapticsPackets > 0)
					{
						hapticsBlock.fill(0);
						hasHapticsBlock = true;
						--trailingZeroHapticsPackets;
						if(trailingZeroHapticsPackets == 0)
							voiceCoilHapticsActive = false;
					}
				}

				if(spkMode && hasSpeakerPacket)
				{
					std::array<uint8_t, kBtHapticsReportSize> pkt36{};
					{
						std::lock_guard<std::mutex> lock(outputStateMutex);
						applyAudioRoutingFlagsLocked();
						const bool micOn = desiredMicActive && !desiredMicMuted;
						pkt36 = buildBtCombinedReport36Locked(hapticsBlock, speakerPacket, dsState, micOn, hp);
						stateReportDirty = false;
					}
					writeOutputReportSync(pkt36.data(), pkt36.size());
				}
				else if(hasHapticsBlock)
				{
					std::optional<std::array<uint8_t, kBtStateReportSize>> unmuteState31;
					std::array<uint8_t, kBtMicReportSize> wave32{};
					{
						std::lock_guard<std::mutex> lock(outputStateMutex);
						const uint8_t prevPowerSave = dsState[9];
						const uint8_t prevFlags0 = dsState[0];
						applyAudioRoutingFlagsLocked();
						if(stateReportDirty || dsState[9] != prevPowerSave || dsState[0] != prevFlags0)
						{
							unmuteState31 = buildBtStateReport31Locked(dsState);
							stateReportDirty = false;
						}
						const bool micOn = desiredMicActive && !desiredMicMuted;
						wave32 = buildBtWaveHapticsReport32Locked(hapticsBlock, micOn);
					}
					if(unmuteState31)
						writeOutputReportSync(unmuteState31->data(), unmuteState31->size());
					writeOutputReportSync(wave32.data(), wave32.size());
				}
			}

			// 3. Flush any pending standalone 0x31 state updates (triggers, LED, player index, rumble)
			std::optional<std::array<uint8_t, kBtStateReportSize>> pendingState31;
			{
				std::lock_guard<std::mutex> lock(outputStateMutex);
				if(stateReportDirty)
				{
					applyAudioRoutingFlagsLocked();
					pendingState31 = buildBtStateReport31Locked(dsState);
					stateReportDirty = false;
				}
			}
			if(pendingState31)
				writeOutputReportSync(pendingState31->data(), pendingState31->size());

			// Sleep until next 10.67 ms boundary or up to 2 ms for responsive trigger/LED updates
			const auto afterWork = std::chrono::steady_clock::now();
			auto sleepDur = std::chrono::duration_cast<std::chrono::microseconds>(nextPacedDeadline - afterWork);
			if(sleepDur > std::chrono::microseconds(2000))
				sleepDur = std::chrono::microseconds(2000);
			if(sleepDur.count() > 100 && timer)
			{
				LARGE_INTEGER due{};
				due.QuadPart = -static_cast<LONGLONG>(sleepDur.count() * 10);
				if(SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0))
				{
					HANDLE waits[2] = {timer, stopEvent};
					WaitForMultipleObjects(2, waits, FALSE, 5);
				}
			}
			else
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}

		if(timer)
			CloseHandle(timer);
	}

	void closeDeviceLocked()
	{
		workerStop.store(true, std::memory_order_release);
		if(stopEvent)
			SetEvent(stopEvent);
		if(inputHandle != INVALID_HANDLE_VALUE)
			CancelIoEx(inputHandle, nullptr);
		if(outputHandle != INVALID_HANDLE_VALUE)
			CancelIoEx(outputHandle, nullptr);

		if(inputThread.joinable())
			inputThread.join();
		if(outputThread.joinable())
			outputThread.join();

		if(inputHandle != INVALID_HANDLE_VALUE)
		{
			CloseHandle(inputHandle);
			inputHandle = INVALID_HANDLE_VALUE;
		}
		if(outputHandle != INVALID_HANDLE_VALUE)
		{
			CloseHandle(outputHandle);
			outputHandle = INVALID_HANDLE_VALUE;
		}
		if(featureHandle != INVALID_HANDLE_VALUE)
		{
			CloseHandle(featureHandle);
			featureHandle = INVALID_HANDLE_VALUE;
		}

		connected.store(false, std::memory_order_release);
		pid.store(0, std::memory_order_relaxed);
		devicePath.clear();
		btAddress.clear();
		batteryPct.store(-1, std::memory_order_relaxed);
		charging.store(false, std::memory_order_relaxed);
		fullCharge.store(false, std::memory_order_relaxed);
		headphonesPlugged.store(false, std::memory_order_relaxed);
		headsetMicPlugged.store(false, std::memory_order_relaxed);
		fingerTouchId[0] = -1;
		fingerTouchId[1] = -1;
		hasRawSensorTimestamp = false;
		prevMicButtonDown = false;
	}

	void pollDevices()
	{
		const auto now = std::chrono::steady_clock::now();
		if(lastPollTime != std::chrono::steady_clock::time_point{}
			&& (now - lastPollTime) < std::chrono::milliseconds(900))
		{
			return;
		}
		lastPollTime = now;

		// If worker thread exited due to Bluetooth disconnect, clean up handles
		if(!connected.load(std::memory_order_acquire) && (inputHandle != INVALID_HANDLE_VALUE || inputThread.joinable()))
		{
			closeDeviceLocked();
		}

		// Ensure DS5LinkBridge service (if installed and running on this PC) is paused
		// so it does not cloak the DualSense via HidHide or send conflicting BT reports.
		if(!connected.load(std::memory_order_acquire) && !pausedExternalBridge)
		{
			std::string stateResp;
			if(sendDs5LinkBridgeCommand("getState", &stateResp))
			{
				if(stateResp.find("\"enabled\":true") != std::string::npos)
				{
					if(sendDs5LinkBridgeCommand("pause"))
					{
						pausedExternalBridge = true;
						std::fprintf(stdout, "[PS5Link] Paused DS5LinkBridge service to take direct Bluetooth DualSense control.\n");
						std::this_thread::sleep_for(std::chrono::milliseconds(150));
					}
				}
			}
		}

		GUID hidGuid{};
		HidD_GetHidGuid(&hidGuid);
		HDEVINFO devInfo = SetupDiGetClassDevsA(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
		if(devInfo == INVALID_HANDLE_VALUE)
			return;

		bool foundUsbDualSense = false;
		std::string foundBtPath;
		std::string foundBtAddress;
		uint16_t foundBtPid = 0;
		size_t foundOutLen = 0;
		size_t foundFeatLen = 0;

		SP_DEVICE_INTERFACE_DATA ifData{};
		ifData.cbSize = sizeof(ifData);
		for(DWORD idx = 0; SetupDiEnumDeviceInterfaces(devInfo, nullptr, &hidGuid, idx, &ifData); ++idx)
		{
			DWORD requiredLen = 0;
			SetupDiGetDeviceInterfaceDetailA(devInfo, &ifData, nullptr, 0, &requiredLen, nullptr);
			if(requiredLen == 0)
				continue;

			std::vector<uint8_t> detailStorage(requiredLen, 0);
			auto *detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_A>(detailStorage.data());
			detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
			SP_DEVINFO_DATA devData{};
			devData.cbSize = sizeof(devData);
			if(!SetupDiGetDeviceInterfaceDetailA(devInfo, &ifData, detail, requiredLen, nullptr, &devData))
				continue;

			const std::string path(detail->DevicePath);
			const std::string lowerPath = toLowerAscii(path);

			HANDLE queryHandle = CreateFileA(
				path.c_str(),
				0,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr,
				OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL,
				nullptr);
			if(queryHandle == INVALID_HANDLE_VALUE)
				continue;

			HIDD_ATTRIBUTES attrs{};
			attrs.Size = sizeof(attrs);
			if(!HidD_GetAttributes(queryHandle, &attrs)
				|| attrs.VendorID != kSonyVendorId
				|| (attrs.ProductID != kDualSenseProductId && attrs.ProductID != kDualSenseEdgeProductId))
			{
				CloseHandle(queryHandle);
				continue;
			}

			PHIDP_PREPARSED_DATA preparsed = nullptr;
			HIDP_CAPS caps{};
			const bool hasCaps = HidD_GetPreparsedData(queryHandle, &preparsed)
				&& HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS;
			if(preparsed)
				HidD_FreePreparsedData(preparsed);
			CloseHandle(queryHandle);

			if(!hasCaps || caps.UsagePage != 0x01 || caps.Usage != 0x05)
				continue;

			const bool isBt = (lowerPath.find(kBluetoothHidServiceUuid) != std::string::npos)
				|| (caps.InputReportByteLength == kBtInputReportSize);

			if(!isBt)
			{
				foundUsbDualSense = true;
				continue;
			}

			if(caps.InputReportByteLength == kBtInputReportSize
				&& caps.OutputReportByteLength >= kBtStateReportSize
				&& foundBtPath.empty())
			{
				foundBtPath = path;
				foundBtPid = attrs.ProductID;
				foundOutLen = caps.OutputReportByteLength;
				foundFeatLen = caps.FeatureReportByteLength;
				foundBtAddress = extractBluetoothAddressFromText(path);
			}
		}

		SetupDiDestroyDeviceInfoList(devInfo);
		usbDualSensePresent.store(foundUsbDualSense, std::memory_order_relaxed);

		if(connected.load(std::memory_order_acquire))
			return;

		if(foundBtPath.empty())
			return;

		// Open overlapped input/output handles and synchronous feature handle
		HANDLE inH = CreateFileA(
			foundBtPath.c_str(),
			GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
			nullptr);
		if(inH == INVALID_HANDLE_VALUE)
			return;

		HANDLE outH = CreateFileA(
			foundBtPath.c_str(),
			GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
			nullptr);
		if(outH == INVALID_HANDLE_VALUE)
		{
			CloseHandle(inH);
			return;
		}

		HANDLE featH = CreateFileA(
			foundBtPath.c_str(),
			GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			nullptr);
		if(featH == INVALID_HANDLE_VALUE)
		{
			CloseHandle(outH);
			CloseHandle(inH);
			return;
		}

		HidD_SetNumInputBuffers(inH, 64);

		inputHandle = inH;
		outputHandle = outH;
		featureHandle = featH;
		outputReportLength = foundOutLen;
		featureReportLength = foundFeatLen;
		devicePath = foundBtPath;
		btAddress = foundBtAddress;
		pid.store(foundBtPid, std::memory_order_relaxed);
		deviceName = (foundBtPid == kDualSenseEdgeProductId)
			? "DualSense Edge Wireless Controller (Bluetooth)"
			: "DualSense Wireless Controller (Bluetooth)";

		readFeature05Calibration();

		{
			std::lock_guard<std::mutex> lock(outputStateMutex);
			resetDsStateLocked();
		}
		{
			std::lock_guard<std::mutex> lock(audioHapticsMutex);
			speakerPcmFifo.clear();
			pendingSpeakerPackets.clear();
			hapticsPcmFifo.clear();
			pendingHapticsBlocks.clear();
			voiceCoilHapticsActive = false;
			trailingZeroHapticsPackets = 0;
		}

		ResetEvent(stopEvent);
		workerStop.store(false, std::memory_order_release);
		connected.store(true, std::memory_order_release);

		inputThread = std::thread([this]() { inputThreadLoop(); });
		outputThread = std::thread([this]() { outputThreadLoop(); });

		std::fprintf(stdout, "[PS5Link] Connected native Bluetooth %s (addr=%s, outReport=%zu).\n",
			deviceName.c_str(),
			btAddress.empty() ? "bt-hid" : btAddress.c_str(),
			outputReportLength);
	}

	void shutdown()
	{
		closeDeviceLocked();
		if(pausedExternalBridge)
		{
			sendDs5LinkBridgeCommand("enable");
			pausedExternalBridge = false;
			std::fprintf(stdout, "[PS5Link] Restored DS5LinkBridge service state.\n");
		}
	}
};

Ds5BtManager &Ds5BtManager::instance()
{
	static Ds5BtManager inst;
	return inst;
}

Ds5BtManager::Ds5BtManager()
	: impl_(new Impl())
{
}

Ds5BtManager::~Ds5BtManager()
{
	delete impl_;
}

void Ds5BtManager::pollDevices()
{
	impl_->pollDevices();
}

void Ds5BtManager::shutdown()
{
	impl_->shutdown();
}

bool Ds5BtManager::isConnected() const
{
	return impl_->connected.load(std::memory_order_acquire);
}

bool Ds5BtManager::hasUsbDualSense() const
{
	return impl_->usbDualSensePresent.load(std::memory_order_relaxed);
}

uint16_t Ds5BtManager::productId() const
{
	return impl_->pid.load(std::memory_order_relaxed);
}

std::string Ds5BtManager::deviceName() const
{
	std::lock_guard<std::mutex> lock(impl_->mutex);
	return impl_->deviceName;
}

std::string Ds5BtManager::bluetoothAddress() const
{
	std::lock_guard<std::mutex> lock(impl_->mutex);
	return impl_->btAddress;
}

int Ds5BtManager::batteryPercent() const
{
	return impl_->batteryPct.load(std::memory_order_relaxed);
}

bool Ds5BtManager::isCharging() const
{
	return impl_->charging.load(std::memory_order_relaxed);
}

bool Ds5BtManager::isFullyCharged() const
{
	return impl_->fullCharge.load(std::memory_order_relaxed);
}

bool Ds5BtManager::isHeadphonesPlugged() const
{
	return impl_->headphonesPlugged.load(std::memory_order_relaxed);
}

bool Ds5BtManager::isHeadsetMicPlugged() const
{
	return impl_->headsetMicPlugged.load(std::memory_order_relaxed);
}

bool Ds5BtManager::isMicMuted() const
{
	return impl_->micMuted.load(std::memory_order_relaxed);
}

bool Ds5BtManager::isMicStreamActive() const
{
	return impl_->micStreamActive.load(std::memory_order_relaxed);
}

void Ds5BtManager::setSpeakerAudioEnabled(bool enabled)
{
	const bool prev = impl_->speakerAudioEnabled.exchange(enabled, std::memory_order_relaxed);
	if(prev != enabled)
	{
		{
			std::lock_guard<std::mutex> lock(impl_->audioHapticsMutex);
			impl_->speakerPcmFifo.clear();
			impl_->pendingSpeakerPackets.clear();
		}
		std::lock_guard<std::mutex> lock(impl_->outputStateMutex);
		impl_->applyAudioRoutingFlagsLocked();
		impl_->stateReportDirty = true;
	}
}

bool Ds5BtManager::isSpeakerAudioEnabled() const
{
	return impl_->speakerAudioEnabled.load(std::memory_order_relaxed);
}

bool Ds5BtManager::isSpeakerAudioActive() const
{
	return impl_->connected.load(std::memory_order_relaxed)
		&& (impl_->headphonesPlugged.load(std::memory_order_relaxed)
			|| impl_->speakerAudioEnabled.load(std::memory_order_relaxed));
}

ChiakiControllerState Ds5BtManager::getControllerState() const
{
	std::lock_guard<std::mutex> lock(impl_->mutex);
	return impl_->controllerState;
}

uint64_t Ds5BtManager::inputSequence() const
{
	return impl_->inputSeq.load(std::memory_order_acquire);
}

bool Ds5BtManager::consumeMicButtonPress()
{
	return impl_->pendingMicButtonPress.exchange(false, std::memory_order_acq_rel);
}

void Ds5BtManager::resetMotion()
{
	std::lock_guard<std::mutex> lock(impl_->mutex);
	chiaki_accel_new_zero_set_active(
		&impl_->accelZero,
		impl_->realAccel.accel_x,
		impl_->realAccel.accel_y,
		impl_->realAccel.accel_z,
		false);
	chiaki_orientation_tracker_init(&impl_->orientationTracker);
	chiaki_orientation_tracker_update(
		&impl_->orientationTracker,
		impl_->controllerState.gyro_x,
		impl_->controllerState.gyro_y,
		impl_->controllerState.gyro_z,
		impl_->realAccel.accel_x,
		impl_->realAccel.accel_y,
		impl_->realAccel.accel_z,
		&impl_->accelZero,
		false,
		impl_->motionTimestampUs);
	chiaki_orientation_tracker_apply_to_controller_state(&impl_->orientationTracker, &impl_->controllerState);
}

void Ds5BtManager::setMicAudioCallback(std::function<void(const int16_t *mono48k, const int16_t *stereo48k, size_t frames)> cb)
{
	std::lock_guard<std::mutex> lock(impl_->micCallbackMutex);
	impl_->micAudioCallback = std::move(cb);
}

void Ds5BtManager::setTriggerEffects(uint8_t leftType, const uint8_t *leftData, uint8_t rightType, const uint8_t *rightData, uint8_t intensity)
{
	std::lock_guard<std::mutex> lock(impl_->outputStateMutex);
	impl_->lastIntensity = intensity;
	impl_->dsState[0] |= (0x04 | 0x08); // allow right & left trigger FFB
	impl_->dsState[1] |= 0x40;          // allow motor power level
	impl_->dsState[36] = intensity;
	impl_->dsState[10] = rightType;
	if(rightData)
		std::memcpy(&impl_->dsState[11], rightData, 10);
	else
		std::memset(&impl_->dsState[11], 0, 10);
	impl_->dsState[21] = leftType;
	if(leftData)
		std::memcpy(&impl_->dsState[22], leftData, 10);
	else
		std::memset(&impl_->dsState[22], 0, 10);
	impl_->stateReportDirty = true;
}

void Ds5BtManager::setRumble(uint8_t left, uint8_t right, uint8_t intensity)
{
	std::lock_guard<std::mutex> lock(impl_->outputStateMutex);
	impl_->lastRumbleLeft = left;
	impl_->lastRumbleRight = right;
	impl_->lastIntensity = intensity;
	impl_->dsState[1] |= 0x40;
	impl_->dsState[36] = intensity;
	impl_->applyAudioRoutingFlagsLocked();
	impl_->stateReportDirty = true;
}

void Ds5BtManager::setLedColor(uint8_t r, uint8_t g, uint8_t b, uint8_t brightnessLevel)
{
	std::lock_guard<std::mutex> lock(impl_->outputStateMutex);
	impl_->ledRed = r;
	impl_->ledGreen = g;
	impl_->ledBlue = b;
	impl_->ledBrightness = std::min<uint8_t>(brightnessLevel, 2);
	impl_->dsState[1] |= 0x04;  // allow_led_color
	impl_->dsState[38] |= 0x01; // allow_light_brightness_change
	impl_->dsState[41] = 0x00;  // light_fade_animation = nothing
	impl_->dsState[42] = impl_->ledBrightness;
	impl_->dsState[44] = r;
	impl_->dsState[45] = g;
	impl_->dsState[46] = b;
	impl_->stateReportDirty = true;
}

void Ds5BtManager::setPlayerIndex(uint8_t index)
{
	// DualSense 5-LED bar patterns for player 1..4
	static constexpr uint8_t kPlayerMasks[] = {
		0x00, // 0: off
		0x04, // 1: --X--
		0x0A, // 2: -X-X-
		0x15, // 3: X-X-X
		0x1B  // 4: XX-XX
	};
	const uint8_t mask = (index <= 4) ? kPlayerMasks[index] : kPlayerMasks[1];
	std::lock_guard<std::mutex> lock(impl_->outputStateMutex);
	impl_->playerIndicators = mask;
	impl_->dsState[1] |= 0x10; // allow_player_indicators
	impl_->dsState[43] = mask;
	impl_->stateReportDirty = true;
}

void Ds5BtManager::setMicState(bool streamActive, bool muted)
{
	impl_->micMuted.store(muted, std::memory_order_relaxed);
	impl_->micStreamActive.store(streamActive && !muted, std::memory_order_relaxed);

	std::lock_guard<std::mutex> lock(impl_->outputStateMutex);
	if(impl_->desiredMicActive != streamActive || impl_->desiredMicMuted != muted)
	{
		impl_->desiredMicActive = streamActive;
		impl_->desiredMicMuted = muted;
		impl_->micToggleDirty = true;
	}
	else
	{
		impl_->applyAudioRoutingFlagsLocked();
		impl_->stateReportDirty = true;
	}
}

void Ds5BtManager::pushHaptics3kHz(const int16_t *stereoSamples, size_t stereoFrames, float gain)
{
	if(!stereoSamples || stereoFrames == 0 || !impl_->connected.load(std::memory_order_relaxed))
		return;

	bool hasNonZero = false;
	std::vector<int8_t> converted(stereoFrames * 2);
	const float clampedGain = std::clamp(gain, 0.0f, 4.0f);
	for(size_t i = 0; i < stereoFrames * 2; ++i)
	{
		const int32_t scaled = static_cast<int32_t>(std::lround(static_cast<float>(stereoSamples[i]) * clampedGain));
		const int32_t clamped16 = std::clamp<int32_t>(scaled, -32768, 32767);
		const int8_t s8 = static_cast<int8_t>(clamped16 >> 8);
		if(s8 != 0)
			hasNonZero = true;
		converted[i] = s8;
	}

	std::lock_guard<std::mutex> lock(impl_->audioHapticsMutex);
	const auto now = std::chrono::steady_clock::now();
	if(!hasNonZero && !impl_->voiceCoilHapticsActive && impl_->pendingHapticsBlocks.empty())
		return;

	if(hasNonZero)
	{
		impl_->voiceCoilHapticsActive = true;
		impl_->lastHapticsPushTime = now;
	}

	impl_->hapticsPcmFifo.insert(impl_->hapticsPcmFifo.end(), converted.begin(), converted.end());
	while(impl_->hapticsPcmFifo.size() >= kHapticsBlockSize)
	{
		std::array<uint8_t, kHapticsBlockSize> block{};
		for(size_t i = 0; i < kHapticsBlockSize; ++i)
			block[i] = static_cast<uint8_t>(impl_->hapticsPcmFifo[i]);
		impl_->hapticsPcmFifo.erase(
			impl_->hapticsPcmFifo.begin(),
			impl_->hapticsPcmFifo.begin() + static_cast<std::ptrdiff_t>(kHapticsBlockSize));

		// Cap queue latency at ~64 ms (6 blocks)
		if(impl_->pendingHapticsBlocks.size() >= 6)
			impl_->pendingHapticsBlocks.pop_front();
		impl_->pendingHapticsBlocks.push_back(block);
	}
}

void Ds5BtManager::pushSpeakerAudio48kHz(const int16_t *stereoSamples, size_t stereoFrames, int volume, int maxVolume)
{
	if(!stereoSamples || stereoFrames == 0 || !isSpeakerAudioActive() || !impl_->speakerEncoder)
		return;

	const float volScale = (maxVolume > 0)
		? std::clamp(static_cast<float>(volume) / static_cast<float>(maxVolume), 0.0f, 1.0f)
		: 1.0f;

	std::lock_guard<std::mutex> lock(impl_->audioHapticsMutex);
	const size_t totalSamples = stereoFrames * 2;
	const size_t oldSize = impl_->speakerPcmFifo.size();
	impl_->speakerPcmFifo.resize(oldSize + totalSamples);
	if(volScale >= 0.999f)
	{
		std::memcpy(impl_->speakerPcmFifo.data() + oldSize, stereoSamples, totalSamples * sizeof(int16_t));
	}
	else
	{
		for(size_t i = 0; i < totalSamples; ++i)
		{
			const int32_t s = static_cast<int32_t>(std::lround(static_cast<float>(stereoSamples[i]) * volScale));
			impl_->speakerPcmFifo[oldSize + i] = static_cast<int16_t>(std::clamp<int32_t>(s, -32768, 32767));
		}
	}

	// Resample 512 stereo frames (10.67 ms @ 48 kHz) -> 480 stereo frames for 10 ms Opus packet
	constexpr size_t kWindowSamples = kPcmWindowFrames * 2;
	std::array<int16_t, kOpusFrameSamples * 2> resampled{};
	while(impl_->speakerPcmFifo.size() >= kWindowSamples)
	{
		const int16_t *src = impl_->speakerPcmFifo.data();
		for(size_t f = 0; f < static_cast<size_t>(kOpusFrameSamples); ++f)
		{
			const size_t srcFrame = (f * kPcmWindowFrames) / static_cast<size_t>(kOpusFrameSamples);
			resampled[f * 2] = src[srcFrame * 2];
			resampled[f * 2 + 1] = src[srcFrame * 2 + 1];
		}
		impl_->speakerPcmFifo.erase(
			impl_->speakerPcmFifo.begin(),
			impl_->speakerPcmFifo.begin() + static_cast<std::ptrdiff_t>(kWindowSamples));

		std::array<uint8_t, kSpeakerOpusSize> opusPkt{};
		const int encodedBytes = opus_encode(
			impl_->speakerEncoder,
			resampled.data(),
			kOpusFrameSamples,
			opusPkt.data(),
			static_cast<opus_int32>(opusPkt.size()));
		if(encodedBytes > 0)
		{
			if(impl_->pendingSpeakerPackets.size() >= 8)
				impl_->pendingSpeakerPackets.pop_front();
			impl_->pendingSpeakerPackets.push_back(opusPkt);
		}
	}
}

#endif // _WIN32
