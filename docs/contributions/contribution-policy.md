# Contribution policy

Policy version: **1.0**, published **2026-11-06**.

This policy applies to Varinomics GPL repositories that link to it from
their `CONTRIBUTING.md`. It governs acceptance into those repositories;
it does not change their published GPL licences or the rights people already
receive under those licences. Varinomics may distribute material it has the
necessary rights to license under both GPL and alternative terms, including
proprietary terms.

## Before submitting

1. Read the repository's `CONTRIBUTING.md` and describe the proposed change.
2. Each human author and co-author must have a verified
   [individual CLA](individual-cla-v1.0.md) covering their contribution. The
   author keeps ownership and grants Varinomics licensing rights, including
   downstream sublicensing under GPL and alternative terms.
3. Identify any employer, client or other rights holder. Work owned or
   controlled by an organisation also requires a verified
   [corporate CLA](corporate-cla-v1.0.md) and authorisation for the relevant
   contributors and work, or an expressly approved alternative rights record.
   An individual's signature cannot grant an employer's rights.
4. Disclose imported or adapted material, even when its source is another
   open-source project, a forum answer, an asset collection or an earlier
   employer's code. Identify source, revision, affected files, rights holders
   and the actual licence. Do not submit confidential material or material
   you lack permission to disclose.
5. Identify all authors, including authors carried through cherry-picks,
   squashes and patches sent outside GitHub. Preserve authorship and required
   notices. Use `Co-authored-by` trailers for co-authors and explain identities
   that GitHub cannot resolve; metadata alone does not prove ownership.

Sign by sending the complete executed agreement and its record to
**info@varinomics.com** with the subject specified in the form. This mailbox
route is for agreement administration. Do not post signatures, residential
addresses, identity documents or corporate authority evidence publicly.
The administrator verifies the record before recording coverage. Merely
sending a form does not turn the check green. See [privacy](privacy.md).

## What is checked before merging

The required GitHub status families are **`cla/coverage (records N)`** and
**`contributions/material-review (records N)`**, where `N` is the published
registry generation required by the repository's merge rules. The first
checks recorded coverage for
the pull-request submitter, commit authors, co-authors and other contributors
identified in the review record. The second requires a maintainer's review
of the exact proposed head and disclosure. Both must pass before merge.

Unresolved identities, missing signatures, missing corporate authorisation,
or an incomplete material review prevent acceptance. A changed head or
changed disclosure requires a corresponding new review. Repository ownership,
maintainer status, organisation membership or a bot account is not an
automatic exception. The checks rely on verified records; they do not prove
the truth of undisclosed authorship, legal identity or ownership claims.

Records are administered on the repository's separate, protected
`contribution-records` branch. A product pull request cannot grant itself
coverage by editing a registry file. Use the [administration procedure](administration.md)
for correction, corporate roster changes and narrowly scoped exceptions.
Publishing changed records advances the required generation before replacing
the registry, so results produced from an older generation cannot satisfy
the current merge requirements.

## Third-party material has a separate acceptance decision

**A CLA signature never approves an import.** Maintainers must inspect the
actual licence and record the origin and obligations before acceptance.
GPL compatibility alone does not establish that Varinomics can offer the same
material under alternative terms. Approval must cover the proposed GPL and
alternative distribution routes, or expressly delimit the material and the
permitted route so no broader rights are claimed.

Use the [third-party material procedure](third-party-materials.md). Keep
approved provenance records with the repository and preserve required licence
texts and notices. If the necessary permission is unavailable, leave the
material out or obtain a suitable separate licence; do not represent it as
the signer's unrestricted original work.

## Versions, earlier contributions and exceptions

Agreement identifiers and exact accepted text are retained with execution
records. A new published agreement does not rewrite an existing signature.
Maintainers may require a new agreement for future acceptance when a change
in required rights warrants it; they must retain the old records.

The initial registry is empty. This policy creates no signature or ownership
evidence for existing commits. Earlier material requires its own rights
review and, where needed, an express grant covering the identified earlier
contributions. Do not infer that an existing file is available for alternative
licensing from its presence in a GPL repository.

Exceptions require an authorised custodian's recorded decision for the exact
repository, pull request, head and subject, with a concrete rights basis and
evidence. Examples include demonstrated Varinomics ownership or material
whose necessary permissions are already established separately. An exception
cannot waive another rights holder's rights or replace import review. There
is no automatic exemption for small, documentation-only or automated changes.
An exception must establish sufficient continuing rights for that exact
contribution; temporary permission is not a substitute for the required
perpetual, irrevocable grant.

## Maintainer responsibility

The maintainer verifies actual authorship and contribution capacity, the
applicable execution record and corporate scope, and any third-party
provenance. A successful check is necessary for merging but does not replace
that judgment. Corrections, disputed authority and rights problems must be
resolved before accepting the affected material. The procedure does not
obligate Varinomics to accept a contribution or require contributors to
maintain it afterwards.
