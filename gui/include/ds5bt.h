#pragma once

#include <chiaki/controller.h>
#include <chiaki/orientation.h>
#include <ds5btidle.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#ifdef _WIN32

class Ds5BtManager
{
public:
	static constexpr int kVirtualDeviceId = 0x4453350; // Synthetic ID for native Bluetooth DualSense

	static Ds5BtManager &instance();

	// Periodically scans Windows Bluetooth HID devices and connects automatically
	// when a DualSense (0x0CE6) or DualSense Edge (0x0DF2) is paired over Bluetooth.
	void startDiscovery(bool waitForInitialScan = false);
	void startPassiveIdleMonitor();
	void setIdlePolicy(Ds5BtIdlePolicy policy);
	void setStreaming(bool streaming);

	// Shuts down worker threads and releases Bluetooth HID handles.
	void shutdown();

	bool isConnected() const;
	bool hasUsbDualSense() const;
	uint16_t productId() const;
	std::string deviceName() const;
	std::string bluetoothAddress() const;

	// Live telemetry from Bluetooth report 0x31
	int batteryPercent() const;
	bool isCharging() const;
	bool isFullyCharged() const;
	bool isHeadphonesPlugged() const;
	bool isHeadsetMicPlugged() const;
	bool isMicMuted() const;
	bool isMicStreamActive() const;

	// Controller speaker / 3.5mm headset audio routing over Bluetooth Report 0x36:
	// - When 3.5mm headphones are plugged into the DualSense jack, or when
	//   speakerAudioEnabled() is turned on by the user, 48 kHz game audio is
	//   encoded to 200-byte Opus packets and streamed over Bluetooth.
	void setSpeakerAudioEnabled(bool enabled);
	bool isSpeakerAudioEnabled() const;
	bool isSpeakerAudioActive() const;

	// Latest CRC-verified controller input state (sticks, triggers, buttons,
	// 2-point touchpad, factory-calibrated 6-axis gyro/accel + orientation quaternion).
	ChiakiControllerState getControllerState() const;
	uint64_t inputSequence() const;
	bool consumeMicButtonPress();
	void resetMotion();

	// Decoded 48 kHz 16-bit PCM callback from DualSense Bluetooth microphone
	// (480 mono frames + 480 stereo frames per 10 ms Opus packet).
	void setMicAudioCallback(std::function<void(const int16_t *mono48k, const int16_t *stereo48k, size_t frames)> cb);

	// DualSense output effects over Bluetooth (0x31 / 0x32 / 0x36)
	void setTriggerEffects(uint8_t leftType, const uint8_t *leftData, uint8_t rightType, const uint8_t *rightData, uint8_t intensity);
	void setRumble(uint8_t left, uint8_t right, uint8_t intensity);
	void setLedColor(uint8_t r, uint8_t g, uint8_t b, uint8_t brightnessLevel = 0);
	void setPlayerIndex(uint8_t index);
	void setMicState(bool streamActive, bool muted);

	// 3000 Hz stereo voice-coil haptics (signed 16-bit PCM stereo frames from PS5 stream)
	void pushHaptics3kHz(const int16_t *stereoSamples, size_t stereoFrames, float gain);

	// 48000 Hz stereo game audio for DualSense built-in speaker / 3.5mm headphone jack
	void pushSpeakerAudio48kHz(const int16_t *stereoSamples, size_t stereoFrames, int volume, int maxVolume);

private:
	Ds5BtManager();
	~Ds5BtManager();
	Ds5BtManager(const Ds5BtManager &) = delete;
	Ds5BtManager &operator=(const Ds5BtManager &) = delete;

	struct Impl;
	Impl *impl_;
};

#endif // _WIN32
