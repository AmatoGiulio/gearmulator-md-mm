#pragma once

#include <cstdint>

namespace md
{
	enum class MachineModel : uint32_t
	{
		Machinedrum,
		Monomachine
	};

	enum class RamRecordingMode : uint8_t
	{
		Original,
		CompleteTail
	};

	// DeviceCreateParams::customData is serialized by the remote-device bridge. Keep the
	// legacy zero value as Machinedrum, while tagged values make new callers explicit.
	static constexpr uint32_t g_deviceCustomDataMachinedrum = 0x4d440001; // "MD"
	static constexpr uint32_t g_deviceCustomDataMonomachine = 0x4d4d0001; // "MM"
	// Builds made during scheduler development serialized three now-retired feature
	// bits in the low byte. Ignore them when reading old bridge parameters; current
	// callers serialize only the model because there is one supported execution path.
	static constexpr uint32_t g_legacyDeviceFeatureMask = 0x0000000e;
	static constexpr uint32_t g_deviceCustomDataModelMask = ~g_legacyDeviceFeatureMask;

	constexpr uint32_t deviceCustomData(const MachineModel _model)
	{
		return _model == MachineModel::Monomachine
			? g_deviceCustomDataMonomachine
			: g_deviceCustomDataMachinedrum;
	}

	constexpr MachineModel machineModelFromDeviceCustomData(const uint32_t _customData)
	{
		return (_customData & g_deviceCustomDataModelMask) == g_deviceCustomDataMonomachine
			? MachineModel::Monomachine
			: MachineModel::Machinedrum;
	}

	// MD OS 1.63 and MM OS 1.32b are distributed as 8 MiB firmware images.
	static constexpr uint32_t g_romSize		= 0x800000;

	// Both emulated machines run their codec path at 44.1 kHz.
	static constexpr uint32_t g_samplerate	= 44100;
	inline constexpr uint64_t g_ucClockHz = 40'000'000;

	// FNV-1a compatibility identifiers for the canonical complete images below.
	// The SHA-1 identities are included so these values can be checked against an
	// independently catalogued digest rather than treated as unexplained constants.
	//   elektron_sps1-1uw_os1.63.bin: a872a2f3527063673d6ea6d3080c4c62ef0cadc1
	//   elektron_sfx6-60_os1.32b.bin: 11a37460a5f47fd1a4d911414288690e6e7da605
	static constexpr uint64_t g_mdOs163Fingerprint = 0x33b7c1a9e29f43fdull;
	// Exact 8 MiB image reconstructed from Elektron's official SPS-1UW OS 1.63
	// updater: persistent 16 KiB bootstrap + decoded transport at 0x4000 + the
	// official factory waveform bank. The updater-emulation probe proved this is
	// byte-for-byte identical to the flash image programmed by the updater.
	static constexpr uint64_t g_mdOs163OfficialSyxFingerprint = 0x6146bc7a7cf67fa0ull;
	constexpr bool isMdOs163Fingerprint(const uint64_t _fingerprint)
	{
		return _fingerprint == g_mdOs163Fingerprint
			|| _fingerprint == g_mdOs163OfficialSyxFingerprint;
	}
	static constexpr uint64_t g_mmOs132bFingerprint = 0xe1c1b461b6d0f21bull;
}
