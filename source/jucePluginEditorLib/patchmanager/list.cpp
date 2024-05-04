#include "list.h"

#include "defaultskin.h"
#include "listitem.h"
#include "patchmanager.h"
#include "search.h"
#include "../pluginEditor.h"
#include "../../juceUiLib/uiObject.h"

#include "../../juceUiLib/uiObjectStyle.h"

namespace jucePluginEditorLib::patchManager
{
	List::List(PatchManager& _pm): m_patchManager(_pm)
	{
		setColour(backgroundColourId, juce::Colour(defaultSkin::colors::background));
		setColour(textColourId, juce::Colour(defaultSkin::colors::itemText));

		getViewport()->setScrollBarsShown(true, false);
		setModel(this);
		setMultipleSelectionEnabled(true);

		if (const auto& t = _pm.getTemplate("pm_listbox"))
			t->apply(_pm.getEditor(), *this);

		if(const auto t = _pm.getTemplate("pm_scrollbar"))
		{
			t->apply(_pm.getEditor(), getVerticalScrollBar());
			t->apply(_pm.getEditor(), getHorizontalScrollBar());
		}
		else
		{
			getVerticalScrollBar().setColour(juce::ScrollBar::thumbColourId, juce::Colour(defaultSkin::colors::scrollbar));
			getVerticalScrollBar().setColour(juce::ScrollBar::trackColourId, juce::Colour(defaultSkin::colors::scrollbar));
			getHorizontalScrollBar().setColour(juce::ScrollBar::thumbColourId, juce::Colour(defaultSkin::colors::scrollbar));
			getHorizontalScrollBar().setColour(juce::ScrollBar::trackColourId, juce::Colour(defaultSkin::colors::scrollbar));
		}

		setRowSelectedOnMouseDown(false);
	}

	void List::setContent(const pluginLib::patchDB::SearchHandle& _handle)
	{
		cancelSearch();

		const auto& search = m_patchManager.getSearch(_handle);

		if (!search)
			return;

		setContent(search);
	}

	void List::setContent(pluginLib::patchDB::SearchRequest&& _request)
	{
		cancelSearch();
		const auto sh = getPatchManager().search(std::move(_request));
		setContent(sh);
		m_searchHandle = sh;
	}

	void List::clear()
	{
		m_search.reset();
		m_patches.clear();
		m_filteredPatches.clear();
		updateContent();
		getPatchManager().setListStatus(0, 0);
	}

	void List::refreshContent()
	{
		setContent(m_search);
	}

	void List::setContent(const std::shared_ptr<pluginLib::patchDB::Search>& _search)
	{
		const std::set<Patch> selectedPatches = getSelectedPatches();

		m_search = _search;

		m_patches.clear();
		{
			std::shared_lock lock(_search->resultsMutex);
			m_patches.insert(m_patches.end(), _search->results.begin(), _search->results.end());
		}

		sortPatches();
		filterPatches();

		updateContent();

		setSelectedPatches(selectedPatches);

		repaint();

		getPatchManager().setListStatus(static_cast<uint32_t>(selectedPatches.size()), static_cast<uint32_t>(getPatches().size()));
	}

	bool List::exportPresets(const bool _selectedOnly, FileType _fileType) const
	{
		Patches patches;

		if(_selectedOnly)
		{
			const auto selected = getSelectedPatches();
			if(selected.empty())
				return false;
			patches.assign(selected.begin(), selected.end());
		}
		else
		{
			patches = getPatches();
		}

		if(patches.empty())
			return false;

		return getPatchManager().exportPresets(std::move(patches), _fileType);
	}

	bool List::onClicked(const juce::MouseEvent& _mouseEvent)
	{
		if(!_mouseEvent.mods.isPopupMenu())
			return false;

		auto fileTypeMenu = [this](const std::function<void(FileType)>& _func)
		{
			juce::PopupMenu menu;
			menu.addItem(".syx", [this, _func]{_func(FileType::Syx);});
			menu.addItem(".mid", [this, _func]{_func(FileType::Mid);});
			return menu;
		};

		auto selectedPatches = getSelectedPatches();

		const auto hasSelectedPatches = !selectedPatches.empty();

		juce::PopupMenu menu;
		if(hasSelectedPatches)
			menu.addSubMenu("Export selected...", fileTypeMenu([this](const FileType _fileType) { exportPresets(true, _fileType); }));
		menu.addSubMenu("Export all...", fileTypeMenu([this](const FileType _fileType) { exportPresets(false, _fileType); }));

		if(hasSelectedPatches)
		{
			menu.addSeparator();

			pluginLib::patchDB::TypedTags tags;

			for (const auto& selectedPatch : selectedPatches)
				tags.add(selectedPatch->getTags());

			if(selectedPatches.size() == 1)
			{
				const auto& patch = *selectedPatches.begin();
				const auto row = getSelectedRow();
				const auto pos = getRowPosition(row, true);

				menu.addItem("Rename...", [this, patch, pos]
				{
					beginEdit(this, pos, patch->getName(), [this, patch](bool _cond, const std::string& _name)
					{
						if(_name != patch->getName())
							getPatchManager().renamePatch(patch, _name);
					});
				});

				menu.addItem("Locate", [this, patch, pos]
				{
					m_patchManager.setSelectedDataSource(patch->source.lock());
				});
			}

			if(!m_search->request.tags.empty())
			{
				menu.addItem("Remove selected", [this, s = selectedPatches]
				{
					const std::vector<pluginLib::patchDB::PatchPtr> patches(s.begin(), s.end());
					pluginLib::patchDB::TypedTags removeTags;

					// converted "added" tags to "removed" tags
					for (const auto& tags : m_search->request.tags.get())
					{
						const pluginLib::patchDB::TagType type = tags.first;
						const auto& t = tags.second;
							
						for (const auto& tag : t.getAdded())
							removeTags.addRemoved(type, tag);
					}

					m_patchManager.modifyTags(patches, removeTags);
					m_patchManager.repaint();
				});
			}
			else if(getSourceType() == pluginLib::patchDB::SourceType::LocalStorage)
			{
				menu.addItem("Deleted selected", [this, s = selectedPatches]
				{
					if(showDeleteConfirmationMessageBox())
					{
						const std::vector<pluginLib::patchDB::PatchPtr> patches(s.begin(), s.end());
						m_patchManager.removePatches(m_search->request.sourceNode, patches);
					}
				});
			}

			if(tags.containsAdded())
			{
				bool haveSeparator = false;

				for (const auto& it : tags.get())
				{
					const auto type = it.first;

					const auto& t = it.second;

					if(t.empty())
						continue;

					const auto tagTypeName = m_patchManager.getTagTypeName(type);

					if(tagTypeName.empty())
						continue;

					juce::PopupMenu tagMenu;

					for (const auto& tag : t.getAdded())
					{
						pluginLib::patchDB::TypedTags removeTags;
						removeTags.addRemoved(type, tag);

						std::vector<pluginLib::patchDB::PatchPtr> patches{selectedPatches.begin(), selectedPatches.end()};

						tagMenu.addItem(tag, [this, s = std::move(patches), removeTags]
						{
							m_patchManager.modifyTags(s, removeTags);
						});
					}

					if(!haveSeparator)
					{
						menu.addSeparator();
						haveSeparator = true;
					}

					menu.addSubMenu("Remove from " + tagTypeName, tagMenu);
				}
			}
		}
		menu.addSeparator();
		menu.addItem("Hide Duplicates", true, m_hideDuplicates, [this]
		{
			setFilter(m_filter, !m_hideDuplicates);
		});
		menu.showMenuAsync({});
		return true;
	}

	void List::cancelSearch()
	{
		if(m_searchHandle == pluginLib::patchDB::g_invalidSearchHandle)
			return;
		getPatchManager().cancelSearch(m_searchHandle);
		m_searchHandle = pluginLib::patchDB::g_invalidSearchHandle;
	}

	int List::getNumRows()
	{
		return static_cast<int>(getPatches().size());
	}

	void List::paintListBoxItem(const int _rowNumber, juce::Graphics& _g, const int _width, const int _height, const bool _rowIsSelected)
	{
		const auto* style = dynamic_cast<genericUI::UiObjectStyle*>(&getLookAndFeel());

		if (_rowNumber >= getNumRows())
			return;	// Juce what are you up to?

		const auto& patch = getPatch(_rowNumber);

		const auto text = patch->getName();

		if(_rowIsSelected)
		{
			if(style)
				_g.setColour(style->getSelectedItemBackgroundColor());
			else
				_g.setColour(juce::Colour(0x33ffffff));
			_g.fillRect(0, 0, _width, _height);
		}

		if (style)
		{
			if (const auto f = style->getFont())
				_g.setFont(*f);
		}

		const auto c = getPatchManager().getPatchColor(patch);

		constexpr int offsetX = 20;

		if(c != pluginLib::patchDB::g_invalidColor)
		{
			_g.setColour(juce::Colour(c));
			constexpr auto s = 8.f;
			constexpr auto sd2 = 0.5f * s;
			_g.fillEllipse(10 - sd2, static_cast<float>(_height) * 0.5f - sd2, s, s);
//			_g.setColour(juce::Colour(0xffffffff));
//			_g.drawEllipse(10 - sd2, static_cast<float>(_height) * 0.5f - sd2, s, s, 1.0f);
//			offsetX += 14;
		}

//		if(c != pluginLib::patchDB::g_invalidColor)
//			_g.setColour(juce::Colour(c));
//		else
		_g.setColour(findColour(textColourId));

		_g.drawText(text, offsetX, 0, _width - 4, _height, style ? style->getAlign() : juce::Justification::centredLeft, true);
	}

	juce::var List::getDragSourceDescription(const juce::SparseSet<int>& rowsToDescribe)
	{
		const auto& ranges = rowsToDescribe.getRanges();

		if (ranges.isEmpty())
			return {};

		juce::Array<juce::var> indices;

		for (const auto& range : ranges)
		{
			for (int i = range.getStart(); i < range.getEnd(); ++i)
			{
				if(i >= 0 && static_cast<size_t>(i) < getPatches().size())
					indices.add(i);
			}
		}

		return indices;
	}

	juce::Component* List::refreshComponentForRow(int rowNumber, bool isRowSelected, Component* existingComponentToUpdate)
	{
		auto* existing = dynamic_cast<ListItem*>(existingComponentToUpdate);

		if (existing)
		{
			existing->setRow(rowNumber);
			return existing;
		}

		delete existingComponentToUpdate;

		return new ListItem(*this, rowNumber);
	}

	void List::selectedRowsChanged(const int lastRowSelected)
	{
		ListBoxModel::selectedRowsChanged(lastRowSelected);

		if(!m_ignoreSelectedRowsChanged)
			activateSelectedPatch();

		const auto patches = getSelectedPatches();
		getPatchManager().setListStatus(static_cast<uint32_t>(patches.size()), static_cast<uint32_t>(getPatches().size()));
	}

	std::set<List::Patch> List::getSelectedPatches() const
	{
		std::set<Patch> result;

		const auto selectedRows = getSelectedRows();
		const auto& ranges = selectedRows.getRanges();

		for (const auto& range : ranges)
		{
			for (int i = range.getStart(); i < range.getEnd(); ++i)
			{
				if (i >= 0 && static_cast<size_t>(i) < getPatches().size())
					result.insert(getPatch(i));
			}
		}
		return result;
	}

	bool List::setSelectedPatches(const std::set<Patch>& _patches)
	{
		if (_patches.empty())
			return false;

		std::set<pluginLib::patchDB::PatchKey> patches;

		for (const auto& patch : _patches)
		{
			if(!patch->source.expired())
				patches.insert(pluginLib::patchDB::PatchKey(*patch));
		}
		return setSelectedPatches(patches);
	}

	bool List::setSelectedPatches(const std::set<pluginLib::patchDB::PatchKey>& _patches)
	{
		if (_patches.empty())
		{
			deselectAllRows();
			return false;
		}

		juce::SparseSet<int> selection;

		int maxRow = std::numeric_limits<int>::min();
		int minRow = std::numeric_limits<int>::max();

		for(int i=0; i<static_cast<int>(getPatches().size()); ++i)
		{
			const auto key = pluginLib::patchDB::PatchKey(*getPatch(i));

			if (_patches.find(key) != _patches.end())
			{
				selection.addRange({ i, i + 1 });

				maxRow = std::max(maxRow, i);
				minRow = std::min(minRow, i);
			}
		}

		if(selection.isEmpty())
		{
			deselectAllRows();
			return false;
		}

		m_ignoreSelectedRowsChanged = true;
		setSelectedRows(selection);
		m_ignoreSelectedRowsChanged = false;
		scrollToEnsureRowIsOnscreen((minRow + maxRow) >> 1);
		return true;
	}

	void List::activateSelectedPatch() const
	{
		const auto patches = getSelectedPatches();

		if(patches.size() == 1)
			m_patchManager.setSelectedPatch(*patches.begin(), m_search->handle);
	}

	void List::processDirty(const pluginLib::patchDB::Dirty& _dirty)
	{
		if (!m_search)
			return;

		if (_dirty.searches.empty())
			return;

		if(_dirty.searches.find(m_search->handle) != _dirty.searches.end())
			setContent(m_search);
	}

	std::vector<pluginLib::patchDB::PatchPtr> List::getPatchesFromDragSource(const juce::DragAndDropTarget::SourceDetails& _dragSourceDetails)
	{
		const auto* list = dynamic_cast<List*>(_dragSourceDetails.sourceComponent.get());
		if(!list)
			return {};

		const auto* arr = _dragSourceDetails.description.getArray();
		if (!arr)
			return {};

		std::vector<pluginLib::patchDB::PatchPtr> patches;

		for (const auto& var : *arr)
		{
			if (!var.isInt())
				continue;
			const int idx = var;
			if (const auto patch = list->getPatch(idx))
				patches.push_back(patch);
		}

		return patches;
	}

	pluginLib::patchDB::DataSourceNodePtr List::getDataSource() const
	{
		if(!m_search)
			return nullptr;

		return m_search->request.sourceNode;
	}

	void List::setFilter(const std::string& _filter)
	{
		setFilter(_filter, m_hideDuplicates);
	}

	void List::setFilter(const std::string& _filter, const bool _hideDuplicates)
	{
		if (m_filter == _filter && _hideDuplicates == m_hideDuplicates)
			return;

		const auto selected = getSelectedPatches();

		m_filter = _filter;
		m_hideDuplicates = _hideDuplicates;

		filterPatches();
		updateContent();

		setSelectedPatches(selected);

		repaint();

		getPatchManager().setListStatus(static_cast<uint32_t>(selected.size()), static_cast<uint32_t>(getPatches().size()));
	}

	void List::sortPatches(Patches& _patches, pluginLib::patchDB::SourceType _sourceType)
	{
		std::sort(_patches.begin(), _patches.end(), [_sourceType](const Patch& _a, const Patch& _b)
		{
			const auto sourceType = _sourceType;

			if(sourceType == pluginLib::patchDB::SourceType::Folder)
			{
				const auto aSource = _a->source.lock();
				const auto bSource = _b->source.lock();
				if (*aSource != *bSource)
					return *aSource < *bSource;
			}
			else if (sourceType == pluginLib::patchDB::SourceType::File || sourceType == pluginLib::patchDB::SourceType::Rom || sourceType == pluginLib::patchDB::SourceType::LocalStorage)
			{
				if (_a->program != _b->program)
					return _a->program < _b->program;
			}

			return _a->getName().compare(_b->getName()) < 0;
		});
	}

	void List::listBoxItemClicked(const int _row, const juce::MouseEvent& _mouseEvent)
	{
		if(!onClicked(_mouseEvent))
			ListBoxModel::listBoxItemClicked(_row, _mouseEvent);
	}

	void List::backgroundClicked(const juce::MouseEvent& _mouseEvent)
	{
		if(!onClicked(_mouseEvent))
			ListBoxModel::backgroundClicked(_mouseEvent);
	}

	bool List::showDeleteConfirmationMessageBox()
	{
		return 1 == juce::NativeMessageBox::showYesNoBox(juce::AlertWindow::WarningIcon, "Confirmation needed", "Delete selected patches from bank?");
	}

	pluginLib::patchDB::SourceType List::getSourceType() const
	{
		if(!m_search)
			return pluginLib::patchDB::SourceType::Invalid;
		return m_search->getSourceType();
	}

	bool List::canReorderPatches() const
	{
		if(!m_search)
			return false;
		if(getSourceType() != pluginLib::patchDB::SourceType::LocalStorage)
			return false;
		if(!m_search->request.tags.empty())
			return false;
		return true;
	}

	bool List::hasTagFilters() const
	{
		if(!m_search)
			return false;
		return !m_search->request.tags.empty();
	}

	bool List::hasFilters() const
	{
		return hasTagFilters() || !m_filter.empty();
	}

	pluginLib::patchDB::SearchHandle List::getSearchHandle() const
	{
		if(!m_search)
			return pluginLib::patchDB::g_invalidSearchHandle;
		return m_search->handle;
	}

	void List::sortPatches()
	{
		// Note: If this list is no longer sorted by calling this function, be sure to modify the second caller in state.cpp, too, as it is used to track the selected entry across multiple parts
		sortPatches(m_patches);
	}

	void List::sortPatches(Patches& _patches) const
	{
		sortPatches(_patches, getSourceType());
	}

	void List::filterPatches()
	{
		if (m_filter.empty() && !m_hideDuplicates)
		{
			m_filteredPatches.clear();
			return;
		}

		m_filteredPatches.reserve(m_patches.size());
		m_filteredPatches.clear();

		std::set<pluginLib::patchDB::PatchHash> knownPatches;

		for (const auto& patch : m_patches)
		{
			if(m_hideDuplicates)
			{
				if(knownPatches.find(patch->hash) != knownPatches.end())
					continue;
				knownPatches.insert(patch->hash);
			}

			if (m_filter.empty() || match(patch))
				m_filteredPatches.emplace_back(patch);
		}
	}

	bool List::match(const Patch& _patch) const
	{
		const auto name = _patch->getName();
		const auto t = Search::lowercase(name);
		return t.find(m_filter) != std::string::npos;
	}
}
