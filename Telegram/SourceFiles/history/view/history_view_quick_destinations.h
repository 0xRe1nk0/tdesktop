/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "data/data_msg_id.h"

#include <QtCore/QPoint>

#include <vector>

class HistoryItem;

namespace Window {
class SessionController;
} // namespace Window

namespace HistoryView {

enum class Context : char;

enum class QuickDestinationAction : uchar {
	Forward,
	SendWithoutAuthor,
};

struct QuickDestinationEligibility {
	std::vector<not_null<HistoryItem*>> items;

	[[nodiscard]] explicit operator bool() const {
		return !items.empty();
	}
};

struct QuickDestinationRequestContext {
	Fn<FullMsgId()> itemId;
	Fn<bool()> selectionMode;
};

[[nodiscard]] QuickDestinationEligibility QuickDestinationEligibilityFor(
	not_null<HistoryItem*> item,
	Context context,
	bool selectionMode);

void ActivateQuickDestination(
	not_null<Window::SessionController*> controller,
	QuickDestinationRequestContext request,
	QuickDestinationAction action,
	QPoint globalPosition);

} // namespace HistoryView
