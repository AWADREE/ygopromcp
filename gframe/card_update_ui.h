#ifndef YGOPROMCP_CARD_UPDATE_UI_H
#define YGOPROMCP_CARD_UPDATE_UI_H

namespace ygo {
class Game;

enum class CardUpdateAction { Status, Check, Download, Schedule, Rollback, Cancel, Cleanup };

// All element mutations happen on the game thread. Backend work never touches GUI.
bool CardUpdateUiAvailable();
void CardUpdateUiOpen(Game& game);
void CardUpdateUiAction(Game& game, CardUpdateAction action);
void CardUpdateUiPoll(Game& game);
void CardUpdateUiShutdown();
}
#endif
