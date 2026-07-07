#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>

#include "restos/kv.hpp"
#include "restos/log.hpp"
#include "sim.hpp"

using namespace restos;

namespace {
RaftConfig fast() { return RaftConfig{100, 300, 30}; }

std::string temp_path() {
    static int counter = 0;
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto p = std::filesystem::temp_directory_path() /
             ("restos_ledger_test_" + std::to_string(stamp) + "_" +
              std::to_string(counter++) + ".log");
    std::filesystem::remove(p);
    std::filesystem::remove(std::filesystem::path(p.string() + ".meta"));
    return p.string();
}
}  // namespace

TEST(KV, ApplyPutDel) {
    KVStateMachine kv;
    kv.apply("put a hello world");
    kv.apply("put b 2");
    EXPECT_EQ(kv.get("a"), "hello world");
    EXPECT_EQ(kv.get("b"), "2");
    kv.apply("del a");
    EXPECT_FALSE(kv.get("a").has_value());
    EXPECT_EQ(kv.size(), 1u);
}

TEST(LogStore, PersistsAndReloads) {
    auto path = temp_path();
    {
        LogStore s(path);
        s.append({1, 1, "put a 1"});
        s.append({1, 2, "put b 2"});
        s.save_meta(3, 1, 2);
    }
    LogStore s2(path);
    EXPECT_EQ(s2.last_index(), 2u);
    EXPECT_EQ(s2.at(2)->command, "put b 2");
    EXPECT_EQ(s2.current_term(), 3u);
    EXPECT_EQ(s2.commit_index(), 2u);
    std::filesystem::remove(path);
    std::filesystem::remove(path + ".meta");
}

TEST(Raft, ElectsExactlyOneLeader) {
    SimCluster c(3, fast());
    c.node(0).set_election_timeout(120);  // node 0 wins the race
    c.node(1).set_election_timeout(10000);
    c.node(2).set_election_timeout(10000);
    c.advance(300);
    EXPECT_EQ(c.leader_count(), 1);
    EXPECT_EQ(c.leader_id(), 0);
    EXPECT_GE(c.node(0).term(), 1u);
}

TEST(Raft, ReplicatesAndCommitsOnMajority) {
    SimCluster c(3, fast());
    c.node(0).set_election_timeout(120);
    c.node(1).set_election_timeout(10000);
    c.node(2).set_election_timeout(10000);
    c.advance(300);
    ASSERT_EQ(c.leader_id(), 0);

    ASSERT_TRUE(c.node(0).client_append("put a 1"));
    c.advance(400);
    for (int i = 0; i < 3; ++i) {
        EXPECT_GE(c.node(i).commit_index(), 1u) << "node " << i;
        EXPECT_EQ(c.sm(i).get("a"), "1") << "node " << i;
    }
}

TEST(Raft, LeaderStepsDownOnHigherTerm) {
    SimCluster c(3, fast());
    c.node(0).set_election_timeout(120);
    c.node(1).set_election_timeout(10000);
    c.node(2).set_election_timeout(10000);
    c.advance(300);
    ASSERT_EQ(c.leader_id(), 0);
    uint64_t t1 = c.node(0).term();

    // a single message from a higher term must make the leader step down immediately
    Message higher{MsgType::AppendEntries, 2, 0, t1 + 5};
    c.node(0).receive(higher);
    EXPECT_EQ(c.node(0).role(), Role::Follower);
    EXPECT_EQ(c.node(0).term(), t1 + 5);
}

TEST(Raft, ClusterReElectsAfterLeaderPartition) {
    SimCluster c(3, fast());
    c.node(0).set_election_timeout(120);   // node 0 wins first
    c.node(1).set_election_timeout(250);   // node 1 is next in line
    c.node(2).set_election_timeout(10000);
    c.advance(300);
    ASSERT_EQ(c.leader_id(), 0);
    uint64_t t1 = c.node(0).term();

    c.set_down(0, true);   // the leader fails
    c.advance(1500);       // the remaining majority must elect a new leader at a higher term

    bool one_new_leader = (c.node(1).role() == Role::Leader) != (c.node(2).role() == Role::Leader);
    EXPECT_TRUE(one_new_leader);
    uint64_t new_term = std::max(c.node(1).term(), c.node(2).term());
    EXPECT_GT(new_term, t1);
}

TEST(Raft, NoCommitWithoutMajority) {
    SimCluster c(3, fast());
    c.node(0).set_election_timeout(120);
    c.node(1).set_election_timeout(10000);
    c.node(2).set_election_timeout(10000);
    c.advance(300);
    ASSERT_EQ(c.leader_id(), 0);

    c.set_down(1, true);
    c.set_down(2, true);
    ASSERT_TRUE(c.node(0).client_append("put a 1"));
    c.advance(300);
    EXPECT_EQ(c.node(0).commit_index(), 0u);      // alone, no majority
    EXPECT_FALSE(c.sm(0).get("a").has_value());

    c.set_down(1, false);                         // one follower returns → majority of 2/3
    c.advance(400);
    EXPECT_GE(c.node(0).commit_index(), 1u);
    EXPECT_EQ(c.sm(0).get("a"), "1");
}

TEST(Raft, StateRecoversAfterCrashRestart) {
    auto path = temp_path();
    RaftConfig cfg = fast();
    {
        KVStateMachine sm;
        LogStore store(path);
        RaftNode node(0, {}, [](const Message&) {}, &sm, &store, cfg);
        node.set_election_timeout(100);
        node.tick(1);
        node.tick(200);  // single-node cluster elects itself
        ASSERT_EQ(node.role(), Role::Leader);
        ASSERT_TRUE(node.client_append("put a 1"));
        ASSERT_TRUE(node.client_append("put b 2"));
        EXPECT_EQ(node.commit_index(), 2u);
        EXPECT_EQ(sm.get("a"), "1");
    }
    {  // crash + restart: fresh objects, same files
        KVStateMachine sm2;
        LogStore store2(path);
        RaftNode node2(0, {}, [](const Message&) {}, &sm2, &store2, cfg);
        EXPECT_EQ(store2.commit_index(), 2u);
        EXPECT_EQ(sm2.get("a"), "1");
        EXPECT_EQ(sm2.get("b"), "2");
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path + ".meta");
}
