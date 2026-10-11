# Native process custody

This standalone library supplies process spawning, descriptor transport and a
separate command owner executable. The caller waits only its direct helper;
the helper retains the workload's wait authority. Workload exit and cleanup
settlement are separate events.

`Cleanup_scope` qualifies completion receipts. `UNSPECIFIED` means that the
sender supplied no recognized scope capability and cannot establish a scoped
cleanup claim. Linux and Windows owners report `TRANSITIVE_TREE`. Darwin owners
report `OWNED_PROCESS_GROUPS`. Optional receipt extensions have their own tags,
versions and lengths; an absent or unknown extension does not upgrade a claim.

## Darwin cleanup contract and technical debt

Darwin command ownership covers the directly owned root and its original
process group. The owner starts the root in a new session, observes its exit
without consuming its wait status, and retains the root zombie until a group
membership snapshot contains only that zombie. This pins the original numeric
group while signaling it. Only then does the owner reap its root. It never
signals a saved group after reaping, opens signal authority by a discovered PID,
or acquires another parent's exclusive wait.

Independent generation containment and orphan adoption are unavailable on
Darwin. `Process_tree_containment::establish()` reports that limitation, and
`terminate_coordinates()` returns false. Losing the final owner leaves cleanup
unconfirmed; helper exit, observation failure, an empty pipe, and root exit are
not replacement settlement receipts. A caller must retain and display the
unconfirmed outcome where cleanup cannot be established.

Deliberately detached descendants, including descendants that create a new
session, may survive. This is accepted technical debt for the initial native
Mac scope, not transitive process-family containment. Ordinary foreground and
background terminal jobs are not classified as deliberately detached: their
controlling-PTY lifecycle belongs to the terminal backend. The command helper's
group receipt does not establish cleanup of interactive jobs moved into other
groups. Applications must use the terminal backend for that lifecycle.
The Darwin command helper rejects `Owner_mode::PTY` before spawning a workload;
the existing native terminal backend supplies interactive terminals.

Darwin process references are observation-only local kqueues. Since macOS does
not transfer kqueues through `SCM_RIGHTS`, a STARTED receipt carries the original
public `PROC_PIDTBSDINFO` birth identity in microseconds. The receiver checks that
identity before and after registering its own `NOTE_EXIT` observer. An already
exited workload may have no receiver observer while its birth and root-exit
receipts remain valid. Never consume the kqueue event: `poll(POLLIN)` remains
latched after the original occurrence dies. A consumer that verifies live
executable identity must check that observer before and after verification.
Neither a birth token nor a local observer authorizes a numeric signal.

Darwin control transport uses bounded length-prefixed Unix stream records.
Transferred descriptors belong to the first byte of one record and are received
close-on-exec. Receive timeouts retain partial records and their descriptors.
A malformed or oversized frame closes the stream. A bounded send owns every
partial write; if a partial record cannot complete, it closes the stream rather
than permitting a replay from an ambiguous byte boundary. Callers serialize
writes and abandon failed START requests.

The native tests cover the stable ownership, framing, descriptor and deadline
contracts. They do not establish integrated terminal job-control behavior,
authenticated Soter recovery, or application edition limits.
