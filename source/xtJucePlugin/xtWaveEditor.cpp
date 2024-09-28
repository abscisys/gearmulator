#include "xtWaveEditor.h"

#include "weWaveTree.h"
#include "weTablesTree.h"
#include "weControlTree.h"
#include "weGraphFreq.h"
#include "weGraphPhase.h"
#include "weGraphTime.h"
#include "xtController.h"

#include "xtEditor.h"
#include "xtLib/xtState.h"

namespace xtJucePlugin
{
	WaveEditor::WaveEditor(Editor& _editor) : ComponentMovementWatcher(this), m_editor(_editor), m_data(_editor.getXtController())
	{
		addComponentListener(this);

		m_data.onWaveChanged.addListener([this](const xt::WaveId& _waveIndex)
		{
			if(_waveIndex != m_selectedWave)
				return;

			setSelectedWave(_waveIndex, true);
		});

		m_graphData.onIntegerChanged.addListener([this](const xt::WaveData& _data)
		{
			onWaveDataChanged(_data);
		});
	}

	WaveEditor::~WaveEditor()
	{
		destroy();
		removeComponentListener(this);
	}

	void WaveEditor::initialize()
	{
		auto* waveListParent = m_editor.findComponent("wecWaveList");
		auto* tablesListParent = m_editor.findComponent("wecWavetableList");
		auto* controlListParent = m_editor.findComponent("wecWaveControlTable");
		auto* waveFreqParent = m_editor.findComponent("wecWaveFreq");
		auto* wavePhaseParent = m_editor.findComponent("wecWavePhase");
		auto* waveTimeParent = m_editor.findComponent("wecWaveTime");

		m_waveTree.reset(new WaveTree(*this));
		m_tablesTree.reset(new TablesTree(*this));
		m_controlTree.reset(new ControlTree(*this));

		m_graphFreq.reset(new GraphFreq(*this));
		m_graphPhase.reset(new GraphPhase(*this));
		m_graphTime.reset(new GraphTime(*this));

		waveListParent->addAndMakeVisible(m_waveTree.get());
		tablesListParent->addAndMakeVisible(m_tablesTree.get());
		controlListParent->addAndMakeVisible(m_controlTree.get());

		waveFreqParent->addAndMakeVisible(m_graphFreq.get());
		wavePhaseParent->addAndMakeVisible(m_graphPhase.get());
		waveTimeParent->addAndMakeVisible(m_graphTime.get());

		constexpr auto colourId = juce::TreeView::ColourIds::backgroundColourId;
		const auto colour = m_waveTree->findColour(colourId);
		m_graphFreq->setColour(colourId, colour);
		m_graphPhase->setColour(colourId, colour);
		m_graphTime->setColour(colourId, colour);

		m_btWavePreview = m_editor.findComponentT<juce::Button>("btWavePreview");
		m_ledWavePreview = m_editor.findComponentT<juce::Button>("ledWavePreview");

		m_btWavetablePreview = m_editor.findComponentT<juce::Button>("btWavetablePreview");
		m_ledWavetablePreview = m_editor.findComponentT<juce::Button>("ledWavetablePreview");

		m_btWavePreview->onClick = [this]
		{
			toggleWavePreview(m_btWavePreview->getToggleState());
		};

		m_btWavetablePreview->onClick = [this]
		{
			toggleWavetablePreview(m_btWavePreview->getToggleState());
		};
	}

	void WaveEditor::destroy()
	{
		m_waveTree.reset();
		m_controlTree.reset();
		m_tablesTree.reset();
		m_graphFreq.reset();
		m_graphPhase.reset();
		m_graphTime.reset();
	}

	void WaveEditor::checkFirstTimeVisible()
	{
		if(isShowing() && !m_wasVisible)
		{
			m_wasVisible = true;
			onFirstTimeVisible();
		}
	}

	void WaveEditor::onFirstTimeVisible()
	{
		m_data.requestData();
	}

	void WaveEditor::toggleWavePreview(const bool _enabled)
	{
		if(_enabled)
			toggleWavetablePreview(false);

		m_btWavePreview->setToggleState(_enabled, juce::dontSendNotification);
		m_ledWavePreview->setToggleState(_enabled, juce::dontSendNotification);
	}

	void WaveEditor::toggleWavetablePreview(const bool _enabled)
	{
		if(_enabled)
			toggleWavePreview(false);

		m_btWavetablePreview->setToggleState(_enabled, juce::dontSendNotification);
		m_ledWavetablePreview->setToggleState(_enabled, juce::dontSendNotification);
	}

	void WaveEditor::onWaveDataChanged(const xt::WaveData& _data) const
	{
		if(m_btWavePreview->getToggleState())
		{
			const auto sysex = xt::State::createWaveData(_data, m_editor.getXtController().getCurrentPart(), true);
			m_editor.getXtController().sendSysEx(sysex);
		}
	}

	void WaveEditor::onReceiveWave(const pluginLib::MidiPacket::Data& _data, const std::vector<uint8_t>& _msg)
	{
		m_data.onReceiveWave(_data, _msg);
	}

	void WaveEditor::onReceiveTable(const pluginLib::MidiPacket::Data& _data, const std::vector<uint8_t>& _msg)
	{
		m_data.onReceiveTable(_data, _msg);
	}

	void WaveEditor::setSelectedTable(xt::TableId _index)
	{
		if(m_selectedTable == _index)
			return;

		m_selectedTable = _index;
		m_controlTree->setTable(_index);
	}

	void WaveEditor::setSelectedWave(const xt::WaveId _waveIndex, bool _forceRefresh/* = false*/)
	{
		if(m_selectedWave == _waveIndex && !_forceRefresh)
			return;

		m_selectedWave = _waveIndex;

		if(const auto wave = m_data.getWave(_waveIndex))
		{
			m_graphData.set(*wave);
			onWaveDataChanged(*wave);
		}
	}
}
