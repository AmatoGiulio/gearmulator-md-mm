#include "mdLib/mdfirmwaresysex.h"
#include "mdLib/mdhardware.h"
#include "mdLib/mdmemorymap.h"
#include "mdLib/mdpanel.h"
#include "mdLib/mdsim.h"
#include "mdLib/mdrom.h"
#include "mdLib/mdtypes.h"
#include "mc68k/cpuState.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <vector>

namespace
{
	uint64_t fnv1a(const std::vector<uint8_t>& data)
	{
		uint64_t value = 14695981039346656037ull;
		for(const auto byte : data)
		{
			value ^= byte;
			value *= 1099511628211ull;
		}
		return value;
	}

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
}

int main()
{
	const auto* path = std::getenv("GEARMULATOR_MD_FIRMWARE_SYX");
	if(!path || !*path)
	{
		std::cout << "mdFirmwareSysexBootstrapInstallTest: SKIP (GEARMULATOR_MD_FIRMWARE_SYX not supplied)\n";
		return 77;
	}

	std::ifstream input(path, std::ios::binary);
	const std::vector<uint8_t> sysex{std::istreambuf_iterator<char>(input), {}};
	md::FirmwareSysexImage image;
	std::string error;
	if(!md::decodeMachinedrumOs163Sysex(image, sysex, error))
	{
		std::cerr << "[install] decode failed: " << error << std::endl;
		return 1;
	}

	// Phase 1: let the official MAIN OS repair/install the persistent bootstrap.
	std::cerr << "[install] phase1: reconstructing bootstrap from official updater" << std::endl;
	auto seed = makeDirectMainOsHardware(image);
	if(!seed->isValid())
		return 1;
	seed->getUC().getCpuState()->vbr = md::memorymap::g_internalSram.begin;
	seed->advance(220500); // 5 seconds: previous probe shows flash has been idle ~4.97 s.
	auto bootstrapFlash = seed->getUC().copyFlashData();
	std::cerr << "[install] bootstrap flash fnv=0x" << std::hex << fnv1a(bootstrapFlash)
		<< std::dec << " dirty=" << seed->getUC().flashDirty() << std::endl;
	seed.reset();

	// Phase 2: cold boot the reconstructed real bootstrap and present the exact
	// official SysEx over the emulated MIDI UART, as the physical updater does.
	md::Rom bootstrapRom(bootstrapFlash, "md-os163-reconstructed-bootstrap");
	if(!bootstrapRom.isValid())
		return 1;
	auto uc = std::make_unique<md::Microcontroller>(
		bootstrapRom, md::MachineModel::Machinedrum,
		std::vector<uint8_t>{}, std::vector<uint8_t>{});

	uint64_t programWords = 0;
	uint64_t eraseSectors = 0;
	std::vector<uint8_t> panelTx;
	uc->setPanelTransmitTap([&](const uint8_t byte)
	{
		if(panelTx.size() < 256)
			panelTx.push_back(byte);
	});
	uc->setFlashOperationObserver([&](const md::FlashCommandDecoder::Operation& op,
		const uint64_t)
	{
		if(op.type == md::FlashCommandDecoder::Operation::Type::ProgramWord)
			++programWords;
		else
			++eraseSectors;
	});

	// Enter the documented EARLY STARTUP MENU exactly like the hardware:
	// hold FUNCTION while powering on, release it once the menu is up, then press
	// TRIG 5 (MIDI UPGRADE). Panel input is UART2 [row][mask].
	const auto function = md::panelPacket(md::MachineModel::Machinedrum,
		md::PanelControl::Function);
	const auto trig5 = md::panelPacket(md::MachineModel::Machinedrum,
		md::PanelControl::Trigger5);
	if(!function || !trig5)
	{
		std::cerr << "[install] missing verified MD panel packet mapping" << std::endl;
		return 2;
	}

	std::cerr << "[install] holding FUNCTION at cold reset row=0x"
		<< std::hex << static_cast<unsigned>(function->row)
		<< " mask=0x" << static_cast<unsigned>(function->mask)
		<< std::dec << std::endl;
	uc->queuePanelRx(function->row);
	uc->queuePanelRx(function->mask);

	uc->reset();
	uc->exec();

	// Give the reconstructed bootstrap time to initialize UART2 and enter the
	// early-startup menu while FUNCTION remains logically held.
	const auto menuDeadline = uc->getCycles() + md::g_ucClockHz * 1ull;
	while(uc->getCycles() < menuDeadline)
		uc->exec();

	std::cerr << "[install] early-menu probe pc=0x" << std::hex << uc->getPC()
		<< std::dec
		<< " panelQueued=" << (md::Sim::g_uartRxCapacity - uc->availablePanelRxBytes())
		<< " panelTxCount=" << panelTx.size()
		<< std::endl;
	std::cerr << "[install] panelTx:";
	for(const auto byte : panelTx)
		std::cerr << " " << std::hex << std::setw(2) << std::setfill('0')
			<< static_cast<unsigned>(byte);
	std::cerr << std::dec << std::endl;
	for(int rel = -16; rel <= 32; rel += 2)
	{
		const auto addr = static_cast<uint32_t>(uc->getPC() + rel);
		std::cerr << "[install] code 0x" << std::hex << std::setw(8)
			<< std::setfill('0') << addr << ":";
		for(unsigned j = 0; j < 8; ++j)
			std::cerr << " " << std::setw(4) << uc->read16(addr + j * 2);
		std::cerr << std::dec << std::endl;
	}
	std::cerr << "[install] releasing FUNCTION and pressing TRIG5"
		<< " pc=0x" << std::hex << uc->getPC() << std::dec << std::endl;
	uc->queuePanelRx(function->row);
	uc->queuePanelRx(0x00);
	for(uint64_t deadline = uc->getCycles() + md::g_ucClockHz / 10;
		uc->getCycles() < deadline;)
		uc->exec();

	uc->queuePanelRx(trig5->row);
	uc->queuePanelRx(trig5->mask);
	for(uint64_t deadline = uc->getCycles() + md::g_ucClockHz / 20;
		uc->getCycles() < deadline;)
		uc->exec();
	uc->queuePanelRx(trig5->row);
	uc->queuePanelRx(0x00);

	// Allow MIDI UPGRADE mode to settle before probing UART1.
	const auto settleDeadline = uc->getCycles() + md::g_ucClockHz * 2ull;
	while(uc->getCycles() < settleDeadline)
		uc->exec();

	const auto uartBase = md::memorymap::g_sim.begin + md::Sim::g_uart1Base;
	const auto usr = uc->read8(uartBase + md::Sim::g_uartUsr);
	const auto uisr = uc->read8(uartBase + md::Sim::g_uartIsr);
	std::cerr << "[install] bootstrap pc=0x" << std::hex << uc->getPC()
		<< " sp=0x" << uc->getAReg(7)
		<< " vbr=0x" << uc->getCpuState()->vbr
		<< " mbar=0x" << uc->getCpuState()->cf_mbar
		<< " rambar=0x" << uc->getCpuState()->cf_rambar
		<< " usr=0x" << static_cast<unsigned>(usr)
		<< " uisr=0x" << static_cast<unsigned>(uisr)
		<< std::dec
		<< " midiIrqEnabled=" << uc->isMidiReceiveReady()
		<< " cycles=" << uc->getCycles() << std::endl;

	// Prove the polling path before attempting the entire 1.6 MiB transfer.
	const auto probeConsumed = uc->midiRxConsumedCount();
	if(!uc->tryQueueMidiRx(sysex.front()))
	{
		std::cerr << "[install] could not queue first SysEx byte" << std::endl;
		return 2;
	}
	constexpr uint64_t pollingProbeInstructions = 5000000ull;
	for(uint64_t i = 0; i < pollingProbeInstructions
		&& uc->midiRxConsumedCount() == probeConsumed; ++i)
		uc->exec();

	std::cerr << "[install] polling probe firstByte=0x" << std::hex
		<< static_cast<unsigned>(sysex.front()) << std::dec
		<< " consumedDelta=" << (uc->midiRxConsumedCount() - probeConsumed)
		<< " queued=" << uc->queuedMidiRxBytes()
		<< " pc=0x" << std::hex << uc->getPC() << std::dec << std::endl;

	if(uc->midiRxConsumedCount() == probeConsumed)
	{
		std::cerr << "[install] bootstrap did not poll/consume the first MIDI byte" << std::endl;
		return 2;
	}

	std::cerr << "[install] phase2: polling MIDI path confirmed; feeding "
		<< sysex.size() << " official SysEx bytes" << std::endl;

	// Byte zero was consumed by the probe above.
	size_t cursor = 1;
	uint64_t lastConsumed = uc->midiRxConsumedCount();
	uint64_t noProgressInstructions = 0;
	size_t nextReport = 100000;
	constexpr uint64_t noProgressLimit = 20000000ull;

	while(cursor < sysex.size() || uc->queuedMidiRxBytes() != 0)
	{
		// Keep a bounded UART backlog; this is still delivered through the normal
		// SIM RX FIFO and firmware interrupt path, just without wall-clock MIDI baud.
		size_t admitted = 0;
		while(cursor < sysex.size() && admitted < 32 && uc->availableMidiRxBytes() != 0)
		{
			if(!uc->tryQueueMidiRx(sysex[cursor]))
				break;
			++cursor;
			++admitted;
		}

		for(unsigned i = 0; i < 64; ++i)
			uc->exec();

		const auto consumed = uc->midiRxConsumedCount();
		if(consumed != lastConsumed)
		{
			lastConsumed = consumed;
			noProgressInstructions = 0;
		}
		else
			noProgressInstructions += 64;

		if(cursor >= nextReport)
		{
			std::cerr << "[install] sent=" << cursor << "/" << sysex.size()
				<< " consumed=" << consumed
				<< " queued=" << uc->queuedMidiRxBytes()
				<< " pc=0x" << std::hex << uc->getPC() << std::dec
				<< " programWords=" << programWords
				<< " eraseSectors=" << eraseSectors
				<< std::endl;
			nextReport += 100000;
		}

		if(noProgressInstructions >= noProgressLimit)
		{
			std::cerr << "[install] stalled: sent=" << cursor
				<< " consumed=" << consumed
				<< " queued=" << uc->queuedMidiRxBytes()
				<< " pc=0x" << std::hex << uc->getPC() << std::dec << std::endl;
			return 3;
		}
	}

	// Let the updater finish decompression/programming after the final F7.
	const auto finishStart = uc->getCycles();
	const auto finishDeadline = finishStart + md::g_ucClockHz * 10ull;
	while(uc->getCycles() < finishDeadline)
		uc->exec();

	auto installedFlash = uc->copyFlashData();
	const auto fingerprint = fnv1a(installedFlash);
	std::cerr << "[install] final pc=0x" << std::hex << uc->getPC()
		<< " vbr=0x" << uc->getCpuState()->vbr
		<< " flashFnv=0x" << fingerprint
		<< " canonical=0x" << md::g_mdOs163Fingerprint
		<< std::dec
		<< " consumed=" << uc->midiRxConsumedCount()
		<< " overflow=" << uc->midiRxOverflowCount()
		<< " programWords=" << programWords
		<< " eraseSectors=" << eraseSectors
		<< " flashDirty=" << uc->flashDirty()
		<< std::endl;

	const bool canonical = fingerprint == md::g_mdOs163Fingerprint;
	std::cerr << "[install] canonicalImage=" << canonical << std::endl;

	// Phase 3: cold-reboot whatever the physical updater produced. We only require
	// that the reconstructed bootstrap still reaches a sane executable state here;
	// if the image is canonical, also exercise the normal full Hardware path.
	md::Rom installedRom(installedFlash, "md-os163-installed-from-official-syx");
	auto verify = std::make_unique<md::Microcontroller>(
		installedRom, md::MachineModel::Machinedrum,
		std::vector<uint8_t>{}, std::vector<uint8_t>{});
	verify->reset();
	verify->exec();

	bool reachedMainOs = false;
	const auto verifyDeadline = verify->getCycles() + md::g_ucClockHz * 5ull;
	while(verify->getCycles() < verifyDeadline)
	{
		verify->exec();
		const auto pc = verify->getPC();
		if(md::memorymap::g_mainRam.contains(pc)
			|| md::memorymap::g_mainExecAlias.contains(pc)
			|| md::memorymap::g_mainHighAlias.contains(pc))
		{
			reachedMainOs = true;
			break;
		}
	}

	std::cerr << "[install] cold reboot pc=0x" << std::hex << verify->getPC()
		<< " sp=0x" << verify->getAReg(7)
		<< " vbr=0x" << verify->getCpuState()->vbr
		<< std::dec << " reachedMainOs=" << reachedMainOs << std::endl;

	if(canonical)
	{
		std::cerr << "[install] phase3: canonical image; testing normal Hardware boot" << std::endl;
		auto hw = std::make_unique<md::Hardware>(
			installedFlash, "md-os163-installed-from-official-syx",
			md::MachineModel::Machinedrum);
		if(!hw->isValid())
		{
			std::cerr << "[install] canonical image rejected by normal Hardware path" << std::endl;
			return 4;
		}
		hw->advance(220500);
		std::cerr << "[install] hardware5s pc=0x" << std::hex << hw->getUC().getPC()
			<< std::dec
			<< " dsp1=" << hw->getDspMixer().booted()
			<< " dsp2=" << hw->getDspProducer().booted()
			<< " panel=" << hw->getUC().isPanelHandshakeComplete()
			<< " midiRx=" << hw->getUC().isMidiReceiveReady()
			<< " ready=" << hw->isFirmwareMidiReady()
			<< std::endl;
	}

	std::cout << "Machinedrum OS 1.63 bootstrap SysEx install probe completed\n";
	return 0;
}
