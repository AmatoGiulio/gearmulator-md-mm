#include "mdLib/mdfirmwaresysex.h"
#include "mdLib/mdhardware.h"
#include "mdLib/mdmemorymap.h"
#include "mdLib/mdstate.h"
#include "mc68k/cpuState.h"

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
	bool executableAddress(const uint32_t pc)
	{
		using namespace md::memorymap;
		return g_flashLow.contains(pc)
			|| g_patchBootstrap.contains(pc)
			|| g_mainRam.contains(pc)
			|| g_loaderRam.contains(pc)
			|| g_patchOsAlias.contains(pc)
			|| g_internalSram.contains(pc)
			|| g_flashFull.contains(pc)
			|| g_mainHighAlias.contains(pc)
			|| g_mainExecAlias.contains(pc);
	}

	void report(md::Hardware& hw, const char* label)
	{
		auto& uc = hw.getUC();
		const auto* cpu = uc.getCpuState();
		std::cerr << "[vbr-boot] " << label
			<< " pc=0x" << std::hex << std::setw(8) << std::setfill('0') << uc.getPC()
			<< " sp=0x" << std::setw(8) << uc.getAReg(7)
			<< " vbr=0x" << std::setw(8) << cpu->vbr
			<< " il=0x" << std::setw(4) << cpu->int_level
			<< " im=0x" << std::setw(4) << cpu->int_mask
			<< std::dec
			<< " ucCycles=" << uc.getCycles()
			<< " dsp1=" << hw.getDspMixer().booted()
			<< " dsp2=" << hw.getDspProducer().booted()
			<< " panel=" << uc.isPanelHandshakeComplete()
			<< " midiRx=" << uc.isMidiReceiveReady()
			<< " ready=" << hw.isFirmwareMidiReady()
			<< " pcValid=" << executableAddress(uc.getPC())
			<< std::endl;
	}
}

int main()
{
	const auto* path = std::getenv("GEARMULATOR_MD_FIRMWARE_SYX");
	if(!path || !*path)
	{
		std::cout << "mdFirmwareSysexVbrBootTest: SKIP (GEARMULATOR_MD_FIRMWARE_SYX not supplied)\n";
		return 77;
	}

	std::ifstream input(path, std::ios::binary);
	const std::vector<uint8_t> sysex{std::istreambuf_iterator<char>(input), {}};
	md::FirmwareSysexImage image;
	std::string error;
	if(!md::decodeMachinedrumOs163Sysex(image, sysex, error))
	{
		std::cerr << "[vbr-boot] decode failed: " << error << std::endl;
		return 1;
	}

	auto flash = md::makeMachinedrumOs163DirectBootFlash(image);
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
		return 1;

	// The decoded MAIN OS builds a live exception table in the MCF5206E's
	// 64 KiB internal SRAM (0x01000000). The canonical bootstrap normally
	// establishes VBR before handing control to the RAM-linked MAIN OS.
	auto& uc = hw->getUC();
	uc.getCpuState()->vbr = md::memorymap::g_internalSram.begin;
	std::cerr << "[vbr-boot] forcing bootstrap VBR=0x01000000" << std::endl;
	report(*hw, "start");

	struct Milestone { uint32_t frames; const char* label; };
	constexpr std::array<Milestone, 6> milestones{{
		{441, "10ms"},
		{1323, "30ms"},
		{4410, "100ms"},
		{22050, "500ms"},
		{44100, "1s"},
		{220500, "5s"},
	}};

	uint32_t advanced = 0;
	for(const auto& milestone : milestones)
	{
		const auto delta = milestone.frames - advanced;
		std::cerr << "[vbr-boot] advancing to " << milestone.label << std::endl;
		hw->advance(delta);
		advanced = milestone.frames;
		report(*hw, milestone.label);
		if(!executableAddress(uc.getPC()))
		{
			std::cerr << "[vbr-boot] escaped mapped executable memory" << std::endl;
			return 2;
		}
		if(hw->isFirmwareMidiReady())
		{
			std::cout << "Machinedrum OS 1.63 direct boot reached firmware-ready state at "
				<< milestone.label << "\n";
			return 0;
		}
	}

	std::cout << "Machinedrum OS 1.63 VBR boot probe remained executable through 5s\n";
	return 0;
}
