#include "mdLib/mdfirmwaresysex.h"
#include "mdLib/mdhardware.h"
#include "mdLib/mdpanel.h"
#include "mdLib/mdromloader.h"
#include "mdLib/mdstate.h"
#include "mdLib/mdtypes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <vector>

namespace
{
	void advance(md::Hardware& hardware, const uint32_t frames)
	{
		constexpr uint32_t block = 128;
		for(uint32_t done = 0; done < frames; done += block)
			hardware.advance(std::min(block, frames - done));
	}

	std::optional<uint8_t> queryLockMode(md::Hardware& hardware)
	{
		std::vector<synthLib::SMidiEvent> discarded;
		hardware.readMidiOut(discarded);
		synthLib::SMidiEvent request(synthLib::MidiEventSource::Host);
		request.sysex = {0xf0, 0x00, 0x20, 0x3c, 0x02, 0x00, 0x70, 0x20, 0xf7};
		hardware.sendMidi(request);
		for(unsigned attempt = 0; attempt < 8; ++attempt)
		{
			hardware.advance(2048);
			std::vector<synthLib::SMidiEvent> events;
			hardware.readMidiOut(events);
			for(const auto& event : events)
			{
				const auto& message = event.sysex;
				if(message.size() == 10
					&& message[0] == 0xf0 && message[1] == 0x00
					&& message[2] == 0x20 && message[3] == 0x3c
					&& message[4] == 0x02 && message[5] == 0x00
					&& message[6] == 0x72 && message[7] == 0x20
					&& message[8] <= 1 && message[9] == 0xf7)
					return message[8];
			}
		}
		return {};
	}

	float triggerDefaultTrack(md::Hardware& hardware, bool& finite)
	{
		const auto trigger = md::panelPacket(md::MachineModel::Machinedrum,
			md::PanelControl::Trigger1);
		if(!trigger)
		{
			finite = false;
			return 0.0f;
		}

		std::array<std::vector<float>, 2> rendered{
			std::vector<float>(8192), std::vector<float>(8192)};
		synthLib::TAudioOutputs outputs{};
		outputs[0] = rendered[0].data();
		outputs[1] = rendered[1].data();

		hardware.sendPanelEvent(trigger->row, trigger->mask);
		hardware.processAudio(outputs, 4096, 0);
		hardware.sendPanelEvent(trigger->row, 0);
		outputs[0] += 4096;
		outputs[1] += 4096;
		hardware.processAudio(outputs, 4096, 0);

		finite = true;
		float peak = 0.0f;
		for(const auto& channel : rendered)
			for(const auto sample : channel)
			{
				finite = finite && std::isfinite(sample);
				if(std::isfinite(sample))
					peak = std::max(peak, std::abs(sample));
			}
		return peak;
	}
}

int main()
{
	const auto* path = std::getenv("GEARMULATOR_MD_FIRMWARE_SYX");
	if(!path || !*path)
	{
		std::cout << "mdFirmwareSysexBootstrapHardwareTest: SKIP "
			"(GEARMULATOR_MD_FIRMWARE_SYX not supplied)\n";
		return 77;
	}

	std::ifstream input(path, std::ios::binary);
	const std::vector<uint8_t> sysex{std::istreambuf_iterator<char>(input), {}};
	std::vector<uint8_t> flash;
	std::string error;
	if(!md::buildMachinedrumOs163FlashFromSysex(flash, sysex, error))
	{
		std::cerr << "[official-syx] reconstruction failed: " << error << '\n';
		return 1;
	}
	if(!md::RomLoader::isRomForModel(flash, md::MachineModel::Machinedrum))
	{
		std::cerr << "[official-syx] reconstructed image was not accepted as MD OS 1.63\n";
		return 2;
	}

	auto firstBoot = std::make_unique<md::Hardware>(
		flash, "md-os163-official-syx", md::MachineModel::Machinedrum);
	if(!firstBoot->isValid())
		return 3;

	constexpr uint32_t initializationDeadline = md::g_samplerate * 18;
	for(uint32_t frames = 0; frames < initializationDeadline
		&& !firstBoot->isFactoryFlashReadyForReboot(); frames += 128)
		firstBoot->advance(std::min<uint32_t>(128, initializationDeadline - frames));

	if(!firstBoot->isFactoryFlashReadyForReboot())
	{
		std::cerr << "[official-syx] first-run UW flash initialization did not complete\n";
		return 4;
	}

	const auto initializedFlash = firstBoot->copyFlashData();
	std::vector<uint8_t> factoryCache;
	if(!md::encodeFactoryFlashCache(factoryCache, initializedFlash, flash))
	{
		std::cerr << "[official-syx] could not encode initialized UW factory cache\n";
		return 5;
	}

	auto hardware = std::make_unique<md::Hardware>(
		flash, "md-os163-official-syx", md::MachineModel::Machinedrum,
		std::vector<uint8_t>{}, std::shared_ptr<md::FrontPanelPublisher>{},
		initializedFlash, factoryCache);
	if(!hardware->isValid())
		return 6;

	advance(*hardware, md::g_samplerate * 20);
	const bool firmwareReady = hardware->isFirmwareMidiReady();
	const bool audioReady = hardware->isAudioReady();
	const auto lockMode = firmwareReady ? queryLockMode(*hardware) : std::optional<uint8_t>{};
	bool finiteAudio = true;
	const auto peak = firmwareReady && audioReady
		? triggerDefaultTrack(*hardware, finiteAudio) : 0.0f;

	const bool pass = firmwareReady && audioReady && lockMode.has_value()
		&& finiteAudio && peak >= 0.001f;
	std::cerr << "[official-syx] firmwareReady=" << firmwareReady
		<< " audioReady=" << audioReady
		<< " statusReply=" << lockMode.has_value()
		<< " audioFinite=" << finiteAudio
		<< " audioPeak=" << peak
		<< " PASS=" << pass << '\n';

	if(!pass)
		return 7;

	std::cout << "Machinedrum official SysEx firmware reconstruction PASS\n";
	return 0;
}
