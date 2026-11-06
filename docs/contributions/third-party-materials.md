# Imported and adapted material

The [contribution policy](contribution-policy.md) requires disclosure and
maintainer review of imported or adapted material independently of CLA
coverage. A signer can grant only rights they actually control. An import
does not become unrestricted original work through a signature, a rewrite
of its header, a squash commit or attribution to the submitter.

## Contributor disclosure

Use the pull-request template's material disclosure. State whether the work
is original or contains imported or adapted material. For each import,
identify:

- the upstream source URL, project and exact release/revision or original
  publication, with the affected source and destination paths;
- known authors and copyright holders;
- the actual licence text and version, including file-specific exceptions,
  notices and any separate permission;
- what was copied, generated from or adapted, and what was changed;
- known restrictions, patents, attribution and source-delivery obligations;
- the employer, client or other rights holder for work not individually owned.

Disclose code snippets, documentation, tests, fixtures, fonts, icons, images,
data and generated artifacts as well as whole libraries. Identify the inputs
and applicable permissions for generated material. For material produced
with assistance tools, disclose any known imported sources and restrictions;
the tool is not evidence of authorship or licence clearance. Do not claim a
licence merely because a public page permits viewing or downloading a file.

## Maintainer acceptance

Inspect the actual upstream licence and file notices at the identified
revision, rather than relying solely on GitHub's detected licence, an SPDX
label, a contributor's summary or a CLA result. Check whether the contributor
can grant rights to their changes and whether other holders' licences allow
the intended combination and distribution.

Assess the GPL route and the proposed alternative licensing route separately.
GPL-only permission for someone else's code is not permission to sublicense
that code under proprietary terms. If separate licensing, exclusion from an
alternative distribution, or a genuinely separate component supplies a valid
route, document exactly that boundary and its consequences. Do not describe
all material as alternatively licensable when only some material is.

Record the duties that apply to the actual use: licence and copyright
notices, attribution, modification notices, source offers or matching source,
relinking or installation information, copyleft scope, and applicable
patent or trademark conditions. Keep required upstream text unaltered.
For an update, inspect licence changes and the affected obligations again.
Reject or leave out material whose necessary rights or source cannot be
established; a custodian exception cannot supply missing third-party rights.

## Keep the provenance with the accepted material

Use an existing suitable record under `THIRD_PARTY/` where one already owns
the dependency or asset. Otherwise add a durable record under
`THIRD_PARTY/contributions/` with the imported change. The record must contain:

```text
Material and repository paths:
Origin URL and exact upstream revision/release/publication:
Authors/copyright holders and preserved notices:
Licence identifier/version and path to the actual licence text:
Import or adaptation description, including modifications:
Permitted GPL distribution and combination:
Permitted alternative distribution, or its precise exclusion/boundary:
Required notices, attribution and modification notices:
Source/relink/installation obligations and the delivery route, if applicable:
Separate permissions or restrictions and evidence reference, if any:
Maintainer, decision date and pull request:
```

Retain public origin and licence facts in the repository. Keep confidential
negotiated permissions in the restricted agreement archive and use an opaque
evidence reference in the public record. Update `THIRD_PARTY_NOTICES.md`,
`NOTICE`, distributed licence texts and release source/access material where
the accepted licence requires it. The maintainer must inspect the actual
licence and provenance before approving the import in normal pull-request
review. A successful CLA check does not represent that import decision, and
ordinary original changes need no separate protected-branch material ledger.

Existing third-party notices and licence records remain the starting
inventory for older material. Adding this process does not certify their
completeness, manufacture historic grants or establish that all existing
code can be offered under alternative terms. Rights review of a new change
must include earlier material it actually depends on where that affects
the proposed distribution rights.

## Reference points

- The [GNU GPL v3](https://www.gnu.org/licenses/gpl-3.0.html), particularly
  sections 4-7, 10 and 11, governs the GPL distribution path; other material's
  actual licence may add separate compatible obligations.
- The [Apache individual CLA](https://www.apache.org/licenses/icla.pdf)
  distinguishes a contributor's original creation from separately identified
  third-party submissions. Its signature does not replace upstream licence
  review. Varinomics uses its own agreements.
