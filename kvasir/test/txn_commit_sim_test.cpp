//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The commit protocol for one range (TODO/2026-11-20-commit-protocol.md, "What
// the simulator proves") under Dannan's simulator: three nodes run Dannan's
// real Agreement and Kvasir's commit change while the world drops, duplicates
// and delays messages and kills and restarts nodes. Clients on every node run
// transactions as list appends over a few rows, each reading its rows with a
// strict read and writing them at the versions it read. Then:
// - the history keeps serializability (Dannan's isolation checker);
// - every append answered as committed is in the final state, unless the
//   recorded faults excuse its loss at its level; none refused is in it;
// - of two writers of one row from one version, at most one commits (the
//   one rule), so a lost update is a failure;
// - a change whose outcome was unknown is applied at most once.
// A change that skips the version check (the one rule broken on purpose) must
// be refused, or the checks have never been shown to refuse anything.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dannan/test/fixtures/sim/agreement.h"
#include "dannan/test/fixtures/sim/durability.h"
#include "dannan/test/fixtures/sim/history.h"
#include "dannan/test/fixtures/sim/serializable.h"
#include "kvasir/txn/range_commit.h"
#include "kvasir/txn/range_proposer.h"

namespace kvasir::txn {
namespace {

namespace sim = dannan::test::sim;
using namespace std::chrono_literals;

constexpr dannan::agree::Slot range_slot = 0;
const dannan::agree::Name range_name{'r'};
const std::vector<std::string> keys{"x", "y", "z"};

// A row's value: its list's elements, comma separated.
std::vector<uint64_t> list_of(const std::string& value) {
  std::vector<uint64_t> out;
  size_t start = 0;
  while (start < value.size()) {
    const auto comma = value.find(',', start);
    out.push_back(std::stoull(value.substr(start, comma - start)));
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return out;
}

std::string with(const std::string& value, uint64_t element) { return value.empty() ? std::to_string(element) : value + "," + std::to_string(element); }

// The broken change: every write applied whatever version its row is at now.
dannan::agree::Change unchecked_change(std::vector<Commit> batch) {
  return [batch = std::move(batch)](std::span<const std::optional<dannan::agree::Bytes>> current) {
    auto state = current.empty() || !current[0] ? RangeState{} : decode_range(*current[0]);
    auto unchecked = batch;
    for (auto& transaction : unchecked)
      for (auto& write : transaction.writes) write.expected = state.rows.contains(write.key) ? state.rows.at(write.key).version : 0;
    const auto verdicts = apply_batch(state, unchecked);
    dannan::agree::Changed changed{{encode_range(state)}, encode_verdicts(verdicts)};
    changed.repeatable = true;
    return changed;
  };
}

struct Run {
  std::optional<std::string> failure;
  size_t committed = 0;
  size_t refused = 0;
  bool settled = false;
  // Batches the proposers sent, and commits submitted to them, when batching.
  size_t batches = 0;
  size_t submitted = 0;
};

// The level the acceptors' store gives a commit: `replicated` syncs each keep
// before answering; `decided` answers from memory and syncs now and then, and
// a crashed acceptor comes back not ready and catches up from a majority
// before it votes again (Dannan's rule for a store that may have lost state).
// A majority killed together then stalls the range; it never chooses twice.
// `batching`: each node runs a RangeProposer, and its clients' commits go
// through it in batches, every node proposing on the one range at once.
Run simulate(uint64_t seed, bool broken, sim::AckLevel level = sim::AckLevel::replicated, bool batching = false) {
  const bool decided = level == sim::AckLevel::decided;
  sim::World world(seed);
  sim::NetworkFaults faults;
  faults.drop_one_in = 10;
  faults.duplicate_one_in = 10;
  faults.max_delay_ms = 30;
  sim::Cluster cluster(world, 3, faults, !decided);
  sim::History history;
  Run run;

  // Each node's proposer, made again when the node restarts; the old ones live
  // to the run's end, since a change in flight may still answer.
  std::vector<RangeProposer*> proposers(cluster.size(), nullptr);
  std::vector<std::unique_ptr<RangeProposer>> made;
  const auto propose_on = [&](size_t i) {
    RangeProposer::Settings settings;
    settings.space = sim::Cluster::space;
    settings.slot = range_slot;
    settings.name = range_name;
    settings.replicas = cluster.ids();
    settings.version = sim::Cluster::genesis_version;
    made.push_back(std::make_unique<RangeProposer>(
        cluster.agreement(i), world.io.get_executor(), settings,
        [&world](std::chrono::milliseconds after, std::function<void()> task) { world.after(static_cast<uint64_t>(after.count()), std::move(task)); }));
    proposers[i] = made.back().get();
  };
  if (batching)
    for (size_t i = 0; i < cluster.size(); ++i) propose_on(i);

  // A restart: as it synced, and at `decided` not ready until it has caught up.
  std::vector<uint64_t> incarnation(cluster.size(), 0);
  std::function<void(size_t, uint64_t)> catch_up = [&](size_t i, uint64_t at) {
    if (!cluster.up(i) || incarnation[i] != at) return;
    cluster.agreement(i).catch_up(sim::Cluster::space, range_slot, {range_name}, cluster.ids(), sim::Cluster::genesis_version, 1000ms, world.io.get_executor(),
                                  [&, i, at](dannan::agree::Outcome outcome) {
                                    if (!cluster.up(i) || incarnation[i] != at) return;
                                    if (outcome.committed) return cluster.store(i).set_ready(true);
                                    // A stalled range retries until the run's window ends, so the world drains.
                                    if (world.now_ms() < 8000) world.after(100, [&, i, at] { catch_up(i, at); });
                                  });
  };
  const auto restart = [&](size_t i) {
    ++incarnation[i];
    cluster.restart(i, false, !decided);
    if (batching) propose_on(i);
    if (decided) catch_up(i, incarnation[i]);
  };
  // At `decided`, each node syncs now and then: a sync is an event the losses are judged by.
  if (decided)
    for (int s = 0; s < 30; ++s)
      world.after(world.random.below(2200), [&] {
        const auto i = world.random.below(cluster.size());
        if (!cluster.up(i)) return;
        cluster.store(i).sync();
        history.fault({world.now_ms(), "sync", {i}, ""});
      });

  // A commit's appends: one element per row, each unique across the history.
  struct Pending {
    size_t index = 0;
    std::vector<uint64_t> elements;
  };
  std::vector<Pending> appends;

  const auto up_node = [&](size_t from) {
    for (size_t tries = 0; tries < cluster.size(); ++tries, from = (from + 1) % cluster.size())
      if (cluster.up(from)) return std::optional<size_t>(from);
    return std::optional<size_t>();
  };

  // Clients at random times over two simulated seconds: a read-modify-append of one or two
  // rows, or a read of two.
  constexpr size_t operations = 40;
  for (uint64_t n = 0; n < operations; ++n) {
    // Batching runs pack the clients closer, so commits meet in a window.
    world.after(world.random.below(batching ? 400 : 2000), [&, n] {
      const auto at = up_node(world.random.below(cluster.size()));
      if (!at) return;
      std::vector<std::string> chosen{keys[world.random.below(keys.size())]};
      if (world.random.one_in(3)) {
        const auto other = keys[world.random.below(keys.size())];
        if (other != chosen[0]) chosen.push_back(other);
      }
      const bool reading = world.random.one_in(4);
      std::vector<sim::Statement> statements;
      for (const auto& key : chosen) statements.push_back({sim::Statement::Kind::read, key, 0, {}});
      // The appends' elements: one per row, unique across the history (n, then the row).
      std::vector<uint64_t> elements;
      if (!reading)
        for (size_t j = 0; j < chosen.size(); ++j) {
          elements.push_back((n + 1) * keys.size() + j);
          statements.push_back({sim::Statement::Kind::append, chosen[j], elements.back(), {}});
        }
      const auto index = history.invoke(n % 5, chosen, sim::encode_statements(statements), world.now_ms(), *at);
      cluster.agreement(*at).read(sim::Cluster::space, range_slot, {range_name}, cluster.ids(), sim::Cluster::genesis_version, {}, 1000ms,
                                  world.io.get_executor(), [&, index, statements, elements, n, at = *at, reading](dannan::agree::Outcome read) mutable {
                                    if (!read.committed) return;  // never answered: it took no effect, nothing was written
                                    const auto state = read.values.empty() || !read.values[0] ? RangeState{} : decode_range(*read.values[0]);
                                    Commit transaction{CommitId{at + 1, 1, n + 1}, {}};
                                    size_t j = 0;
                                    for (auto& statement : statements) {
                                      if (statement.kind != sim::Statement::Kind::read) continue;
                                      const auto row = state.rows.find(statement.key);
                                      statement.list = row == state.rows.end() ? std::vector<uint64_t>{} : list_of(row->second.value);
                                      if (!reading)
                                        transaction.writes.push_back({statement.key, row == state.rows.end() ? 0 : row->second.version,
                                                                      with(row == state.rows.end() ? std::string() : row->second.value, elements[j++])});
                                    }
                                    if (reading) return history.answer(index, sim::encode_reads(statements), world.now_ms());
                                    if (!cluster.up(at)) return;
                                    appends.push_back({index, elements});
                                    if (batching) ++run.submitted;
                                    if (batching)
                                      return proposers[at]->submit(transaction, [&, index, statements](RangeProposer::Answer answer) {
                                        if (answer == RangeProposer::Answer::unknown) return;  // it may or may not have taken effect
                                        if (answer == RangeProposer::Answer::committed) {
                                          ++run.committed;
                                          history.answer(index, sim::encode_reads(statements), world.now_ms());
                                        } else {
                                          ++run.refused;
                                          history.cancel(index, world.now_ms());
                                        }
                                      });
                                    cluster.agreement(at).change(sim::Cluster::space, range_slot, {range_name}, cluster.ids(), sim::Cluster::genesis_version,
                                                                 broken ? unchecked_change({transaction}) : commit_change({transaction}), {}, 2000ms,
                                                                 world.io.get_executor(), [&, index, statements](dannan::agree::Outcome outcome) {
                                                                   if (!outcome.committed) return;  // unknown: it may or may not have taken effect
                                                                   if (decode_verdicts(outcome.result).at(0) == Verdict::committed) {
                                                                     ++run.committed;
                                                                     history.answer(index, sim::encode_reads(statements), world.now_ms());
                                                                   } else {
                                                                     ++run.refused;
                                                                     history.cancel(index, world.now_ms());
                                                                   }
                                                                 });
                                  });
    });
  }
  // Kills and restarts: a node at a time, sometimes two together, each kept as it synced.
  for (int k = 0; k < 4; ++k) {
    world.after(200 + world.random.below(1600), [&] {
      // At `decided`, a node is killed only while every other is up and caught up, as Dannan's
      // own runs do; a pair killed together is the one majority loss, and stalls the range.
      if (decided)
        for (size_t i = 0; i < cluster.size(); ++i)
          if (!cluster.up(i) || !cluster.store(i).ready()) return;
      std::vector<uint64_t> killed{world.random.below(cluster.size())};
      if (world.random.one_in(decided ? 6 : 3)) killed.push_back((killed[0] + 1) % cluster.size());
      for (const auto i : killed) {
        if (!cluster.up(i)) continue;
        // The proposer stops before the agreement it proposes on goes.
        if (batching) proposers[i]->stop();
        cluster.crash(i);
      }
      history.fault({world.now_ms(), "kill", killed, ""});
      world.after(50 + world.random.below(300), [&, killed] {
        for (const auto i : killed)
          if (!cluster.up(i)) restart(i);
        history.fault({world.now_ms(), "restart", killed, ""});
      });
    });
  }
  world.run_until([] { return false; }, 2'000'000);

  // The final state, read once everything is quiet and every node is up. A
  // range stalled by a majority's lost state never settles: its history is
  // still checked below, and nothing it answered may contradict another.
  cluster.faults() = sim::NetworkFaults{};
  for (size_t i = 0; i < cluster.size(); ++i)
    if (!cluster.up(i)) restart(i);
  std::optional<RangeState> final;
  cluster.agreement(0).read(sim::Cluster::space, range_slot, {range_name}, cluster.ids(), sim::Cluster::genesis_version, {}, 5000ms, world.io.get_executor(),
                            [&](dannan::agree::Outcome read) {
                              if (read.committed) final = read.values.empty() || !read.values[0] ? RangeState{} : decode_range(*read.values[0]);
                            });
  world.run_until([&] { return final.has_value(); }, 2'000'000);
  for (const auto& proposer : made) run.batches += proposer->batches();
  const auto serializable = [&] {
    const auto verdict = sim::check_isolation(history, sim::Isolation::serializable);
    if (!verdict.ok) run.failure = verdict.anomalies.front().kind + ": " + verdict.anomalies.front().why;
    return run;
  };
  if (!final) return serializable();
  run.settled = true;

  // Each element in the final lists, and how many times.
  std::map<uint64_t, size_t> present;
  for (const auto& [key, row] : final->rows)
    for (const auto element : list_of(row.value)) ++present[element];
  for (const auto& append : appends) {
    const auto& operation = history.operations().at(append.index);
    const auto name = "commit " + std::to_string(append.index);
    // Each of its rows' elements once, or none of them: a commit is whole.
    size_t seen = 0;
    for (const auto element : append.elements) {
      const auto times = present.contains(element) ? present.at(element) : 0;
      if (times > 1) return run.failure = name + " was applied twice", run;
      seen += times;
    }
    if (seen != 0 && seen != append.elements.size())
      return run.failure = name + " is in " + std::to_string(seen) + " of its " + std::to_string(append.elements.size()) + " rows", run;
    if (operation.answer == sim::Answer::cancelled && seen) return run.failure = "refused " + name + " was applied", run;
    if (operation.answer == sim::Answer::ok && !seen &&
        !sim::loss_excused(history.faults(), level, operation.invoked_ms, *operation.answered_ms, operation.node, cluster.size()))
      return run.failure = name + " was committed and lost, no fault excusing it", run;
  }
  return serializable();
}

TEST(TxnCommitSim, OneRangeKeepsSerializabilityAndLosesNothingUnderFaults) {
  size_t committed = 0, refused = 0, settled = 0;
  for (uint64_t seed = 1; seed <= 40; ++seed) {
    const auto run = simulate(seed, false);
    ASSERT_FALSE(run.failure) << "seed " << seed << ": " << *run.failure;
    committed += run.committed;
    refused += run.refused;
    settled += run.settled ? 1 : 0;
  }
  // Not idle: the runs settled, committed, and met the one rule's refusal.
  EXPECT_EQ(settled, 40u);
  EXPECT_GT(committed, 200u);
  EXPECT_GT(refused, 10u);
}

// At `decided`: acceptors answer from memory and sync now and then. A
// minority's crash loses nothing a majority holds; a majority killed together
// stalls the range, and what was answered before never contradicts itself.
TEST(TxnCommitSim, AtDecidedAMinorityLosesNothingAndAMajorityStalls) {
  size_t committed = 0, settled = 0;
  for (uint64_t seed = 1; seed <= 40; ++seed) {
    const auto run = simulate(seed, false, sim::AckLevel::decided);
    ASSERT_FALSE(run.failure) << "seed " << seed << ": " << *run.failure;
    committed += run.committed;
    settled += run.settled ? 1 : 0;
  }
  // Not idle: fewer than at replicated, since a stalled range commits nothing after its stall.
  EXPECT_GT(committed, 100u);
  // Most runs see no majority killed together, and settle; some do, and stall.
  EXPECT_GT(settled, 20u);
  EXPECT_LT(settled, 40u);
}

// Every node's proposer batching its clients' commits on the one range at
// once: safety holds, and commits share batches (fewer batches than commits
// sent).
TEST(TxnCommitSim, ProposersOnEveryNodeBatchAndStaySafe) {
  size_t committed = 0, refused = 0, batches = 0, submitted = 0;
  for (uint64_t seed = 1; seed <= 40; ++seed) {
    const auto run = simulate(seed, false, sim::AckLevel::replicated, true);
    ASSERT_FALSE(run.failure) << "seed " << seed << ": " << *run.failure;
    EXPECT_TRUE(run.settled) << "seed " << seed;
    committed += run.committed;
    refused += run.refused;
    batches += run.batches;
    submitted += run.submitted;
  }
  // Not idle. Packed onto three rows, most commits contend and the version
  // check refuses them, so few commit: the point here is safety and sharing.
  EXPECT_GT(committed, 25u);
  EXPECT_GT(refused, 10u);
  // Commits shared batches.
  EXPECT_LT(batches, submitted);
}

TEST(TxnCommitSim, AChangeWithoutTheVersionCheckIsCaught) {
  size_t caught = 0;
  for (uint64_t seed = 1; seed <= 40; ++seed)
    if (simulate(seed, true).failure) ++caught;
  EXPECT_GT(caught, 0u);
}

}  // namespace
}  // namespace kvasir::txn
