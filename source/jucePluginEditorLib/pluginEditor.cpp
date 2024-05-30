#include "pluginEditor.h"

#include "pluginProcessor.h"

#include "../jucePluginLib/parameterbinding.h"

#include "../synthLib/os.h"
#include "../synthLib/sysexToMidi.h"

#include "patchmanager/patchmanager.h"
#include "patchmanager/savepatchdesc.h"

namespace jucePluginEditorLib
{
	Editor::Editor(Processor& _processor, pluginLib::ParameterBinding& _binding, std::string _skinFolder)
		: genericUI::Editor(static_cast<EditorInterface&>(*this))
		, m_processor(_processor)
		, m_binding(_binding)
		, m_skinFolder(std::move(_skinFolder))
	{
		showDisclaimer();
	}

	Editor::~Editor()
	{
		for (const auto& file : m_dragAndDropFiles)
			file.deleteFile();
	}

	void Editor::loadPreset(const std::function<void(const juce::File&)>& _callback)
	{
		const auto path = m_processor.getConfig().getValue("load_path", "");

		m_fileChooser = std::make_unique<juce::FileChooser>(
			"Choose syx/midi banks to import",
			path.isEmpty() ? juce::File::getSpecialLocation(juce::File::currentApplicationFile).getParentDirectory() : path,
			"*.syx,*.mid,*.midi,*.vstpreset,*.fxb,*.cpr", true);

		constexpr auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::FileChooserFlags::canSelectFiles;

		const std::function onFileChosen = [this, _callback](const juce::FileChooser& _chooser)
		{
			if (_chooser.getResults().isEmpty())
				return;

			const auto result = _chooser.getResult();

			m_processor.getConfig().setValue("load_path", result.getParentDirectory().getFullPathName());

			_callback(result);
		};
		m_fileChooser->launchAsync(flags, onFileChosen);
	}

	void Editor::savePreset(const std::function<void(const juce::File&)>& _callback)
	{
#if !SYNTHLIB_DEMO_MODE
		const auto path = m_processor.getConfig().getValue("save_path", "");

		m_fileChooser = std::make_unique<juce::FileChooser>(
			"Save preset(s) as syx or mid",
			path.isEmpty() ? juce::File::getSpecialLocation(juce::File::currentApplicationFile).getParentDirectory() : path,
			"*.syx,*.mid", true);

		constexpr auto flags = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::FileChooserFlags::canSelectFiles;

		auto onFileChosen = [this, _callback](const juce::FileChooser& _chooser)
		{
			if (_chooser.getResults().isEmpty())
				return;

			const auto result = _chooser.getResult();
			m_processor.getConfig().setValue("save_path", result.getParentDirectory().getFullPathName());

			if (!result.existsAsFile() || juce::NativeMessageBox::showYesNoBox(juce::AlertWindow::WarningIcon, "File exists", "Do you want to overwrite the existing file?") == 1)
			{
				_callback(result);
			}
		};
		m_fileChooser->launchAsync(flags, onFileChosen);
#else
		showDemoRestrictionMessageBox();
#endif
	}

#if !SYNTHLIB_DEMO_MODE
	bool Editor::savePresets(const FileType _type, const std::string& _pathName, const std::vector<std::vector<uint8_t>>& _presets)
	{
		if (_presets.empty())
			return false;

		if (_type == FileType::Mid)
			return synthLib::SysexToMidi::write(_pathName.c_str(), _presets);

		FILE* hFile = fopen(_pathName.c_str(), "wb");

		if (!hFile)
			return false;

		for (const auto& message : _presets)
		{
			const auto written = fwrite(&message.front(), 1, message.size(), hFile);

			if (written != message.size())
			{
				fclose(hFile);
				return false;
			}
		}
		fclose(hFile);
		return true;
	}
#endif

	std::string Editor::createValidFilename(FileType& _type, const juce::File& _file)
	{
		const auto ext = _file.getFileExtension();
		auto file = _file.getFullPathName().toStdString();
		
		if (ext.endsWithIgnoreCase("mid"))
			_type = FileType::Mid;
		else if (ext.endsWithIgnoreCase("syx"))
			_type = FileType::Syx;
		else
			file += _type == FileType::Mid ? ".mid" : ".syx";
		return file;
	}

	void Editor::showDemoRestrictionMessageBox() const
	{
		const auto &[title, msg] = getDemoRestrictionText();
		juce::NativeMessageBox::showMessageBoxAsync(juce::AlertWindow::WarningIcon, title, msg);
	}

	void Editor::setPatchManager(patchManager::PatchManager* _patchManager)
	{
		m_patchManager.reset(_patchManager);

		if(_patchManager && !m_instanceConfig.empty())
			m_patchManager->setPerInstanceConfig(m_instanceConfig);
	}

	void Editor::setPerInstanceConfig(const std::vector<uint8_t>& _data)
	{
		m_instanceConfig = _data;

		if(m_patchManager)
			m_patchManager->setPerInstanceConfig(_data);
	}

	void Editor::getPerInstanceConfig(std::vector<uint8_t>& _data)
	{
		if(m_patchManager)
		{
			m_instanceConfig.clear();
			m_patchManager->getPerInstanceConfig(m_instanceConfig);
		}

		if(!m_instanceConfig.empty())
			_data.insert(_data.end(), m_instanceConfig.begin(), m_instanceConfig.end());
	}

	void Editor::setCurrentPart(uint8_t _part)
	{
		genericUI::Editor::setCurrentPart(_part);

		if(m_patchManager)
			m_patchManager->setCurrentPart(_part);
	}

	void Editor::showDisclaimer() const
	{
		if(!m_processor.getConfig().getBoolValue("disclaimerSeen", false))
		{
			const juce::MessageBoxOptions options = juce::MessageBoxOptions::makeOptionsOk(juce::MessageBoxIconType::WarningIcon, m_processor.getProperties().name,
	           "It is the sole responsibility of the user to operate this emulator within the bounds of all applicable laws.\n\n"

				"Usage of emulators in conjunction with ROM images you are not legally entitled to own is forbidden by copyright law.\n\n"

				"If you are not legally entitled to use this emulator please discontinue usage immediately.\n\n", 

				"I Agree"
			);

			juce::NativeMessageBox::showAsync(options, [this](int)
			{
				m_processor.getConfig().setValue("disclaimerSeen", true);
				onDisclaimerFinished();
			});
		}
		else
		{
			onDisclaimerFinished();
		}
	}

	bool Editor::shouldDropFilesWhenDraggedExternally(const juce::DragAndDropTarget::SourceDetails& sourceDetails, juce::StringArray& files, bool& canMoveFiles)
	{
		const auto* savePatchDesc = patchManager::SavePatchDesc::fromDragSource(sourceDetails);

		if(!savePatchDesc || !savePatchDesc->hasPatches())
			return false;

		// try to create human-readable filename first
		const auto patchFileName = savePatchDesc->getExportFileName(m_processor.getProperties().name);
		const auto pathName = juce::File::getSpecialLocation(juce::File::tempDirectory).getFullPathName().toStdString() + "/" + patchFileName;

		auto file = juce::File(pathName);

		if(file.hasWriteAccess())
		{
			m_dragAndDropFiles.emplace_back(file);
		}
		else
		{
			// failed, create temp file
			const auto& tempFile = m_dragAndDropTempFiles.emplace_back(std::make_shared<juce::TemporaryFile>(patchFileName));
			file = tempFile->getFile();
		}

		if(!savePatchDesc->writePatchesToFile(file))
			return false;

		files.add(file.getFullPathName());

		canMoveFiles = true;
		return true;
	}

	void Editor::copyCurrentPatchToClipboard() const
	{
		// copy patch of current part to Clipboard
		const auto p = m_patchManager->requestPatchForPart(m_patchManager->getCurrentPart());

		if(!p)
			return;

		const auto patchAsString = m_patchManager->toString(p);

		if(patchAsString.empty())
			return;

		const auto time = juce::Time::getCurrentTime();

		std::stringstream ss;
		ss << getProcessor().getProperties().name << " - Patch copied at " << time.formatted("%Y.%m.%d %H:%M") << time.getUTCOffsetString(true);
		ss << '\n';
		ss << "Patch '" << p->getName() << "' data:\n";
		ss << "```\n";
		ss << patchAsString << '\n';
		ss << "```";
		juce::SystemClipboard::copyTextToClipboard(ss.str());
	}

	bool Editor::replaceCurrentPatchFromClipboard() const
	{
		return m_patchManager->activatePatchFromClipboard();
	}

	void Editor::openMenu(juce::MouseEvent* _event)
	{
		onOpenMenu(this, _event);
	}

	bool Editor::openContextMenuForParameter(const juce::MouseEvent* _event)
	{
		if(!_event || !_event->originalComponent)
			return false;

		// TODO
		return false;
	}

	bool Editor::keyPressed(const juce::KeyPress& _key)
	{
		if(_key.getModifiers().isCommandDown())
		{
			switch(_key.getKeyCode())
			{
			case 'c':
			case 'C':
				copyCurrentPatchToClipboard();
				return true;
			case 'v':
			case 'V':
				if(replaceCurrentPatchFromClipboard())
					return true;
				break;
			default:
				return genericUI::Editor::keyPressed(_key);
			}
		}
		return genericUI::Editor::keyPressed(_key);
	}

	void Editor::onDisclaimerFinished() const
	{
		if(!synthLib::isRunningUnderRosetta())
			return;

		const auto& name = m_processor.getProperties().name;

		juce::NativeMessageBox::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
			name + " - Rosetta detected", 
			name + " appears to be running in Rosetta mode.\n"
			"\n"
			"The DSP emulation core will perform much worse when being executed under Rosetta. We strongly recommend to run your DAW as a native Apple Silicon application", 
			nullptr, juce::ModalCallbackFunction::create([this](int)
			{
				m_processor.getConfig().setValue("disclaimerSeen", true);
			}));
	}

	const char* Editor::getResourceByFilename(const std::string& _name, uint32_t& _dataSize)
	{
		if(!m_skinFolder.empty())
		{
			auto readFromCache = [this, &_name, &_dataSize]()
			{
				const auto it = m_fileCache.find(_name);
				if(it == m_fileCache.end())
				{
					_dataSize = 0;
					return static_cast<char*>(nullptr);
				}
				_dataSize = static_cast<uint32_t>(it->second.size());
				return &it->second.front();
			};

			const auto* res = readFromCache();

			if(res)
				return res;

			const auto modulePath = synthLib::getModulePath();
			const auto folder = synthLib::validatePath(m_skinFolder.find(modulePath) == 0 ? m_skinFolder : modulePath + m_skinFolder);

			// try to load from disk first
			FILE* hFile = fopen((folder + _name).c_str(), "rb");
			if(hFile)
			{
				fseek(hFile, 0, SEEK_END);
				_dataSize = ftell(hFile);
				fseek(hFile, 0, SEEK_SET);

				std::vector<char> data;
				data.resize(_dataSize);
				const auto readCount = fread(&data.front(), 1, _dataSize, hFile);
				fclose(hFile);

				if(readCount == _dataSize)
					m_fileCache.insert(std::make_pair(_name, std::move(data)));

				res = readFromCache();

				if(res)
					return res;
			}
		}

		uint32_t size = 0;
		const auto res = findResourceByFilename(_name, size);
		if(!res)
			throw std::runtime_error("Failed to find file named " + _name);
		_dataSize = size;
		return res;
	}

	int Editor::getParameterIndexByName(const std::string& _name)
	{
		return static_cast<int>(m_processor.getController().getParameterIndexByName(_name));
	}

	bool Editor::bindParameter(juce::Button& _target, int _parameterIndex)
	{
		m_binding.bind(_target, _parameterIndex);
		return true;
	}

	bool Editor::bindParameter(juce::ComboBox& _target, int _parameterIndex)
	{
		m_binding.bind(_target, _parameterIndex);
		return true;
	}

	bool Editor::bindParameter(juce::Slider& _target, int _parameterIndex)
	{
		m_binding.bind(_target, _parameterIndex);
		return true;
	}

	juce::Value* Editor::getParameterValue(int _parameterIndex, uint8_t _part)
	{
		return m_processor.getController().getParamValueObject(_parameterIndex, _part);
	}
}
