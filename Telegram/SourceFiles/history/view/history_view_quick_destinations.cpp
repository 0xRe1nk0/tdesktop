/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/history_view_quick_destinations.h"

#include "apiwrap.h"
#include "base/unique_qptr.h"
#include "chat_helpers/share_message_phrase_factory.h"
#include "data/data_chat_participant_status.h"
#include "data/data_msg_id.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_types.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_helpers.h"
#include "history/view/controls/history_view_forward_panel.h"
#include "history/view/history_view_element.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "mainwindow.h"
#include "settings/sections/settings_quick_destinations.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"

#include <QtGui/QCursor>

namespace HistoryView {
namespace {

[[nodiscard]] bool DestinationIsAvailable(not_null<PeerData*> peer) {
	if (peer->isRepliesChat() || peer->isVerifyCodes() || peer->isForum()) {
		return false;
	}
	return peer->isSelf() || Data::CanSendAnything(peer);
}

struct CollectedDestinations {
	std::vector<not_null<PeerData*>> peers;
	bool loading = false;
};

[[nodiscard]] CollectedDestinations CollectDestinations(
		not_null<Main::Session*> session) {
	auto result = CollectedDestinations();
	for (const auto id : session->settings().quickDestinationIds()) {
		if (const auto peer = session->data().peerLoaded(id)) {
			if (DestinationIsAvailable(peer)) {
				result.peers.push_back(peer);
			}
		} else {
			result.loading = true;
		}
	}
	if (result.loading) {
		session->api().requestMoreDialogsIfNeeded();
	}
	return result;
}

[[nodiscard]] QString DestinationName(not_null<PeerData*> peer) {
	return peer->isSelf()
		? tr::lng_saved_messages(tr::now)
		: peer->name();
}

void ShowConfiguration(
		not_null<Window::SessionController*> controller) {
	controller->showSettings(Settings::QuickDestinationsId());
	controller->showToast(
		tr::lng_settings_quick_destinations_about(tr::now));
}

void SendToDestination(
		not_null<Window::SessionController*> controller,
		QuickDestinationRequestContext request,
		QuickDestinationAction action,
		not_null<PeerData*> peer,
		int starsApproved = 0) {
	const auto session = &controller->session();
	if (&peer->session() != session
		|| !request.itemId
		|| !request.selectionMode
		|| request.selectionMode()) {
		return;
	}
	const auto itemId = request.itemId();
	if (!itemId) {
		return;
	}
	const auto item = session->data().message(itemId);
	if (!item) {
		return;
	}
	const auto eligible = QuickDestinationEligibilityFor(
		item,
		Context::History,
		request.selectionMode());
	if (!eligible) {
		controller->showToast(tr::lng_forward_cant(tr::now));
		return;
	}
	if (!DestinationIsAvailable(peer)) {
		Data::ShowSendErrorToast(
			controller->uiShow(),
			peer,
			tr::lng_forward_cant(tr::now));
		return;
	}
	const auto error = GetErrorForSending(
		peer,
		{ .forward = &eligible.items });
	if (error) {
		Data::ShowSendErrorToast(controller->uiShow(), peer, error);
		return;
	}

	const auto single = (eligible.items.size() == 1);
	const auto show = controller->uiShow();
	const auto requestedOptions = (action == QuickDestinationAction::Forward)
		? Data::ForwardOptions::PreserveInfo
		: Data::ForwardOptions::NoSenderNames;
	const auto options = Controls::NormalizeForwardOptions(
		session,
		eligible.items,
		requestedOptions);
	if (options != requestedOptions) {
		controller->showToast(
			tr::lng_quick_send_without_author_unavailable(tr::now));
		return;
	}

	auto sendOptions = Api::SendOptions();
	sendOptions.starsApproved = starsApproved;
	const auto payment = std::make_shared<SendPaymentHelper>();
	const auto withPaymentApproved = crl::guard(controller, [=](int approved) {
		payment->clear();
		SendToDestination(controller, request, action, peer, approved);
	});
	if (!payment->check(
			controller,
			peer,
			sendOptions,
			int(eligible.items.size()),
			withPaymentApproved)) {
		return;
	}
	session->api().forwardMessages(
		Data::ResolvedForwardDraft{
			.items = eligible.items,
			.options = options,
		},
		Api::SendAction(session->data().history(peer), sendOptions),
		[=] {
			using namespace ChatHelpers;
			auto text = rpl::variable<TextWithEntities>(
				ForwardedMessagePhrase({
					.toCount = 1,
					.singleMessage = single,
					.to1 = peer,
				})).current();
			show->showToast({
				.text = std::move(text),
				.filter = ForwardedToSavedMessagesFilter(session),
			});
		});
}

} // namespace

QuickDestinationEligibility QuickDestinationEligibilityFor(
		not_null<HistoryItem*> item,
		Context context,
		bool selectionMode) {
	if (context != Context::History
		|| selectionMode
		|| item->isSponsored()) {
		return {};
	}
	const auto owner = &item->history()->owner();
	auto items = owner->idsToItems(owner->itemOrItsGroup(item));
	if (items.empty()
		|| !ranges::contains(items, item)
		|| ranges::any_of(items, [](not_null<HistoryItem*> grouped) {
			return !grouped->allowsForward();
		})) {
		return {};
	}
	return { .items = std::move(items) };
}

void ActivateQuickDestination(
		not_null<Window::SessionController*> controller,
		QuickDestinationRequestContext request,
		QuickDestinationAction action,
		QPoint globalPosition) {
	const auto session = &controller->session();
	if (!request.itemId
		|| !request.selectionMode
		|| request.selectionMode()) {
		return;
	}
	const auto itemId = request.itemId();
	if (!itemId) {
		return;
	}
	const auto item = session->data().message(itemId);
	const auto eligible = item ? QuickDestinationEligibilityFor(
			item,
			Context::History,
			request.selectionMode()) : QuickDestinationEligibility();
	if (!eligible) {
		return;
	}
	const auto requestedOptions = (action == QuickDestinationAction::Forward)
		? Data::ForwardOptions::PreserveInfo
		: Data::ForwardOptions::NoSenderNames;
	if (Controls::NormalizeForwardOptions(
			session,
			eligible.items,
			requestedOptions) != requestedOptions) {
		controller->showToast(
			tr::lng_quick_send_without_author_unavailable(tr::now));
		return;
	}
	const auto collected = CollectDestinations(session);
	const auto &destinations = collected.peers;
	if (destinations.empty()) {
		if (collected.loading) {
			controller->showToast(tr::lng_quick_destinations_loading(tr::now));
		} else {
			ShowConfiguration(controller);
		}
		return;
	} else if (destinations.size() == 1) {
		SendToDestination(controller, request, action, destinations.front());
		return;
	}

	auto menu = base::make_unique_q<Ui::PopupMenu>(
		controller->widget(),
		st::popupMenuWithIcons);
	const auto weak = base::make_weak(controller);
	const auto icon = (action == QuickDestinationAction::Forward)
		? &st::menuIconForward
		: &st::menuIconSend;
	for (const auto peer : destinations) {
		menu->addAction(DestinationName(peer), [=] {
			if (const auto strong = weak.get()) {
				SendToDestination(strong, request, action, peer);
			}
		}, icon);
	}
	menu->addSeparator();
	menu->addAction(
		tr::lng_settings_quick_destinations(tr::now),
		[=] {
			if (const auto strong = weak.get()) {
				strong->showSettings(Settings::QuickDestinationsId());
			}
		},
		&st::menuIconSettings);
	menu->deleteOnHide(true);
	menu->popup(globalPosition.isNull() ? QCursor::pos() : globalPosition);
	menu.release();
}

} // namespace HistoryView
