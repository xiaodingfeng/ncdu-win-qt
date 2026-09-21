#pragma once

// What a "software data folder" row currently is, and the one rule that decides
// whether the move button may act on it.
//
// The state constants live here rather than inside the panel because the rule
// below is checked directly by the probe lab, and it is worth checking: the
// case it exists for — this app's own interrupted move — carries a link target
// AND a failed state, so a simpler "has a link target, therefore not movable"
// test gets that case exactly backwards and leaves the user with no way forward
// except an operation that undoes the copy.
//
// Pure header: no Qt, no Windows, no state. Include it anywhere.

namespace MoveSelect {

constexpr int kStateIdle = 0;
constexpr int kStateCopying = 1;
constexpr int kStateDeleting = 2;
constexpr int kStateLinking = 3;
constexpr int kStateDone = 4;
constexpr int kStateFailed = 5;
constexpr int kStateMoved = 6;      // already relocated, a junction sits in its place
constexpr int kStateRestoring = 7;  // copying the data back to the system drive
constexpr int kStateQueued = 8;     // waiting behind the move that is running
constexpr int kStateVerifying = 9;  // measuring the copy against the original
constexpr int kStateChecking = 10;  // finding out who holds the folder, before copying
constexpr int kStateCleaning = 11;  // dropping a leftover copy that stayed behind

// May this folder be handed to a new move?
//
//   * Moved / Done — its data already sits on the other drive; "restore" or
//     "abandon" owns that row. Decided by the STATE and not by the link target,
//     because a row can be marked relocated while its target failed to resolve,
//     and that is still not a folder to copy a second time.
//   * a link target this app did not create (knownMove false) — somebody else's
//     junction, so nothing about it is ours to move.
//   * everything else, which includes this app's own interrupted move: a failed
//     state WITH a link target. That one must stay movable, because starting the
//     move again is how it gets finished — the copy on the other drive is picked
//     up by the incremental copy rather than made a second time.
inline bool mayStartMove(int state, bool knownMove, bool hasLinkTarget)
{
    if (state == kStateMoved || state == kStateDone)
        return false;
    if (hasLinkTarget && !knownMove)
        return false;
    return true;
}

// The state a row shows once the leftover cleanup that was running on it is
// over — whatever the outcome was.
//
// "Dropping a leftover" is borrowed for the length of that job and is not
// something the folder goes on being, so a row that still shows it afterwards
// reads as busy for ever: the user is told the cleanup succeeded and the list
// says it never stopped. Everything else is left exactly as it is, because a
// failed cleanup has to be able to put "failed" (with its reason) on the row.
inline int stateAfterCleanup(int stateWhenItStarted, int stateNow)
{
    return stateNow == kStateCleaning ? stateWhenItStarted : stateNow;
}

// How many states there are. The values above run 0..kStateCount-1 without a
// gap, which is what lets the filter below index its selection by state value
// instead of carrying a lookup table around.
constexpr int kStateCount = 12;

// What "restore" may do about the programs holding the folder it has to empty.
enum RestoreGate {
    kRestoreGo,      // nobody in the way: start the hand-over
    kRestoreAsk,     // closable holders: offer to close them, then ask again
    kRestoreRefuse,  // still held: leave everything exactly as it is
};

// May the restore start emptying the original folder?
//
// A restore is the one operation that cannot be half-done: it takes the junction
// away and rewrites the original path. Doing that while a program still holds
// files in there does not merely fail, it fails in the worst way — the folder is
// emptied under the running program, the copy that comes back cannot be verified
// (the numbers keep moving), and what is left behind is a folder Explorer refuses
// to delete with the app's data on the other drive. So the question is asked
// before the junction is touched, where the answer still costs nothing, and ONE
// offer to close the holders is all there is: if they are back afterwards, that
// is a program that restarts itself, and refusing is the only honest answer left.
//
//   * blocked — something other than the system programs this app never closes
//     holds the folder.
//   * closable — how many of those the app is allowed to close at all; with none
//     of them closable there is nothing to offer and the refusal comes at once.
inline int restoreGate(bool blocked, int closable, bool offered)
{
    if (!blocked)
        return kRestoreGo;
    if (!offered && closable > 0)
        return kRestoreAsk;
    return kRestoreRefuse;
}

// Which states the filter offers, in the order it lists them. The states the
// user waits on come first: a list that is narrowed down is nearly always
// narrowed down to "what still needs me".
constexpr int kFilterOrder[] = {
    kStateIdle,     kStateFailed,    kStateMoved,     kStateDone,
    kStateQueued,   kStateChecking,  kStateCopying,   kStateVerifying,
    kStateLinking,  kStateDeleting,  kStateRestoring, kStateCleaning,
};
constexpr int kFilterOrderCount = static_cast<int>(sizeof(kFilterOrder) / sizeof(kFilterOrder[0]));

// The state filter, as a plain predicate: a row passes when nothing is
// selected (no filter at all) or when its state is one of the selected ones.
//
// The difference between "nothing selected" and "everything selected" matters
// and is why an empty selection means NO filter rather than "show nothing": the
// user reaches the empty selection by unticking the last box, and having the
// whole list vanish as a result is a puzzle, not a filter.
inline bool passesFilter(int state, const bool* selected)
{
    bool any = false;
    for (int i = 0; i < kFilterOrderCount; ++i) {
        if (selected[kFilterOrder[i]]) {
            any = true;
            break;
        }
    }
    if (!any)
        return true;
    return state >= 0 && state < kStateCount && selected[state];
}

}  // namespace MoveSelect
