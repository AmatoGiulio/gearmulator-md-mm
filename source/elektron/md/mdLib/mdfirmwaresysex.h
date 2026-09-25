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
}
