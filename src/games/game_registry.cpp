#include "game_registry.h"

#include "cipher_sprint.h"
#include "firewall_frenzy.h"
#include "packet_chase.h"
#include "signal_memory.h"

#include <string.h>

// ── HexHound - Minigame Registry Implementation ─────────────────

namespace {

// Function-local rather than a namespace-scope array: taking the address of a
// Meyers singleton is not a constant expression, so a file-scope array would
// need dynamic initialisation and an init-order guard. This has neither.
Minigame** table(int& n) {
    // Order is the order the select screen shows, so it is the order a new
    // player meets them: the two reflex-and-memory games first, then the two
    // that ask for a judgement.
    static Minigame* entries[] = {
        &PacketChase::instance(),
        &SignalMemory::instance(),
        &FirewallFrenzy::instance(),
        &CipherSprint::instance()
    };
    n = (int)(sizeof(entries) / sizeof(entries[0]));
    return entries;
}

}  // namespace

int games::count() {
    int n = 0;
    table(n);
    return n;
}

Minigame* games::at(int index) {
    int n = 0;
    Minigame** t = table(n);
    if (index < 0 || index >= n) return nullptr;
    return t[index];
}

Minigame* games::byId(const char* id) {
    if (!id) return nullptr;
    int n = 0;
    Minigame** t = table(n);
    for (int i = 0; i < n; i++) {
        if (strcmp(t[i]->id(), id) == 0) return t[i];
    }
    return nullptr;
}

int games::playableCount() {
    int n = 0;
    Minigame** t = table(n);
    int c = 0;
    for (int i = 0; i < n; i++) {
        if (playable(t[i])) c++;
    }
    return c;
}

Minigame* games::playableAt(int index) {
    if (index < 0) return nullptr;
    int n = 0;
    Minigame** t = table(n);
    int c = 0;
    for (int i = 0; i < n; i++) {
        if (!playable(t[i])) continue;
        if (c == index) return t[i];
        c++;
    }
    return nullptr;
}
