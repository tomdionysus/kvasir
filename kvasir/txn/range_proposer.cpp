//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/txn/range_proposer.h"

#include <boost/asio/post.hpp>
#include <utility>

namespace kvasir::txn {

RangeProposer::RangeProposer(dannan::agree::Agreement& agreement, boost::asio::any_io_executor executor, Settings settings, Schedule schedule)
    : _agreement(agreement), _executor(std::move(executor)), _settings(std::move(settings)), _schedule(std::move(schedule)) {}

void RangeProposer::submit(Commit commit, Done done) {
  if (_stopped) return boost::asio::post(_executor, [done = std::move(done)] { done(Answer::unknown); });
  _pending.push_back({std::move(commit), std::move(done)});
  if (_pending.size() >= _settings.most && !_in_flight) return send();
  arm();
}

// The window: the batch goes when it closes, unless one is still in flight,
// whose end sends it.
void RangeProposer::arm() {
  if (_armed || _in_flight || _stopped) return;
  _armed = true;
  _schedule(_settings.window, [this] {
    _armed = false;
    if (!_in_flight) send();
  });
}

void RangeProposer::stop() {
  _stopped = true;
  for (auto& pending : _pending) boost::asio::post(_executor, [done = std::move(pending.done)] { done(Answer::unknown); });
  _pending.clear();
}

void RangeProposer::send() {
  if (_pending.empty() || _stopped) return;
  auto batch = std::move(_pending);
  _pending.clear();
  std::vector<Commit> commits;
  commits.reserve(batch.size());
  for (const auto& pending : batch) commits.push_back(pending.commit);
  _in_flight = true;
  ++_batches;
  _agreement.change(_settings.space, _settings.slot, {_settings.name}, _settings.replicas, _settings.version, commit_change(std::move(commits)), {},
                    _settings.deadline, _executor, [this, batch = std::move(batch)](dannan::agree::Outcome outcome) {
                      _in_flight = false;
                      std::vector<Verdict> verdicts;
                      if (outcome.committed) verdicts = decode_verdicts(outcome.result);
                      for (size_t i = 0; i < batch.size(); ++i) {
                        const auto answer = !outcome.committed || i >= verdicts.size() ? Answer::unknown
                                            : verdicts[i] == Verdict::committed        ? Answer::committed
                                                                                       : Answer::refused;
                        // Posted, never inline: a commit's caller may submit again from its answer.
                        boost::asio::post(_executor, [done = batch[i].done, answer] { done(answer); });
                      }
                      // What arrived while this batch was in flight goes next (not after stop()).
                      if (!_pending.empty()) arm();
                    });
}

}  // namespace kvasir::txn
