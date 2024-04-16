#include "controller.h"

#include <cassert>

#include "parameter.h"
#include "processor.h"
#include "dsp56kEmu/logging.h"

namespace pluginLib
{
	uint8_t getParameterValue(Parameter* _p)
	{
		return static_cast<uint8_t>(roundToInt(_p->getValueObject().getValue()));
	}

	Controller::Controller(pluginLib::Processor& _processor, const std::string& _parameterDescJson) : m_processor(_processor), m_descriptions(_parameterDescJson)
	{
	}

	Controller::~Controller()
	{
		m_softKnobs.clear();
	}

	void Controller::registerParams(juce::AudioProcessor& _processor)
    {
		auto globalParams = std::make_unique<juce::AudioProcessorParameterGroup>("global", "Global", "|");

		std::map<ParamIndex, int> knownParameterIndices;

    	for (uint8_t part = 0; part < 16; part++)
		{
			m_paramsByParamType[part].reserve(m_descriptions.getDescriptions().size());

    		const auto partNumber = juce::String(part + 1);
			auto group = std::make_unique<juce::AudioProcessorParameterGroup>("ch" + partNumber, "Ch " + partNumber, "|");

			for (const auto& desc : m_descriptions.getDescriptions())
			{
				const ParamIndex idx = {static_cast<uint8_t>(desc.page), part, desc.index};

				int uid = 0;

				auto itKnownParamIdx = knownParameterIndices.find(idx);

				if(itKnownParamIdx == knownParameterIndices.end())
					knownParameterIndices.insert(std::make_pair(idx, 0));
				else
					uid = ++itKnownParamIdx->second;

				std::unique_ptr<Parameter> p;
				p.reset(createParameter(*this, desc, part, uid));

				if(uid > 0)
				{
					const auto& existingParams = findSynthParam(idx);

					for (auto& existingParam : existingParams)
					{
						if(isDerivedParameter(*existingParam, *p))
							existingParam->addDerivedParameter(p.get());
					}
				}

				const bool isNonPartExclusive = desc.isNonPartSensitive();

				if (isNonPartExclusive && part != 0)
				{
					// only register on first part!
					m_paramsByParamType[part].push_back(m_paramsByParamType[0][m_paramsByParamType[part].size()]);
					continue;
				}

				m_paramsByParamType[part].push_back(p.get());

				if (p->getDescription().isPublic)
				{
					// lifecycle managed by Juce

					auto itExisting = m_synthParams.find(idx);
					if (itExisting != m_synthParams.end())
					{
						itExisting->second.push_back(p.get());
					}
					else
					{
						ParameterList params;
						params.emplace_back(p.get());
						m_synthParams.insert(std::make_pair(idx, std::move(params)));
					}

					if (isNonPartExclusive)
					{
						jassert(part == 0);
						globalParams->addChild(std::move(p));
					}
					else
						group->addChild(std::move(p));
				}
				else
				{
					// lifecycle handled by us

					auto itExisting = m_synthInternalParams.find(idx);
					if (itExisting != m_synthInternalParams.end())
					{
						itExisting->second.push_back(p.get());
					}
					else
					{
						ParameterList params;
						params.emplace_back(p.get());
						m_synthInternalParams.insert(std::make_pair(idx, std::move(params)));
					}
					m_synthInternalParamList.emplace_back(std::move(p));
				}
			}
			_processor.addParameterGroup(std::move(group));
		}

		_processor.addParameterGroup(std::move(globalParams));

		// initialize all soft knobs for all parts
		std::vector<size_t> softKnobs;

		for (size_t i=0; i<m_descriptions.getDescriptions().size(); ++i)
		{
			const auto& desc = m_descriptions.getDescriptions()[i];
			if(!desc.isSoftKnob())
				continue;
			softKnobs.push_back(i);
		}

		for(size_t part = 0; part<m_paramsByParamType.size(); ++part)
		{
			for (const auto& softKnobParam : softKnobs)
			{
				auto* sk = new SoftKnob(*this, static_cast<uint8_t>(part), static_cast<uint32_t>(softKnobParam));
				m_softKnobs.insert({sk->getParameter(), std::unique_ptr<SoftKnob>(sk)});
			}
		}
    }

	void Controller::sendSysEx(const pluginLib::SysEx& msg) const
    {
        synthLib::SMidiEvent ev;
        ev.sysex = msg;
		ev.source = synthLib::MidiEventSourceEditor;
		sendMidiEvent(ev);
    }

	void Controller::sendMidiEvent(const synthLib::SMidiEvent& _ev) const
    {
        m_processor.addMidiEvent(_ev);
    }

	void Controller::sendMidiEvent(const uint8_t _a, const uint8_t _b, const uint8_t _c, const uint32_t _offset/* = 0*/, const synthLib::MidiEventSource _source/* = synthLib::MidiEventSourceEditor*/) const
	{
        m_processor.addMidiEvent(synthLib::SMidiEvent(_a, _b, _c, _offset, _source));
	}

	bool Controller::combineParameterChange(uint8_t& _result, const std::string& _midiPacket, const Parameter& _parameter, uint8_t _value) const
	{
		const auto &desc = _parameter.getDescription();

		std::map<MidiDataType, uint8_t> data;

		const auto *packet = getMidiPacket(_midiPacket);

		if (!packet)
		{
			LOG("Failed to find midi packet " << _midiPacket);
			return false;
		}

		const ParamIndex idx = {static_cast<uint8_t>(desc.page), _parameter.getPart(), desc.index};

		const auto params = findSynthParam(idx);

		uint32_t byte = MidiPacket::InvalidIndex;

		for (auto param : params)
		{
			byte = packet->getByteIndexForParameterName(param->getDescription().name);
			if (byte != MidiPacket::InvalidIndex)
				break;
		}

		if (byte == MidiPacket::InvalidIndex)
		{
			LOG("Failed to find byte index for parameter " << desc.name);
			return false;
		}

		std::vector<const MidiPacket::MidiDataDefinition*> definitions;

		if(!packet->getDefinitionsForByteIndex(definitions, byte))
			return false;

		if (definitions.size() == 1)
		{
			_result = _value;
			return true;
		}

		_result = 0;

	    for (const auto& it : definitions)
	    {
			uint32_t i = 0;

	    	if(!m_descriptions.getIndexByName(i, it->paramName))
			{
				LOG("Failed to find index for parameter " << it->paramName);
				return false;
			}

			auto* p = getParameter(i, _parameter.getPart());
			const auto v = p == &_parameter ? _value : getParameterValue(p);
			_result |= it->getMaskedValue(v);
	    }

		return true;
	}
	
	bool Controller::sendSysEx(const std::string& _packetName) const
    {
	    const std::map<pluginLib::MidiDataType, uint8_t> params;
        return sendSysEx(_packetName, params);
    }

    bool Controller::sendSysEx(const std::string& _packetName, const std::map<pluginLib::MidiDataType, uint8_t>& _params) const
    {
	    std::vector<uint8_t> sysex;

    	if(!createMidiDataFromPacket(sysex, _packetName, _params, 0))
            return false;

        sendSysEx(sysex);
        return true;
    }

	const Controller::ParameterList& Controller::findSynthParam(const uint8_t _part, const uint8_t _page, const uint8_t _paramIndex) const
	{
		const ParamIndex paramIndex{ _page, _part, _paramIndex };

		return findSynthParam(paramIndex);
	}

	const Controller::ParameterList& Controller::findSynthParam(const ParamIndex& _paramIndex) const
    {
		const auto it = m_synthParams.find(_paramIndex);

		if (it != m_synthParams.end())
			return it->second;

    	const auto iti = m_synthInternalParams.find(_paramIndex);

		if (iti == m_synthInternalParams.end())
		{
			static ParameterList empty;
			return empty;
		}

		return iti->second;
    }

    juce::Value* Controller::getParamValueObject(const uint32_t _index, uint8_t _part) const
    {
	    const auto res = getParameter(_index, _part);
		return res ? &res->getValueObject() : nullptr;
    }

    Parameter* Controller::getParameter(const uint32_t _index) const
    {
		return getParameter(_index, 0);
	}

	Parameter* Controller::getParameter(const uint32_t _index, const uint8_t _part) const
	{
		if (_part >= m_paramsByParamType.size())
			return nullptr;

		if (_index >= m_paramsByParamType[_part].size())
			return nullptr;

		return m_paramsByParamType[_part][_index];
	}

	uint32_t Controller::getParameterIndexByName(const std::string& _name) const
	{
		uint32_t index;
		return m_descriptions.getIndexByName(index, _name) ? index : InvalidParameterIndex;
	}

	const MidiPacket* Controller::getMidiPacket(const std::string& _name) const
	{
		return m_descriptions.getMidiPacket(_name);
	}

	bool Controller::createNamedParamValues(MidiPacket::NamedParamValues& _params, const std::string& _packetName, const uint8_t _part) const
	{
        const auto* m = getMidiPacket(_packetName);
		assert(m && "midi packet not found");
        if(!m)
            return false;

        MidiPacket::ParamIndices indices;
		m->getParameterIndices(indices, m_descriptions);

		if(indices.empty())
			return true;

		for (const auto& index : indices)
        {
	        auto* p = getParameter(index.second, _part);
	        if(!p)
		        return false;

	        const auto v = getParameterValue(p);
	        _params.insert(std::make_pair(std::make_pair(index.first, p->getDescription().name), v));
        }

		return true;
	}

	bool Controller::createNamedParamValues(MidiPacket::NamedParamValues& _dest, const MidiPacket::AnyPartParamValues& _source) const
	{
        for(uint32_t i=0; i<_source.size(); ++i)
        {
            const auto& v = _source[i];
            if(!v)
                continue;
            const auto* p = getParameter(i);
            assert(p);
            if(!p)
                return false;
            const auto key = std::make_pair(MidiPacket::AnyPart, p->getDescription().name);
            _dest.insert(std::make_pair(key, *v));
        }
		return true;
	}

	bool Controller::createMidiDataFromPacket(std::vector<uint8_t>& _sysex, const std::string& _packetName, const std::map<MidiDataType, uint8_t>& _data, uint8_t _part) const
	{
		MidiPacket::NamedParamValues paramValues;

		if(!createNamedParamValues(paramValues, _packetName, _part))
			return false;

		return createMidiDataFromPacket(_sysex, _packetName, _data, paramValues);
	}

	bool Controller::createMidiDataFromPacket(std::vector<uint8_t>& _sysex, const std::string& _packetName, const std::map<MidiDataType, uint8_t>& _data, const MidiPacket::NamedParamValues& _values) const
	{
        const auto* m = getMidiPacket(_packetName);

		if(!m->create(_sysex, _data, _values))
        {
	        assert(false && "failed to create midi packet");
	        _sysex.clear();
	        return false;
        }
        return true;
	}

	bool Controller::createMidiDataFromPacket(std::vector<uint8_t>& _sysex, const std::string& _packetName, const std::map<MidiDataType, uint8_t>& _data, const MidiPacket::AnyPartParamValues& _values) const
	{
		MidiPacket::NamedParamValues namedParams;
		if(!createNamedParamValues(namedParams, _values))
			return false;
		return createMidiDataFromPacket(_sysex, _packetName, _data, namedParams);
	}

	bool Controller::parseMidiPacket(const MidiPacket& _packet, MidiPacket::Data& _data, MidiPacket::ParamValues& _parameterValues, const std::vector<uint8_t>& _src) const
	{
		_data.clear();
		_parameterValues.clear();
		return _packet.parse(_data, _parameterValues, m_descriptions, _src);
	}

	bool Controller::parseMidiPacket(const MidiPacket& _packet, MidiPacket::Data& _data, MidiPacket::AnyPartParamValues& _parameterValues, const std::vector<uint8_t>& _src) const
	{
		_data.clear();
		_parameterValues.clear();
		return _packet.parse(_data, _parameterValues, m_descriptions, _src);
	}

	bool Controller::parseMidiPacket(const MidiPacket& _packet, MidiPacket::Data& _data, const std::function<void(MidiPacket::ParamIndex, uint8_t)>& _parameterValues, const std::vector<uint8_t>& _src) const
	{
		_data.clear();
		return _packet.parse(_data, _parameterValues, m_descriptions, _src);
	}

	bool Controller::parseMidiPacket(const std::string& _name, MidiPacket::Data& _data, MidiPacket::ParamValues& _parameterValues, const std::vector<uint8_t>& _src) const
	{
		auto* m = getMidiPacket(_name);
		assert(m);
		if(!m)
			return false;
		return parseMidiPacket(*m, _data, _parameterValues, _src);
	}

	bool Controller::parseMidiPacket(std::string& _name, MidiPacket::Data& _data, MidiPacket::ParamValues& _parameterValues, const std::vector<uint8_t>& _src) const
	{
		const auto& packets = m_descriptions.getMidiPackets();

		for (const auto& packet : packets)
		{
			if(!parseMidiPacket(packet.second, _data, _parameterValues, _src))
				continue;

			_name = packet.first;
			return true;
		}
		return false;
	}

	void Controller::addPluginMidiOut(const std::vector<synthLib::SMidiEvent>& _events)
	{
        const std::lock_guard l(m_pluginMidiOutLock);
        m_pluginMidiOut.insert(m_pluginMidiOut.end(), _events.begin(), _events.end());
	}

	void Controller::getPluginMidiOut(std::vector<synthLib::SMidiEvent>& _events)
	{
		const std::lock_guard l(m_pluginMidiOutLock);
        std::swap(m_pluginMidiOut, _events);
		m_pluginMidiOut.clear();
	}

	bool Controller::lockRegion(const std::string& _id)
	{
		if(m_lockedRegions.find(_id) != m_lockedRegions.end())
			return true;

		if(m_descriptions.getRegions().find(_id) == m_descriptions.getRegions().end())
			return false;

		m_lockedRegions.insert(_id);
		return true;
	}

	bool Controller::unlockRegion(const std::string& _id)
	{
		return m_lockedRegions.erase(_id);
	}

	const std::set<std::string>& Controller::getLockedRegions() const
	{
		return m_lockedRegions;
	}

	bool Controller::isRegionLocked(const std::string& _id)
	{
		return m_lockedRegions.find(_id) != m_lockedRegions.end();
	}

	std::unordered_set<std::string> Controller::getLockedParameterNames() const
	{
		if(m_lockedRegions.empty())
			return {};

		std::unordered_set<std::string> result;

		for (const auto& name : m_lockedRegions)
		{
			const auto& it = m_descriptions.getRegions().find(name);
			if(it == m_descriptions.getRegions().end())
				continue;

			const auto& region = it->second;
			for (const auto& itParam : region.getParams())
				result.insert(itParam.first);
		}

		return result;
	}

	std::unordered_set<const Parameter*> Controller::getLockedParameters(const uint8_t _part) const
	{
		const auto paramNames = getLockedParameterNames();

		std::unordered_set<const Parameter*> results;

		for (const auto& paramName : paramNames)
		{
			const auto idx = getParameterIndexByName(paramName);
			assert(idx != InvalidParameterIndex);
			const auto* p = getParameter(idx, _part);
			assert(p != nullptr);
			results.insert(p);
		}

		return results;
	}

	bool Controller::isParameterLocked(const std::string& _name) const
	{
		const auto& regions = getLockedRegions();
		for (const auto& region : regions)
		{
			const auto& it = m_descriptions.getRegions().find(region);
			if(it == m_descriptions.getRegions().end())
				continue;

			if(it->second.containsParameter(_name))
				return true;
		}
		return false;
	}

	Parameter* Controller::createParameter(Controller& _controller, const Description& _desc, uint8_t _part, int _uid)
	{
		return new Parameter(_controller, _desc, _part, _uid);
	}
}
