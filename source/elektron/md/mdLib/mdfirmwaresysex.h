#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace md
{
	struct FirmwareSysexImage
	{
		std::string version;
		std::vector<uint8_t> decodedTransport;
		std::vector<uint8_t> mainOs;
		std::vector<uint8_t> dsp1;
		std::vector<uint8_t> dsp2;
		std::vector<uint8_t> factoryWaveforms;
	};

	bool decodeMachinedrumOs163Sysex(FirmwareSysexImage& _out,
		const std::vector<uint8_t>& _sysex, std::string& _error);

	// Build the bootable 8 MiB Machinedrum OS 1.63 flash image from Elektron's
	// official updater. No external ROM dump is required.
	bool buildMachinedrumOs163Flash(std::vector<uint8_t>& _out,
		const FirmwareSysexImage& _image, std::string& _error);
	bool buildMachinedrumOs163FlashFromSysex(std::vector<uint8_t>& _out,
		const std::vector<uint8_t>& _sysex, std::string& _error);

	// Internal seed used while reconstructing the persistent bootstrap.
	std::vector<uint8_t> makeMachinedrumOs163DirectBootFlash(
		const FirmwareSysexImage& _image);
}
