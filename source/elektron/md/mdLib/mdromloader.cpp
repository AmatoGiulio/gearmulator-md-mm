#include "mdromloader.h"
#include "mdfirmwaresysex.h"
#include "baseLib/filesystem.h"

namespace md
{
	Rom RomLoader::findROM()
	{
		return findROM(MachineModel::Machinedrum);
	}

	Rom RomLoader::loadFirmware(const std::vector<uint8_t>& data,
		const std::string& name, const MachineModel model)
	{
		if(data.size() == g_romSize && isRomForModel(data, model))
			return Rom(data, name);

		if(model == MachineModel::Machinedrum)
		{
			std::vector<uint8_t> flash;
			std::string error;
			if(buildMachinedrumOs163FlashFromSysex(flash, data, error)
				&& isRomForModel(flash, model))
				return Rom(flash, name);
		}
		return {};
	}

	Rom RomLoader::findROM(const MachineModel _model)
	{
		for(const auto& file : findFiles(".bin", g_romSize, g_romSize))
		{
			std::vector<uint8_t> data;
			if(baseLib::filesystem::readFile(data, file))
				if(auto rom = loadFirmware(data, file, _model); rom.isValid())
					return rom;
		}

		// Machinedrum OS 1.63 is also available as Elektron's official updater.
		// Reconstruct its bootable flash locally instead of requiring a full dump.
		if(_model == MachineModel::Machinedrum)
		{
			constexpr size_t officialSysexSize = 1644582;
			for(const auto& file : findFiles(".syx", officialSysexSize, officialSysexSize))
			{
				std::vector<uint8_t> data;
				if(baseLib::filesystem::readFile(data, file))
					if(auto rom = loadFirmware(data, file, _model); rom.isValid())
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
