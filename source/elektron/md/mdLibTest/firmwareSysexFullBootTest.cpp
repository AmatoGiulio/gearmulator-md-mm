#include "mdLib/mdfirmwaresysex.h"
#include "mdLib/mdhardware.h"
#include "mdLib/mdstate.h"

#include <array>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <vector>

namespace
{
	void report(md::Hardware& hw, const char* label)
	{
		auto& uc = hw.getUC();
		char disasm[128] = {};
		uc.disassemble(uc.getPC(), disasm);
		std::cerr << "[full-boot] " << label
			<< " pc=0x" << std::hex << std::setw(8) << std::setfill('0') << uc.getPC()
			<< " sp=0x" << std::setw(8) << uc.getAReg(7)
			<< std::dec
			<< " ucCycles=" << uc.getCycles()
			<< " dsp1=" << hw.getDspMixer().booted()
			<< " dsp2=" << hw.getDspProducer().booted()
			<< " panel=" << uc.isPanelHandshakeComplete()
			<< " midiRx=" << uc.isMidiReceiveReady()
			<< " ready=" << hw.isFirmwareMidiReady()
			<< " next=\"" << disasm << "\""
			<< std::endl;
	}
}

int main()
{
	const auto* path = std::getenv("GEARMULATOR_MD_FIRMWARE_SYX");
	if(!path || !*path)
	{
		std::cout << "mdFirmwareSysexFullBootTest: SKIP (GEARMULATOR_MD_FIRMWARE_SYX not supplied)\n";
		return 77;
	}

	std::cerr << "[full-boot] reading " << path << std::endl;
	std::ifstream input(path, std::ios::binary);
	const std::vector<uint8_t> sysex{std::istreambuf_iterator<char>(input), {}};
	md::FirmwareSysexImage image;
	std::string error;
	if(!md::decodeMachinedrumOs163Sysex(image, sysex, error))
	{
		std::cerr << "[full-boot] decode failed: " << error << std::endl;
		return 1;
	}

	auto flash = md::makeMachinedrumOs163DirectBootFlash(image);
	std::cerr << "[full-boot] constructing MD hardware" << std::endl;
	auto hw = std::make_unique<md::Hardware>(
		flash, "md-os163-official-syx-direct-boot",
		md::MachineModel::Machinedrum,
		std::vector<uint8_t>{},
		std::shared_ptr<md::FrontPanelPublisher>{},
		std::vector<uint8_t>{},
		std::vector<uint8_t>{},
		md::FlashSectorOverlay{},
		std::vector<uint8_t>{},
		image.mainOs);

	if(!hw->isValid())
	{
		std::cerr << "[full-boot] hardware rejected direct-boot image" << std::endl;
		return 1;
	}

	report(*hw, "start");

	struct Milestone { uint32_t frames; const char* label; };
	constexpr std::array<Milestone, 4> milestones{{
		{44, "1ms"},
		{441, "10ms"},
		{4410, "100ms"},
		{44100, "1s"},
	}};

	uint32_t advanced = 0;
	for(const auto& milestone : milestones)
	{
		const auto delta = milestone.frames - advanced;
		std::cerr << "[full-boot] advancing to " << milestone.label << std::endl;
		hw->advance(delta);
		advanced = milestone.frames;
		report(*hw, milestone.label);
	}

	std::cout << "Machinedrum OS 1.63 full-hardware direct boot probe completed\n";
	return 0;
}
