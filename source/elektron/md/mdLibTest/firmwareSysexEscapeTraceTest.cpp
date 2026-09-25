#include "mdLib/mdfirmwaresysex.h"
#include "mdLib/mdhardware.h"
#include "mdLib/mdmemorymap.h"
#include "mdLib/mdstate.h"

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
			printRow(capture(uc), "  AFTER ");
			return 0;
		}
	}

	std::cerr << "[escape-trace] frame boundary found, but no escape in "
		<< maxInstructions << " direct UC instructions" << std::endl;
	return 2;
}
