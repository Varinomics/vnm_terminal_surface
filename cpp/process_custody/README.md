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
without consuming its wait status, and retains the root zombie while observing
group membership. This pins the original numeric group while signaling it.
A membership snapshot containing only that zombie permits a confirmed receipt
and the owner's sole root reap. If an authorized forced-termination attempt and
bounded observation exhaust without that snapshot, a fresh exact-root exit
observation instead permits the sole reap and a terminal `UNCONFIRMED` receipt.
For example, another
parent may retain a dead group member indefinitely. The owner reports root exit
before this receipt and exits; the caller retains the negative cleanup outcome.
A failed signal remains an error, including Darwin's `EPERM` for a group of
zombies; it cannot prove settlement or prevent an exhausted negative receipt.
A live root remains pending. Exhausted cleanup never becomes confirmed on a
later call. The owner never signals a saved group after reaping, opens signal
authority by a discovered PID, or acquires another parent's exclusive wait.

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
The terminal backend launches the same helper with `--darwin-pty-outcome-v1`.
It retains the controlling PTY session while the shell runs in a separate
foreground group. During cleanup, discovered group numbers are inputs to
`TIOCSPGRP`, which validates membership in that retained session; `TIOCSIG`
then signals the kernel-retained tty group. A fixed two-PID tty snapshot naming
only the helper establishes scoped settlement after the original root's sole
reap. Truncated or failed observations never establish emptiness. Ordinary
foreground and background jobs remain in scope after shell exit.

Before sending START, the backend requires an accepted native receipt hook:
the receiver must retain a local exact-helper status observer and persist its
identity and `darwin-pty-owner-v1` contract. Root birth and exact root exit are
separate retained receipts. This admitted helper's normal exit codes mean
`90 = NO_WORKLOAD`, `91 = CONFIRMED`, and `92 = UNCONFIRMED`. Signals, generic
exit codes, and unrelated helper modes carry no positive cleanup meaning.
The receiver consumes and latches the status event once and persists the raw
status before interpreting it; loss before persistence remains unknown.

Darwin process references are observation-only local kqueues. Since macOS does
not transfer kqueues through `SCM_RIGHTS`, a STARTED receipt carries the original
public `PROC_PIDTBSDINFO` birth identity in microseconds. The receiver checks that
identity before and after registering its own `NOTE_EXIT` observer. An already
exited workload may have no receiver observer while its birth and root-exit
receipts remain valid. STARTED establishes native birth, not successful exec.
An admitted command or PTY birth that fails before exec still supplies its exact
ROOT_EXITED receipt before START_REJECTED and final settlement. A failure before
birth, or a positively reaped child whose execution was never admitted, needs
no native birth/exit receipts.

Never consume the kqueue event: `poll(POLLIN)` remains
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
