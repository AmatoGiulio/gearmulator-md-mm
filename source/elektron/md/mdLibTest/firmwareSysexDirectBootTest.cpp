#include "mdLib/mdfirmwaresysex.h"
#include "mdLib/mdmc.h"
#include "mdLib/mdrom.h"
#include "mdLib/mdtypes.h"

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <vector>

int main()
{
	const auto* path = std::getenv("GEARMULATOR_MD_FIRMWARE_SYX");
	if(!path || !*path)
	{
		std::cout << "mdFirmwareSysexDirectBootTest: SKIP (GEARMULATOR_MD_FIRMWARE_SYX not supplied)\n";
		return 77;
	}

	std::cerr << "[direct-boot] reading SysEx: " << path << std::endl;
	std::ifstream input(path, std::ios::binary);
	const std::vector<uint8_t> sysex{std::istreambuf_iterator<char>(input), {}};
	std::cerr << "[direct-boot] bytes=" << sysex.size() << std::endl;
	md::FirmwareSysexImage image;
	std::string error;
	std::cerr << "[direct-boot] decoding updater..." << std::endl;
	if(!md::decodeMachinedrumOs163Sysex(image, sysex, error))
	{
		std::cerr << "decode failed: " << error << '\n';
		return 1;
	}

	std::cerr << "[direct-boot] decoded MAIN=" << image.mainOs.size()
		<< " DSP1=" << image.dsp1.size() << " DSP2=" << image.dsp2.size()
		<< " wave=" << image.factoryWaveforms.size() << std::endl;
	std::cerr << "[direct-boot] creating synthetic flash..." << std::endl;
	auto flash = md::makeMachinedrumOs163DirectBootFlash(image);
	std::cerr << "[direct-boot] flash bytes=" << flash.size() << std::endl;
	md::Rom rom(flash, "md-os163-official-syx-direct-boot");
	if(!rom.isValid())
	{
		std::cerr << "synthetic direct-boot flash is invalid\n";
		return 1;
	}

	std::cerr << "[direct-boot] constructing ColdFire..." << std::endl;
	auto uc = std::make_unique<md::Microcontroller>(
		rom, md::MachineModel::Machinedrum, std::vector<uint8_t>{}, std::vector<uint8_t>{});
	std::cerr << "[direct-boot] ColdFire constructed" << std::endl;
	if(!uc->stageDirectBootMainOs(image.mainOs))
	{
		std::cerr << "could not stage MAIN OS at 0x00200000\n";
		return 1;
	}

	std::cerr << std::hex << std::setfill('0')
		<< "[direct-boot] vectors sp=0x" << std::setw(8) << uc->getResetSP()
		<< " pc=0x" << std::setw(8) << uc->getResetPC() << std::dec << std::endl;
	std::cerr << "[direct-boot] calling reset()" << std::endl;
	uc->reset();
	std::cerr << "[direct-boot] reset() returned" << std::endl;
	const auto resetPc = uc->getPC();
	const auto resetSp = uc->getAReg(7);
	std::cout << std::hex << std::setfill('0')
		<< "reset  pc=0x" << std::setw(8) << resetPc
		<< " sp=0x" << std::setw(8) << resetSp << '\n';

	if(resetPc != 0x00200000u || resetSp != 0x00300000u)
	{
		std::cerr << "direct-boot reset vectors were not applied\n";
		return 1;
	}

	// The first exec consumes the ColdFire reset exception cycles.
	std::cerr << "[direct-boot] consuming reset cycles" << std::endl;
	const auto resetCycles = uc->exec();
	std::cerr << "[direct-boot] executing first MAIN OS instruction" << std::endl;
	const auto firstCycles = uc->exec();
	std::cerr << "[direct-boot] first instruction returned" << std::endl;
	const auto firstPc = uc->getPC();
	const auto firstSp = uc->getAReg(7);
	std::cout << "first  pc=0x" << std::setw(8) << firstPc
		<< " sp=0x" << std::setw(8) << firstSp
		<< std::dec << " resetCycles=" << resetCycles
		<< " instrCycles=" << firstCycles << '\n';

	// MAIN OS starts with MOVEA.L #$00300000,A7 (6 bytes).
	if(firstPc != 0x00200006u || firstSp != 0x00300000u)
	{
		std::cerr << "MAIN OS entry instruction did not execute as expected\n";
		return 1;
	}

	for(int i = 0; i < 31; ++i)
		uc->exec();

	std::cout << std::hex
		<< "after32 pc=0x" << std::setw(8) << uc->getPC()
		<< " sp=0x" << std::setw(8) << uc->getAReg(7)
		<< std::dec << " totalCycles=" << uc->getCycles() << '\n';

	if(uc->getPC() == 0 || uc->getPC() == 0xffffffffu)
	{
		std::cerr << "MAIN OS execution escaped to an invalid PC\n";
		return 1;
	}

	std::cout << "Machinedrum OS 1.63 direct MAIN OS boot probe passed\n";
	return 0;
}
