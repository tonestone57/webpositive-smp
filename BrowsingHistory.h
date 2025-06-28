/*
 * Copyright (C) 2010 Stephan Aßmus <superstippi@gmx.de>
 *
 * All rights reserved. Distributed under the terms of the MIT License.
 */
#ifndef BROWSING_HISTORY_H
#define BROWSING_HISTORY_H

#include "DateTime.h"
#include <List.h>
#include <Locker.h>
#include <Handler.h>      // For BHandler
#include <MessageRunner.h> // For BMessageRunner

class BFile;
class BString;


class BrowsingHistoryItem {
public:
								BrowsingHistoryItem(const BString& url);
								BrowsingHistoryItem(
									const BrowsingHistoryItem& other);
								BrowsingHistoryItem(const BMessage* archive);
								~BrowsingHistoryItem();

			status_t			Archive(BMessage* archive) const;

			BrowsingHistoryItem& operator=(const BrowsingHistoryItem& other);

			bool				operator==(
									const BrowsingHistoryItem& other) const;
			bool				operator!=(
									const BrowsingHistoryItem& other) const;
			bool				operator<(
									const BrowsingHistoryItem& other) const;
			bool				operator<=(
									const BrowsingHistoryItem& other) const;
			bool				operator>(
									const BrowsingHistoryItem& other) const;
			bool				operator>=(
									const BrowsingHistoryItem& other) const;

			const BString&		URL() const { return fURL; }
			const BDateTime&	DateTime() const { return fDateTime; }
			uint32				InvokationCount() const {
									return fInvokationCount; }
			void				Invoked();

private:
			BString				fURL;
			BDateTime			fDateTime;
			uint32				fInvokationCount;
};


class BrowsingHistory : public BLocker {
public:
	static	BrowsingHistory*	DefaultInstance();
	static	const uint32		MSG_HISTORY_LOADED = 'HlDd';
	static	const uint32		MSG_DO_SAVE_HISTORY = 'HdSf';

			void				LoadAsync(BHandler* completionTarget = NULL);
			bool				IsLoaded() const;

			bool				AddItem(const BrowsingHistoryItem& item);

	// Should Lock() the object when using these in some loop or so:
			int32				CountItems() const;
			BrowsingHistoryItem	HistoryItemAt(int32 index) const;
			void				Clear();

			void				SetMaxHistoryItemAge(int32 days);
			int32				MaxHistoryItemAge() const;

private:
								BrowsingHistory();
	virtual						~BrowsingHistory();

			void				_Clear();
			bool				_AddItem(const BrowsingHistoryItem& item,
									bool invoke);

			void				_LoadSettings();
			void				_PerformSave(); // Renamed from _SaveSettings
			bool				_OpenSettingsFile(BFile& file, uint32 mode);

	static	int32				_LoadThreadEntry(void* data);

public: // Made public for BrowserApp to call
			void				SaveImmediatelyIfNeeded();
			void				ScheduleSave();

private:
			BList				fHistoryItems;
			int32				fMaxHistoryItemAge;

	static	BrowsingHistory		sDefaultInstance;
			bool				fSettingsLoaded;
			BHandler*			fCompletionTarget;
			thread_id			fLoadThreadId;
			BMessageRunner*		fSaveRunner;
};


#endif // BROWSING_HISTORY_H

