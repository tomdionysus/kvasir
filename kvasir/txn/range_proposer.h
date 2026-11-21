//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "dannan/agree/agreement.h"
#include "kvasir/txn/range_commit.h"

// A range's proposer on one node (TODO/2026-11-20-commit-protocol.md, "A
// commit on one range", step 2): the commits sent to it within a short window
// go as one batch, one agreed change, so a burst of commits costs one round
// and one sync per replica. One change is in flight at a time, so batches are
// decided in the order they were made. Every node may run one for a range at
// once: ballots and the version check inside the change keep that safe, and
// only cost the losers a retry or a refusal.
//
// Thread safety: none of its own. Its calls, and the completions it is given,
// run on the one executor it is built with (Dannan's agreement posts there).
// It outlives the answer to its change in flight, which Dannan posts once.
namespace kvasir::txn {

class RangeProposer {
 public:
  // How a commit ended: committed or refused by the range, or unknown (the
  // change may or may not have taken effect: read the commit id back).
  enum class Answer : uint8_t { committed, refused, unknown };
  using Done = std::function<void(Answer)>;
  // A wait the caller's clock runs: the simulator's schedule, or a timer.
  using Schedule = std::function<void(std::chrono::milliseconds, std::function<void()>)>;

  struct Settings {
    std::string space;
    dannan::agree::Slot slot = 0;
    dannan::agree::Name name;
    std::vector<dannan::identity::NodeId> replicas;
    uint64_t version = 0;
    // A batch waits this long for more commits, then goes.
    std::chrono::milliseconds window{10};
    // A batch goes at once when it holds this many.
    size_t most = 64;
    // Each change's deadline.
    std::chrono::milliseconds deadline{2000};
  };

  RangeProposer(dannan::agree::Agreement& agreement, boost::asio::any_io_executor executor, Settings settings, Schedule schedule);

  // Adds the commit to the next batch; `done` is called once, never inline.
  // After stop(), it is answered unknown at once (posted).
  void submit(Commit commit, Done done);

  // Before the agreement it proposes on goes: no batch is sent after it, and
  // each commit not yet sent is answered unknown (it took no effect, but its
  // caller cannot tell that from a lost answer). The batch in flight is
  // answered as the agreement answers it, if it does.
  void stop();

  // Batches sent, for a test that the proposer batches at all.
  size_t batches() const noexcept { return _batches; }

 private:
  struct Pending {
    Commit commit;
    Done done;
  };
  void arm();
  void send();

  dannan::agree::Agreement& _agreement;
  boost::asio::any_io_executor _executor;
  const Settings _settings;
  const Schedule _schedule;
  std::vector<Pending> _pending;
  bool _armed = false;
  bool _in_flight = false;
  bool _stopped = false;
  size_t _batches = 0;
};

}  // namespace kvasir::txn
