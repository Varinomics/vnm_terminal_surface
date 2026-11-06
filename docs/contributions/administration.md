# Contribution agreement administration

This procedure administers the [contribution policy](contribution-policy.md).
It separates private executed agreements from the minimal records consumed
by GitHub checks. An entry is a custodian's attestation that the necessary
evidence was verified; automation does not read private signed documents or
determine legal authority.

## Responsibility and record locations

The initial contribution-record custodian is GitHub user **imakris**, numeric
account ID **20025756**. This is an administrative role, not a CLA signature
or evidence of ownership of any contribution. Additional custodians require
the repository owner's express appointment and an update to the trusted
configuration and record-branch protection.

Each repository has a dedicated `contribution-records` branch containing
`coverage.json`. Protect that branch against ordinary contributor updates,
force pushes and deletion. Only the configured custodian can publish record
updates. Keep product-branch merge protection requiring pull requests and
both `cla/coverage` and `contributions/material-review`, with no ordinary
bypass. Their actual required contexts include the registry generation:
`cla/coverage (records N)` and `contributions/material-review (records N)`.
A pull request's own proposed registry or workflow changes do not
provide evidence for its contribution coverage.

Keep original execution and authority evidence in a maintained, restricted
company archive, accessible only to those administering rights or handling
a necessary legal matter. The registry starts empty. Before the first entry,
the custodian must store the complete original, issue an opaque receipt and
confirm that the stored evidence can be retrieved. GitHub Actions logs,
temporary artifacts and a contributor's public comment are not the archive.

## Verify and register an individual agreement

1. Receive the entire executed [ICLA](individual-cla-v1.0.md), its execution
   record and any earlier-contributions schedule through
   `info@varinomics.com`, subject `[CLA] Individual v1.0`.
2. Confirm the signer's legal name, contact details, express assent, signature
   date and the agreement text. Resolve ambiguity privately. Verify the
   GitHub numeric account ID and associated commit email identities with the
   account holder; a matching display name or an unauthenticated commit
   email is insufficient.
3. Confirm that the signer controls the stated rights. Identify employer or
   client claims and obtain corporate or separate rights-holder evidence
   where needed. For material submitted before signing, verify that the
   signed earlier-contributions schedule covers it.
4. Archive the original executed document or electronic-signing package,
   authenticated identity correspondence and material scope. Preserve the
   signature timestamp and source record without inventing a historic date.
5. Register only verified coverage. Retain the agreement identifier, exact
   agreement digest, evidence receipt and evidence digest, signing time,
   verifier and verification time. The public individual entry also records
   the GitHub numeric ID and hashes of verified commit emails.

The legal identifiers **Varinomics-ICLA-1.0** and
**Varinomics-CCLA-1.0** correspond to registry agreement IDs
`individual-v1.0` and `corporate-v1.0`. Agreement digests use SHA-256 of the
canonical UTF-8 text with LF line endings. Preserve those exact text bytes
with the executed record; a version label or a moving URL alone is not
enough. The evidence digest identifies the archived original evidence file
or package; it is separate from the agreement-text digest. Store a stable
archive package if several documents form one evidence record.

## Corporate execution and contributor authorisation

Receive the complete executed [CCLA](corporate-cla-v1.0.md), execution record
and schedules through the same mailbox, subject `[CLA] Corporate v1.0`.
Verify the Entity's legal identity, the signatory's identity and authority
to bind it, and its control of the rights it proposes to grant. Preserve the
authority evidence with a separate receipt and digest. A domain, job title
or GitHub organisation membership alone is not sufficient proof.

Register the corporate agreement separately from each designated contributor
authorisation. Every authorisation must identify the verified contributor's
numeric account ID and commit email identities, the corporate agreement,
the Projects and work covered, effective dates, and the evidence that the
Entity authorised that person. Preserve the detailed scope privately; the
material-review custodian verifies it for each accepted contribution.

Each human author still needs an individual CLA for rights they personally
control. The corporate grant supplements it for employer/client rights.
Where the Entity does not control a necessary author's right or consent,
obtain that person's separate grant before recording complete coverage.
Contractors and affiliates must be checked on their actual rights; do not
assume they are covered by the Entity's employee roster.

Accept roster amendments or withdrawal only from a verified authorised
representative. Keep the earlier record and append the change, its evidence,
verification and effective time. Withdrawal stops future authority and does
not revoke an already valid grant. In the registry, use an authorisation's
`valid_from` and `valid_until` to delimit prospective corporate authority.
The gate checks those times against the trusted material-review `verified_at`
for the exact submitted head, not a forgeable commit-author timestamp. A valid
reviewed grant survives a later roster expiry or withdrawal; a new head
requires a new review and appropriate authority. The relevant individual and
corporate agreements must already have been executed by that review time.
Do not backdate an authorisation to
cover earlier work without an express earlier-contribution grant from the
rights holder. Review a disputed or inaccurate record promptly and prevent
acceptance of affected new material until the dispute is resolved. Withdrawal
of future authority is distinct from discovery that the necessary grant or
evidence was invalid: an old review cannot cure actual missing rights.

The registry field `withdrawn_at` has the latter meaning: it invalidates the
record's evidence or identity assertion for current acceptance, including
acceptance relying on an older review. It is not revocation of a valid
irrevocable licence, and is not the way to end only future corporate
authority. Do not schedule a future `withdrawn_at`; use it when a disputed or
mistaken record must cease to be relied upon now. Preserve the reason and
corrected evidence in the archive and replacement record.

To shorten a corporate authorisation's `valid_until`, retain the original
authorisation and signature fields and add or replace its nested `ending`
record. That record requires a new `id`, the archived corporate notice's
`evidence_id` and `evidence_sha256`, the custodian's `verified_by` and
`verified_at`, and `withdrawn_at: null`. Set the new cutoff no earlier than
that notice's `verified_at`; it cannot retroactively remove grants already
reviewed. Each further shortening needs a new ending ID and a verification
time no earlier than the preceding ending's. A finite cutoff may be shortened
but cannot be extended or removed by mutating the record. The original signed
roster's finite cutoff needs no `ending` until it is shortened. Git retains
the previous cutoffs and notice records. An ending's `withdrawn_at` invalidates
its evidence for current acceptance in the same way as other withdrawn proof.

## Material review for every pull request

Review the proposed head, all commit authors and co-authors, the submitter,
and the disclosure in the pull-request body. Record each additional author
the review identifies, even if no commit metadata names them. Correct
unresolved attribution or obtain a documented exact-subject exception; do
not silently drop a contributor from coverage.

The material-review record identifies the pull request, exact head, SHA-256
of its disclosure body, reviewer and review time. Record
`rights_confirmed: true` only after verifying the rights basis, including
the applicable execution records and any earlier-contribution scope.
Exhaustively enumerate authors and their rights paths: individually held
contribution rights in `individual_github_ids`, and corporate rights in
`corporate_authorization_ids`. These lists also declare additional authors
whose names do not appear in Git metadata; the checker includes them in its
coverage assessment. A contributor can require both routes, and personal
coverage does not substitute for any separately declared corporate rights.
Every relevant human must have reviewed capacity and individual
CLA coverage unless a valid exact-subject exception establishes another
rights basis.

For original work, record the `original` disposition. For imported or adapted
material, apply [third-party review](third-party-materials.md), record
`imports-approved` only after that review, and name the maintained provenance
files in `provenance_paths`. The trusted record is the maintainer's retained
attestation; the source/licence records remain in the repository. The
checker does not independently determine whether an import is compatible
with GPL, alternative terms or a release's obligations.

A changed proposed head or disclosure needs a matching new review. Do not
copy an old decision forward without inspecting the changes. A bot-generated
dependency update still needs rights and provenance review; the bot account
does not establish who owns imported code.

## Exceptions and corrections

Only an appointed custodian may approve an exception. Record the exact
repository, pull request, proposed head and author/subject; the reason;
the alternative rights basis; the private evidence receipt and digest;
and the verifier, time and limited scope. Demonstrated company
ownership, a separate sufficient grant, or a justified noncopyrightable
change may support a narrow decision. "Maintainer", "bot", "trusted",
"small change" and "signed off" alone do not establish the necessary rights.
An exception must establish sufficient continuing rights for the exact
contribution. Do not register a time-limited permission as an exception to
the required perpetual and irrevocable grant. The exception attests
`rights_irrevocable: true`; an exception with `valid_until` is rejected.
There is no promise of a
timer that revokes a previously green check at a future expiry instant.

An exception does not approve imported material and cannot waive another
person's licence, notices or rights. A material review must still address
the proposed work. There is no general emergency merge bypass. Correct the
record or remove the affected material rather than forcing a green status.

Preserve corrections and withdrawal history instead of silently overwriting
past evidence. Each published registry has a monotonically increasing
generation, starting at `0`. To publish generation `N+1`, the administration
tool first replaces only the two required contribution contexts in the
existing product-branch ruleset with `cla/coverage (records N+1)` and
`contributions/material-review (records N+1)`. It then publishes registry
`N+1` and requests checks against those records. Old results or an in-flight
checker using `N` cannot satisfy the newly required contexts. If publication
stops between those operations, merging remains blocked; resume the same
publication rather than restoring older required contexts or bypassing them.
A newer generation does not migrate signatures to another agreement version.

### Administration tooling

Run the repository's [administration tool](../../tools/contribution_coverage.js)
with Node.js from its repository root. Use the custodian's existing GitHub
CLI authentication and provide `GH_TOKEN` in the process environment; do not
place a token in arguments, records, logs or documentation. Record publication
requires repository-administration permission for its merge-rules update,
along with the contents and status permissions needed to publish and check
records. Verification of legal documents and authority remains a human step.

The one-time bootstrap creates an empty records branch and the minimal
trusted check caller. It does not sign an agreement or establish any coverage:

```text
node tools/contribution_coverage.js bootstrap --repository Varinomics/vnm_terminal_surface --custodian 20025756
```

Keep the working copy of the next registry outside product source and prepare
only evidence-backed entries. The tool validates the record structure and
preservation of existing evidence; its validation does not prove that a
private grant is genuine or sufficient. Use these operations with the actual
registry and exact pull-request body files:

```text
node tools/contribution_coverage.js validate-records --file PATH_TO_COVERAGE_JSON
node tools/contribution_coverage.js publish-records --repository Varinomics/vnm_terminal_surface --ruleset PRODUCT_RULESET_ID --file PATH_TO_COVERAGE_JSON
node tools/contribution_coverage.js email-hash --email VERIFIED_COMMIT_EMAIL
node tools/contribution_coverage.js disclosure-hash --file PATH_TO_EXACT_PR_BODY_TEXT
```

Repeat setup for each adopting repository, preserving its repository identity
and branch protections. After publication, verify the required generation,
registry generation and resulting exact-head statuses agree. A green result
from before a rights correction is not evidence that the corrected state
was reviewed.

## Versions, retention and recovery

Do not edit a published agreement under its existing version identifier.
Issue a new identifier and document the change. Keep the old canonical
agreement text and every execution record that relies on it. Do not migrate
a signature to a new version merely by editing the registry.

Follow the [contribution privacy and retention notice](privacy.md). Preserve
the evidence needed to establish continuing licences and required claims;
review necessity at least annually. Minimise access and public metadata.
Keep a recoverable backup of the restricted archive and record branch;
verify retrieval before relying on a record. Removing an account, closing
a pull request or ending employment does not by itself erase evidence
needed for a licence already relied upon.

If evidence is lost or cannot be verified, report the affected record
accurately and withhold new acceptance that depends on it. Recover the
evidence or obtain a new sufficient grant; do not fabricate a signature,
verification date or legal clearance. The initial empty registry makes no
representation that historic commits are cleared for alternative licensing.
