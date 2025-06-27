/*
 * Copyright (C) 2010 Stephan Aßmus <superstippi@gmx.de>
 *
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "BrowsingHistory.h"

#include <new>
#include <stdio.h>

#include <Autolock.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <Message.h>
#include <Path.h>

#include "BrowserApp.h"
#include <os/kernel/OS.h> // For spawn_thread, wait_for_thread, etc.
#include <Looper.h>      // For BLooper
#include <Handler.h>     // For BHandler


BrowsingHistoryItem::BrowsingHistoryItem(const BString& url)
	:
	fURL(url),
	fDateTime(BDateTime::CurrentDateTime(B_LOCAL_TIME)),
	fInvokationCount(0)
{
}


BrowsingHistoryItem::BrowsingHistoryItem(const BrowsingHistoryItem& other)
{
	*this = other;
}


BrowsingHistoryItem::BrowsingHistoryItem(const BMessage* archive)
{
	if (!archive)
		return;
	BMessage dateTimeArchive;
	if (archive->FindMessage("date time", &dateTimeArchive) == B_OK)
		fDateTime = BDateTime(&dateTimeArchive);
	archive->FindString("url", &fURL);
	archive->FindUInt32("invokations", &fInvokationCount);
}


BrowsingHistoryItem::~BrowsingHistoryItem()
{
}


status_t
BrowsingHistoryItem::Archive(BMessage* archive) const
{
	if (!archive)
		return B_BAD_VALUE;
	BMessage dateTimeArchive;
	status_t status = fDateTime.Archive(&dateTimeArchive);
	if (status == B_OK)
		status = archive->AddMessage("date time", &dateTimeArchive);
	if (status == B_OK)
		status = archive->AddString("url", fURL.String());
	if (status == B_OK)
		status = archive->AddUInt32("invokations", fInvokationCount);
	return status;
}


BrowsingHistoryItem&
BrowsingHistoryItem::operator=(const BrowsingHistoryItem& other)
{
	if (this == &other)
		return *this;

	fURL = other.fURL;
	fDateTime = other.fDateTime;
	fInvokationCount = other.fInvokationCount;

	return *this;
}


bool
BrowsingHistoryItem::operator==(const BrowsingHistoryItem& other) const
{
	if (this == &other)
		return true;

	return fURL == other.fURL && fDateTime == other.fDateTime
		&& fInvokationCount == other.fInvokationCount;
}


bool
BrowsingHistoryItem::operator!=(const BrowsingHistoryItem& other) const
{
	return !(*this == other);
}


bool
BrowsingHistoryItem::operator<(const BrowsingHistoryItem& other) const
{
	if (this == &other)
		return false;

	return fDateTime < other.fDateTime || fURL < other.fURL;
}


bool
BrowsingHistoryItem::operator<=(const BrowsingHistoryItem& other) const
{
	return (*this == other) || (*this < other);
}


bool
BrowsingHistoryItem::operator>(const BrowsingHistoryItem& other) const
{
	if (this == &other)
		return false;

	return fDateTime > other.fDateTime || fURL > other.fURL;
}


bool
BrowsingHistoryItem::operator>=(const BrowsingHistoryItem& other) const
{
	return (*this == other) || (*this > other);
}


void
BrowsingHistoryItem::Invoked()
{
	// Eventually, we may overflow...
	uint32 count = fInvokationCount + 1;
	if (count > fInvokationCount)
		fInvokationCount = count;
	fDateTime = BDateTime::CurrentDateTime(B_LOCAL_TIME);
}


// #pragma mark - BrowsingHistory


BrowsingHistory
BrowsingHistory::sDefaultInstance;


#include <AppDefs.h> // For be_app_messenger
#include <MessageRunner.h> // For BMessageRunner

const uint32 BrowsingHistory::MSG_HISTORY_LOADED; // Definition for static const
const uint32 BrowsingHistory::MSG_DO_SAVE_HISTORY;


BrowsingHistory::BrowsingHistory()
	:
	BLocker("browsing history"),
	fHistoryItems(64),
	fMaxHistoryItemAge(7),
	fSettingsLoaded(false),
	fCompletionTarget(NULL),
	fLoadThreadId(B_NO_THREAD),
	fSaveRunner(NULL)
{
}


BrowsingHistory::~BrowsingHistory()
{
	if (fLoadThreadId >= B_NO_THREAD && fLoadThreadId != find_thread(NULL)) {
		// Don't wait_for_thread if it's our own thread, can happen if
		// app quits very early during history load.
		status_t exitValue;
		wait_for_thread(fLoadThreadId, &exitValue);
	}
	delete fSaveRunner;
	fSaveRunner = NULL;
	_PerformSave(); // Ensure any pending changes are written
	_Clear();
}


/*static*/ BrowsingHistory*
BrowsingHistory::DefaultInstance()
{
	// Loading is now handled by LoadAsync
	return &sDefaultInstance;
}


/*static*/ int32
BrowsingHistory::_LoadThreadEntry(void* data)
{
	BrowsingHistory* history = static_cast<BrowsingHistory*>(data);
	if (history->Lock()) {
		history->_LoadSettings();
		BHandler* target = history->fCompletionTarget;
		history->fLoadThreadId = B_NO_THREAD;
		history->Unlock();

		if (target && target->Looper()) {
			BMessenger messenger(target);
			messenger.SendMessage(MSG_HISTORY_LOADED);
		}
	}
	return B_OK;
}


void
BrowsingHistory::LoadAsync(BHandler* completionTarget)
{
	BAutolock _(this);
	if (fSettingsLoaded || fLoadThreadId != B_NO_THREAD)
		return;

	fCompletionTarget = completionTarget;
	fLoadThreadId = spawn_thread(_LoadThreadEntry, "history_load_thread",
		B_NORMAL_PRIORITY, this);

	if (fLoadThreadId < 0) {
		// Thread spawning failed
		fprintf(stderr, "Failed to spawn history loading thread!\n");
		fLoadThreadId = B_NO_THREAD; // Reset
		// Optionally, could fall back to synchronous loading or set an error state
	}
}


bool
BrowsingHistory::IsLoaded() const
{
	// This method should be called with the lock held or from the same thread
	// if there's a possibility of fSettingsLoaded changing.
	// For simplicity, we assume it's mostly checked from the UI thread.
	return fSettingsLoaded;
}


bool
BrowsingHistory::AddItem(const BrowsingHistoryItem& item)
{
	BAutolock _(this);

	// The actual AddItem logic
	int32 count = CountItems(); // CountItems is lock-protected
	int32 insertionIndex = count;
	for (int32 i = 0; i < count; i++) {
		BrowsingHistoryItem* existingItem
			= reinterpret_cast<BrowsingHistoryItem*>(
			fHistoryItems.ItemAtFast(i));
		if (item.URL() == existingItem->URL()) {
			if (!internal) { // 'internal' is true if called from _LoadSettings
				existingItem->Invoked();
				// ScheduleSave(); // Moved to the caller AddItem
			}
			return true; // Item already exists, updated if necessary
		}
		// This comparison logic might need review if BList isn't always sorted
		// or if a different sort order is desired.
		// Assuming items are added in a way that this check is meaningful
		// or that BList maintains some order for AddItem(item, index).
		// For now, keeping original logic for insertion point.
		if (item < *existingItem)
			insertionIndex = i;
	}
	BrowsingHistoryItem* newItem = new(std::nothrow) BrowsingHistoryItem(item);
	if (!newItem || !fHistoryItems.AddItem(newItem, insertionIndex)) {
		delete newItem;
		return false; // Failed to add new item
	}

	if (!internal) { // 'internal' is true if called from _LoadSettings
		newItem->Invoked(); // Update timestamp and count for new user-added item
		// ScheduleSave(); // Moved to the caller AddItem
	}

	return true; // Item added successfully
}


// This is the public AddItem that schedules a save
bool
BrowsingHistory::AddItem(const BrowsingHistoryItem& item)
{
	BAutolock _(this);
	bool result = _AddItem(item, false); // Call with internal = false
	if (result)
		ScheduleSave();
	return result;
}


void
BrowsingHistory::ScheduleSave()
{
	BAutolock _(this);
	delete fSaveRunner;
	fSaveRunner = NULL; // In case new fails
	BMessage saveMsg(MSG_DO_SAVE_HISTORY);
	// Target be_app, as BrowsingHistory is not a BHandler
	fSaveRunner = new BMessageRunner(be_app_messenger, &saveMsg, 5 * 1000000, 1);
	if (!fSaveRunner || fSaveRunner->InitCheck() != B_OK) {
		delete fSaveRunner;
		fSaveRunner = NULL;
		fprintf(stderr, "Failed to schedule history save!\n");
		// Fallback or error logging if BMessageRunner fails
	}
}


void
BrowsingHistory::SaveImmediatelyIfNeeded()
{
	BAutolock _(this);
	// If a save was scheduled, cancel it and save now.
	if (fSaveRunner) {
		delete fSaveRunner;
		fSaveRunner = NULL;
		_PerformSave();
	}
	// If no save was scheduled, this does nothing, which is fine.
	// _PerformSave() is also called in ~BrowsingHistory for final save.
}


int32
BrowsingHistory::BrowsingHistory::CountItems() const
{
	BAutolock _(const_cast<BrowsingHistory*>(this));

	return fHistoryItems.CountItems();
}


BrowsingHistoryItem
BrowsingHistory::HistoryItemAt(int32 index) const
{
	BAutolock _(const_cast<BrowsingHistory*>(this));

	BrowsingHistoryItem* existingItem = reinterpret_cast<BrowsingHistoryItem*>(
		fHistoryItems.ItemAt(index));
	if (!existingItem)
		return BrowsingHistoryItem(BString());

	return BrowsingHistoryItem(*existingItem);
}


void
BrowsingHistory::Clear()
{
	BAutolock _(this);
	_Clear();
	ScheduleSave();
}	


void
BrowsingHistory::SetMaxHistoryItemAge(int32 days)
{
	BAutolock _(this);
	if (fMaxHistoryItemAge != days) {
		fMaxHistoryItemAge = days;
		ScheduleSave();
	}
}	


int32
BrowsingHistory::MaxHistoryItemAge() const
{
	return fMaxHistoryItemAge;
}	


// #pragma mark - private


void
BrowsingHistory::_Clear()
{
	int32 count = CountItems();
	for (int32 i = 0; i < count; i++) {
		BrowsingHistoryItem* item = reinterpret_cast<BrowsingHistoryItem*>(
			fHistoryItems.ItemAtFast(i));
		delete item;
	}
	fHistoryItems.MakeEmpty();
}


bool
BrowsingHistory::_AddItem(const BrowsingHistoryItem& item, bool internal)
{
	int32 count = CountItems();
	int32 insertionIndex = count;
	for (int32 i = 0; i < count; i++) {
		BrowsingHistoryItem* existingItem
			= reinterpret_cast<BrowsingHistoryItem*>(
			fHistoryItems.ItemAtFast(i));
		if (item.URL() == existingItem->URL()) {
			if (!internal) {
				existingItem->Invoked();
				_SaveSettings();
			}
			return true;
		}
		if (item < *existingItem)
			insertionIndex = i;
	}
	BrowsingHistoryItem* newItem = new(std::nothrow) BrowsingHistoryItem(item);
	if (!newItem || !fHistoryItems.AddItem(newItem, insertionIndex)) {
		delete newItem;
		return false;
	}

	if (!internal) {
		newItem->Invoked();
		_SaveSettings();
	}

	return true;
}


void
BrowsingHistory::_LoadSettings()
{
	if (fSettingsLoaded)
		return;

	fSettingsLoaded = true;

	BFile settingsFile;
	if (_OpenSettingsFile(settingsFile, B_READ_ONLY)) {
		BMessage settingsArchive;
		settingsArchive.Unflatten(&settingsFile);
		if (settingsArchive.FindInt32("max history item age",
				&fMaxHistoryItemAge) != B_OK) {
			fMaxHistoryItemAge = 7;
		}
		BDateTime oldestAllowedDateTime
			= BDateTime::CurrentDateTime(B_LOCAL_TIME);
		oldestAllowedDateTime.Date().AddDays(-fMaxHistoryItemAge);

		BMessage historyItemArchive;
		for (int32 i = 0; settingsArchive.FindMessage("history item", i,
				&historyItemArchive) == B_OK; i++) {
			BrowsingHistoryItem item(&historyItemArchive);
			if (oldestAllowedDateTime < item.DateTime())
				_AddItem(item, true);
			historyItemArchive.MakeEmpty();
		}
	}
}


void
BrowsingHistory::_PerformSave()
{
	BFile settingsFile;
	if (_OpenSettingsFile(settingsFile,
			B_CREATE_FILE | B_ERASE_FILE | B_WRITE_ONLY)) {
		BMessage settingsArchive;
		settingsArchive.AddInt32("max history item age", fMaxHistoryItemAge);
		BMessage historyItemArchive;
		int32 count = CountItems();
		for (int32 i = 0; i < count; i++) {
			BrowsingHistoryItem item = HistoryItemAt(i);
			if (item.Archive(&historyItemArchive) != B_OK)
				break;
			if (settingsArchive.AddMessage("history item",
					&historyItemArchive) != B_OK) {
				break;
			}
			historyItemArchive.MakeEmpty();
		}
		settingsArchive.Flatten(&settingsFile);
	}
}


bool
BrowsingHistory::_OpenSettingsFile(BFile& file, uint32 mode)
{
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK
		|| path.Append(kApplicationName) != B_OK
		|| path.Append("BrowsingHistory") != B_OK) {
		return false;
	}
	return file.SetTo(path.Path(), mode) == B_OK;
}

