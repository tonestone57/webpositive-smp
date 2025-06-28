/*
 * Copyright 2014, Adrien Destugues <pulkomandy@pulkomandy.tk>.
 * Distributed under the terms of the MIT License.
 */
#ifndef BOOKMARK_BAR_H
#define BOOKMARK_BAR_H


#include <map>

#include <MenuBar.h>
#include <Node.h>
#include <NodeMonitor.h>
#include <PopUpMenu.h>
#include <Size.h>


class BEntry;

namespace BPrivate {
	class IconMenuItem;
}


class BookmarkBar: public BMenuBar {
public:
									BookmarkBar(const char* title,
										BHandler* target,
										const entry_ref* navDir);
									~BookmarkBar();

	void							AttachedToWindow();
	void							MessageReceived(BMessage* message);

	void							FrameResized(float width, float height);
	BSize							MinSize();

	void 							MouseDown(BPoint where);
private:
	void							_AddItem(ino_t inode, const entry_ref& ref); // Modified to take entry_ref

private:
	static int32					_LoadBookmarksThreadEntry(void* data);

private:
	node_ref						fNodeRef;
	std::map<ino_t, BPrivate::IconMenuItem*>	fItemsMap;
	BMenu*							fOverflowMenu;
	// True if fOverflowMenu is currently added to BookmarkBar
	bool							fOverflowMenuAdded;
	BPopUpMenu*						fPopUpMenu;
	int32 							fSelectedItemIndex;
	thread_id						fLoadThreadId;

public:
	static const uint32				MSG_ADD_BOOKMARK_ITEMS = 'AbMi';
	static const uint32				MSG_BOOKMARKS_LOADED = 'BmLd';
};


#endif // BOOKMARK_BAR_H
