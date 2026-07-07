# Demo output — restos-ledger

A real 3-node Raft cluster over TCP (2026-07-07, unedited). Boot with `scripts/cluster.sh`,
then drive it with `restos-cli` — the CLI follows the leader via the REDIRECT hint, and
writes are replicated and committed by a majority before they return.

```text
$ scripts/cluster.sh
restos-node 0 listening on 5000 with 2 peers
restos-node 1 listening on 5001 with 2 peers
restos-node 2 listening on 5002 with 2 peers

$ CLI="restos-cli --nodes 127.0.0.1:5000,127.0.0.1:5001,127.0.0.1:5002"
$ $CLI put order:42 '2x zurek, 1x kompot'    # appended by the leader, replicated
OK 1
$ $CLI put order:43 'kotlet schabowy'
OK 2
$ $CLI get order:42                             # served from the committed state machine
VALUE 2x zurek, 1x kompot
$ $CLI get order:43
VALUE kotlet schabowy
$ $CLI get order:99                             # absent key
NULL
```

Leader election, replication safety, no-commit-without-majority, re-election after a leader
partition, and crash-restart recovery are all covered by the deterministic simulation tests
(`ctest`), which run under AddressSanitizer and ThreadSanitizer in CI.
