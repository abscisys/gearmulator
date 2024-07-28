#pragma once

#include <deque>
#include <vector>
#include <cstdint>
#include <mutex>

namespace synthLib
{
	struct SMidiEvent;
}

namespace mc68k
{
	class Qsm;
}

namespace hwLib
{
	class SciMidi
	{
	public:
		explicit SciMidi(mc68k::Qsm& _qsm);

		void process(uint32_t _numSamples);

		void write(uint8_t _byte);
		void write(const std::initializer_list<uint8_t>& _bytes)
		{
			for (const uint8_t byte : _bytes)
				write(byte);
		}
		void write(const std::vector<uint8_t>& _bytes)
		{
			for (const uint8_t byte : _bytes)
				write(byte);
		}
		void write(const synthLib::SMidiEvent& _e);


		void read(std::vector<uint8_t>& _result);

	private:
		mc68k::Qsm& m_qsm;

		bool m_readingSysex = false;
		bool m_writingSysex = false;
		uint32_t m_remainingSysexDelay = 0;

		std::deque< std::vector<uint8_t> > m_pendingSysexBuffers;
		std::vector<uint8_t> m_pendingSysexMessage;
		std::mutex m_mutex;
	};
}
