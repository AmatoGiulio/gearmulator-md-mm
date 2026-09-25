#include "mdfirmwaresysex.h"

#include "mdtypes.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace md
{
	namespace
	{
		constexpr uint8_t kStart = 0xf0;
		constexpr uint8_t kEnd = 0xf7;
		constexpr uint8_t kElektron0 = 0x00;
		constexpr uint8_t kElektron1 = 0x20;
		constexpr uint8_t kElektron2 = 0x3c;
		constexpr uint8_t kMachinedrumDevice = 0x02;
		constexpr uint8_t kDataCommand = 0x7e;
		constexpr size_t kLegacyHeader = 14;
		constexpr uint32_t kAplibBias = 767;
		constexpr uint32_t kAplibFar = 3328;
		constexpr size_t kAplibHeader = 8;

		uint32_t be32(const uint8_t* p)
		{
			return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16)
				| (uint32_t(p[2]) << 8) | uint32_t(p[3]);
		}

		void decodeLegacyPayload(std::vector<uint8_t>& out,
			const uint8_t* data, const size_t size)
		{
			for(size_t i = 0; i + 2 < size; i += 3)
			{
				const uint16_t word = uint16_t(((data[i] & 3u) << 14)
					| ((data[i + 1] & 0x7fu) << 7)
					| (data[i + 2] & 0x7fu));
				out.push_back(uint8_t(word >> 8));
				out.push_back(uint8_t(word));
			}
		}

		bool depackAplib(std::vector<uint8_t>& out, const uint8_t* src,
			const size_t size, std::string& error)
		{
			if(size < kAplibHeader)
			{
				error = "aPLib section is shorter than its header";
				return false;
			}

			size_t ip = kAplibHeader;
			uint32_t tag = 0;
			bool truncated = false;
			uint32_t lastOffset = 1;
			out.clear();
			out.reserve(1024 * 1024);

			auto readByte = [&]() -> uint8_t
			{
				if(ip >= size)
				{
					truncated = true;
					return 0;
				}
				return src[ip++];
			};

			auto getBit = [&]() mutable -> uint32_t
			{
				tag <<= 1;
				if((tag & 0xffu) == 0)
				{
					const auto value = readByte();
					tag = (uint32_t(value) << 1) | 1u;
					return (value >> 7) & 1u;
				}
				return (tag >> 8) & 1u;
			};

			auto getGamma = [&]() mutable -> uint32_t
			{
				uint32_t value = 1;
				for(;;)
				{
					value = (value << 1) + getBit();
					if(getBit())
						break;
					if(truncated || value > 0x02000000u)
					{
						truncated = true;
						break;
					}
				}
				return value;
			};

			constexpr size_t maxOutput = 64u * 1024u * 1024u;
			for(;;)
			{
				if(truncated)
				{
					error = "truncated aPLib stream";
					return false;
				}
				if(getBit())
				{
					out.push_back(readByte());
					if(truncated || out.size() > maxOutput)
					{
						error = "invalid literal in aPLib stream";
						return false;
					}
					continue;
				}

				const auto gamma = getGamma();
				uint32_t offset = 0;
				if(gamma == 2)
					offset = lastOffset;
				else
				{
					const auto encoded = (gamma << 8) + readByte();
					if(truncated)
					{
						error = "truncated aPLib offset";
						return false;
					}
					if(encoded == kAplibBias)
						return true;
					offset = encoded - kAplibBias;
					lastOffset = offset;
				}

				const auto selector = (getBit() << 1) | getBit();
				uint32_t length = selector ? selector : getGamma() + 2;
				if(truncated)
				{
					error = "truncated aPLib length";
					return false;
				}
				if(offset > kAplibFar)
					++length;
				const size_t count = size_t(length) + 1;
				if(!offset || offset > out.size() || out.size() + count > maxOutput)
				{
					error = "invalid aPLib back-reference";
					return false;
				}
				size_t cp = out.size() - offset;
				for(size_t n = 0; n < count; ++n)
					out.push_back(out[cp++]);
			}
		}

		struct Section
		{
			size_t offset = 0;
			size_t stored = 0;
			std::vector<uint8_t> data;
		};

		bool parseSections(std::vector<Section>& sections,
			const std::vector<uint8_t>& stream, std::string& error)
		{
			for(size_t off = 0; off + 8 <= stream.size() && sections.size() < 64;)
			{
				const auto size = be32(stream.data() + off);
				const auto expected = be32(stream.data() + off + 4);
				bool valid = size >= 16 && size_t(size) <= stream.size() - off - 8;
				if(valid)
				{
					uint32_t sum = 0;
					for(size_t i = 0; i < size; ++i)
						sum += stream[off + 8 + i];
					valid = sum == expected;
				}
				if(!valid)
				{
					++off;
					continue;
				}

				Section section;
				section.offset = off;
				section.stored = size_t(size) + 8;
				if(!depackAplib(section.data, stream.data() + off,
					section.stored, error))
					return false;
				sections.push_back(std::move(section));
				off += size_t(size) + 8;
			}
			return true;
		}
	}

	std::vector<uint8_t> makeMachinedrumOs163DirectBootFlash(
		const FirmwareSysexImage& image)
	{
		std::vector<uint8_t> flash(g_romSize, 0xff);

		// ColdFire reset vectors. MAIN OS is linked at 0x00200000 and its first
		// instruction establishes A7=0x00300000 itself as well.
		flash[0] = 0x00; flash[1] = 0x30; flash[2] = 0x00; flash[3] = 0x00;
		flash[4] = 0x00; flash[5] = 0x20; flash[6] = 0x00; flash[7] = 0x00;

		// MAME's documented MD UW flash map places the updater's 1 MiB factory
		// waveform bank at 0x100000..0x1fffff.
		constexpr size_t waveformOffset = 0x100000;
		if(image.factoryWaveforms.size() == 0x100000)
			std::copy(image.factoryWaveforms.begin(), image.factoryWaveforms.end(),
				flash.begin() + waveformOffset);
		return flash;
	}

	bool decodeMachinedrumOs163Sysex(FirmwareSysexImage& out,
		const std::vector<uint8_t>& sysex, std::string& error)
	{
		out = {};
		error.clear();
		if(sysex.empty())
		{
			error = "empty SysEx file";
			return false;
		}

		size_t pos = 0;
		size_t packets = 0;
		while(pos < sysex.size())
		{
			const auto start = std::find(sysex.begin() + pos, sysex.end(), kStart);
			if(start == sysex.end())
				break;
			const auto finish = std::find(start + 1, sysex.end(), kEnd);
			if(finish == sysex.end())
			{
				error = "unterminated SysEx message";
				return false;
			}
			const auto bodySize = size_t(finish - start - 1);
			const auto* body = &*(start + 1);
			if(bodySize > kLegacyHeader && body[0] == kElektron0
				&& body[1] == kElektron1 && body[2] == kElektron2
				&& body[3] == kMachinedrumDevice && body[5] == kDataCommand)
			{
				std::vector<uint8_t> decoded;
				decodeLegacyPayload(decoded, body + kLegacyHeader,
					bodySize - kLegacyHeader);
				uint32_t checksum = 0;
				for(const auto byte : decoded)
					checksum += byte;
				checksum += body[9] + (body[10] << 4)
					+ body[11] + ((body[12] & 0x0c) << 4);
				const auto c = uint8_t(checksum);
				if(((c >> 4) & 0x0f) != body[6] || (c & 0x0f) != body[7])
				{
					error = "Machinedrum transport checksum mismatch";
					return false;
				}
				out.decodedTransport.insert(out.decodedTransport.end(),
					decoded.begin(), decoded.end());
				++packets;
			}
			pos = size_t(finish - sysex.begin()) + 1;
		}

		if(packets != 14684 || out.decodedTransport.size() != 939744)
		{
			error = "not the expected Machinedrum OS 1.63 SysEx transport";
			return false;
		}

		std::vector<Section> sections;
		if(!parseSections(sections, out.decodedTransport, error))
			return false;
		if(sections.size() != 5)
		{
			error = "Machinedrum OS 1.63 container must contain five sections";
			return false;
		}

		const std::array<size_t, 5> expectedSizes{404766, 750369, 56469, 524288, 524288};
		for(size_t i = 0; i < sections.size(); ++i)
			if(sections[i].data.size() != expectedSizes[i])
			{
				error = "unexpected decompressed Machinedrum section size";
				return false;
			}
		if(sections[0].data.size() < 6
			|| sections[0].data[0] != 0x2e || sections[0].data[1] != 0x7c
			|| sections[0].data[2] != 0x00 || sections[0].data[3] != 0x30
			|| sections[0].data[4] != 0x00 || sections[0].data[5] != 0x00)
		{
			error = "unexpected Machinedrum OS 1.63 entry stub";
			return false;
		}

		const auto firstDataOffset = sections[3].offset;
		const auto priorEnd = sections[2].offset + sections[2].stored;
		if(firstDataOffset != priorEnd + 4
			|| out.decodedTransport[priorEnd] != '1'
			|| out.decodedTransport[priorEnd + 1] != '6'
			|| out.decodedTransport[priorEnd + 2] != '3'
			|| out.decodedTransport[priorEnd + 3] != ' ')
		{
			error = "Machinedrum version marker 163 is missing";
			return false;
		}

		out.version = "1.63";
		out.mainOs = std::move(sections[0].data);
		out.dsp1 = std::move(sections[1].data);
		out.dsp2 = std::move(sections[2].data);
		out.factoryWaveforms.reserve(1024 * 1024);
		out.factoryWaveforms.insert(out.factoryWaveforms.end(),
			sections[3].data.begin(), sections[3].data.end());
		out.factoryWaveforms.insert(out.factoryWaveforms.end(),
			sections[4].data.begin(), sections[4].data.end());
		return true;
	}
}
