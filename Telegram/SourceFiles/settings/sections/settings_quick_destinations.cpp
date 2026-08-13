/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/sections/settings_quick_destinations.h"

#include "boxes/filters/edit_filter_chats_preview.h"
#include "boxes/peer_list_box.h"
#include "boxes/peer_list_controllers.h"
#include "data/data_chat_participant_status.h"
#include "data/data_changes.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "apiwrap.h"
#include "settings/sections/settings_chat.h"
#include "settings/settings_builder.h"
#include "settings/settings_common_session.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

namespace Settings {
namespace {

using namespace Builder;

[[nodiscard]] bool IsEligibleDestination(not_null<PeerData*> peer) {
	if (peer->isRepliesChat() || peer->isVerifyCodes() || peer->isForum()) {
		return false;
	}
	return peer->isSelf() || Data::CanSendAnything(peer);
}

[[nodiscard]] base::flat_set<not_null<History*>> LoadedHistories(
		not_null<Main::Session*> session,
		const std::vector<PeerId> &ids) {
	auto result = base::flat_set<not_null<History*>>();
	for (const auto id : ids) {
		if (const auto peer = session->data().peerLoaded(id)) {
			if (!peer->isRepliesChat()
				&& !peer->isVerifyCodes()
				&& !peer->isForum()) {
				result.emplace(session->data().history(peer));
			}
		}
	}
	return result;
}

void SaveDestinations(
		not_null<Main::Session*> session,
		std::vector<PeerId> ids) {
	session->settings().setQuickDestinationIds(std::move(ids));
	session->saveSettingsDelayed();
}

class QuickDestinationsBoxController final
	: public ChatsListBoxController {
public:
	QuickDestinationsBoxController(not_null<Main::Session*> session);

	Main::Session &session() const override;
	void rowClicked(not_null<PeerListRow*> row) override;
	QString savedMessagesChatStatus() const override;

protected:
	void prepareViewHook() override;
	std::unique_ptr<Row> createRow(not_null<History*> history) override;

private:
	const not_null<Main::Session*> _session;

};

QuickDestinationsBoxController::QuickDestinationsBoxController(
		not_null<Main::Session*> session)
: ChatsListBoxController(session)
, _session(session) {
}

Main::Session &QuickDestinationsBoxController::session() const {
	return *_session;
}

void QuickDestinationsBoxController::rowClicked(
		not_null<PeerListRow*> row) {
	delegate()->peerListSetRowChecked(row, !row->checked());
}

QString QuickDestinationsBoxController::savedMessagesChatStatus() const {
	return tr::lng_saved_forward_here(tr::now);
}

void QuickDestinationsBoxController::prepareViewHook() {
	delegate()->peerListSetTitle(tr::lng_settings_quick_destinations());

	auto selected = std::vector<not_null<PeerData*>>();
	for (const auto id : session().settings().quickDestinationIds()) {
		if (const auto peer = session().data().peerLoaded(id)) {
			if (!peer->isRepliesChat()
				&& !peer->isVerifyCodes()
				&& !peer->isForum()) {
				selected.emplace_back(peer);
			}
		}
	}
	delegate()->peerListAddSelectedPeers(selected);
}

std::unique_ptr<ChatsListBoxController::Row>
QuickDestinationsBoxController::createRow(not_null<History*> history) {
	return IsEligibleDestination(history->peer)
		? std::make_unique<Row>(history)
		: nullptr;
}

void SetupContent(
		not_null<Ui::VerticalLayout*> content,
		not_null<Window::SessionController*> controller) {
	const auto session = &controller->session();
	session->api().requestMoreDialogsIfNeeded();

	Ui::AddSkip(content);
	Ui::AddSubsectionTitle(
		content,
		tr::lng_settings_quick_destinations_chats());

	const auto preview = content->add(object_ptr<FilterChatsPreview>(
		content,
		Data::ChatFilter::Flags(),
		LoadedHistories(session, session->settings().quickDestinationIds())));
	preview->peerRemoved(
	) | rpl::on_next([=](not_null<History*> history) {
		auto ids = session->settings().quickDestinationIds();
		ids.erase(ranges::remove(ids, history->peer->id), end(ids));
		SaveDestinations(session, std::move(ids));
	}, preview->lifetime());
	session->changes().peerUpdates(
		Data::PeerUpdate::Flag::Name
		| Data::PeerUpdate::Flag::Rights
	) | rpl::filter([=](const Data::PeerUpdate &update) {
		return ranges::contains(
			session->settings().quickDestinationIds(),
			update.peer->id);
	}) | rpl::on_next([=] {
		preview->updateData(
			Data::ChatFilter::Flags(),
			LoadedHistories(
				session,
				session->settings().quickDestinationIds()));
	}, preview->lifetime());

	const auto choose = content->add(object_ptr<Ui::SettingsButton>(
		content,
		tr::lng_settings_quick_destinations_choose(),
		st::settingsButtonNoIcon));
	choose->setClickedCallback([=, weakPreview = QPointer(preview)] {
		auto editableAtOpen = base::flat_set<PeerId>();
		for (const auto id : session->settings().quickDestinationIds()) {
			if (session->data().peerLoaded(id)) {
				editableAtOpen.emplace(id);
			}
		}
		auto boxController = std::make_unique<
			QuickDestinationsBoxController>(session);
		auto initBox = [=](not_null<PeerListBox*> box) {
			box->setCloseByOutsideClick(false);
			box->addButton(tr::lng_settings_save(), [=] {
				auto ids = std::vector<PeerId>();
				for (const auto id
						: session->settings().quickDestinationIds()) {
					if (!editableAtOpen.contains(id)) {
						ids.push_back(id);
					}
				}
				for (const auto peer : box->collectSelectedRows()) {
					if (IsEligibleDestination(peer)) {
						ids.push_back(peer->id);
					}
				}
				SaveDestinations(session, std::move(ids));
				if (weakPreview) {
					weakPreview->updateData(
						Data::ChatFilter::Flags(),
						LoadedHistories(
							session,
							session->settings().quickDestinationIds()));
				}
				box->closeBox();
			});
			box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
		};
		controller->show(
			Box<PeerListBox>(std::move(boxController), std::move(initBox)));
	});

	Ui::AddDividerText(
		content,
		tr::lng_settings_quick_destinations_about());
}

class QuickDestinations final : public Section<QuickDestinations> {
public:
	QuickDestinations(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent();

};

QuickDestinations::QuickDestinations(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

rpl::producer<QString> QuickDestinations::title() {
	return tr::lng_settings_quick_destinations();
}

void QuickDestinations::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	SetupContent(content, controller());
	Ui::ResizeFitChild(this, content);
}

const auto kMeta = BuildHelper({
	.id = QuickDestinations::Id(),
	.parentId = ChatId(),
	.title = &tr::lng_settings_quick_destinations,
	.icon = &st::menuIconForward,
}, [](SectionBuilder &builder) {
	builder.add(nullptr, [] {
		return SearchEntry{
			.id = u"quick-destinations/chats"_q,
			.title = tr::lng_settings_quick_destinations_chats(tr::now),
			.keywords = {
				u"quick"_q,
				u"forward"_q,
				u"destinations"_q,
				u"chats"_q,
			},
		};
	});
});

} // namespace

Type QuickDestinationsId() {
	return QuickDestinations::Id();
}

namespace Builder {

SectionBuildMethod QuickDestinationsSection = kMeta.build;

} // namespace Builder
} // namespace Settings
