#include "db.h"

#include <cassert>

#include "datasource.h"
#include "patch.h"
#include "patchmodifications.h"

#include "../../synthLib/os.h"
#include "../../synthLib/midiToSysex.h"
#include "../../synthLib/hybridcontainer.h"

#include "dsp56kEmu/logging.h"

namespace pluginLib::patchDB
{
	namespace
	{
		std::string createValidFilename(const std::string& _name)
		{
			std::string result;
			result.reserve(_name.size());

			for (const char c : _name)
			{
				if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
					result += c;
				else
					result += '_';
			}
			return result;
		}
	}
	DB::DB(juce::File _dir)
	: m_settingsDir(std::move(_dir))
	, m_jsonFileName(m_settingsDir.getChildFile("patchmanagerdb.json"))
	, m_loader("PatchLoader", false, dsp56k::ThreadPriority::Lowest)
	{
	}

	DB::~DB()
	{
		assert(m_loader.destroyed() && "stopLoaderThread() needs to be called by derived class in destructor");
		stopLoaderThread();
	}

	DataSourceNodePtr DB::addDataSource(const DataSource& _ds)
	{
		return addDataSource(_ds, true);
	}

	bool DB::writePatchesToFile(const juce::File& _file, const std::vector<PatchPtr>& _patches)
	{
		std::vector<uint8_t> sysexBuffer;
		sysexBuffer.reserve(_patches.front()->sysex.size() * _patches.size());

		for (const auto& patch : _patches)
		{
			const auto patchSysex = patch->sysex;

			if(!patchSysex.empty())
				sysexBuffer.insert(sysexBuffer.end(), patchSysex.begin(), patchSysex.end());
		}

		return _file.replaceWithData(sysexBuffer.data(), sysexBuffer.size());
	}

	DataSourceNodePtr DB::addDataSource(const DataSource& _ds, const bool _save)
	{
		const auto needsSave = _save && _ds.origin == DataSourceOrigin::Manual && _ds.type != SourceType::Rom;

		auto ds = std::make_shared<DataSourceNode>(_ds);

		runOnLoaderThread([this, ds, needsSave]
			{
				addDataSource(ds);
				if(needsSave)
					saveJson();
			});

		return ds;
	}

	void DB::removeDataSource(const DataSource& _ds, bool _save/* = true*/)
	{
		runOnLoaderThread([this, _ds, _save]
		{
			std::unique_lock lockDs(m_dataSourcesMutex);

			const auto it = m_dataSources.find(_ds);
			if (it == m_dataSources.end())
				return;

			const auto ds = it->second;

			// if a DS is removed that is of type Manual, and it has a parent, switch it to Autogenerated but don't remove it
			if (ds->origin == DataSourceOrigin::Manual && ds->hasParent())
			{
				ds->origin = DataSourceOrigin::Autogenerated;

				std::unique_lock lockUi(m_uiMutex);
				m_dirty.dataSources = true;
				return;
			}

			std::set<DataSourceNodePtr> removedDataSources{it->second};
			std::vector<PatchPtr> removedPatches;

			// remove all datasources that are a child of the one being removed
			std::function<void(const DataSourceNodePtr&)> removeChildren = [&](const DataSourceNodePtr& _parent)
			{
				for (auto& child : _parent->getChildren())
				{
					const auto c = child.lock();

					if (!c || c->origin == DataSourceOrigin::Manual)
						continue;

					removedDataSources.insert(c);
					removeChildren(c);
				}
			};

			removeChildren(ds);

			for (const auto& removed : removedDataSources)
			{
				removedPatches.insert(removedPatches.end(), removed->patches.begin(), removed->patches.end());
				m_dataSources.erase(*removed);
			}

			lockDs.unlock();

			const auto patchesChanged = !removedPatches.empty();

			removePatchesFromSearches(removedPatches);

			{
				std::unique_lock lockUi(m_uiMutex);

				m_dirty.dataSources = true;
				if (patchesChanged)
					m_dirty.patches = true;
			}

			for (auto& removedDataSource : removedDataSources)
			{
				removedDataSource->setParent(nullptr);
				removedDataSource->removeAllChildren();
				removedDataSource->patches.clear();
			}
			removedDataSources.clear();

			if(_save)
				saveJson();
		});
	}

	void DB::refreshDataSource(const DataSourceNodePtr& _ds)
	{
		auto parent = _ds->getParent();

		removeDataSource(*_ds, false);

		runOnLoaderThread([this, parent, _ds]
		{
			_ds->setParent(parent);
			addDataSource(_ds);
		});
	}

	void DB::renameDataSource(const DataSourceNodePtr& _ds, const std::string& _newName)
	{
		if(_ds->type != SourceType::LocalStorage)
			return;

		if(_newName.empty())
			return;

		runOnLoaderThread([this, _ds, _newName]
		{
			{
				std::unique_lock lockDs(m_dataSourcesMutex);
				const auto it = m_dataSources.find(*_ds);

				if(it == m_dataSources.end())
					return;

				const auto ds = it->second;

				if(ds->name == _newName)
					return;

				for (const auto& [_, d] : m_dataSources)
				{
					if(d->type == SourceType::LocalStorage && d->name == _newName)
						return;
				}

				ds->name = _newName;

				m_dataSources.erase(it);
				m_dataSources.insert({*ds, ds});
			}

			std::unique_lock lockUi(m_uiMutex);
			m_dirty.dataSources = true;

			saveJson();
		});
	}

	bool DB::setTagColor(const TagType _type, const Tag& _tag, const Color _color)
	{
		std::shared_lock lock(m_patchesMutex);
		if(_color == g_invalidColor)
		{
			const auto itType = m_tagColors.find(_type);

			if(itType == m_tagColors.end())
				return false;

			if(!itType->second.erase(_tag))
				return false;
		}
		else
		{
			if(m_tagColors[_type][_tag] == _color)
				return false;
			m_tagColors[_type][_tag] = _color;
		}

		std::unique_lock lockUi(m_uiMutex);
		m_dirty.tags.insert(_type);

		// TODO: this might spam saving if this function is called too often
		runOnLoaderThread([this]
		{
			saveJson();
		});
		return true;
	}

	Color DB::getTagColor(const TagType _type, const Tag& _tag) const
	{
		std::shared_lock lock(m_patchesMutex);
		return getTagColorInternal(_type, _tag);
	}

	Color DB::getPatchColor(const PatchPtr& _patch, const TypedTags& _tagsToIgnore) const
	{
		const auto& tags = _patch->getTags();

		for (const auto& itType : tags.get())
		{
			for (const auto& tag : itType.second.getAdded())
			{
				if(_tagsToIgnore.containsAdded(itType.first, tag))
					continue;

				const auto c = getTagColor(itType.first, tag);
				if(c != g_invalidColor)
					return c;
			}
		}

		return g_invalidColor;
	}

	bool DB::addTag(const TagType _type, const std::string& _tag)
	{
		{
			std::unique_lock lock(m_patchesMutex);
			if (!internalAddTag(_type, _tag))
				return false;
		}
		saveJson();
		return true;
	}

	bool DB::removeTag(TagType _type, const Tag& _tag)
	{
		{
			std::unique_lock lock(m_patchesMutex);
			if (!internalRemoveTag(_type, _tag))
				return false;
		}
		saveJson();
		return true;
	}

	void DB::uiProcess(Dirty& _dirty)
	{
		std::list<std::function<void()>> uiFuncs;
		{
			std::scoped_lock lock(m_uiMutex);
			std::swap(uiFuncs, m_uiFuncs);
			_dirty = m_dirty;
			m_dirty = {};
		}

		for (const auto& func : uiFuncs)
			func();
	}

	uint32_t DB::search(SearchRequest&& _request, SearchCallback&& _callback)
	{
		const auto handle = m_nextSearchHandle++;

		auto s = std::make_shared<Search>();

		s->handle = handle;
		s->request = std::move(_request);
		s->callback = std::move(_callback);

		{
			std::unique_lock lock(m_searchesMutex);
			m_searches.insert({ s->handle, s });
		}

		runOnLoaderThread([this, s]
		{
			executeSearch(*s);
		});

		return handle;
	}

	SearchHandle DB::findDatasourceForPatch(const PatchPtr& _patch, SearchCallback&& _callback)
	{
		SearchRequest req;
		req.patch = _patch;
		return search(std::move(req), std::move(_callback));
	}

	void DB::cancelSearch(const uint32_t _handle)
	{
		std::unique_lock lock(m_searchesMutex);
		m_cancelledSearches.insert(_handle);
		m_searches.erase(_handle);
	}

	std::shared_ptr<Search> DB::getSearch(const SearchHandle _handle)
	{
		std::shared_lock lock(m_searchesMutex);
		const auto it = m_searches.find(_handle);
		if (it == m_searches.end())
			return {};
		return it->second;
	}

	std::shared_ptr<Search> DB::getSearch(const DataSource& _dataSource)
	{
		std::shared_lock lock(m_searchesMutex);

		for (const auto& it : m_searches)
		{
			const auto& search = it.second;
			if(!search->request.sourceNode)
				continue;
			if(*search->request.sourceNode == _dataSource)
				return search;
		}
		return nullptr;
	}

	void DB::copyPatchesTo(const DataSourceNodePtr& _ds, const std::vector<PatchPtr>& _patches, int _insertRow/* = -1*/)
	{
		if (_ds->type != SourceType::LocalStorage)
			return;

		runOnLoaderThread([this, _ds, _patches, _insertRow]
		{
			{
				std::shared_lock lockDs(m_dataSourcesMutex);
				const auto itDs = m_dataSources.find(*_ds);
				if (itDs == m_dataSources.end())
					return;
			}

			// filter out all patches that are already part of _ds
			std::vector<PatchPtr> patchesToAdd;
			patchesToAdd.reserve(_patches.size());

			for (const auto& patch : _patches)
			{
				if (_ds->contains(patch))
					continue;

				patchesToAdd.push_back(patch);
			}

			if(patchesToAdd.empty())
				return;

			std::vector<PatchPtr> newPatches;
			newPatches.reserve(patchesToAdd.size());

			uint32_t newPatchProgramNumber = _insertRow >= 0 ? static_cast<uint32_t>(_insertRow) : _ds->getMaxProgramNumber() + 1;

			if(newPatchProgramNumber > _ds->getMaxProgramNumber() + 1)
				newPatchProgramNumber = _ds->getMaxProgramNumber() + 1;

			_ds->makeSpaceForNewPatches(newPatchProgramNumber, static_cast<uint32_t>(patchesToAdd.size()));

			for (const auto& patch : patchesToAdd)
			{
				auto [newPatch, newMods] = patch->createCopy(_ds);

				newPatch->program = newPatchProgramNumber++;

				newPatches.push_back(newPatch);
			}

			addPatches(newPatches);

			createConsecutiveProgramNumbers(_ds);

			saveJson();
		});
	}

	void DB::removePatches(const DataSourceNodePtr& _ds, const std::vector<PatchPtr>& _patches)
	{
		if (_ds->type != SourceType::LocalStorage)
			return;

		runOnLoaderThread([this, _ds, _patches]
		{
			{
				std::shared_lock lockDs(m_dataSourcesMutex);
				const auto itDs = m_dataSources.find(*_ds);
				if (itDs == m_dataSources.end())
					return;
			}

			{
				std::vector<PatchPtr> removedPatches;
				removedPatches.reserve(_patches.size());

				std::unique_lock lock(m_patchesMutex);

				for (const auto& patch : _patches)
				{
					if(_ds->patches.erase(patch))
						removedPatches.emplace_back(patch);
				}

				if (removedPatches.empty())
					return;

				removePatchesFromSearches(removedPatches);

				{
					std::unique_lock lockUi(m_uiMutex);
					m_dirty.patches = true;
				}
			}

			saveJson();
		});
	}

	bool DB::movePatchesTo(const uint32_t _position, const std::vector<PatchPtr>& _patches)
	{
		if(_patches.empty())
			return false;

		{
			std::unique_lock lock(m_patchesMutex);

			const auto ds = _patches.front()->source.lock();

			if(!ds || ds->type != SourceType::LocalStorage)
				return false;

			if(!ds->movePatchesTo(_position, _patches))
				return false;
		}

		{
			std::unique_lock lockUi(m_uiMutex);
			m_dirty.dataSources = true;
		}

		runOnLoaderThread([this]
		{
			saveJson();
		});

		return true;
	}

	bool DB::isValid(const PatchPtr& _patch)
	{
		if (!_patch)
			return false;
		if (_patch->getName().empty())
			return false;
		if (_patch->sysex.empty())
			return false;
		if (_patch->sysex.front() != 0xf0)
			return false;
		if (_patch->sysex.back() != 0xf7)
			return false;
		return true;
	}

	PatchPtr DB::requestPatchForPart(const uint32_t _part)
	{
		Data data;
		requestPatchForPart(data, _part);
		return initializePatch(std::move(data));
	}

	void DB::getTags(const TagType _type, std::set<Tag>& _tags)
	{
		_tags.clear();

		std::shared_lock lock(m_patchesMutex);
		const auto it = m_tags.find(_type);
		if (it == m_tags.end())
			return;

		_tags = it->second;
	}

	bool DB::modifyTags(const std::vector<PatchPtr>& _patches, const TypedTags& _tags)
	{
		if(_tags.empty())
			return false;

		std::vector<PatchPtr> changed;
		changed.reserve(_patches.size());

		std::unique_lock lock(m_patchesMutex);

		for (const auto& patch : _patches)
		{
			if(patch->source.expired())
				continue;

			const auto key = PatchKey(*patch);

			auto mods = patch->modifications;

			if(!mods)
			{
				mods = std::make_shared<PatchModifications>();
				mods->patch = patch;
				patch->modifications = mods;
			}

			if (!mods->modifyTags(_tags))
				continue;

			changed.push_back(patch);
		}

		if(!changed.empty())
		{
			updateSearches(changed);
		}

		lock.unlock();

		if(!changed.empty())
			saveJson();

		return true;
	}

	bool DB::renamePatch(const PatchPtr& _patch, const std::string& _name)
	{
		if(_patch->getName() == _name)
			return false;

		if(_name.empty())
			return false;

		{
			std::unique_lock lock(m_patchesMutex);

			const auto ds = _patch->source.lock();
			if(!ds)
				return false;

			auto mods = _patch->modifications;
			if(!mods)
			{
				mods = std::make_shared<PatchModifications>();
				mods->patch = _patch;
				_patch->modifications = mods;
			}

			mods->name = _name;

			mods->updateCache();

			updateSearches({_patch});
		}

		runOnLoaderThread([this]
		{
			saveJson();
		});

		return true;
	}

	bool DB::replacePatch(const PatchPtr& _existing, const PatchPtr& _new)
	{
		if(!_existing || !_new)
			return false;

		if(_existing == _new)
			return false;

		const auto ds = _existing->source.lock();

		if(!ds || ds->type != SourceType::LocalStorage)
			return false;

		std::unique_lock lock(m_patchesMutex);

		_existing->replaceData(*_new);

		if(_existing->modifications)
			_existing->modifications->name.clear();

		updateSearches({_existing});

		runOnLoaderThread([this]
		{
			saveJson();
		});

		return true;
	}

	SearchHandle DB::search(SearchRequest&& _request)
	{
		return search(std::move(_request), [](const Search&) {});
	}

	bool DB::loadData(DataList& _results, const DataSourceNodePtr& _ds)
	{
		return loadData(_results, *_ds);
	}

	bool DB::loadData(DataList& _results, const DataSource& _ds)
	{
		switch (_ds.type)
		{
		case SourceType::Rom:
			return loadRomData(_results, _ds.bank, g_invalidProgram);
		case SourceType::File:
			return loadFile(_results, _ds.name);
		case SourceType::Invalid:
		case SourceType::Folder:
		case SourceType::Count:
			return false;
		case SourceType::LocalStorage:
			return loadLocalStorage(_results, _ds);
		}
		return false;
	}

	bool DB::loadFile(DataList& _results, const std::string& _file)
	{
		const auto size = synthLib::getFileSize(_file);

		// unlikely that a 4mb file has useful data for us, skip
		if (!size || size >= static_cast<size_t>(4 * 1024 * 1024))
			return false;

		Data data;
		if (!synthLib::readFile(data, _file) || data.empty())
			return false;

		return parseFileData(_results, data);
	}

	bool DB::loadLocalStorage(DataList& _results, const DataSource& _ds)
	{
		const auto file = getLocalStorageFile(_ds);

		std::vector<uint8_t> data;
		if (!synthLib::readFile(data, file.getFullPathName().toStdString()))
			return false;

		synthLib::MidiToSysex::splitMultipleSysex(_results, data);
		return !_results.empty();
	}

	bool DB::loadFolder(const DataSourceNodePtr& _folder)
	{
		assert(_folder->type == SourceType::Folder);

		std::vector<std::string> files;
		synthLib::findFiles(files, _folder->name, {}, 0, 0);

		for (const auto& file : files)
		{
			const auto child = std::make_shared<DataSourceNode>();
			child->setParent(_folder);
			child->name = file;
			child->origin = DataSourceOrigin::Autogenerated;

			if(synthLib::isDirectory(file))
				child->type = SourceType::Folder;
			else
				child->type = SourceType::File;

			addDataSource(child);
		}

		return !files.empty();
	}

	bool DB::parseFileData(DataList& _results, const Data& _data)
	{
		return synthLib::MidiToSysex::extractSysexFromData(_results, _data);
	}

	void DB::startLoaderThread()
	{
		m_loader.start();

		runOnLoaderThread([this]
		{
			loadJson();
		});
	}

	void DB::stopLoaderThread()
	{
		m_loader.destroy();
	}

	void DB::runOnLoaderThread(std::function<void()>&& _func)
	{
		m_loader.add([this, f = std::move(_func)]
		{
			f();

			if(isLoading() && !m_loader.pending())
			{
				runOnUiThread([this]
				{
					m_loading = false;
					onLoadFinished();
				});
			}
		});
	}

	void DB::runOnUiThread(const std::function<void()>& _func)
	{
		m_uiFuncs.push_back(_func);
	}

	void DB::addDataSource(const DataSourceNodePtr& _ds)
	{
		if (m_loader.destroyed())
			return;

		auto ds = _ds;

		bool dsExists;

		{
			std::unique_lock lockDs(m_dataSourcesMutex);

			const auto itExisting = m_dataSources.find(*ds);

			dsExists = itExisting != m_dataSources.end();

			if (dsExists)
			{
				// two things can happen here:
				// * a child DS already exists and the one being added has a parent that was previously unknown to the existing DS
				// * a DS is added again (for example manually requested by a user) even though it already exists because of a parent DS added earlier

				ds = itExisting->second;

				if(_ds->origin == DataSourceOrigin::Manual)
				{
					// user manually added a DS that already exists as a child
					assert(!_ds->hasParent());
					ds->origin = _ds->origin;
				}
				else if(_ds->hasParent() && !ds->hasParent())
				{
					// a parent datasource is added and this DS previously didn't have a parent
					ds->setParent(_ds->getParent());
				}
				else
				{
					// nothing to be done
					assert(_ds->getParent().get() == ds->getParent().get());
					return;
				}

				std::unique_lock lockUi(m_uiMutex);
				m_dirty.dataSources = true;
			}
		}

		auto addDsToList = [&]
		{
			if (dsExists)
				return;

			std::unique_lock lockDs(m_dataSourcesMutex);

			m_dataSources.insert({ *ds, ds });
			std::unique_lock lockUi(m_uiMutex);
			m_dirty.dataSources = true;

			dsExists = true;
		};

		if (ds->type == SourceType::Folder)
		{
			addDsToList();
			loadFolder(ds);
			return;
		}

		// always add DS if manually requested by user
		if (ds->origin == DataSourceOrigin::Manual)
			addDsToList();

		std::vector<std::vector<uint8_t>> data;

		if(loadData(data, ds) && !data.empty())
		{
			std::vector<PatchPtr> patches;
			patches.reserve(data.size());

			for (uint32_t p = 0; p < data.size(); ++p)
			{
				if (const auto patch = initializePatch(std::move(data[p])))
				{
					patch->source = ds->weak_from_this();

					if(isValid(patch))
					{
						patch->program = p;
						patches.push_back(patch);
						ds->patches.insert(patch);
					}
				}
			}

			if (!patches.empty())
			{
				addDsToList();
				loadPatchModifications(ds, patches);
				addPatches(patches);
			}
		}
	}

	bool DB::addPatches(const std::vector<PatchPtr>& _patches)
	{
		if (_patches.empty())
			return false;

		std::unique_lock lock(m_patchesMutex);

		for (const auto& patch : _patches)
		{
			const auto key = PatchKey(*patch);

			// find modification and apply it to the patch
			const auto itMod = m_patchModifications.find(key);
			if (itMod != m_patchModifications.end())
			{
				patch->modifications = itMod->second;

				m_patchModifications.erase(itMod);

				patch->modifications->patch = patch;
				patch->modifications->updateCache();
			}

			// add to all known categories, tags, etc
			for (const auto& it : patch->getTags().get())
			{
				const auto type = it.first;
				const auto& tags = it.second;

				for (const auto& tag : tags.getAdded())
					internalAddTag(type, tag);
			}
		}

		// add to ongoing searches
		updateSearches(_patches);

		return true;
	}

	bool DB::removePatch(const PatchPtr& _patch)
	{
		std::unique_lock lock(m_patchesMutex);

		const auto itDs = m_dataSources.find(*_patch->source.lock());

		if(itDs == m_dataSources.end())
			return false;

		const auto& ds = itDs->second;
		auto& patches = ds->patches;

		const auto it = patches.find(_patch);
		if (it == patches.end())
			return false;

		auto mods = _patch->modifications;

		if(mods && !mods->empty())
		{
			mods->patch.reset();
			m_patchModifications.insert({PatchKey(*_patch), mods});
		}

		patches.erase(it);

		removePatchesFromSearches({ _patch });

		std::unique_lock lockUi(m_uiMutex);
		m_dirty.patches = true;

		return true;
	}

	bool DB::internalAddTag(TagType _type, const Tag& _tag)
	{
		const auto itType = m_tags.find(_type);

		if (itType == m_tags.end())
		{
			m_tags.insert({ _type, {_tag} });

			std::unique_lock lockUi(m_uiMutex);
			m_dirty.tags.insert(_type);
			return true;
		}

		auto& tags = itType->second;

		if (tags.find(_tag) != tags.end())
			return false;

		tags.insert(_tag);
		std::unique_lock lockUi(m_uiMutex);
		m_dirty.tags.insert(_type);

		return true;
	}

	bool DB::internalRemoveTag(const TagType _type, const Tag& _tag)
	{
		const auto& itType = m_tags.find(_type);

		if (itType == m_tags.end())
			return false;

		auto& tags = itType->second;
		const auto itTag = tags.find(_tag);

		if (itTag == tags.end())
			return false;

		tags.erase(itTag);

		std::unique_lock lockUi(m_uiMutex);
		m_dirty.tags.insert(_type);

		return true;
	}

	bool DB::executeSearch(Search& _search)
	{
		_search.state = SearchState::Running;

		const auto reqPatch = _search.request.patch;
		if(reqPatch)
		{
			// we're searching by patch content to find patches within datasources
			SearchResult results;

			std::shared_lock lockDs(m_dataSourcesMutex);

			for (const auto& [_, ds] : m_dataSources)
			{
				for (const auto& patch : ds->patches)
				{
					if(patch->hash == reqPatch->hash)
						results.insert(patch);
					else if(patch->sysex.size() == reqPatch->sysex.size() && patch->getName() == reqPatch->getName())
					{
						// if patches are not 100% identical, they might still be the same patch as unknown/unused data in dumps might have different values
						if(equals(patch, reqPatch))
							results.insert(patch);
					}
				}
			}

			if(!results.empty())
			{
				std::unique_lock searchLock(_search.resultsMutex);
				std::swap(_search.results, results);
			}

			_search.setCompleted();

			std::unique_lock lockUi(m_uiMutex);
			m_dirty.searches.insert(_search.handle);
			return true;
		}

		auto searchInDs = [&](const DataSourceNodePtr& _ds)
		{
			if(!_search.request.sourceNode && _search.getSourceType() != SourceType::Invalid)
			{
				if(_ds->type != _search.request.sourceType)
					return true;
			}

			bool isCancelled;
			{
				std::shared_lock lockSearches(m_searchesMutex);
				const auto it = m_cancelledSearches.find(_search.handle);
				isCancelled = it != m_cancelledSearches.end();
				if(isCancelled)
					m_cancelledSearches.erase(it);
			}

			if(isCancelled)
			{
				_search.state = SearchState::Cancelled;
				std::unique_lock lockUi(m_uiMutex);
				m_dirty.searches.insert(_search.handle);
				return false;
			}

			for (const auto& patchPtr : _ds->patches)
			{
				const auto* patch = patchPtr.get();
				assert(patch);

				if(_search.request.match(*patch))
				{
					std::unique_lock searchLock(_search.resultsMutex);
					_search.results.insert(patchPtr);
				}
			}
			return true;
		};

		if(_search.request.sourceNode && (_search.getSourceType() == SourceType::File || _search.getSourceType() == SourceType::LocalStorage))
		{
			const auto& it = m_dataSources.find(*_search.request.sourceNode);

			if(it == m_dataSources.end())
			{
				_search.setCompleted();
				return false;
			}

			if(!searchInDs(it->second))
				return false;
		}
		else
		{
			for (const auto& it : m_dataSources)
			{
				if(!searchInDs(it.second))
					return false;
			}
		}

		_search.setCompleted();

		{
			std::unique_lock lockUi(m_uiMutex);
			m_dirty.searches.insert(_search.handle);
		}

		return true;
	}

	void DB::updateSearches(const std::vector<PatchPtr>& _patches)
	{
		std::shared_lock lockSearches(m_searchesMutex);

		std::set<SearchHandle> dirtySearches;

		for (const auto& it : m_searches)
		{
			const auto handle = it.first;
			auto& search = it.second;

			bool searchDirty = false;

			for (const auto& patch : _patches)
			{
				const auto match = search->request.match(*patch);

				bool countChanged;

				{
					std::unique_lock lock(search->resultsMutex);
					const auto oldCount = search->results.size();

					if (match)
						search->results.insert(patch);
					else
						search->results.erase(patch);

					const auto newCount = search->results.size();
					countChanged = newCount != oldCount;
				}

				if (countChanged)
					searchDirty = true;
			}
			if (searchDirty)
				dirtySearches.insert(handle);
		}

		if (dirtySearches.empty())
			return;

		std::unique_lock lockUi(m_uiMutex);

		for (SearchHandle dirtySearch : dirtySearches)
			m_dirty.searches.insert(dirtySearch);
	}

	bool DB::removePatchesFromSearches(const std::vector<PatchPtr>& _keys)
	{
		bool res = false;

		std::shared_lock lockSearches(m_searchesMutex);

		for (auto& itSearches : m_searches)
		{
			const auto& search = itSearches.second;

			bool countChanged;
			{
				std::unique_lock lockResults(search->resultsMutex);
				const auto oldCount = search->results.size();

				for (const auto& key : _keys)
					search->results.erase(key);

				const auto newCount = search->results.size();
				countChanged = newCount != oldCount;
			}

			if (countChanged)
			{
				res = true;
				std::unique_lock lockUi(m_uiMutex);
				m_dirty.searches.insert(itSearches.first);
			}
		}
		return res;
	}

	bool DB::createConsecutiveProgramNumbers(const DataSourceNodePtr& _ds)
	{
		std::unique_lock lockPatches(m_patchesMutex);
		return _ds->createConsecutiveProgramNumbers();
	}

	Color DB::getTagColorInternal(const TagType _type, const Tag& _tag) const
	{
		const auto itType = m_tagColors.find(_type);
		if(itType == m_tagColors.end())
			return 0;
		const auto itTag = itType->second.find(_tag);
		if(itTag == itType->second.end())
			return 0;
		return itTag->second;
	}

	bool DB::loadJson()
	{
		bool success = true;

		const auto json = juce::JSON::parse(m_jsonFileName);
		const auto* datasources = json["datasources"].getArray();

		if(datasources)
		{
			for(int i=0; i<datasources->size(); ++i)
			{
				const auto var = datasources->getUnchecked(i);

				DataSource ds;

				ds.type = toSourceType(var["type"].toString().toStdString());
				ds.name = var["name"].toString().toStdString();
				ds.origin = DataSourceOrigin::Manual;

				if (ds.type != SourceType::Invalid && !ds.name.empty())
				{
					addDataSource(ds, false);
				}
				else
				{
					LOG("Unexpected data source type " << toString(ds.type) << " with name '" << ds.name << "'");
					success = false;
				}
			}
		}

		{
			std::unique_lock lockPatches(m_patchesMutex);

			if(auto* tags = json["tags"].getDynamicObject())
			{
				const auto& props = tags->getProperties();
				for (const auto& it : props)
				{
					const auto strType = it.name.toString().toStdString();
					const auto type = toTagType(strType);

					const auto* tagsArray = it.value.getArray();
					if(tagsArray)
					{
						std::set<Tag> newTags;
						for(int i=0; i<tagsArray->size(); ++i)
						{
							const auto tag = tagsArray->getUnchecked(i).toString().toStdString();
							newTags.insert(tag);
						}
						m_tags.insert({ type, newTags });
						m_dirty.tags.insert(type);
					}
					else
					{
						LOG("Unexpected empty tags for tag type " << strType);
						success = false;
					}
				}
			}

			if(auto* tagColors = json["tagColors"].getDynamicObject())
			{
				const auto& props = tagColors->getProperties();

				for (const auto& it : props)
				{
					const auto strType = it.name.toString().toStdString();
					const auto type = toTagType(strType);

					auto* colors = it.value.getDynamicObject();
					if(colors)
					{
						std::unordered_map<Tag, Color> newTags;
						for (auto itCol : colors->getProperties())
						{
							const auto tag = itCol.name.toString().toStdString();
							const auto col = static_cast<juce::int64>(itCol.value);
							if(!tag.empty() && col != g_invalidColor && col >= std::numeric_limits<Color>::min() && col <= std::numeric_limits<Color>::max())
								newTags[tag] = static_cast<Color>(col);
						}
						m_tagColors[type] = newTags;
						m_dirty.tags.insert(type);
					}
					else
					{
						LOG("Unexpected empty tags for tag type " << strType);
						success = false;
					}
				}
			}

			if(!loadPatchModifications(m_patchModifications, json))
				success = false;
		}

		return success;
	}

	bool DB::loadPatchModifications(const DataSourceNodePtr& _ds, const std::vector<PatchPtr>& _patches)
	{
		if(_patches.empty())
			return true;

		const auto file = getJsonFile(*_ds);
		if(file.getFileName().isEmpty())
			return false;

		if(!file.exists())
			return true;

		const auto json = juce::JSON::parse(file);

		std::map<PatchKey, PatchModificationsPtr> patchModifications;

		if(!loadPatchModifications(patchModifications, json, _ds))
			return false;

		// apply modifications to patches
		for (const auto& patch : _patches)
		{
			const auto key = PatchKey(*patch);
			const auto it = patchModifications.find(key);
			if(it != patchModifications.end())
			{
				patch->modifications = it->second;
				patch->modifications->patch = patch;
				patch->modifications->updateCache();

				patchModifications.erase(it);

				if(patchModifications.empty())
					break;
			}
		}

		if(!patchModifications.empty())
		{
			// any patch modification that we couldn't apply to a patch is added to the global modifications
			for (const auto& patchModification : patchModifications)
				m_patchModifications.insert(patchModification);
		}

		return true;
	}

	bool DB::loadPatchModifications(std::map<PatchKey, PatchModificationsPtr>& _patchModifications, const juce::var& _parentNode, const DataSourceNodePtr& _dataSource/* = nullptr*/)
	{
		auto* patches = _parentNode["patches"].getDynamicObject();
		if(!patches)
			return true;

		bool success = true;

		const auto& props = patches->getProperties();
		for (const auto& it : props)
		{
			const auto strKey = it.name.toString().toStdString();
			const auto var = it.value;

			auto key = PatchKey::fromString(strKey, _dataSource);

			auto mods = std::make_shared<PatchModifications>();

			if (!mods->deserialize(var))
			{
				LOG("Failed to parse patch modifications for key " << strKey);
				success = false;
				continue;
			}

			if(!key.isValid())
			{
				LOG("Failed to parse patch key from string " << strKey);
				success = false;
			}

			_patchModifications.insert({ key, mods });
		}

		return success;
	}

	bool DB::saveJson()
	{
		if (!m_jsonFileName.hasWriteAccess())
		{
			pushError("No write access to file:\n" + m_jsonFileName.getFullPathName().toStdString());
			return false;
		}

		auto* json = new juce::DynamicObject();

		{
			std::shared_lock lockDs(m_dataSourcesMutex);
			std::shared_lock lockP(m_patchesMutex);

			auto patchModifications = m_patchModifications;

			juce::Array<juce::var> dss;

			for (const auto& it : m_dataSources)
			{
				const auto& dataSource = it.second;

				// if we cannot save patch modifications to a separate file, add them to the global file
				if(!saveJson(dataSource))
				{
					for (const auto& patch : dataSource->patches)
					{
						if(!patch->modifications || patch->modifications->empty())
							continue;

						patchModifications.insert({PatchKey(*patch), patch->modifications});
					}
				}
				if (dataSource->origin != DataSourceOrigin::Manual)
					continue;

				if (dataSource->type == SourceType::Rom)
					continue;

				auto* o = new juce::DynamicObject();

				o->setProperty("type", juce::String(toString(dataSource->type)));
				o->setProperty("name", juce::String(dataSource->name));

				dss.add(o);
			}
			json->setProperty("datasources", dss);

			saveLocalStorage();

			auto* tagTypes = new juce::DynamicObject();

			for (const auto& it : m_tags)
			{
				const auto type = it.first;
				const auto& tags = it.second;

				if(tags.empty())
					continue;

				juce::Array<juce::var> tagsArray;
				for (const auto& tag : tags)
					tagsArray.add(juce::String(tag));

				tagTypes->setProperty(juce::String(toString(type)), tagsArray);
			}

			json->setProperty("tags", tagTypes);
			
			auto* tagColors = new juce::DynamicObject();

			for (const auto& it : m_tagColors)
			{
				const auto type = it.first;
				const auto& tags = it.second;

				if(tags.empty())
					continue;

				auto* colors = new juce::DynamicObject();
				for (const auto& [tag, col] : tags)
					colors->setProperty(juce::String(tag), static_cast<juce::int64>(col));

				tagColors->setProperty(juce::String(toString(type)), colors);
			}

			json->setProperty("tagColors", tagColors);

			auto* patchMods = new juce::DynamicObject();

			for (const auto& it : patchModifications)
			{
				const auto& key = it.first;
				const auto& mods = it.second;

				if (mods->empty())
					continue;

				auto* obj = mods->serialize();

				patchMods->setProperty(juce::String(key.toString()), obj);
			}

			json->setProperty("patches", patchMods);
		}

		return saveJson(m_jsonFileName, json);
	}

	juce::File DB::getJsonFile(const DataSource& _ds) const
	{
		if(_ds.type == SourceType::LocalStorage)
			return {getLocalStorageFile(_ds).getFullPathName() + ".json"};
		if(_ds.type == SourceType::File)
			return {_ds.name + ".json"};
		return {};
	}

	bool DB::saveJson(const DataSourceNodePtr& _ds)
	{
		if(!_ds)
			return false;

		auto filename = getJsonFile(*_ds);

		if(filename.getFileName().isEmpty())
			return _ds->patches.empty();

		if(!juce::File::isAbsolutePath(filename.getFullPathName()))
			filename = m_settingsDir.getChildFile(filename.getFullPathName());

		if(!filename.hasWriteAccess())
		{
			pushError("No write access to file:\n" + filename.getFullPathName().toStdString());
			return false;
		}

		if(_ds->patches.empty())
		{
			filename.deleteFile();
			return true;
		}

		juce::DynamicObject* patchMods = nullptr;

		for (const auto& patch : _ds->patches)
		{
			const auto mods = patch->modifications;

			if(!mods || mods->empty())
				continue;

			auto* obj = mods->serialize();

			if(!patchMods)
				patchMods = new juce::DynamicObject();

			const auto key = PatchKey(*patch);

			patchMods->setProperty(juce::String(key.toString(false)), obj);
		}

		if(!patchMods)
		{
			filename.deleteFile();
			return true;
		}

		auto* json = new juce::DynamicObject();

		json->setProperty("patches", patchMods);

		return saveJson(filename, json);
	}

	bool DB::saveJson(const juce::File& _target, juce::DynamicObject* _src)
	{
		if (!_target.hasWriteAccess())
		{
			pushError("No write access to file:\n" + _target.getFullPathName().toStdString());
			return false;
		}
		const auto tempFile = juce::File(_target.getFullPathName() + "_tmp.json");
		if (!tempFile.hasWriteAccess())
		{
			pushError("No write access to file:\n" + tempFile.getFullPathName().toStdString());
			return false;
		}
		const auto jsonText = juce::JSON::toString(juce::var(_src), false);
		if (!tempFile.replaceWithText(jsonText))
		{
			pushError("Failed to write data to file:\n" + tempFile.getFullPathName().toStdString());
			return false;
		}
		if (!tempFile.copyFileTo(_target))
		{
			pushError("Failed to copy\n" + tempFile.getFullPathName().toStdString() + "\nto\n" + _target.getFullPathName().toStdString());
			return false;
		}
		tempFile.deleteFile();
		return true;
	}

	juce::File DB::getLocalStorageFile(const DataSource& _ds) const
	{
		const auto filename = createValidFilename(_ds.name);

		return m_settingsDir.getChildFile(filename + ".syx");
	}

	bool DB::saveLocalStorage() const
	{
		std::map<DataSourceNodePtr, std::set<PatchPtr>> localStoragePatches;

		for (const auto& it : m_dataSources)
		{
			const auto& ds = it.second;

			if (ds->type == SourceType::LocalStorage)
				localStoragePatches.insert({ds, ds->patches});
		}

		if (localStoragePatches.empty())
			return false;

		std::vector<PatchPtr> patchesVec;
		patchesVec.reserve(128);

		bool res = true;

		for (const auto& it : localStoragePatches)
		{
			const auto& ds = it.first;
			const auto& patches = it.second;

			const auto file = getLocalStorageFile(*ds);

			if(patches.empty())
			{
				file.deleteFile();
			}
			else
			{
				patchesVec.assign(patches.begin(), patches.end());
				DataSource::sortByProgram(patchesVec);
				if(!writePatchesToFile(file, patchesVec))
					res = false;
			}
		}
		return res;
	}

	void DB::pushError(std::string _string)
	{
		std::unique_lock lockUi(m_uiMutex);
		m_dirty.errors.emplace_back(std::move(_string));
	}
}
