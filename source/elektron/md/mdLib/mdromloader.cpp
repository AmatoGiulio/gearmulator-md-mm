#include "mdromloader.h"
#include "mdfirmwaresysex.h"
#include "baseLib/filesystem.h"

namespace md
{
	Rom RomLoader::findROM()
	{
		return findROM(MachineModel::Machinedrum);
	}

	Rom RomLoader::findROM(const MachineModel _model)
	{
		const auto files = findFiles(".bin", g_romSize, g_romSize);
		for(const auto& file : files)
		{
			auto rom = Rom(file);
			if(rom.isValid() && isRomForModel(rom.data(), _model))
				return rom;
		}

		// Machinedrum OS 1.63 is also available as Elektron's official updater.
		// Reconstruct its bootable flash locally instead of requiring a full dump.
		if(_model == MachineModel::Machinedrum)
		{
			constexpr size_t officialSysexSize = 1644582;
			for(const auto& file : findFiles(".syx", officialSysexSize, officialSysexSize))
			{
				std::vector<uint8_t> sysex;
				if(!baseLib::filesystem::readFile(sysex, file))
					continue;
				std::vector<uint8_t> flash;
				std::string error;
				if(!buildMachinedrumOs163FlashFromSysex(flash, sysex, error))
					continue;
				Rom rom(flash, file);
				if(rom.isValid() && isRomForModel(rom.data(), _model))
					return rom;
			}
		}
		return {};
	}

	bool RomLoader::isSupportedImage(const size_t _size,
		const uint64_t _fingerprint, const MachineModel _model)
	{
		if(_size != g_romSize)
			return false;
		return _model == MachineModel::Monomachine
			? _fingerprint == g_mmOs132bFingerprint
			: isMdOs163Fingerprint(_fingerprint);
	}

	bool RomLoader::isRomForModel(const std::vector<uint8_t>& _data,
		const MachineModel _model)
	{
		if(_data.size() != g_romSize)
			return false;

		uint64_t fingerprint = 14695981039346656037ull;
		for(const auto byte : _data)
		{
			fingerprint ^= byte;
			fingerprint *= 1099511628211ull;
		}

		// Accept only a supported image for the requested product.
		return isSupportedImage(_data.size(), fingerprint, _model);
	}
}
