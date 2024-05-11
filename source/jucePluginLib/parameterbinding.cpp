#include "parameterbinding.h"

#include <cassert>

#include "parameter.h"
#include "controller.h"

namespace pluginLib
{
	ParameterBinding::MouseListener::MouseListener(pluginLib::Parameter* _param, juce::Slider& _slider)
	: m_param(_param), m_slider(&_slider)
	{
	}

	void ParameterBinding::MouseListener::mouseDown(const juce::MouseEvent& event)
	{
		m_param->beginChangeGesture();
	}

	void ParameterBinding::MouseListener::mouseUp(const juce::MouseEvent& event)
	{
		m_param->endChangeGesture();
	}

	void ParameterBinding::MouseListener::mouseDrag(const juce::MouseEvent& event)
	{
		m_param->setValueNotifyingHost(m_param->convertTo0to1(static_cast<float>(m_slider->getValue())), Parameter::ChangedBy::ControlChange);
	}

	void ParameterBinding::MouseListener::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
	{
		m_param->setValueNotifyingHost(m_param->convertTo0to1(static_cast<float>(m_slider->getValue())), Parameter::ChangedBy::ControlChange);
	}

	void ParameterBinding::MouseListener::mouseDoubleClick(const juce::MouseEvent& event)
	{
		m_param->setValueNotifyingHost(m_param->getDefaultValue(), Parameter::ChangedBy::ControlChange);
	}

	ParameterBinding::~ParameterBinding()
	{
		clearBindings();
	}

	void ParameterBinding::bind(juce::Slider &_slider, uint32_t _param)
	{
		bind(_slider, _param, CurrentPart);
	}
	void ParameterBinding::bind(juce::Slider &_slider, uint32_t _param, const uint8_t _part)
	{
		const auto v = m_controller.getParameter(_param, _part == CurrentPart ? m_controller.getCurrentPart() : _part);

		if (!v)
		{
			assert(false && "Failed to find parameter");
			return;
		}

		removeMouseListener(_slider);

		auto* listener = new MouseListener(v, _slider);
		m_sliderMouseListeners.insert(std::make_pair(&_slider, listener));

		_slider.addMouseListener(listener, false);

		const auto& range = v->getNormalisableRange();

		_slider.setRange(range.start, range.end, range.interval);
		_slider.setDoubleClickReturnValue(true, v->convertFrom0to1(v->getDefaultValue()));
		_slider.getValueObject().referTo(v->getValueObject());
		_slider.getProperties().set("type", "slider");
		_slider.getProperties().set("name", juce::String(v->getDescription().name));

		if (v->isBipolar()) 
			_slider.getProperties().set("bipolar", true);

		const BoundParameter p{v, &_slider, _param, _part};
		addBinding(p);
	}

	void ParameterBinding::bind(juce::ComboBox& _combo, uint32_t _param)
	{
		bind(_combo, _param, CurrentPart);
	}

	void ParameterBinding::bind(juce::ComboBox& _combo, const uint32_t _param, uint8_t _part)
	{
		const auto v = m_controller.getParameter(_param, _part == CurrentPart ? m_controller.getCurrentPart() : _part);

		if (!v)
		{
			assert(false && "Failed to find parameter");
			return;
		}

		_combo.setTextWhenNothingSelected("-");
		_combo.setScrollWheelEnabled(true);

		_combo.onChange = nullptr;
		_combo.clear();

		using Entry = std::pair<uint8_t, std::string>;

		std::vector<Entry> sortedValues;

		const auto& desc = v->getDescription();
		const auto& valueList = desc.valueList;

		if(valueList.order.empty())
		{
			uint8_t i = 0;
			const auto& allValues = v->getAllValueStrings();
			for (const auto& vs : allValues)
			{
				if(vs.isNotEmpty())
					sortedValues.emplace_back(i, vs.toStdString());
				++i;
			}
		}
		else
		{
			for(uint32_t i=0; i<valueList.order.size(); ++i)
			{
				const auto value = valueList.orderToValue(i);
				if(value == ValueList::InvalidIndex)
					continue;
				const auto text = valueList.valueToText(value);
				if(text.empty())
					continue;
				sortedValues.emplace_back(value, text);
			}
		}
	
		uint32_t count = 0;

		// we want our long menus to be split into columns of 16 rows each
		// but only if we have have more entries than one and a half such column
		const uint32_t splitAt = (sortedValues.size() > 24) ? 16 : 0;

		for (const auto &vs : sortedValues)
		{
			_combo.addItem(vs.second, vs.first + 1);

			if (++count == splitAt)
			{
				_combo.getRootMenu()->addColumnBreak();
				count = 0;
			}
		}

		_combo.setSelectedId(static_cast<int>(v->getValueObject().getValueSource().getValue()) + 1, juce::dontSendNotification);

		_combo.onChange = [this, &_combo, v]()
		{
			const auto id = _combo.getSelectedId();

			if(id == 0)
				return;

			const int current = v->getValueObject().getValueSource().getValue();

			if((id - 1) == current)
				return;

			if(v->getDescription().isPublic)
			{
				v->beginChangeGesture();
				v->setValueNotifyingHost(v->convertTo0to1(static_cast<float>(id - 1)), Parameter::ChangedBy::ControlChange);
				v->endChangeGesture();
			}
			v->getValueObject().setValue(id - 1);
		};

		const auto listenerId = m_nextListenerId++;

		v->onValueChanged.emplace_back(listenerId, [this, &_combo, v]()
		{
			const auto value = static_cast<int>(v->getValueObject().getValueSource().getValue());
			_combo.setSelectedId(value + 1, juce::dontSendNotification);
		});

		const BoundParameter p{v, &_combo, _param, _part, listenerId};
		addBinding(p);
	}

	void ParameterBinding::bind(juce::Button &_btn, const uint32_t _param)
	{
		bind(_btn, _param, CurrentPart);
	}

	void ParameterBinding::bind(juce::Button& _control, uint32_t _param, uint8_t _part)
	{
		const auto v = m_controller.getParameter(_param, _part == CurrentPart ? m_controller.getCurrentPart() : _part);
		if (!v)
		{
			assert(false && "Failed to find parameter");
			return;
		}
		_control.getToggleStateValue().referTo(v->getValueObject());
		const BoundParameter p{v, &_control, _param, CurrentPart};
		addBinding(p);
	}

	juce::Component* ParameterBinding::getBoundComponent(const Parameter* _parameter) const
	{
		const auto it = m_boundParameters.find(_parameter);
		if(it == m_boundParameters.end())
			return nullptr;
		return it->second;
	}

	void ParameterBinding::removeMouseListener(juce::Slider& _slider)
	{
		const auto it = m_sliderMouseListeners.find(&_slider);

		if(it != m_sliderMouseListeners.end())
		{
			_slider.removeMouseListener(it->second);
			delete it->second;
			m_sliderMouseListeners.erase(it);
		}
	}

	void ParameterBinding::bind(const std::vector<BoundParameter>& _bindings, const bool _currentPartOnly)
	{
		for (const auto& b : _bindings)
		{
			auto* slider = dynamic_cast<juce::Slider*>(b.component);
			if(slider)
			{
				bind(*slider, b.type, b.part);
				continue;
			}
			auto* button = dynamic_cast<juce::DrawableButton*>(b.component);
			if(button)
			{
				bind(*button, b.type, b.part);
				continue;
			}
			auto* comboBox = dynamic_cast<juce::ComboBox*>(b.component);
			if(comboBox)
			{
				bind(*comboBox, b.type, b.part);
				continue;
			}
			assert(false && "unknown component type");
		}
	}

	void ParameterBinding::addBinding(const BoundParameter& _boundParameter)
	{
		m_bindings.emplace_back(_boundParameter);

		m_boundComponents.erase(_boundParameter.component);
		m_boundParameters.erase(_boundParameter.parameter);

		m_boundParameters.insert(std::make_pair(_boundParameter.parameter, _boundParameter.component));
		m_boundComponents.insert(std::make_pair(_boundParameter.component, _boundParameter.parameter));
	}

	void ParameterBinding::disableBinding(const BoundParameter& _b)
	{
		m_boundParameters.erase(_b.parameter);
		m_boundComponents.erase(_b.component);

		auto* slider = dynamic_cast<juce::Slider*>(_b.component);

		if(slider != nullptr)
		{
			removeMouseListener(*slider);
			slider->getValueObject().referTo(juce::Value());
		}

		auto* combo = dynamic_cast<juce::ComboBox*>(_b.component);
		if(combo != nullptr)
			combo->onChange = nullptr;

		auto* button = dynamic_cast<juce::Button*>(_b.component);
		if(button != nullptr)
			button->getToggleStateValue().referTo(juce::Value());

		if(_b.onChangeListenerId)
			_b.parameter->removeListener(_b.onChangeListenerId);
	}

	void ParameterBinding::clearBindings()
	{
		for (const auto& b : m_bindings)
			disableBinding(b);
		clear();
	}

	void ParameterBinding::clear()
	{
		m_bindings.clear();
		m_boundParameters.clear();
		m_boundComponents.clear();
	}

	void ParameterBinding::setPart(uint8_t _part)
	{
		const std::vector<BoundParameter> bindings = m_bindings;

		clearBindings();

		m_controller.setCurrentPart(_part);

		bind(bindings, true);
	}

	void ParameterBinding::disableBindings()
	{
		m_disabledBindings.clear();
		std::swap(m_bindings, m_disabledBindings);

		for (const auto& b : m_disabledBindings)
			disableBinding(b);
	}

	void ParameterBinding::enableBindings()
	{
		bind(m_disabledBindings, false);
		m_disabledBindings.clear();
	}
}
