/*
 * Copyright 2014, Adrien Destugues <pulkomandy@pulkomandy.tk>.
 * Distributed under the terms of the MIT License.
 */


#include "BookmarkBar.h"

#include <Alert.h>
#include <Catalog.h>
#include <Directory.h>
#include <Entry.h>
#include <GroupLayout.h>
#include <IconMenuItem.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <PopUpMenu.h>
#include <PromptWindow.h>
#include <TextControl.h>
#include <Window.h>
#include <Looper.h> // Required for BMessenger target in BMessageRunner if used, and general Looper context
#include <AppDefs.h> // For B_OK
#include <OS.h> // For thread_id, find_thread, spawn_thread, wait_for_thread, snooze

#include "tracker_private.h"

#include "BrowserWindow.h"
#include "NavMenu.h"

#include <stdio.h>


#undef B_TRANSLATION_CONTEXT // Ensure B_TRANSLATION_CONTEXT is not doubly defined
#define B_TRANSLATION_CONTEXT "BookmarkBar"

static const thread_id B_NO_THREAD = -1;

const uint32 BookmarkBar::MSG_ADD_BOOKMARK_ITEMS;
const uint32 BookmarkBar::MSG_BOOKMARKS_LOADED;

const uint32 kOpenNewTabMsg = 'opnt';
const uint32 kDeleteMsg = 'dele';
const uint32 kAskBookmarkNameMsg = 'askn';
const uint32 kShowInTrackerMsg = 'otrk';
const uint32 kRenameBookmarkMsg = 'rena';
const uint32 kFolderMsg = 'fold';


BookmarkBar::BookmarkBar(const char* title, BHandler* target,
	const entry_ref* navDir)
	:
	BMenuBar(title),
	fNodeRef(), // Initialize properly
	fOverflowMenu(NULL),
	fOverflowMenuAdded(false),
	fPopUpMenu(NULL),
	fSelectedItemIndex(-1),
	fLoadThreadId(B_NO_THREAD)
{
	SetFlags(Flags() | B_FRAME_EVENTS);
	if (navDir)
		BEntry(navDir).GetNodeRef(&fNodeRef);

	fOverflowMenu = new BMenu(B_UTF8_ELLIPSIS);
	// fOverflowMenuAdded is false by default

	fPopUpMenu = new BPopUpMenu("Bookmark Popup", false, false);
	fPopUpMenu->AddItem(
		new BMenuItem(B_TRANSLATE("Open in new tab"), new BMessage(kOpenNewTabMsg)));
	fPopUpMenu->AddItem(new BMenuItem(B_TRANSLATE("Rename"), new BMessage(kAskBookmarkNameMsg)));
	fPopUpMenu->AddItem(
		new BMenuItem(B_TRANSLATE("Show in Tracker"), new BMessage(kShowInTrackerMsg)));
	fPopUpMenu->AddItem(new BSeparatorItem());
	fPopUpMenu->AddItem(new BMenuItem(B_TRANSLATE("Delete"), new BMessage(kDeleteMsg)));
}


BookmarkBar::~BookmarkBar()
{
	stop_watching(BMessenger(this));
	if (fLoadThreadId != B_NO_THREAD && fLoadThreadId != find_thread(NULL)) {
		// Check against B_NO_THREAD which is -1, or specific positive thread IDs.
		status_t exit_val;
		wait_for_thread(fLoadThreadId, &exit_val);
	}
	// fOverflowMenu is owned by BMenuBar if added, otherwise needs deletion.
	if (!fOverflowMenuAdded)
		delete fOverflowMenu;
	else {
		// If it was added, BMenuBar owns it. But if it might be manipulated
		// after this point, ensure it's cleaned from fItemsMap if necessary.
		// However, fItemsMap refers to items *within* menus, not the menu itself.
	}
	delete fPopUpMenu;
	// IconMenuItems in fItemsMap are owned by the BMenu they are added to.
	// BMenuBar's destructor will delete its items, which in turn delete submenus.
	fItemsMap.clear();
}


void
BookmarkBar::MouseDown(BPoint where)
{
	fSelectedItemIndex = -1;
	BMessage* message = Window()->CurrentMessage();
	if (message != nullptr) {
		int32 buttons = 0;
		if (message->FindInt32("buttons", &buttons) == B_OK) {
			if (buttons & B_SECONDARY_MOUSE_BUTTON) {

				bool foundItem = false;
				for (int32 i = 0; i < CountItems(); i++) {
					BRect itemBounds = ItemAt(i)->Frame();
					if (itemBounds.Contains(where)) {
						foundItem = true;
						fSelectedItemIndex = i;
						break;
					}
				}
				if (foundItem) {
					BPoint screenWhere(where);
					ConvertToScreen(&screenWhere);
					if (ItemAt(fSelectedItemIndex)->Message()->what == kFolderMsg) {
						// This is a directory item, disable "open in new tab"
						fPopUpMenu->ItemAt(0)->SetEnabled(false);
					} else
						fPopUpMenu->ItemAt(0)->SetEnabled(true);

					// Pop up the menu
					fPopUpMenu->SetTargetForItems(this);
					fPopUpMenu->Go(screenWhere, true, true, true);
					return;
				}
			}
		}
	}

	BMenuBar::MouseDown(where);
}


void
BookmarkBar::AttachedToWindow()
{
	BMenuBar::AttachedToWindow();
	watch_node(&fNodeRef, B_WATCH_DIRECTORY, BMessenger(this));

	// Asynchronously load initial directory content
	fLoadThreadId = spawn_thread(_LoadBookmarksThreadEntry,
		"bookmark_load_thread", B_NORMAL_PRIORITY, this);

	if (fLoadThreadId < 0) {
		fprintf(stderr, "Failed to spawn bookmark loading thread!\n");
		fLoadThreadId = B_NO_THREAD;
		// Fallback: could load synchronously or leave bar empty
	}
}


/*static*/ int32
BookmarkBar::_LoadBookmarksThreadEntry(void* data)
{
	BookmarkBar* bar = static_cast<BookmarkBar*>(data);
	BDirectory dir(&bar->fNodeRef);
	if (dir.InitCheck() != B_OK) {
		bar->fLoadThreadId = B_NO_THREAD;
		return B_ERROR;
	}

	BEntry entry;
	BMessage batchMessage(MSG_ADD_BOOKMARK_ITEMS);
	int32 itemsInBatch = 0;
	const int32 BATCH_SIZE = 10; // Send items in batches

	while (dir.GetNextEntry(&entry, false) == B_OK) {
		if (!entry.IsFile() && !entry.IsDirectory() && !entry.IsSymLink())
			continue;

		entry_ref ref;
		node_ref nref; // For inode
		if (entry.GetRef(&ref) == B_OK && entry.GetNodeRef(&nref) == B_OK) {
			batchMessage.AddInt64("node", nref.node);
			batchMessage.AddRef("refs", &ref);
			itemsInBatch++;

			if (itemsInBatch >= BATCH_SIZE) {
				BMessenger(bar).SendMessage(&batchMessage);
				batchMessage.MakeEmpty(); // Prepare for next batch
				batchMessage.what = MSG_ADD_BOOKMARK_ITEMS;
				itemsInBatch = 0;
				// Yield for a moment to allow UI to process
				snooze(20000); // 20ms
			}
		}
	}

	if (itemsInBatch > 0) {
		BMessenger(bar).SendMessage(&batchMessage);
	}

	BMessenger(bar).SendMessage(MSG_BOOKMARKS_LOADED);
	bar->fLoadThreadId = B_NO_THREAD;
	return B_OK;
}


void
BookmarkBar::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case MSG_ADD_BOOKMARK_ITEMS:
		{
			entry_ref ref;
			int64 node;
			for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK
				&& message->FindInt64("node", i, &node) == B_OK; i++) {
				_AddItem(node, ref);
			}
			// Reevaluate whether the "more" menu is needed after adding items
			BRect rect = Bounds();
			FrameResized(rect.Width(), rect.Height());
			break;
		}
		case MSG_BOOKMARKS_LOADED:
		{
			// Optional: any final actions after all bookmarks are loaded.
			// For example, ensure FrameResized is called one last time.
			BRect rect = Bounds();
			FrameResized(rect.Width(), rect.Height());
			printf("Bookmarks loaded.\n");
			break;
		}
		case B_NODE_MONITOR:
		{
			// If still loading, node monitor events might conflict or be redundant.
			// However, node monitoring is essential for live updates after initial load.
			// For simplicity, let it run. If initial load is slow, user actions
			// (like deleting a bookmark being loaded) could have race conditions.
			// A robust solution might queue node monitor events during initial load
			// or disable/re-enable monitoring. For now, proceed with direct handling.

			int32 opcode = message->FindInt32("opcode");
			ino_t inode = message->FindInt64("node");
			switch (opcode) {
				case B_ENTRY_CREATED:
				{
					entry_ref ref;
					const char* name;

					message->FindInt32("device", &ref.device);
					message->FindInt64("directory", &ref.directory);
					message->FindString("name", &name);
					ref.set_name(name);

					BEntry entry(&ref);
					if (entry.InitCheck() == B_OK)
						_AddItem(inode, ref); // Pass the entry_ref directly
					break;
				}
				case B_ENTRY_MOVED:
				{
					entry_ref ref;
					const char* name;

					message->FindInt32("device", &ref.device);
					message->FindInt64("to directory", &ref.directory);
					message->FindString("name", &name);
					ref.set_name(name);

					BEntry entry(&ref);
					BEntry followedEntry(&ref, true); // traverse in case it's a symlink

					if (fItemsMap[inode] == NULL) {
						// Pass the entry_ref directly
						// Ensure BEntry 'entry' is initialized if used to get the ref,
						// but 'ref' is already available.
						_AddItem(inode, ref);
						break;
					} else {
						// Existing item. Check if it's a rename or a move
						// to some other folder.
						ino_t from, to;
						message->FindInt64("to directory", &to);
						message->FindInt64("from directory", &from);
						if (from == to) {
							const char* name;
							if (message->FindString("name", &name) == B_OK)
								fItemsMap[inode]->SetLabel(name);

							BMessage* itemMessage = new BMessage(
								followedEntry.IsDirectory() ? kFolderMsg : B_REFS_RECEIVED);
							itemMessage->AddRef("refs", &ref);
							fItemsMap[inode]->SetMessage(itemMessage);

							break;
						}
					}

					// fall through: the item was moved from here to
					// elsewhere, remove it from the bar.
				}
				case B_ENTRY_REMOVED:
				{
					IconMenuItem* item = fItemsMap[inode];
					RemoveItem(item);
					fOverflowMenu->RemoveItem(item);
					fItemsMap.erase(inode);
					delete item;

					// Reevaluate whether the "more" menu is still needed
					BRect rect = Bounds();
					FrameResized(rect.Width(), rect.Height());
					break;
				}
			}
			break;
		}

		case kOpenNewTabMsg:
		{
			if (fSelectedItemIndex >= 0 && fSelectedItemIndex < CountItems()) {
				// Get the bookmark refs
				entry_ref ref;
				BMenuItem* selectedItem = ItemAt(fSelectedItemIndex);
				if (selectedItem->Message()->what == kFolderMsg
						|| selectedItem->Message()->FindRef("refs", &ref) != B_OK) {
					break;
				}

				// Use the entry_ref to create a BEntry instance and get its path
				BEntry entry(&ref);
				BPath path;
				entry.GetPath(&path);

				BMessage* message = new BMessage(B_REFS_RECEIVED);
				message->AddRef("refs", &ref);
				Window()->PostMessage(message);
			}
			break;
		}
		case kDeleteMsg:
		{
			if (fSelectedItemIndex >= 0 && fSelectedItemIndex < CountItems()) {
				BMenuItem* selectedItem = ItemAt(fSelectedItemIndex);
				// Get the bookmark refs
				entry_ref ref;
				if (selectedItem->Message()->FindRef("refs", &ref) != B_OK)
					break;

				// Use the entry_ref to create a BEntry instance and get its path
				BEntry entry(&ref);
				BPath path;
				entry.GetPath(&path);

				// Remove the bookmark file
				if (entry.Remove() != B_OK) {
					// handle error case if necessary
					BString errorMessage = B_TRANSLATE("Failed to delete bookmark:\n'%path%'");
					errorMessage.ReplaceFirst("%path%", path.Path());
					BAlert* alert = new BAlert("Error", errorMessage.String(), B_TRANSLATE("OK"));
					alert->Go();
					break;
				}

				// Remove the item from the bookmark bar
				if (!RemoveItem(fSelectedItemIndex)) {
					// handle error case if necessary
					BString errorMessage = B_TRANSLATE("Failed to remove bookmark '%leaf%' "
							"from boookmark bar.");
					errorMessage.ReplaceFirst("%leaf%", path.Leaf());
					BAlert* alert = new BAlert("Error", errorMessage.String(), B_TRANSLATE("OK"));
					alert->Go();
				}
			}
			break;
		}
		case kShowInTrackerMsg:
		{
			entry_ref ref;
			if (fSelectedItemIndex >= 0 && fSelectedItemIndex < CountItems()) {
				BMenuItem* selectedItem = ItemAt(fSelectedItemIndex);
				// Get the bookmark refs
				if (selectedItem->Message()->FindRef("refs", &ref) != B_OK)
					break;

				BEntry entry(&ref);
				BEntry parent;
				entry.GetParent(&parent);
				entry_ref folderRef;
				parent.GetRef(&folderRef);
				BMessenger msgr(kTrackerSignature);
				// Open parent folder in Tracker
				BMessage refMsg(B_REFS_RECEIVED);
				refMsg.AddRef("refs", &folderRef);
				msgr.SendMessage(&refMsg);

				// Select file
				BMessage selectMessage(kSelect);
				entry_ref target;
				if (entry.GetRef(&target) != B_OK)
					break;

				selectMessage.AddRef("refs", &target);
				// wait 0.3 sec to give Tracker time to populate
				BMessageRunner::StartSending(BMessenger(kTrackerSignature),
					&selectMessage, 300000, 1);
			}
			break;
		}
		case kAskBookmarkNameMsg:
		{
			// Get the index of the selected item
			int32 index = fSelectedItemIndex;

			// Get the selected item
			if (index >= 0 && index < CountItems()) {
				BMenuItem* selectedItem = ItemAt(index);
				BString oldName = selectedItem->Label();
				BMessage* message = new BMessage(kRenameBookmarkMsg);
				message->AddPointer("item", selectedItem);
				BString request;
				request.SetToFormat(B_TRANSLATE("Old name: %s"), oldName.String());
				// Create a text control to get the new name from the user
				PromptWindow* prompt = new PromptWindow(B_TRANSLATE("Rename bookmark"),
					B_TRANSLATE("New name:"), request, this, message);
				prompt->Show();
				prompt->CenterOnScreen();
			}
			break;
		}
		case kRenameBookmarkMsg:
		{
			// User clicked OK, get the new name
			BString newName = message->FindString("text");
			BMenuItem* selectedItem = NULL;
			message->FindPointer("item", (void**)&selectedItem);

			// Rename the bookmark file
			entry_ref ref;
			if (selectedItem->Message()->FindRef("refs", &ref) == B_OK) {
				BEntry entry(&ref);
				entry.Rename(newName.String());

				// Update the menu item label
				selectedItem->SetLabel(newName);
			}
			break;
		}

		default:
			BMenuBar::MessageReceived(message);
			break;
	}
}


void
BookmarkBar::FrameResized(float width, float height)
{
	int32 count = CountItems();

	// Account for the "more" menu, in terms of item count and space occupied
	int32 overflowMenuWidth = 0;
	if (IndexOf(fOverflowMenu) != B_ERROR) {
		count--;
		// Ignore the width of the "more" menu if it would disappear after
		// removing a bookmark from it.
		if (fOverflowMenu->CountItems() > 1)
			overflowMenuWidth = 32;
	}

	int32 i = 0;
	float rightmost = 0.f;
	while (i < count) {
		BMenuItem* item = ItemAt(i);
		BRect frame = item->Frame();
		if (frame.right > width - overflowMenuWidth)
			break;
		rightmost = frame.right;
		i++;
	}

	if (i == count) {
		// See if we can move some items from the "more" menu in the remaining
		// space.
		BMenuItem* extraItem = fOverflowMenu->ItemAt(0);
		while (extraItem != NULL) {
			BRect frame = extraItem->Frame();
			if (frame.Width() + rightmost > width - overflowMenuWidth)
				break;
			AddItem(fOverflowMenu->RemoveItem((int32)0), i);
			i++;

			rightmost = ItemAt(i)->Frame().right;
			if (fOverflowMenu->CountItems() <= 1)
				overflowMenuWidth = 0;
			extraItem = fOverflowMenu->ItemAt(0);
		}
		if (fOverflowMenu->CountItems() == 0) {
			RemoveItem(fOverflowMenu);
			fOverflowMenuAdded = false;
		}

	} else {
		// Remove any overflowing item and move them to the "more" menu.
		// Counting backwards avoids complications when indices shift
		// after an item is removed, and keeps bookmarks in the same order,
		// provided they are added at index 0 of the "more" menu.
		for (int j = count - 1; j >= i; j--)
			fOverflowMenu->AddItem(RemoveItem(j), 0);

		if (IndexOf(fOverflowMenu) == B_ERROR) {
			AddItem(fOverflowMenu);
			fOverflowMenuAdded = true;
		}
	}

	BMenuBar::FrameResized(width, height);
}


BSize
BookmarkBar::MinSize()
{
	BSize size = BMenuBar::MinSize();

	// We only need space to show the "more" button.
	size.width = 32;

	// We need enough vertical space to show bookmark icons.
	if (size.height < 20)
		size.height = 20;

	return size;
}


// #pragma mark - private methods


void
BookmarkBar::_AddItem(ino_t inode, const entry_ref& ref)
{
	// make sure the item doesn't already exist in the map by inode
	if (fItemsMap.count(inode)) // Use .count for std::map
		return;

	BEntry entry(&ref, false); // Don't traverse symlinks for GetName
	if (entry.InitCheck() != B_OK)
		return;

	char name[B_FILE_NAME_LENGTH];
	entry.GetName(name);

	// In case it's a symlink, follow link to get the right icon for display,
	// but the message should operate on the symlink itself (using the passed 'ref').
	BEntry followedEntry(&ref, true); // Traverse link for icon and type check

	IconMenuItem* item = NULL;

	if (followedEntry.IsDirectory()) {
		BNavMenu* menu = new BNavMenu(name, B_REFS_RECEIVED, Window());
		// It's crucial that SetNavDir uses the original ref if it's a symlink
		// to a directory, or the followed ref if that's the desired behavior.
		// Current BNavMenu likely expects a real directory.
		// For simplicity, let's assume we always want to navigate the target for directories.
		entry_ref targetRef;
		followedEntry.GetRef(&targetRef);
		menu->SetNavDir(&targetRef);

		BMessage* message = new BMessage(kFolderMsg);
		message->AddRef("refs", &ref); // Message uses original ref
		item = new IconMenuItem(menu, message, "application/x-vnd.Be-directory", B_MINI_ICON);
	} else {
		BNode node(&followedEntry); // Use followed entry for icon
		BNodeInfo info(&node);
		BMessage* message = new BMessage(B_REFS_RECEIVED);
		message->AddRef("refs", &ref); // Message uses original ref
		item = new IconMenuItem(name, message, &info, B_MINI_ICON);
	}

	int32 count = CountItems();
	if (IndexOf(fOverflowMenu) != B_ERROR)
		count--;

	BMenuBar::AddItem(item, count);
	fItemsMap[inode] = item;

	// Move the item to the "more" menu if it overflows.
	BRect rect = Bounds();
	FrameResized(rect.Width(), rect.Height());
}
