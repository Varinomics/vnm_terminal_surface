# Contribution agreement administration

This procedure administers the [contribution policy](contribution-policy.md).
Ordinary contributors accept online in their pull request. Maintainers do
not manually register each signer or publish a separate material-review
record for every change.

## Automatic acceptance and evidence

The trusted CLA workflow asks uncovered contributors to read the applicable
[individual](individual-cla-v1.1.md) or
[corporate](corporate-cla-v1.1.md) agreement and reply with the displayed
acceptance command. It processes the authenticated comment-creation event,
not an arbitrary quoted statement or text supplied by another account.
The acceptance is recorded automatically on the repository's protected
`contribution-records` branch and reused for future pull requests to that
repository under the same agreement version and contributing capacity.

Keep the exact agreement bytes and SHA-256 digest with each acceptance.
Agreement text is canonical UTF-8 with LF line endings. Preserve the
authenticated numeric actor ID and login, exact assent, acceptance time,
source comment/event identity, repository and pull-request scope, agreement
identifier/version and digest, and applicable corporate name and authority
statement. Corporate authorisations also identify the covered accounts and
work. The retained receipt and agreement snapshot survive later edits or
deletion of the source comment; editable comments and temporary Actions logs
alone are not the archive.

The immutable receipt path is
`assents/v1.1/COMMENT_ID.json` on `contribution-records`, with an accepted
agreement snapshot and workflow run/event reference. The online-signing
records have their own schema; adding this capability does not rewrite
published 1.0 agreements or legacy evidence.

The record branch uses the repository's existing write permissions:
authorised repository writers and trusted automation using the repository's
privileged, repository-scoped `GITHUB_TOKEN` can update it. Repository writers
and workflow editors are trusted maintainers within that boundary. Ordinary
fork contributors without repository write access cannot update the records
or choose the privileged workflow token's permissions.

The automatic writer is **GitHub Actions**, app ID **15368**; it is not a
separate actor allowlist enforced by a record-branch update ruleset.
**imakris**, numeric account ID **20025756**, is the current human writer and
configured correction custodian. Correction records must name a configured
custodian as verifier; tooling validates that declared verifier. Those roles
confer no personal CLA coverage or ownership exemption. Retain the record
branch's no-force-push and no-deletion
history rules, and the product branch's required CLA and pull-request gates
without a product-branch bypass. Proposing check or record changes in a
product pull request does not grant that contribution coverage.

Retain a recoverable copy of the accepted agreement snapshots and records.
Verify recovery when maintaining that storage. Additional private documents
are not a routine prerequisite; retain them privately only when an actual
authority question, correction, separate permission or legal matter needs
them. Follow [privacy and retention](privacy.md).

## Individual and company capacity

An individual acceptance grants the rights that person controls. A corporate
acceptance names the Entity and explicitly represents permission to
contribute and authority to bind it to the CLA. It grants the accepting
person's and Entity's relevant rights together; do not require an additional
individual CLA for those same rights. Ordinary acceptance is recorded from
that explicit representation without mandatory postal details, identity
documents, emailed signatures or custodian approval.

If the contributor cannot bind the Entity, an authorised representative may
accept online for the Entity and identify the additional contributors and
covered work. A roster is needed only for that additional-author route.
The representative cannot grant personal or other third-party rights they
do not control. Obtain those rights through the relevant author's own
acceptance or a sufficient separate grant. A new employer or rights holder
requires new appropriate acceptance; company affiliation alone does not
extend an existing grant to another Entity.

The commands are `/cla agree individual v1.1` and
`/cla agree corporate v1.1 {"company":"Exact legal entity","role":"Position or signing authority"}`.
Corporate acceptance may include `"authors":["@alice","@bob"]` for other
authors whose rights are controlled by the representative. A representative
with a matching corporate acceptance can later use
`/cla authorize v1.1 {"company":"Exact legal entity","authors":["@alice"]}`.
Only authenticated newly created comments are processed as assent; each
named handle is resolved to its numeric GitHub identity in the retained
record. The accepting representative must actually control the stated rights.

Ordinary review should resolve a concrete ambiguity or contradiction in the
rights statements. Ask for additional authority evidence when there is an
actual reason to question the representation, rather than requiring everyone
to complete a second corporate process. Confidential evidence must not be
posted in a public pull request; arrange private handling for the particular
case if necessary. Neither a self-attestation nor an emailed document is
conclusive proof of authority.

## Authorship and third-party review

The check assesses all relevant authors against the recorded individual or
corporate rights. Preserve authorship through cherry-picks and squashes and
resolve genuinely unlinked identities rather than silently excluding them.
Do not exempt owners, maintainers, organisation members or bot accounts
merely because of their role. Identify the actual rights basis for automated
or imported work.

Review imported or adapted material using the
[third-party procedure](third-party-materials.md), inspect its actual licence
and retain origin and obligation records with the change. Use normal
pull-request review and approval for that acceptance decision. A CLA check
can pass before the import has been reviewed; the maintainer must complete
the actual licence review before merging it. There is no separate per-PR
CLI material-review ledger for ordinary original work.

For an imported change, use `Material origin: imported-or-adapted` in the
pull-request body and identify maintained repository-relative paths in
`Provenance records:`. A current-head maintainer GitHub approval records the
normal review decision. A maintainer may instead explicitly attest the
completed import review with `/cla review imports HEAD_SHA`, including when
the maintainer is the pull-request author. In either case the maintainer
must actually inspect the licence and obligations; the syntax does not
perform that review. Ordinary original work uses `Material origin: original`.

## Changes, corrections and exceptions

A recorded corporate-authorisation ending stops automatic reliance on that
coverage for all new merges, including pending pull requests containing older
work. It does not revoke a valid perpetual licence or erase the acceptance
receipt. A pending contribution can use fresh sufficient coverage or, where
appropriate, a narrowly verified exception establishing an actual prior grant
for that exact contribution. Ordinary contributions do not acquire a new
manual-review requirement.

Distinguish this administrative ending from discovery that an acceptance,
identity assertion or rights representation was invalid. An older green
check cannot establish missing rights; correct disputed coverage before
accepting affected work. Preserve the original receipt and linked ending or
correction record rather than rewriting the historical acceptance. Record
an ending when it takes effect; do not promise future expiry timers or infer
per-head submission histories from commit timestamps.

For that ending, append an entry to `online_signing.authority_endings` with
`id`, `receipt_id`, `github_id`, `reason`, `evidence_id`, `evidence_sha256`,
`verified_by` and `verified_at`. The `receipt_id` identifies the corporate
acceptance or later company-authorisation receipt, such as `comment-123`.
Use `github_id: null` to end all automatic reliance on that receipt, or a
numeric account ID to end only that author's reliance. Retain the archived
notice or other necessary evidence referenced by the entry. Do not invalidate
or rewrite the original receipt merely to end corporate authority.

Publish the changed control through
`node tools/contribution_coverage.js publish-records --repository OWNER/REPO --ruleset PRODUCT_RULESET_ID --file PATH_TO_CONTROL_JSON`.
The new control generation makes the ending effective for current acceptance
and requires fresh check results; it is not a future-scheduled expiry.
New corporate acceptance or authorisation can supply new coverage only where
it actually grants sufficient current rights. Existing exact-subject
exceptions remain available to establish a prior grant for specific pending
work without constructing a general per-head history.

Only the correction custodian may approve an exception, for an identified
repository, pull request, proposed head and contributor/subject, with the
reason and actual rights basis. Retain the decision, date, actor and necessary
evidence. Demonstrated company ownership, a separate sufficient grant or a
justified noncopyrightable change can support a narrow decision. A temporary
permission is not a substitute for the required continuing rights, and an
exception cannot approve third-party material or waive its obligations.

The custodian's correction tools must make stale coverage unavailable for
the affected acceptance and request fresh checks. Do not bypass merge rules
to force a green result. Use the repository's administration tooling for the
rare correction or exception; contributors do not need to run it for online
signing or ordinary pull requests.

## Agreement versions and historic work

Published 1.0 agreements remain byte-for-byte unchanged. Their old email
execution route is not the current online workflow. Version 1.1 has its own
identifier, exact text and acceptance evidence; no signature is silently
migrated between versions. A new version requires a new appropriate online
acceptance when it is adopted.

Acceptance grants the relevant rights actually controlled by the accepting
person or named Entity in original Contributions intentionally submitted to
this repository before or after acceptance. It begins at actual acceptance,
not at an invented past signature. Merely publishing this process does not
prove that all existing code can be offered under alternative terms, and it
does not grant another owner's rights in historic or imported material.
Lost or genuinely disputed evidence must be recovered or replaced by a
sufficient grant, never fabricated.
