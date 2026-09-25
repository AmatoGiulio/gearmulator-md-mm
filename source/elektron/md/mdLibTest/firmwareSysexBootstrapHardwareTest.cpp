#include "mdLib/mdfirmwaresysex.h"
#include "mdLib/mdhardware.h"
#include "mdLib/mdmemorymap.h"
#include "mdLib/mdpanel.h"
#include "mdLib/mdtypes.h"
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
	std::unique_ptr<md::Hardware> makeDirectMainOsHardware(
		const md::FirmwareSysexImage& image)
	{
		auto flash = md::makeMachinedrumOs163DirectBootFlash(image);
		return std::make_unique<md::Hardware>(
			flash, "md-os163-official-syx-bootstrap-seed",
			md::MachineModel::Machinedrum,
			std::vector<uint8_t>{},
			std::shared_ptr<md::FrontPanelPublisher>{},
			std::vector<uint8_t>{},
			std::vector<uint8_t>{},
			md::FlashSectorOverlay{},
			std::vector<uint8_t>{},
			image.mainOs);
	}

	void report(md::Hardware& hw, const char* label)
	{
		auto& uc = hw.getUC();
		std::cerr << "[hw-bootstrap] " << label
			<< " pc=0x" << std::hex << uc.getPC()
			<< " sp=0x" << uc.getAReg(7)
			<< " vbr=0x" << uc.getCpuState()->vbr
			<< std::dec
			<< " dsp1=" << hw.getDspMixer().booted()
			<< " dsp2=" << hw.getDspProducer().booted()
			<< " panel=" << uc.isPanelHandshakeComplete()
			<< " midiRx=" << uc.isMidiReceiveReady()
			<< " midiConsumed=" << uc.midiRxConsumedCount()
			<< std::endl;
	}
}

int main()
{
	const auto* path = std::getenv("GEARMULATOR_MD_FIRMWARE_SYX");
	if(!path || !*path)
	{
		std::cout << "mdFirmwareSysexBootstrapHardwareTest: SKIP (GEARMULATOR_MD_FIRMWARE_SYX not supplied)\n";
		return 77;
	}

	std::ifstream input(path, std::ios::binary);
	const std::vector<uint8_t> sysex{std::istreambuf_iterator<char>(input), {}};
	md::FirmwareSysexImage image;
	std::string error;
	if(!md::decodeMachinedrumOs163Sysex(image, sysex, error))
	{
		std::cerr << "[hw-bootstrap] decode failed: " << error << std::endl;
		return 1;
	}

	std::cerr << "[hw-bootstrap] phase1 reconstructing persistent bootstrap" << std::endl;
	auto seed = makeDirectMainOsHardware(image);
	if(!seed->isValid())
		return 1;
	seed->getUC().getCpuState()->vbr = md::memorymap::g_internalSram.begin;
	seed->advance(220500);
	auto bootstrapFlash = seed->getUC().copyFlashData();
	seed.reset();

	// Full Hardware is required here. The reconstructed bootstrap performs a DSP1
	// startup/test before the early-startup menu; a bare Microcontroller leaves the
	// HI08/DSP side absent and deliberately lands on the ERROR DSP1 halt loop.
	std::cerr << "[hw-bootstrap] phase2 cold boot with both DSPs attached" << std::endl;
	auto hw = std::make_unique<md::Hardware>(
		bootstrapFlash, "md-os163-reconstructed-bootstrap",
		md::MachineModel::Machinedrum,
		std::vector<uint8_t>{},
		std::shared_ptr<md::FrontPanelPublisher>{},
		bootstrapFlash,
		std::vector<uint8_t>{},
		md::FlashSectorOverlay{},
		std::vector<uint8_t>{},
		image.mainOs);
	if(!hw->isValid())
		return 1;

	auto& uc = hw->getUC();
	std::vector<uint8_t> panelTx;
	uc.setPanelTransmitTap([&](const uint8_t byte)
	{
		if(panelTx.size() < 256)
			panelTx.push_back(byte);
	});

	const auto function = md::panelPacket(md::MachineModel::Machinedrum,
		md::PanelControl::Function);
	const auto trig5 = md::panelPacket(md::MachineModel::Machinedrum,
		md::PanelControl::Trigger5);
	if(!function || !trig5)
		return 2;

	// Hardware constructor has consumed reset timing only; no real bootstrap
	// instruction has retired yet, so this is equivalent to holding FUNCTION at power-on.
	uc.queuePanelRx(function->row);
	uc.queuePanelRx(function->mask);
	report(*hw, "reset+FUNCTION");

	hw->advance(44100); // 1 s
	report(*hw, "1s held");
	std::cerr << "[hw-bootstrap] panelTx:";
	for(const auto byte : panelTx)
		std::cerr << " " << std::hex << std::setw(2) << std::setfill('0')
			<< static_cast<unsigned>(byte);
	std::cerr << std::dec << std::endl;

	// Release FUNCTION, then select MIDI UPGRADE with TRIG 5.
	uc.queuePanelRx(function->row);
	uc.queuePanelRx(0x00);
	hw->advance(4410); // 100 ms
	uc.queuePanelRx(trig5->row);
	uc.queuePanelRx(trig5->mask);
	hw->advance(2205); // 50 ms
	uc.queuePanelRx(trig5->row);
	uc.queuePanelRx(0x00);
	hw->advance(8820); // 200 ms
	report(*hw, "after TRIG5");

	// Previous probe proved that MIDI UPGRADE consumes UART1 bytes here. Do NOT
	// consume F0 as a standalone liveness probe: leaving the receiver inside an
	// open SysEx frame for hundreds of milliseconds can invalidate packet zero.
	// Start the real transfer at byte zero and keep every firmware packet intact.
	report(*hw, "MIDI UPGRADE ready");

	// Feed the official updater one complete SysEx message at a time. Waiting for
	// UART1 RX to drain before admitting the next message preserves protocol
	// boundaries without forcing the ~9 minute physical DIN baud rate into this
	// headless test.
	uint64_t programWords = 0;
	uint64_t eraseSectors = 0;
	uc.setFlashOperationObserver([&](const md::FlashCommandDecoder::Operation& op,
		const uint64_t)
	{
		if(op.type == md::FlashCommandDecoder::Operation::Type::ProgramWord)
			++programWords;
		else if(op.type == md::FlashCommandDecoder::Operation::Type::EraseSector)
			++eraseSectors;
	});

	size_t cursor = 0
	size_t messageIndex = 0;
	constexpr uint32_t maxDrainFramesPerMessage = 44100 * 2; // 2 s emulated
	while(cursor < sysex.size())
	{
		const auto eox = std::find(sysex.begin() + cursor, sysex.end(), uint8_t{0xf7});
		if(eox == sysex.end())
		{
			std::cerr << "[hw-bootstrap] malformed updater stream at byte " << cursor << std::endl;
			return 5;
		}
		const auto end = static_cast<size_t>(eox - sysex.begin()) + 1;

		for(; cursor < end; ++cursor)
		{
			if(!uc.tryQueueMidiRx(sysex[cursor]))
			{
				std::cerr << "[hw-bootstrap] UART1 RX full at byte " << cursor << std::endl;
				return 6;
			}
		}

		uint32_t frames = 0;
		while(uc.queuedMidiRxBytes() != 0 && frames < maxDrainFramesPerMessage)
		{
			hw->advance(1);
			++frames;
		}
		if(uc.queuedMidiRxBytes() != 0)
		{
			std::cerr << "[hw-bootstrap] message " << messageIndex
				<< " stalled queued=" << uc.queuedMidiRxBytes()
				<< " pc=0x" << std::hex << uc.getPC() << std::dec << std::endl;
			return 7;
		}

		// Give packet validation / flash programming a small scheduler window before
		// the next SysEx frame. This is protocol pacing, not physical MIDI baud pacing.
		hw->advance(4);
		++messageIndex;

		if((messageIndex % 1000) == 0 || cursor == sysex.size())
		{
			std::cerr << "[hw-bootstrap] messages=" << messageIndex
				<< " bytes=" << cursor << "/" << sysex.size()
				<< " consumed=" << uc.midiRxConsumedCount()
				<< " pc=0x" << std::hex << uc.getPC() << std::dec
				<< " programWords=" << programWords
				<< " eraseSectors=" << eraseSectors
				<< std::endl;
		}
	}

	std::cerr << "[hw-bootstrap] updater stream drained; allowing final flash work" << std::endl;
	hw->advance(44100 * 5);

	const auto installedFlash = uc.copyFlashData();
	uint64_t fingerprint = 14695981039346656037ull;
	for(const auto byte : installedFlash)
	{
		fingerprint ^= byte;
		fingerprint *= 1099511628211ull;
	}
	const bool canonical = fingerprint == md::g_mdOs163Fingerprint;
	std::cerr << "[hw-bootstrap] final pc=0x" << std::hex << uc.getPC()
		<< " flashFnv=0x" << fingerprint
		<< " canonical=0x" << md::g_mdOs163Fingerprint
		<< std::dec
		<< " canonicalImage=" << canonical
		<< " consumed=" << uc.midiRxConsumedCount()
		<< " overflow=" << uc.midiRxOverflowCount()
		<< " programWords=" << programWords
		<< " eraseSectors=" << eraseSectors
		<< " dsp1=" << hw->getDspMixer().booted()
		<< " dsp2=" << hw->getDspProducer().booted()
		<< std::endl;

	if(canonical)
	{
		std::cerr << "[hw-bootstrap] cold booting canonical installed flash through normal Hardware" << std::endl;
		auto normal = std::make_unique<md::Hardware>(
			installedFlash, "md-os163-installed-from-official-syx",
			md::MachineModel::Machinedrum);
		if(!normal->isValid())
			return 8;
		normal->advance(220500);
		report(*normal, "canonical cold boot 5s");
	}

	std::cout << "Machinedrum official SysEx full bootstrap install probe completed\n";
	return canonical ? 0 : 9;
}
