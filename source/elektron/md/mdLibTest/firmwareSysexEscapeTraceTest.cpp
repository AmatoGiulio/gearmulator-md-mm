#include "mdLib/mdfirmwaresysex.h"
#include "mdLib/mdhardware.h"
#include "mdLib/mdmemorymap.h"
#include "mdLib/mdstate.h"
#include "mc68k/cpuState.h"

#include <array>
#include <cstdlib>
#include <deque>
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

	uint32_t read32(md::Microcontroller& uc, const uint32_t addr)
	{
		return (static_cast<uint32_t>(uc.read16(addr)) << 16)
			| uc.read16(addr + 2);
	}

	struct TraceRow
	{
		uint64_t cycles = 0;
		uint32_t pc = 0;
		uint32_t sp = 0;
		uint32_t stack0 = 0;
		uint16_t op0 = 0;
		uint16_t op1 = 0;
		uint32_t vbr = 0;
		uint32_t intLevel = 0;
		uint32_t intMask = 0;
		uint32_t cfMbar = 0;
		uint32_t cfRambar = 0;
		std::array<uint32_t, 8> a{};
		std::array<uint32_t, 8> d{};
	};

	TraceRow capture(md::Microcontroller& uc)
	{
		TraceRow row;
		row.cycles = uc.getCycles();
		row.pc = uc.getPC();
		row.sp = uc.getAReg(7);
		if(executableAddress(row.pc))
		{
			row.op0 = uc.readImm16(row.pc);
			row.op1 = uc.readImm16(row.pc + 2);
		}
		if(row.sp >= md::memorymap::g_mainRam.begin
			&& row.sp + 4 <= md::memorymap::g_mainRam.end)
			row.stack0 = read32(uc, row.sp);
		const auto* cpu = uc.getCpuState();
		row.vbr = cpu->vbr;
		row.intLevel = cpu->int_level;
		row.intMask = cpu->int_mask;
		row.cfMbar = cpu->cf_mbar;
		row.cfRambar = cpu->cf_rambar;
		for(uint32_t i = 0; i < 8; ++i)
		{
			row.a[i] = uc.getAReg(i);
			row.d[i] = uc.getDReg(i);
		}
		return row;
	}

	void printRow(const TraceRow& r, const char* prefix)
	{
		std::cerr << prefix
			<< " cyc=" << std::dec << r.cycles
			<< std::hex << std::setfill('0')
			<< " pc=0x" << std::setw(8) << r.pc
			<< " op=" << std::setw(4) << r.op0 << " " << std::setw(4) << r.op1
			<< " sp=0x" << std::setw(8) << r.sp
			<< " [sp]=0x" << std::setw(8) << r.stack0
			<< " vbr=0x" << std::setw(8) << r.vbr
			<< " il=0x" << std::setw(4) << r.intLevel
			<< " im=0x" << std::setw(4) << r.intMask
			<< " mbar=0x" << std::setw(8) << r.cfMbar
			<< " rambar=0x" << std::setw(8) << r.cfRambar
			<< " a0=0x" << std::setw(8) << r.a[0]
			<< " a1=0x" << std::setw(8) << r.a[1]
			<< " a2=0x" << std::setw(8) << r.a[2]
			<< " a3=0x" << std::setw(8) << r.a[3]
			<< " d0=0x" << std::setw(8) << r.d[0]
			<< " d1=0x" << std::setw(8) << r.d[1]
			<< std::dec << std::endl;
	}

	std::unique_ptr<md::Hardware> makeHardware(const md::FirmwareSysexImage& image)
	{
		auto flash = md::makeMachinedrumOs163DirectBootFlash(image);
		return std::make_unique<md::Hardware>(
			flash, "md-os163-official-syx-direct-boot",
			md::MachineModel::Machinedrum,
			std::vector<uint8_t>{},
			std::shared_ptr<md::FrontPanelPublisher>{},
			std::vector<uint8_t>{},
			std::vector<uint8_t>{},
			md::FlashSectorOverlay{},
			std::vector<uint8_t>{},
			image.mainOs);
	}
}

int main()
{
	const auto* path = std::getenv("GEARMULATOR_MD_FIRMWARE_SYX");
	if(!path || !*path)
	{
		std::cout << "mdFirmwareSysexEscapeTraceTest: SKIP (GEARMULATOR_MD_FIRMWARE_SYX not supplied)\n";
		return 77;
	}

	std::ifstream input(path, std::ios::binary);
	const std::vector<uint8_t> sysex{std::istreambuf_iterator<char>(input), {}};
	md::FirmwareSysexImage image;
	std::string error;
	if(!md::decodeMachinedrumOs163Sysex(image, sysex, error))
	{
		std::cerr << "decode failed: " << error << std::endl;
		return 1;
	}

	std::cerr << "[escape-trace] locating first bad frame..." << std::endl;
	auto probe = makeHardware(image);
	if(!probe->isValid())
		return 1;

	uint32_t badFrame = 0;
	constexpr uint32_t maxFrames = 4410; // 100 ms
	for(uint32_t frame = 1; frame <= maxFrames; ++frame)
	{
		probe->advance(1);
		const auto pc = probe->getUC().getPC();
		if(!executableAddress(pc))
		{
			badFrame = frame;
			std::cerr << "[escape-trace] first invalid frame=" << badFrame
				<< " pc=0x" << std::hex << pc << std::dec
				<< " dsp1=" << probe->getDspMixer().booted()
				<< " dsp2=" << probe->getDspProducer().booted()
				<< std::endl;
			break;
		}
	}

	if(!badFrame)
	{
		std::cerr << "[escape-trace] no invalid PC within 100ms" << std::endl;
		return 0;
	}

	// Replay to the frame immediately before the escape, then single-step the
	// ColdFire. At this point both DSPs are expected to still be in reset, so
	// direct UC stepping preserves the part of the boot we need to inspect.
	auto trace = makeHardware(image);
	if(badFrame > 1)
		trace->advance(badFrame - 1);

	auto& uc = trace->getUC();

	// ColdFire VBR ignores the low 20 bits, so only 1 MiB boundaries are valid.
	// Probe the mapped boundaries that could plausibly contain the exception table
	// prepared by the skipped bootstrap.
	constexpr std::array<uint32_t, 8> vbrCandidates{{
		0x00000000u, 0x00100000u, 0x00200000u, 0x00700000u,
		0x01000000u, 0x10000000u, 0x20000000u, 0x40000000u
	}};
	for(const auto base : vbrCandidates)
	{
		const auto v11 = read32(uc, base + 0x11u * 4u);
		const auto v19 = read32(uc, base + 0x19u * 4u);
		const auto v1b = read32(uc, base + 0x1bu * 4u);
		std::cerr << "[escape-trace] VBR candidate 0x"
			<< std::hex << std::setw(8) << std::setfill('0') << base
			<< " vec11=0x" << std::setw(8) << v11
			<< " vec19=0x" << std::setw(8) << v19
			<< " vec1b=0x" << std::setw(8) << v1b
			<< " valid11=" << std::dec << executableAddress(v11)
			<< std::endl;
	}

	const auto flashNow = uc.copyFlashData();
	size_t programmedLowFlash = 0;
	for(size_t i = 8; i < 0x100000 && i < flashNow.size(); ++i)
		if(flashNow[i] != 0xff)
			++programmedLowFlash;
	std::cerr << "[escape-trace] programmed low-flash bytes (excluding reset vectors)="
		<< programmedLowFlash << std::endl;
	std::cerr << "[escape-trace] flash[0x40..0x4f]=";
	for(size_t i = 0x40; i < 0x50 && i < flashNow.size(); ++i)
		std::cerr << ' ' << std::hex << std::setw(2) << std::setfill('0')
			<< static_cast<unsigned>(flashNow[i]);
	std::cerr << std::dec << std::endl;

	std::deque<TraceRow> history;
	constexpr size_t historySize = 24;
	constexpr uint32_t maxInstructions = 20000;

	for(uint32_t i = 0; i < maxInstructions; ++i)
	{
		if(history.size() == historySize)
			history.pop_front();
		history.push_back(capture(uc));

		uc.exec();
		if(!executableAddress(uc.getPC()))
		{
			std::cerr << "[escape-trace] exact escape after instruction " << i
				<< " -> pc=0x" << std::hex << uc.getPC() << std::dec << std::endl;
			for(const auto& row : history)
				printRow(row, "  before");
			const auto after = capture(uc);
			printRow(after, "  AFTER ");
			if((after.stack0 >> 28) >= 4 && (after.stack0 >> 28) <= 7)
			{
				const auto vector = static_cast<uint8_t>((after.stack0 >> 18) & 0xff);
				const auto vectorAddress = after.vbr + static_cast<uint32_t>(vector) * 4u;
				const auto vectorTarget = read32(uc, vectorAddress);
				std::cerr << "[escape-trace] ColdFire exception frame: vector=0x"
					<< std::hex << static_cast<unsigned>(vector)
					<< " VBR=0x" << after.vbr
					<< " vectorAddress=0x" << vectorAddress
					<< " target=0x" << vectorTarget
					<< " savedPC=0x" << read32(uc, after.sp + 4)
					<< std::dec << std::endl;
			}
			return 0;
		}
	}

	std::cerr << "[escape-trace] frame boundary found, but no escape in "
		<< maxInstructions << " direct UC instructions" << std::endl;
	return 2;
}
