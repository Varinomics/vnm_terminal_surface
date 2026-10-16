# Releasing vnm_terminal_surface

This library is published as a tagged source release on GitHub. The terminal
application's packages are built and published by `vnm_terminal`; follow its
[release guide](https://github.com/Varinomics/vnm_terminal/blob/master/docs/releasing.md)
in the sibling checkout at `../vnm_terminal/docs/releasing.md`.

## Coordinate the release

Every new `vnm_terminal_surface` release must be accompanied by a coordinated
`vnm_terminal` release. Use the same version in both repositories' `project()`
declarations, tags, and GitHub releases. Choose an unused version for the pair
after inspecting both repositories' latest published releases and unreleased
changes. Matching numbers record release coordination; build compatibility
depends on the required targets and API, not version equality.

Start from current `master` in both repositories, preserving local work. Compare
the surface changes with its previous release tag and with the surface revision
recorded in the preceding terminal release's `release/dependencies.lock.json`.
The two comparisons identify library changes and changes newly shipped by the
application, respectively. Owned source dependency declarations continue to
track `master`; the terminal release lock records the exact build inputs for
provenance and rebuilds.

## Prepare and validate

1. Finalize the shared version in `CMakeLists.txt` and prepare release notes for
   the selected source. Describe caller-visible changes and required migration,
   including public C++ layout changes that require consumers to rebuild.
   macOS hosts must supply the retained native receipt hook and deploy the
   custody helper described in the
   [backend lifecycle contract](backend_lifecycle_and_signals.md).
2. Obtain independent review, then stage, commit, and push the surface changes
   to `master`. Follow the terminal release guide to refresh its dependency
   lock and commit and push its corresponding release changes. Keep the surface
   publication ahead of the application so that the consumer sees the intended
   provider API.
3. Use the applicable hosted Linux, Windows, macOS, and Downstream Windows
   workflows to validate the selected sources. Downstream Windows builds the
   terminal application's `master` against the surface under test; its advisory
   status does not make a concrete failure safe to ignore. Use the
   [change-to-test matrix](change_to_test_matrix.md) to select any relevant check
   not covered by applicable CI. Do not duplicate hosted validation locally.

## Publish and finish the pair

Create an annotated `v<version>` tag on the reviewed, validated surface release
commit and push the tag. Publish a GitHub release from that tag with the prepared
notes. Verify that the release and its source archives resolve to the intended
tagged source. This source release has no application installer or signing
pipeline.

Complete the matching terminal release using its release guide. Verify that its
tag carries the dependency lock identifying the selected surface release commit,
and that its release workflows finish and publish the required application
artifacts. Report the coordinated release complete only when both GitHub
releases and the terminal artifacts are available. Apply the shared release
payload requirements to the application's staged and packed deliverables through
that existing release process.
