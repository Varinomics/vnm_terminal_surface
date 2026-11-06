"use strict";

const assert = require("node:assert/strict");
const fs     = require("node:fs");
const path   = require("node:path");
const test   = require("node:test");
const checker = require("../contribution_coverage.js");

// These synthetic records exercise the owner's coverage contract; none is an executed agreement.
const REPOSITORY_ID = 31001;
const CUSTODIAN     = 21010;
const AUTHOR_A      = 21001;
const AUTHOR_B      = 21002;
const NOW = Date.parse("2026-11-06T12:00:00Z");
const TEXTS = Object.fromEntries(Object.entries(checker.AGREEMENT_PATHS).map(([id, file]) =>
    [id, fs.readFileSync(path.resolve(__dirname, "../..", file), "utf8")]));

function verification(id)
{
    return { id, verified_by: CUSTODIAN, verified_at: "2026-11-05T12:00:00Z", withdrawn_at: null };
}

function evidence(id)
{
    return { evidence_id: `synthetic:${id}`, evidence_sha256: checker.sha256(`synthetic:${id}`) };
}

function individual(id)
{
    return {
        ...verification(`individual-${id}`), ...evidence(`individual-${id}`),
        github_id: id,
        email_sha256: [checker.email_digest(`author-${id}@example.invalid`)],
        agreement_id: "individual-v1.0",
        agreement_sha256: checker.text_digest(TEXTS["individual-v1.0"]),
        signed_at: "2026-11-05T11:00:00Z"
    };
}

function pull_request()
{
    return { number: 7, state: "open", user: { id: AUTHOR_A, login: "synthetic-author" },
        body: "Material origin: original\n\nSynthetic test contribution.\n",
        head: { sha: "a".repeat(40) }, commits: 1 };
}

function material_review(pr)
{
    return {
        ...verification("review-007"), pull_request: pr.number, head_sha: pr.head.sha,
        disclosure_sha256: checker.text_digest(pr.body), rights_confirmed: true,
        disposition: "original", provenance_paths: [], individual_github_ids: [AUTHOR_A],
        corporate_authorization_ids: []
    };
}

function fixture()
{
    const pr = pull_request();
    const registry = checker.empty_registry(REPOSITORY_ID, [CUSTODIAN], TEXTS);
    registry.individuals.push(individual(AUTHOR_A));
    registry.material_reviews.push(material_review(pr));
    return { pr, registry, actors: [{ github_id: AUTHOR_A }] };
}

function add_corporate(registry, author = AUTHOR_A)
{
    registry.corporations.push({
        ...verification("corporate-example"), ...evidence("corporate-example"), entity_id: "entity-example",
        agreement_id: "corporate-v1.0",
        agreement_sha256: checker.text_digest(TEXTS["corporate-v1.0"]),
        signed_at: "2026-11-05T11:00:00Z",
        authority_evidence_id: "synthetic:signatory-authority",
        authority_evidence_sha256: checker.sha256("synthetic:signatory-authority")
    });
    registry.corporate_authorizations.push({
        ...verification("authorization-example"), ...evidence("authorization-example"),
        corporation_id: "corporate-example", github_id: author, email_sha256: [],
        valid_from: "2026-11-05T11:00:00Z", valid_until: null,
        work_scope: "Synthetic project contribution named in the executed schedule"
    });
    registry.material_reviews[0].corporate_authorization_ids.push("authorization-example");
}

function evaluate(item)
{
    checker.validate_registry(item.registry, REPOSITORY_ID, TEXTS);
    return checker.evaluate_coverage(item.pr, item.actors, item.registry, NOW);
}

test("agreement and disclosure hashes use UTF8 LF; verified email aliases ignore case and edge whitespace", () => {
    assert.equal(checker.text_digest("first\r\nsecond\r\n"), checker.text_digest("first\nsecond\n"));
    assert.equal(checker.email_digest(" Author@Example.invalid "), checker.email_digest("author@example.invalid"));
});

test("all metadata authors need coverage, including co-authors and a distinct submitter", () => {
    const item = fixture();
    item.actors.push({ github_id: AUTHOR_B });
    assert.equal(evaluate(item).coverage_ok, false);
    item.registry.individuals.push(individual(AUTHOR_B));
    item.registry.material_reviews[0].individual_github_ids.push(AUTHOR_B);
    assert.equal(evaluate(item).coverage_ok, true);
});

test("an additional disclosed author absent from Git metadata must also have an ICLA", () => {
    const item = fixture();
    item.registry.material_reviews[0].individual_github_ids.push(AUTHOR_B);
    assert.equal(evaluate(item).coverage_ok, false);
    item.registry.individuals.push(individual(AUTHOR_B));
    assert.equal(evaluate(item).coverage_ok, true);
});

test("a separately disclosed corporate author must have both the individual and corporate grants", () => {
    const item = fixture();
    add_corporate(item.registry, AUTHOR_B);
    assert.equal(evaluate(item).coverage_ok, false);
    item.registry.individuals.push(individual(AUTHOR_B));
    assert.equal(evaluate(item).coverage_ok, true);
});

test("mixed personal and corporate ownership requires every corporate grant even with an ICLA", () => {
    const item = fixture();
    add_corporate(item.registry);
    assert.equal(evaluate(item).coverage_ok, true);
    item.registry.corporate_authorizations[0].withdrawn_at = "2026-11-06T11:00:00Z";
    assert.equal(evaluate(item).coverage_ok, false);
});

test("authority must cover the reviewed work and current execution evidence must remain valid", () => {
    for (const mutate of [
        registry => { registry.corporate_authorizations[0].valid_until = "2026-11-05T11:30:00Z"; },
        registry => { registry.corporate_authorizations[0].valid_from = "2026-11-07T11:00:00Z"; },
        registry => { registry.corporations[0].withdrawn_at = "2026-11-06T11:00:00Z"; }
    ]) {
        const item = fixture();
        add_corporate(item.registry);
        mutate(item.registry);
        assert.equal(evaluate(item).coverage_ok, false);
    }
});

test("lawful prospective authority cessation preserves reviewed irrevocable grants but denies new work", () => {
    const item = fixture();
    add_corporate(item.registry);
    item.registry.corporate_authorizations[0].valid_until = "2026-11-06T11:00:00Z";
    assert.equal(evaluate(item).coverage_ok, true);
    item.pr.head.sha = "d".repeat(40);
    item.registry.material_reviews[0].head_sha = item.pr.head.sha;
    item.registry.material_reviews[0].verified_at = "2026-11-06T12:00:00Z";
    assert.equal(evaluate(item).coverage_ok, false);
});

test("an unlinked author fails until a verified alias maps to a signed contributor", () => {
    const item = fixture();
    item.actors.push({ email: "unlinked@example.invalid" });
    assert.equal(evaluate(item).coverage_ok, false);
    item.registry.individuals[0].email_sha256.push(checker.email_digest("unlinked@example.invalid"));
    assert.equal(evaluate(item).coverage_ok, true);
    const conflict = individual(AUTHOR_B);
    conflict.email_sha256.push(checker.email_digest("unlinked@example.invalid"));
    item.registry.individuals.push(conflict);
    assert.throws(() => evaluate(item), /two different contributors/);
});

test("withdrawing a mistaken identity assertion retires its alias and permits a verified correction", () => {
    const item = fixture();
    const alias = checker.email_digest("mistaken@example.invalid");
    item.registry.individuals[0].email_sha256.push(alias);
    item.registry.individuals[0].withdrawn_at = "2026-11-06T11:00:00Z";
    const renewed = individual(AUTHOR_A);
    renewed.id = "individual-renewed";
    item.registry.individuals.push(renewed);
    item.actors.push({ email: "mistaken@example.invalid" });
    assert.equal(evaluate(item).coverage_ok, false);
    const corrected = individual(AUTHOR_B);
    corrected.email_sha256.push(alias);
    item.registry.individuals.push(corrected);
    item.registry.material_reviews[0].individual_github_ids.push(AUTHOR_B);
    assert.equal(evaluate(item).coverage_ok, true);
});

test("an ICLA does not approve imported material or a changed disclosure/head", () => {
    const item = fixture();
    assert.equal(evaluate(item).material_ok, true);
    item.pr.body = "Material origin: imported-or-adapted\nSource: https://example.invalid/source\n";
    assert.equal(evaluate(item).material_ok, false);
    const review = item.registry.material_reviews[0];
    review.disclosure_sha256 = checker.text_digest(item.pr.body);
    assert.equal(evaluate(item).material_ok, false);
    review.disposition = "imports-approved";
    assert.throws(() => evaluate(item), /tracked origin\/licence records/);
    review.provenance_paths = ["THIRD_PARTY/example.toml"];
    assert.equal(evaluate(item).material_ok, true);
    item.pr.head.sha = "b".repeat(40);
    assert.equal(evaluate(item).material_ok, false);
});

test("exceptions require exact author, PR, head, effective scope and archived rights evidence", () => {
    const item = fixture();
    item.actors.push({ github_id: AUTHOR_B });
    item.registry.exceptions.push({
        ...verification("exception-mechanical"), ...evidence("exception-mechanical"),
        pull_request: item.pr.number, head_sha: item.pr.head.sha, github_id: AUTHOR_B,
        basis: "mechanical-bot", reason: "Verified mechanical regeneration with no new third-party authorship",
        rights_irrevocable: true
    });
    assert.equal(evaluate(item).coverage_ok, true);
    item.registry.exceptions[0].head_sha = "c".repeat(40);
    assert.equal(evaluate(item).coverage_ok, false);
    item.registry.exceptions[0].head_sha = item.pr.head.sha;
    item.registry.exceptions[0].valid_until = "2026-11-07T11:00:00Z";
    assert.throws(() => evaluate(item), /time-limited acceptance is unsupported/);
    delete item.registry.exceptions[0].valid_until;
    item.registry.exceptions[0].evidence_sha256 = "";
    assert.throws(() => evaluate(item), /archived evidence/);
});

test("a signature is tied to its exact agreement version and the registry to the repository", () => {
    const item = fixture();
    item.registry.individuals[0].agreement_sha256 = "b".repeat(64);
    assert.throws(() => evaluate(item), /exact applicable agreement digest/);
    item.registry.individuals[0] = individual(AUTHOR_A);
    assert.throws(() => checker.validate_registry(item.registry, REPOSITORY_ID + 1, TEXTS), /repository identity/);
});

test("retained signatures cannot be deleted or rewritten; a withdrawal preserves their evidence", () => {
    const item = fixture();
    const proposed = structuredClone(item.registry);
    proposed.individuals[0].withdrawn_at = "2026-11-06T11:00:00Z";
    assert.doesNotThrow(() => checker.require_retained_records(item.registry, proposed));
    proposed.individuals[0].evidence_id = "synthetic:different-evidence";
    assert.throws(() => checker.require_retained_records(item.registry, proposed), /immutable/);
    proposed.individuals = [];
    assert.throws(() => checker.require_retained_records(item.registry, proposed), /cannot be deleted/);
});

test("a finite corporate cutoff can be shortened prospectively with retained ending evidence", () => {
    const item = fixture();
    add_corporate(item.registry);
    item.registry.corporate_authorizations[0].valid_until = "2026-12-01T12:00:00Z";
    const proposed = structuredClone(item.registry);
    const authorization = proposed.corporate_authorizations[0];
    authorization.valid_until = "2026-11-10T12:00:00Z";
    assert.throws(() => checker.require_retained_records(item.registry, proposed), /ending attestation/);
    authorization.ending = { ...verification("ending-notice-001"), ...evidence("ending-notice-001") };
    checker.validate_registry(proposed, REPOSITORY_ID, TEXTS);
    assert.doesNotThrow(() => checker.require_retained_records(item.registry, proposed));
    assert.equal(checker.evaluate_coverage(item.pr, item.actors, proposed, NOW).coverage_ok, true);
    const later = Date.parse("2026-11-11T12:00:00Z");
    proposed.material_reviews[0].verified_at = "2026-11-11T12:00:00Z";
    assert.equal(checker.evaluate_coverage(item.pr, item.actors, proposed, later).coverage_ok, false);
    authorization.valid_until = "2026-12-02T12:00:00Z";
    assert.throws(() => checker.require_retained_records(item.registry, proposed), /only be shortened/);
});

function connection(nodes, next = null, total = nodes.length)
{
    return { nodes, totalCount: total, pageInfo: { hasNextPage: next !== null, endCursor: next } };
}

function commit(oid, authors, message = "Synthetic contribution")
{
    return { commit: { oid, message, author: { email: authors.nodes[0].email }, authors } };
}

function actor(id)
{
    return { email: `author-${id}@example.invalid`, name: "Synthetic Author",
        user: { databaseId: id, login: `synthetic-${id}` } };
}

test("author enumeration follows every PR-commit and co-author page beyond REST's 250-commit cap", async () => {
    const pr = pull_request();
    pr.commits = 251;
    const api = { graphql: async (query, variables) => {
        if (query.includes("$oid:")) {
            return { repository: { object: { authors: connection([actor(AUTHOR_B)], null, 2) } } };
        }
        const offset = variables.after === null ? 0 : Number(variables.after);
        const count = Math.min(100, 251 - offset);
        const nodes = Array.from({ length: count }, (_, index) => commit(
            (offset + index + 1).toString(16).padStart(40, "0"),
            connection([actor(AUTHOR_A)], offset + index === 250 ? "author-next" : null,
                offset + index === 250 ? 2 : 1)));
        return { repository: { pullRequest: { headRefOid: pr.head.sha,
            commits: connection(nodes, offset + count < 251 ? String(offset + count) : null, 251) } } };
    } };
    const actors = await checker.collect_actors(api, "synthetic/repository", pr);
    assert.equal(actors.length, 253);
    assert.equal(actors.at(-1).github_id, AUTHOR_B);
});

test("partial pagination, repeated pages and malformed author attributions fail closed", async () => {
    const pr = pull_request();
    for (const commits of [
        connection([commit("a", connection([actor(AUTHOR_A)]))], null, 2),
        connection([commit("a", connection([actor(AUTHOR_A)]))], "same", 3),
        connection([commit("a", connection([actor(AUTHOR_A)]), "Co-authored-by: unidentified person")])
    ]) {
        const api = { graphql: async () => ({ repository: {
            pullRequest: { headRefOid: pr.head.sha, commits }
        } }) };
        await assert.rejects(checker.collect_actors(api, "synthetic/repository", pr),
            /complete PR commit list|pagination|Co-authored-by/);
    }
});

test("a co-author attribution outside GitHub's trailer parser remains a required author", async () => {
    const pr = pull_request();
    const api = { graphql: async () => ({ repository: { pullRequest: { headRefOid: pr.head.sha,
        commits: connection([commit("a", connection([actor(AUTHOR_A)]),
            "Co-authored-by: Extra Author <extra@example.invalid>\n\nFurther message")])
    } } }) };
    const actors = await checker.collect_actors(api, "synthetic/repository", pr);
    assert.equal(actors.at(-1).email, "extra@example.invalid");
    assert.equal(actors.at(-1).github_id, undefined);
});

function product_ruleset(generation = 0)
{
    return {
        name: "Contribution rights", target: "branch", enforcement: "active", bypass_actors: [],
        conditions: { ref_name: { include: ["~DEFAULT_BRANCH"], exclude: [] } },
        rules: [
            { type: "pull_request", parameters: { required_approving_review_count: 0 } },
            { type: "non_fast_forward" },
            { type: "required_status_checks", parameters: {
                strict_required_status_checks_policy: true,
                do_not_enforce_on_create: false,
                required_status_checks: [
                    ...checker.status_contexts(generation).map(context => ({ context, integration_id: 15368 })),
                    { context: "unrelated-passing-check", integration_id: 18001 }
                ]
            } }
        ]
    };
}

test("record publication advances only its two required contexts, preserves other rules and retries a blocked prefix", async () => {
    let ruleset = product_ruleset();
    const previous = structuredClone(ruleset);
    let writes = 0;
    const api = { request: async (method, route, body) => {
        assert.equal(route, "/repos/synthetic/repository/rulesets/9001");
        if (method === "GET") {
            return structuredClone(ruleset);
        }
        assert.equal(method, "PUT");
        ruleset = structuredClone(body);
        writes += 1;
        return ruleset;
    } };
    await checker.advance_required_generation(api, "synthetic/repository", 9001, 0, "master");
    const checks = ruleset.rules[2].parameters.required_status_checks;
    assert.deepEqual(checks.slice(0, 2).map(item => item.context), checker.status_contexts(1));
    assert.deepEqual(checks[2], previous.rules[2].parameters.required_status_checks[2]);
    assert.deepEqual(ruleset.rules.slice(0, 2), previous.rules.slice(0, 2));
    assert.deepEqual(ruleset.conditions, previous.conditions);
    assert.deepEqual(ruleset.bypass_actors, []);
    await checker.advance_required_generation(api, "synthetic/repository", 9001, 0, "master");
    assert.equal(writes, 1);
});

test("an old checker posting green after record publication cannot satisfy the new generation's merge gate", async () => {
    const item = fixture();
    let ruleset = product_ruleset();
    let ref_reads = 0;
    const statuses = new Map();
    const api = {
        list: async route => {
            assert.equal(route, "/repos/synthetic/repository/pulls?state=open");
            return [item.pr];
        },
        graphql: async () => ({ repository: { pullRequest: { headRefOid: item.pr.head.sha,
            commits: connection([commit("a", connection([actor(AUTHOR_A)]))]) } } }),
        request: async (method, route, body) => {
            if (route.endsWith("/rulesets/9001")) {
                if (method === "PUT") {
                    ruleset = structuredClone(body);
                }
                return structuredClone(ruleset);
            }
            if (route === "/repos/synthetic/repository") {
                return { id: REPOSITORY_ID, default_branch: "master" };
            }
            if (route.includes("/git/ref/heads/")) {
                ref_reads += 1;
                if (ref_reads === 2) {
                    // The old snapshot read has completed; admin publication crosses its boundary now.
                    await checker.advance_required_generation(api, "synthetic/repository", 9001, 0, "master");
                    item.registry.generation = 1;
                    item.registry.individuals[0].withdrawn_at = "2026-11-06T11:00:00Z";
                }
                return { object: { sha: "old-registry" } };
            }
            if (route.includes("/contents/coverage.json")) {
                return { type: "file", encoding: "base64", sha: "registry-blob",
                    content: Buffer.from(JSON.stringify(item.registry)).toString("base64") };
            }
            if (route.includes("/statuses/")) {
                statuses.set(body.context, body.state);
                return {};
            }
            if (route.endsWith("/pulls/7")) {
                return structuredClone(item.pr);
            }
            throw new Error(`Unexpected synthetic API route ${route}`);
        }
    };
    assert.equal(await checker.check_repository(api, "synthetic/repository", null,
        "https://example.invalid/test-run", NOW), true);
    assert.equal(statuses.get(checker.status_contexts(0)[0]), "success");
    const required = ruleset.rules[2].parameters.required_status_checks.slice(0, 2);
    assert.equal(required.every(check => statuses.get(check.context) === "success"), false);
    assert.deepEqual(required.map(check => check.context), checker.status_contexts(1));
});
