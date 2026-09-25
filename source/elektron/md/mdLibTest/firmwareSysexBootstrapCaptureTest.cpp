#include "mdLib/mdfirmwaresysex.h"
#include "mdLib/mdflash.h"
#include "mdLib/mdhardware.h"
#include "mdLib/mdmemorymap.h"
#include "mdLib/mdrom.h"
#include "mc68k/cpuState.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <vector>

namespace
{
	void dumpBytes(const std::vector<uint8_t>& data, const size_t offset, const size_t count)
	{
		std::cerr << std::hex << std::setfill('0');
		for(size_t i = 0; i < count && offset + i < data.size(); ++i)
			std::cerr << (i ? " " : "") << std::setw(2)
				<< static_cast<unsigned>(data[offset + i]);
		std::cerr << std::dec << std::endl;
	}

	bool mappedPc(const uint32_t pc)
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
}

int main()
{
	const auto* path = std::getenv("GEARMULATOR_MD_FIRMWARE_SYX");
	if(!path || !*path)
	{
		std::cout << "mdFirmwareSysexBootstrapCaptureTest: SKIP (GEARMULATOR_MD_FIRMWARE_SYX not supplied)\n";
		return 77;
	}

	std::ifstream input(path, std::ios::binary);
	const std::vector<uint8_t> sysex{std::istreambuf_iterator<char>(input), {}};
	md::FirmwareSysexImage image;
	std::string error;
	if(!md::decodeMachinedrumOs163Sysex(image, sysex, error))
	{
		std::cerr << "[capture] decode failed: " << error << std::endl;
		return 1;
	}

	auto seedFlash = md::makeMachinedrumOs163DirectBootFlash(image);
	auto hw = std::make_unique<md::Hardware>(
		seedFlash, "md-os163-official-syx-bootstrap-capture",
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

	auto& uc = hw->getUC();
	uc.getCpuState()->vbr = md::memorymap::g_internalSram.begin;

	uint64_t programWords = 0;
	uint64_t eraseSectors = 0;
	uint64_t firstFlashCycle = 0;
	uint64_t lastFlashCycle = 0;
	uc.setFlashOperationObserver([&](const md::FlashCommandDecoder::Operation& op,
		const uint64_t cycle)
	{
		if(!firstFlashCycle)
			firstFlashCycle = cycle;
		lastFlashCycle = cycle;
		if(op.type == md::FlashCommandDecoder::Operation::Type::ProgramWord)
			++programWords;
		else if(op.type == md::FlashCommandDecoder::Operation::Type::EraseSector)
			++eraseSectors;
	});

	std::cerr << "[capture] running direct MAIN OS for 5s" << std::endl;
	hw->advance(220500);

	const auto idlePc = uc.getPC();
	const auto idleOp = mappedPc(idlePc) ? uc.readImm16(idlePc) : 0xffff;
	std::cerr << "[capture] end pc=0x" << std::hex << idlePc
		<< " op=0x" << std::setw(4) << std::setfill('0') << idleOp
		<< std::dec
		<< " flashDirty=" << uc.flashDirty()
		<< " flashIdleCycles=" << uc.flashIdleCycles()
		<< " programWords=" << programWords
		<< " eraseSectors=" << eraseSectors
		<< " firstFlashCycle=" << firstFlashCycle
		<< " lastFlashCycle=" << lastFlashCycle
		<< std::endl;

	auto flash = uc.copyFlashData();
	const auto lowEnd = std::min<size_t>(0x100000, flash.size());
	const auto programmed = static_cast<size_t>(std::count_if(
		flash.begin(), flash.begin() + lowEnd,
		[](const uint8_t b) { return b != 0xff; }));
	std::cerr << "[capture] programmed bytes in low 1MiB=" << programmed << std::endl;
	std::cerr << "[capture] flash[0x0000..0x003f]=";
	dumpBytes(flash, 0, 64);
	std::cerr << "[capture] flash[0x0040..0x007f]=";
	dumpBytes(flash, 0x40, 64);

	md::Rom capturedRom(flash, "captured-updater-flash");
	if(!capturedRom.isValid())
	{
		std::cerr << "[capture] captured flash rejected by Rom size validation" << std::endl;
		return 1;
	}

	std::cerr << "[capture] cold reboot from captured flash" << std::endl;
	auto reboot = std::make_unique<md::Microcontroller>(
		capturedRom, md::MachineModel::Machinedrum,
		std::vector<uint8_t>{}, std::vector<uint8_t>{});
	reboot->reset();
	std::cerr << "[capture] reset sp=0x" << std::hex << reboot->getAReg(7)
		<< " pc=0x" << reboot->getPC()
		<< " op=0x" << std::setw(4) << std::setfill('0')
		<< (mappedPc(reboot->getPC()) ? reboot->readImm16(reboot->getPC()) : 0xffff)
		<< std::dec << std::endl;

	for(uint32_t i = 0; i < 50000; ++i)
	{
		reboot->exec();
		if(!mappedPc(reboot->getPC()))
		{
			std::cerr << "[capture] reboot escaped after " << i + 1
				<< " instructions pc=0x" << std::hex << reboot->getPC()
				<< std::dec << std::endl;
			return 2;
		}
	}

	std::cerr << "[capture] reboot survived 50000 instructions pc=0x"
		<< std::hex << reboot->getPC()
		<< " sp=0x" << reboot->getAReg(7)
		<< " vbr=0x" << reboot->getCpuState()->vbr
		<< " mbar=0x" << reboot->getCpuState()->cf_mbar
		<< " rambar=0x" << reboot->getCpuState()->cf_rambar
		<< std::dec << std::endl;

	std::cout << "Machinedrum OS 1.63 updater flash capture/reboot probe completed\n";
	return 0;
}
