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

	// Experimental clean-room bootstrap image for the official updater path.
	// It is deliberately NOT a canonical Elektron ROM: only reset vectors and
	// the updater-provided factory waveform bank are staged. The decoded MAIN OS
	// is loaded separately into 0x00200000 before reset.
	std::vector<uint8_t> makeMachinedrumOs163DirectBootFlash(
		const FirmwareSysexImage& _image);
}
