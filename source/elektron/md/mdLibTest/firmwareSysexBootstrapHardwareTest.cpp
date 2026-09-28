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
		// Non-empty direct-boot payload admits this reconstructed, non-canonical
		// bootstrap through initRom(). A single zero byte leaves MAIN RAM fully
		// zeroed, so phase 2 is still a genuine bootstrap cold boot.
		std::vector<uint8_t>{0});
	if(!hw->isValid())
	{
		std::cerr << "[hw-bootstrap] reconstructed bootstrap Hardware rejected" << std::endl;
		return 1;
	}

	auto& uc = hw->getUC();
	std::vector<uint8_t> panelTx;
	std::vector<uint8_t> midiTx;
	uint64_t midiTxTotal = 0;
	uc.setPanelTransmitTap([&](const uint8_t byte)
	{
		if(panelTx.size() < 4096)
			panelTx.push_back(byte);
	});
	uc.setMidiTransmitTap([&](const uint8_t byte)
	{
		++midiTxTotal;
		if(midiTx.size() < 4096)
			midiTx.push_back(byte);
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

	// Feed the official updater with a maximum of two pending UART1 bytes.
	// Sim::rx is a host-side scheduling backlog, but USR currently reports FFULL
	// when that backlog reaches three bytes. Queuing an entire 112-byte firmware
	// packet therefore exposes an artificial physical-FIFO-full condition to the
	// bootstrap. Keep the backlog below the 3-byte hardware FIFO threshold while
	// retaining accelerated (non-wall-clock) delivery.
	panelTx.clear();
	midiTx.clear();
	midiTxTotal = 0;
	const auto patchBeforeUpdate = hw->copyPatchRam();
	const auto mainBeforeUpdate = hw->copyMainRam();
	const auto loaderBeforeUpdate = uc.copyLoaderRam();
	const auto internalBeforeUpdate = uc.copyInternalSram();
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

	size_t cursor = 0;
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

		uint32_t frames = 0;
		while(cursor < end)
		{
			while(cursor < end && uc.queuedMidiRxBytes() < 2)
			{
				if(!uc.tryQueueMidiRx(sysex[cursor]))
				{
					std::cerr << "[hw-bootstrap] UART1 RX full at byte " << cursor << std::endl;
					return 6;
				}
				++cursor;
			}
			hw->advance(1);
			++frames;
			if(frames >= maxDrainFramesPerMessage && uc.queuedMidiRxBytes() != 0)
				break;
		}
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
				<< " midiTx=" << midiTxTotal
				<< std::endl;
		}
	}

	std::cerr << "[hw-bootstrap] updater stream drained; inspecting state before post-update reboot" << std::endl;

	const auto flashImmediately = uc.copyFlashData();
	const auto patchImmediately = hw->copyPatchRam();
	const auto mainImmediately = hw->copyMainRam();
	const auto loaderImmediately = uc.copyLoaderRam();
	const auto internalImmediately = uc.copyInternalSram();
	const auto immediatePc = uc.getPC();
	const auto immediateSp = uc.getAReg(7);
	const auto immediateVbr = uc.getCpuState()->vbr;
	const auto immediateMbar = uc.getCpuState()->cf_mbar;
	const auto immediateRambar = uc.getCpuState()->cf_rambar;
	const auto consumedImmediately = uc.midiRxConsumedCount();
	const auto overflowImmediately = uc.midiRxOverflowCount();
	const auto dsp1Immediately = hw->getDspMixer().booted();
	const auto dsp2Immediately = hw->getDspProducer().booted();
	const auto changedBytes = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
	{
		const auto n = std::min(a.size(), b.size());
		size_t changed = a.size() == b.size() ? 0 : std::max(a.size(), b.size()) - n;
		for(size_t i = 0; i < n; ++i)
			changed += a[i] != b[i];
		return changed;
	};
	const bool mainOsPrefixMatch = mainImmediately.size() >= image.mainOs.size()
		&& std::equal(image.mainOs.begin(), image.mainOs.end(), mainImmediately.begin());
	const auto findOffset = [](const std::vector<uint8_t>& haystack,
		const std::vector<uint8_t>& needle) -> int64_t
	{
		if(needle.empty() || needle.size() > haystack.size())
			return -1;
		const auto it = std::search(haystack.begin(), haystack.end(),
			needle.begin(), needle.end());
		return it == haystack.end() ? -1
			: static_cast<int64_t>(std::distance(haystack.begin(), it));
	};
	const auto mainOsOffset = findOffset(mainImmediately, image.mainOs);
	const auto decodedTransportOffset = findOffset(mainImmediately, image.decodedTransport);
	uint64_t immediateFlashFnv = 14695981039346656037ull;
	for(const auto byte : flashImmediately)
	{
		immediateFlashFnv ^= byte;
		immediateFlashFnv *= 1099511628211ull;
	}
	std::cerr << "[hw-bootstrap] immediate pc=0x" << std::hex << immediatePc
		<< " sp=0x" << immediateSp
		<< " vbr=0x" << immediateVbr
		<< " mbar=0x" << immediateMbar
		<< " rambar=0x" << immediateRambar
		<< " flashFnv=0x" << immediateFlashFnv << std::dec
		<< " changedPatch=" << changedBytes(patchBeforeUpdate, patchImmediately)
		<< " changedMain=" << changedBytes(mainBeforeUpdate, mainImmediately)
		<< " changedLoader=" << changedBytes(loaderBeforeUpdate, loaderImmediately)
		<< " changedInternal=" << changedBytes(internalBeforeUpdate, internalImmediately)
		<< " mainOsPrefixMatch=" << mainOsPrefixMatch
		<< " mainOsOffset=" << mainOsOffset
		<< " decodedTransportOffset=" << decodedTransportOffset
		<< " decodedTransportSize=" << image.decodedTransport.size()
		<< " programWords=" << programWords
		<< " eraseSectors=" << eraseSectors
		<< std::endl;

	// PC back in low flash after the last updater packet means the bootstrap has
	// restarted. Advancing the existing Hardware would re-bootstrap already-running
	// DSP instances and currently trips dsp56kEmu's JIT assertion. Stop before that
	// known emulator-lifecycle bug; the state above tells us whether the update was
	// accepted and where its decoded image lives.
	const bool bootstrapRestarted = md::memorymap::g_flashLow.contains(immediatePc);
	std::cerr << "[hw-bootstrap] bootstrapRestarted=" << bootstrapRestarted << std::endl;
	auto installedFlash = flashImmediately;

	if(bootstrapRestarted)
	{
		// This is not a ColdFire reset: PC has returned to bootstrap code while the
		// received transport remains in MAIN RAM. Preserve the exact ColdFire/SIM/RAM
		// state and only hardware-reset the two DSPs, matching the updater's next step.
		std::cerr << "[hw-bootstrap] phase3 same-machine DSP bootstrap reload" << std::endl;
		std::cerr << "[hw-bootstrap] expected decoded DSP words dsp1="
			<< (image.dsp1.size() / 3) << " dsp2=" << (image.dsp2.size() / 3) << std::endl;
		// Do not execute the reloaded DSPs yet. First prove what boot image the
		// ColdFire installer is actually sending and allow flash programming to finish.
		hw->prepareDspsForBootstrapReload(true);

		uint64_t phase3ProgramWords = 0;
		uint64_t phase3EraseSectors = 0;
		uint64_t phase3LastFlashCycle = 0;
		uint32_t phase3MinFlashOffset = 0xffffffffu;
		uint32_t phase3MaxFlashOffset = 0;
		uc.setFlashOperationObserver([&](const md::FlashCommandDecoder::Operation& op,
			const uint64_t cycle)
		{
			phase3LastFlashCycle = cycle;
			phase3MinFlashOffset = std::min(phase3MinFlashOffset, op.offset);
			phase3MaxFlashOffset = std::max(phase3MaxFlashOffset, op.offset);
			if(op.type == md::FlashCommandDecoder::Operation::Type::ProgramWord)
				++phase3ProgramWords;
			else if(op.type == md::FlashCommandDecoder::Operation::Type::EraseSector)
				++phase3EraseSectors;
		});

		uint64_t previousFlashOps = 0;
		unsigned idleSeconds = 0;
		bool phase3Quiescent = false;
		for(unsigned second = 1; second <= 30; ++second)
		{
			hw->advance(44100);
			const auto flashOps = phase3ProgramWords + phase3EraseSectors;
			if(flashOps != 0 && flashOps == previousFlashOps)
				++idleSeconds;
			else
				idleSeconds = 0;
			previousFlashOps = flashOps;

			std::cerr << "[hw-bootstrap] phase3 " << second << "s"
				<< " pc=0x" << std::hex << uc.getPC() << std::dec
				<< " dsp1=" << hw->getDspMixer().booted()
				<< " dsp2=" << hw->getDspProducer().booted()
				<< " panel=" << uc.isPanelHandshakeComplete()
				<< " programWords=" << phase3ProgramWords
				<< " eraseSectors=" << phase3EraseSectors
				<< " idleSeconds=" << idleSeconds
				<< " d1BootDone=" << hw->getDspMixer().bootstrapReloadFinished()
				<< " d1Len=" << hw->getDspMixer().bootstrapReloadLength()
				<< " d1Pc=0x" << std::hex << hw->getDspMixer().bootstrapReloadInitialPc() << std::dec
				<< " d1Words=" << hw->getDspMixer().bootstrapReloadWordsSeen()
				<< " d1Post=" << hw->getDspMixer().bootstrapReloadPostBootWords()
				<< " d2BootDone=" << hw->getDspProducer().bootstrapReloadFinished()
				<< " d2Len=" << hw->getDspProducer().bootstrapReloadLength()
				<< " d2Pc=0x" << std::hex << hw->getDspProducer().bootstrapReloadInitialPc() << std::dec
				<< " d2Words=" << hw->getDspProducer().bootstrapReloadWordsSeen()
				<< " d2Post=" << hw->getDspProducer().bootstrapReloadPostBootWords()
				<< std::endl;

			if(flashOps != 0 && idleSeconds >= 2)
			{
				phase3Quiescent = true;
				break;
			}
		}

		installedFlash = uc.copyFlashData();
		uint64_t phase3Fnv = 14695981039346656037ull;
		for(const auto byte : installedFlash)
		{
			phase3Fnv ^= byte;
			phase3Fnv *= 1099511628211ull;
		}
		const bool factoryWaveformsMatch =
			installedFlash.size() >= 0x100000u + image.factoryWaveforms.size()
			&& std::equal(image.factoryWaveforms.begin(), image.factoryWaveforms.end(),
				installedFlash.begin() + 0x100000u);

		const auto installedTransportOffset = findOffset(installedFlash, image.decodedTransport);
		auto directInstalledCandidate = bootstrapFlash;
		constexpr size_t updaterStoreOffset = 0x4000;
		if(directInstalledCandidate.size() >= updaterStoreOffset + image.decodedTransport.size())
			std::copy(image.decodedTransport.begin(), image.decodedTransport.end(),
				directInstalledCandidate.begin() + updaterStoreOffset);
		const auto directCandidateMismatchCount =
			changedBytes(directInstalledCandidate, installedFlash);
		size_t directCandidateFirstMismatch = installedFlash.size();
		size_t directCandidateLastMismatch = 0;
		if(directInstalledCandidate.size() == installedFlash.size())
		{
			for(size_t i = 0; i < installedFlash.size(); ++i)
			{
				if(directInstalledCandidate[i] == installedFlash[i])
					continue;
				directCandidateFirstMismatch = std::min(directCandidateFirstMismatch, i);
				directCandidateLastMismatch = i;
			}
		}
		std::cerr << "[hw-bootstrap] phase3 flashFnv=0x" << std::hex << phase3Fnv
			<< " canonical=0x" << md::g_mdOs163Fingerprint << std::dec
			<< " canonicalImage=" << (phase3Fnv == md::g_mdOs163Fingerprint)
			<< " quiescent=" << phase3Quiescent
			<< " factoryWaveformsMatch=" << factoryWaveformsMatch
			<< " transportFlashOffset=" << installedTransportOffset
			<< " directCandidateMismatches=" << directCandidateMismatchCount;
		if(directCandidateMismatchCount != 0
			&& directCandidateFirstMismatch != installedFlash.size())
		{
			std::cerr << " directMismatchRange=[0x" << std::hex
				<< directCandidateFirstMismatch << ",0x" << directCandidateLastMismatch
				<< "]" << std::dec;
		}
		std::cerr << " programWords=" << phase3ProgramWords
			<< " eraseSectors=" << phase3EraseSectors
			<< " lastFlashCycle=" << phase3LastFlashCycle;
		if(phase3ProgramWords || phase3EraseSectors)
			std::cerr << " flashRange=[0x" << std::hex << phase3MinFlashOffset
				<< ",0x" << phase3MaxFlashOffset << "]" << std::dec;
		std::cerr << std::endl;
		std::cerr << "[hw-bootstrap] install-layout"
			<< " bootstrapPreserved0x4000="
			<< (installedFlash.size() >= updaterStoreOffset
				&& bootstrapFlash.size() >= updaterStoreOffset
				&& std::equal(installedFlash.begin(),
					installedFlash.begin() + updaterStoreOffset, bootstrapFlash.begin()))
			<< " expectedProgramBytes=" << image.decodedTransport.size()
			<< " observedProgramBytes=" << (phase3ProgramWords * 2)
			<< " directCandidateExact=" << (directCandidateMismatchCount == 0)
			<< std::endl;

		if(phase3Quiescent)
		{
			std::cerr << "[hw-bootstrap] phase4 cold boot from updater-installed flash" << std::endl;
			auto installed = std::make_unique<md::Hardware>(
				installedFlash, "md-os163-installed-from-official-syx",
				md::MachineModel::Machinedrum,
				std::vector<uint8_t>{},
				std::shared_ptr<md::FrontPanelPublisher>{},
				installedFlash,
				std::vector<uint8_t>{},
				md::FlashSectorOverlay{},
				std::vector<uint8_t>{},
				std::vector<uint8_t>{0});
			if(installed->isValid())
			{
				for(unsigned second = 1; second <= 5; ++second)
				{
					installed->advance(44100);
					std::cerr << "[hw-bootstrap] phase4 " << second << "s"
						<< " pc=0x" << std::hex << installed->getUC().getPC() << std::dec
						<< " dsp1=" << installed->getDspMixer().booted()
						<< " dsp2=" << installed->getDspProducer().booted()
						<< " panel=" << installed->getUC().isPanelHandshakeComplete()
						<< " midiRx=" << installed->getUC().isMidiReceiveReady()
						<< std::endl;
				}
			}
			else
			{
				std::cerr << "[hw-bootstrap] phase4 installed flash Hardware rejected" << std::endl;
			}
		}
	}

	std::cerr << "[hw-bootstrap] midiTx captured=" << midiTx.size()
		<< " total=" << midiTxTotal << " bytes:";
	for(const auto byte : midiTx)
		std::cerr << " " << std::hex << std::setw(2) << std::setfill('0')
			<< static_cast<unsigned>(byte);
	std::cerr << std::dec << std::endl;
	std::cerr << "[hw-bootstrap] update panelTx captured=" << panelTx.size() << " bytes:";
	const auto panelStart = panelTx.size() > 256 ? panelTx.size() - 256 : 0;
	for(size_t i = panelStart; i < panelTx.size(); ++i)
		std::cerr << " " << std::hex << std::setw(2) << std::setfill('0')
			<< static_cast<unsigned>(panelTx[i]);
	std::cerr << std::dec << std::endl;

	uint64_t fingerprint = 14695981039346656037ull;
	for(const auto byte : installedFlash)
	{
		fingerprint ^= byte;
		fingerprint *= 1099511628211ull;
	}
	const bool canonical = fingerprint == md::g_mdOs163Fingerprint;
	std::cerr << "[hw-bootstrap] phase2-final pc=0x" << std::hex << immediatePc
		<< " flashFnv=0x" << fingerprint
		<< " canonical=0x" << md::g_mdOs163Fingerprint
		<< std::dec
		<< " canonicalImage=" << canonical
		<< " consumed=" << consumedImmediately
		<< " overflow=" << overflowImmediately
		<< " programWords=" << programWords
		<< " eraseSectors=" << eraseSectors
		<< " dsp1=" << dsp1Immediately
		<< " dsp2=" << dsp2Immediately
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

	std::cout << "Machinedrum official SysEx bootstrap update/reboot-boundary probe completed\n";
	return 0;
}
