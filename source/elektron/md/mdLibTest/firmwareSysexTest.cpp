#include "mdLib/mdfirmwaresysex.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

int main()
{
	const auto* path = std::getenv("GEARMULATOR_MD_FIRMWARE_SYX");
	if(!path || !*path)
	{
		std::cout << "mdFirmwareSysexTest: SKIP (GEARMULATOR_MD_FIRMWARE_SYX not supplied)\n";
		return 77;
	}
	std::ifstream input(path, std::ios::binary);
	const std::vector<uint8_t> sysex{std::istreambuf_iterator<char>(input), {}};
	md::FirmwareSysexImage image;
	std::string error;
	if(!md::decodeMachinedrumOs163Sysex(image, sysex, error))
	{
		std::cerr << "decode failed: " << error << '\n';
		return 1;
	}
	if(image.version != "1.63" || image.mainOs.size() != 404766
		|| image.dsp1.size() != 750369 || image.dsp2.size() != 56469
		|| image.factoryWaveforms.size() != 1024 * 1024)
	{
		std::cerr << "decoded firmware topology is unexpected\n";
		return 1;
	}
	std::cout << "Machinedrum OS 1.63 SysEx decode passed\n";
	return 0;
}
