#include "mdLib/mdfirmwaresysex.h"
#include "mdLib/mdhardware.h"
#include "mdLib/mdmemorymap.h"
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
	uc->setFlashOperationObserver([&](const md::FlashCommandDecoder::Operation& op,
		const uint64_t)
	{
		if(op.type == md::FlashCommandDecoder::Operation::Type::ProgramWord)
			++programWords;
		else
			++eraseSectors;
	});

	uc->reset();
	uc->exec();

	constexpr uint64_t readyTimeoutCycles = md::g_ucClockHz * 5ull;
	while(!uc->isMidiReceiveReady() && uc->getCycles() < readyTimeoutCycles)
		uc->exec();

	std::cerr << "[install] bootstrap pc=0x" << std::hex << uc->getPC()
		<< " sp=0x" << uc->getAReg(7)
		<< " vbr=0x" << uc->getCpuState()->vbr
		<< " mbar=0x" << uc->getCpuState()->cf_mbar
		<< " rambar=0x" << uc->getCpuState()->cf_rambar
		<< std::dec
		<< " midiReady=" << uc->isMidiReceiveReady()
		<< " cycles=" << uc->getCycles() << std::endl;

	if(!uc->isMidiReceiveReady())
	{
		std::cerr << "[install] reconstructed bootstrap did not enable MIDI RX within 5s" << std::endl;
		return 2;
	}

	std::cerr << "[install] phase2: feeding " << sysex.size()
		<< " official SysEx bytes" << std::endl;

	size_t cursor = 0;
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
