#pragma once

#include <deque>
#include <vector>
#include <cstdint>

namespace mc68k
{
	class Qsm;
}

namespace mqLib
{
	class Midi
	{
	public:
		explicit Midi(mc68k::Qsm& _qsm);

		void process(uint32_t _numSamples);

		void writeMidi(uint8_t _byte);
		void writeMidi(const std::initializer_list<uint8_t>& _bytes)
		{
			for (const uint8_t byte : _bytes)
				writeMidi(byte);
		}
		void writeMidi(const std::vector<uint8_t>& _bytes)
		{
			for (const uint8_t byte : _bytes)
				writeMidi(byte);
		}
		void readTransmitBuffer(std::vector<uint8_t>& _result);

	private:
		mc68k::Qsm& m_qsm;

		bool m_transmittingSysex = false;
		bool m_receivingSysex = false;
		uint32_t m_remainingSysexDelay = 0;

		std::deque< std::vector<uint8_t> > m_pendingSysexBuffers;
		std::vector<uint8_t> m_pendingSysexMessage;
	};
}
