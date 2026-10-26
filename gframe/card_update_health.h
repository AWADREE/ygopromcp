#ifndef YGOPROMCP_CARD_UPDATE_HEALTH_H
#define YGOPROMCP_CARD_UPDATE_HEALTH_H

namespace ygo {
class Game;

// No-op without a launcher-owned pending transaction. With one, this returns
// true only after the actual client loaded the managed archive, its databases,
// and support scripts, then atomically wrote a process-bound health receipt.
bool WriteCardUpdateStartupHealth(Game& game);
}

#endif
