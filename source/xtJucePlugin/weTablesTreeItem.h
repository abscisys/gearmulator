#pragma once

#include "weTreeItem.h"

#include "jucePluginLib/event.h"

#include "xtLib/xtId.h"

namespace xtJucePlugin
{
	class WaveEditor;

	class TablesTreeItem : public TreeItem
	{
	public:
		TablesTreeItem(WaveEditor& _editor, xt::TableId _tableIndex);

		bool mightContainSubItems() override { return false; }

		void itemSelectionChanged(bool _isNowSelected) override;

	private:
		void onTableChanged(xt::TableId _index);
		void onTableChanged();

		WaveEditor& m_editor;
		const xt::TableId m_index;
		pluginLib::EventListener<xt::TableId> m_onTableChanged;
	};
}
