#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace jucePluginEditorLib
{
	class PluginEditorState;

	//==============================================================================
	class EditorWindow : public juce::AudioProcessorEditor
	{
	public:
	    explicit EditorWindow (juce::AudioProcessor& _p, PluginEditorState& _s, juce::PropertiesFile& _config);
	    ~EditorWindow() override;

		void mouseDown(const juce::MouseEvent& event) override;

		void paint(juce::Graphics& g) override {}

		void resized() override;

	private:
		void setGuiScale(juce::Component* _comp, float _percent);
		void setUiRoot(juce::Component* _component);

		PluginEditorState& m_state;
		juce::PropertiesFile& m_config;

	    juce::ComponentBoundsConstrainer m_sizeConstrainer;

		JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EditorWindow)
	};
}
