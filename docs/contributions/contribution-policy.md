# Contribution policy

Policy version: **1.1**, published **2026-11-06**.

This policy applies to Varinomics GPL repositories that adopt it in their
`CONTRIBUTING.md`. Contributors retain ownership while granting Varinomics the
rights needed to distribute and sublicense their Contributions under GPL and
alternative terms, including proprietary terms and downstream sublicensing.
The policy does not change a repository's published GPL licence or rights
already granted to its recipients.

CLA acceptance is reused per repository and agreement version; it is not
required for every push. The repository owner, **imakris**, may make ordinary
direct maintenance pushes without signing a CLA. That access creates no
acceptance receipt and supplies no rights in another person's material.
Contributor pull requests still use the automated coverage checks, and
external authorship and imported-material obligations remain applicable.

## Accept online when the CLA bot asks

Open your pull request normally. If the relevant agreement is not already
recorded for your GitHub account, the bot links its exact text and shows the
acceptance command. Read it and reply from your own account. The bot records
your acceptance and updates the required CLA check automatically. There is
no emailed form, document upload or routine manual registration.

For individual acceptance reply:

```text
/cla agree individual v1.1
```

Choose the capacity that matches the rights in the work:

- **Individual:** accept the [individual CLA v1.1](individual-cla-v1.1.md) for
  original work whose necessary rights you control.
- **Company or other Entity:** use the
  [corporate CLA v1.1](corporate-cla-v1.1.md), name the Entity and expressly
  confirm authority to bind it. This one acceptance grants your own and the
  Entity's relevant rights; a second individual signature is not required.
  If you cannot bind the Entity, its authorised representative can use the
  online procedure to cover the relevant authors and work. They can grant
  only rights they actually control.

For corporate acceptance use your actual legal entity and authority details:

```text
/cla agree corporate v1.1 {"company":"Exact legal entity","role":"Position or signing authority"}
```

An authorised representative can include an `authors` list or use the
additional-author command described in the corporate agreement. An ordinary
signer contributing only their own work needs no roster.

Acceptance grants the relevant rights you actually control in your original
Contributions intentionally submitted to this repository before or after
acceptance. The grant begins on actual acceptance, not an invented earlier
signature. It does not sign for a
different repository or a new employer. Future pull requests reuse the
record automatically unless a new agreement version or capacity is needed.
Use a new appropriate acceptance when the rights holder changes.

Every relevant author must be covered, including commit authors, co-authors
and contributors whose work arrived through patches or squashes. Identify
all authors, preserve attribution and required notices, and use
`Co-authored-by` trailers where appropriate. Unresolved identities or missing
rights prevent acceptance; do not remove authorship to make the check pass.
A corporate representative may cover other authors only to the extent the
representative controls the required rights. Any remaining personal rights
need that author's own online acceptance.

GitHub Actions retains the exact assent, authenticated account identity,
source event, time, applicable agreement version and digest, and agreement
snapshot. Editing or deleting the original comment does not erase that
evidence or revoke a valid grant. Public pull requests and their recorded
acceptances are public, including a named company. See [privacy](privacy.md).

## Disclose imported or adapted material

Use the pull-request template to identify material copied or adapted from
elsewhere, including code snippets, documentation, tests, data, fonts and
artwork. State its origin, revision, authors or rights holders, actual
licence and modifications. Do not submit confidential work or work you lack
permission to disclose.

**A CLA acceptance does not approve an import.** Maintainers must inspect its
actual licence and preserve origins and obligations using the
[third-party material procedure](third-party-materials.md). GPL compatibility
does not establish permission for proprietary sublicensing. Obtain the needed
permission, document a valid distribution boundary, or leave the material out.

Maintainers perform that review as part of ordinary pull-request review.
Original changes do not require a separately curated material-review ledger
or a maintainer's CLI registration before their CLA check can pass. Approved
imports must still have maintained provenance, licence texts and notices
where required. Automated coverage is not a replacement for actual licence
review or a guarantee about undisclosed authorship or authority.

## Versions, disputes and exceptions

Published agreement versions remain immutable. The retained 1.0 documents do
not change, and acceptance of 1.1 is recorded against its own text. A new
version does not rewrite an earlier acceptance. An existing file or historic
commit is not cleared for alternative licensing merely because this policy
exists; other owners' rights still need their own established basis.

Authority changes, disputed identities or mistaken records can be raised on
the affected pull request. Maintainers use the
[administration procedure](administration.md) for corrections and narrowly
scoped exceptions. A custodian role, organisation membership, bot account or
small change is not automatically a rights grant. An exception must establish
sufficient continuing rights for the exact contribution and never overrides
third-party terms or approves an import by itself.

When corporate authorisation is recorded as ended, automatic reliance on
that coverage stops for new merges, including older pending pull requests.
Actual perpetual licences already validly granted remain in effect and their
receipts remain retained. Pending work needs fresh sufficient coverage or a
rare verified exception establishing the prior grant for that contribution.
This is distinct from finding that a record was false or invalid.
