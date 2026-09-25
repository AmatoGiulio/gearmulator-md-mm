#include "mdLib/mdfirmwaresysex.h"
#include "mdLib/mdhardware.h"
#include "mdLib/mdmemorymap.h"
#include "mdLib/mdpanel.h"
#include "mdLib/mdtypes.h"
#include "mc68k/cpuState.h"

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

	const auto before = uc.midiRxConsumedCount();
	if(!uc.tryQueueMidiRx(sysex.front()))
	{
		std::cerr << "[hw-bootstrap] failed to queue F0" << std::endl;
		return 3;
	}
	hw->advance(22050); // 500 ms
	const auto delta = uc.midiRxConsumedCount() - before;
	report(*hw, "after F0");
	std::cerr << "[hw-bootstrap] F0 consumedDelta=" << delta
		<< " queued=" << uc.queuedMidiRxBytes() << std::endl;

	if(delta == 0)
	{
		std::cerr << "[hw-bootstrap] MIDI UPGRADE still did not consume F0" << std::endl;
		return 4;
	}

	std::cout << "Machinedrum reconstructed bootstrap entered MIDI UPGRADE on full Hardware\n";
	return 0;
}
