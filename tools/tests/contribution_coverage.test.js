"use strict";

const assert = require("node:assert/strict");
const fs     = require("node:fs");
const path   = require("node:path");
const test   = require("node:test");
const checker = require("../contribution_coverage.js");

// Synthetic events test the owner's contract; they are not executed contributor agreements.
const REPOSITORY = "synthetic/repository";
const REPOSITORY_ID = 31001;
const CUSTODIAN = 21010;
const AUTHOR_A  = 21001;
const AUTHOR_B  = 21002;
const TEXTS = checker.agreement_texts();
const CONTEXT = { event_name: "issue_comment", run_id: "71001", run_attempt: "1",
    workflow_ref: `${REPOSITORY}/.github/workflows/contribution-coverage.yml@refs/heads/master` };
const controls = () => checker.empty_registry(REPOSITORY_ID, [CUSTODIAN], TEXTS);

function pr(number = 7)
{
    return { number, state: "open", user: { id: AUTHOR_A, login: "author-a" },
        body: "Material origin: original\nAdditional authors: None\nProvenance records: None\n",
        head: { sha: number.toString(16).padStart(40, "0") }, commits: 1 };
}

function event(id = 51001, actor = AUTHOR_A, body = "/cla agree individual v1.1")
{
    return { action: "created", repository: { id: REPOSITORY_ID }, sender: { id: actor },
        issue: { number: 7, pull_request: { url: "https://api.github.com/repos/synthetic/repository/pulls/7" } },
        comment: { id, node_id: `synthetic-node-${id}`, body,
            user: { id: actor, login: actor === AUTHOR_A ? "author-a" : "author-b", type: "User" },
            created_at: "2026-11-06T12:00:00Z", updated_at: "2026-11-06T12:00:00Z",
            html_url: `https://github.com/${REPOSITORY}/pull/7#issuecomment-${id}` } };
}

function identity_api()
{
    return { request: async (method, route) => {
        assert.equal(method, "GET");
        if (route === "/users/author-b" || route === `/user/${AUTHOR_B}`) {
            return { id: AUTHOR_B, login: "author-b", type: "User" };
        }
        if (route.includes("/collaborators/")) { return { permission: "admin" }; }
        if (route.endsWith("/pulls/7"))       { return pr(); }
        throw new Error(`Unexpected synthetic API call ${route}`);
    } };
}

const receipt = (input = event(), registry = controls(), api = identity_api()) =>
    checker.capture_receipt(api, REPOSITORY, input, CONTEXT, registry, TEXTS);
const evaluate = (registry, receipts, actors = [{ github_id: AUTHOR_A, login: "author-a" }], pull = pr()) => {
    checker.validate_registry(registry, REPOSITORY_ID, TEXTS);
    return checker.evaluate_coverage(pull, actors, registry, receipts);
};
const evidence = id => ({ id, evidence_id: `synthetic:${id}`, evidence_sha256: checker.sha256(id),
    verified_by: CUSTODIAN, verified_at: "2026-11-06T12:01:00Z" });

test("personal assent retains authenticated creation and exact text/version, and covers same-project original submissions", async () => {
    const saved = await receipt();
    assert.equal(saved.github_id, AUTHOR_A);
    assert.equal(saved.created_at, event().comment.created_at);
    assert.equal(saved.comment_node_id, event().comment.node_id);
    assert.equal(saved.assent_text, "/cla agree individual v1.1");
    assert.equal(saved.agreement_identifier, "Varinomics-ICLA-1.1");
    assert.equal(checker.text_digest(saved.agreement_text), saved.agreement_sha256);
    assert.equal(evaluate(controls(), [saved]).coverage_ok, true);
    const earlier = pr(3); earlier.created_at = "2026-11-01T12:00:00Z";
    assert.equal(evaluate(controls(), [saved], [{ github_id: AUTHOR_A }], earlier).coverage_ok, true);
    assert.equal(evaluate(controls(), [saved], [{ github_id: AUTHOR_A }], pr(9)).coverage_ok, true);
});

test("edited/deleted events, impersonated senders and bots cannot create assent; retained grants survive comment changes", async () => {
    for (const mutate of [input => { input.action = "edited"; }, input => { input.action = "deleted"; },
        input => { input.sender.id = AUTHOR_B; }, input => { input.comment.user.type = "Bot"; }])
    {
        const input = event(); mutate(input);
        await assert.rejects(receipt(input), /newly created PR comment/);
    }
    const saved = await receipt();
    const later = event(); later.comment.body = "An edited comment cannot erase captured assent.";
    assert.equal(evaluate(controls(), [saved]).coverage_ok, true);
    assert.equal(saved.assent_text, "/cla agree individual v1.1");
});

test("one corporate assent covers signer/company and resolves authorised handles into stable IDs", async () => {
    const saved = await receipt(event(51001, AUTHOR_A,
        '/cla agree corporate v1.1 {"company":"Synthetic Example Ltd","role":"Director","authors":["@author-b"]}'));
    assert.deepEqual(saved.authorized_authors, [{ input: "@author-b", github_id: AUTHOR_B,
        login: "author-b", account_type: "User" }]);
    assert.equal(evaluate(controls(), [saved], [{ github_id: AUTHOR_A }, { github_id: AUTHOR_B }]).coverage_ok, true);
});

test("later online employee authorisation needs that representative's prior matching company assent", async () => {
    const company = await receipt(event(51001, AUTHOR_A,
        '/cla agree corporate v1.1 {"company":"Synthetic Example Ltd"}'));
    const auth = await receipt(event(51002, AUTHOR_A,
        '/cla authorize v1.1 {"company":"Synthetic Example Ltd","authors":["@author-b"]}'));
    assert.equal(evaluate(controls(), [auth], [{ github_id: AUTHOR_B }]).coverage_ok, false);
    assert.equal(evaluate(controls(), [company, auth], [{ github_id: AUTHOR_B }]).coverage_ok, true);
    company.created_at = "2026-11-06T12:01:00Z";
    assert.equal(evaluate(controls(), [company, auth], [{ github_id: AUTHOR_B }]).coverage_ok, false);
});

test("authority endings stop new automatic reliance, including pending heads, without erasing valid grants", async () => {
    const registry = controls();
    const company = await receipt(event(51001, AUTHOR_A,
        '/cla agree corporate v1.1 {"company":"Synthetic Ltd","authors":["@author-b"]}'));
    registry.online_signing.authority_endings.push({ ...evidence("ending-example"), receipt_id: company.id,
        github_id: AUTHOR_B, reason: "Synthetic removal of future automatic authorisation" });
    assert.equal(evaluate(registry, [company], [{ github_id: AUTHOR_B }], pr(3)).coverage_ok, false);
    assert.equal(evaluate(registry, [company], [{ github_id: AUTHOR_A }]).coverage_ok, true);
    registry.online_signing.authority_endings[0].github_id = null;
    assert.equal(evaluate(registry, [company]).coverage_ok, false);
    assert.equal(company.agreement_identifier, "Varinomics-CCLA-1.1");
    assert.equal(checker.text_digest(company.agreement_text), company.agreement_sha256);
});

test("all metadata and separately disclosed authors need actual coverage", async () => {
    const first = await receipt();
    const actors = [{ github_id: AUTHOR_A }, { github_id: AUTHOR_B }];
    assert.equal(evaluate(controls(), [first], actors).coverage_ok, false);
    const second = await receipt(event(51002, AUTHOR_B));
    assert.equal(evaluate(controls(), [first, second], actors).coverage_ok, true);
    const additional = await checker.additional_actors(identity_api(), "Additional authors: @author-b\n");
    assert.equal(evaluate(controls(), [first], [{ github_id: AUTHOR_A }, ...additional]).coverage_ok, false);
});

test("unlinked authors need verified identity mappings and disputed receipts remain retained but unusable", async () => {
    const registry = controls(); const saved = await receipt();
    assert.equal(evaluate(registry, [saved], [{ email: "unlinked@example.invalid" }]).coverage_ok, false);
    registry.online_signing.aliases.push({ ...evidence("alias-example"), github_id: AUTHOR_A, login: "author-a",
        email_sha256: checker.email_digest("unlinked@example.invalid"), withdrawn_at: null });
    assert.equal(evaluate(registry, [saved], [{ email: "unlinked@example.invalid" }]).coverage_ok, true);
    registry.online_signing.invalid_receipts.push({ ...evidence("correction-example"), receipt_id: saved.id,
        reason: "Synthetic disputed ownership evidence" });
    assert.equal(evaluate(registry, [saved]).coverage_ok, false);
    assert.equal(saved.assent_text, "/cla agree individual v1.1");
});

test("ordinary original work needs no per-PR administrative ledger; a CLA cannot approve an import", async () => {
    assert.equal(await checker.review_material({}, REPOSITORY, pr(), controls(), []), true);
    const pull = pr(); pull.body = "Material origin: imported-or-adapted\nProvenance records: THIRD_PARTY/example.toml\n";
    let approvals = [];
    const api = { request: async (method, route) => route.includes("/contents/")
        ? { type: "file" } : { permission: "write" }, list: async () => approvals };
    const saved = await receipt();
    assert.equal(await checker.review_material(api, REPOSITORY, pull, controls(), [saved]), false);
    approvals = [{ state: "APPROVED", commit_id: pull.head.sha, user: { id: AUTHOR_B, login: "author-b" } }];
    assert.equal(await checker.review_material(api, REPOSITORY, pull, controls(), [saved]), true);
    approvals[0].commit_id = pr(8).head.sha;
    assert.equal(await checker.review_material(api, REPOSITORY, pull, controls(), [saved]), false);
});

test("a sole maintainer's genuine current-head import-review comment is viable; another author cannot approve it", async () => {
    const pull = pr();
    const review = await receipt(event(51003, AUTHOR_A, `/cla review imports ${pull.head.sha}`));
    pull.body = "Material origin: imported-or-adapted\nProvenance records: THIRD_PARTY/example.toml\n";
    const api = { request: async (method, route) => route.includes("/contents/")
        ? { type: "file" } : { permission: "admin" }, list: async () => [] };
    assert.equal(await checker.review_material(api, REPOSITORY, pull, controls(), [review]), true);
    pull.head.sha = pr(8).head.sha;
    assert.equal(await checker.review_material(api, REPOSITORY, pull, controls(), [review]), false);
    await assert.rejects(receipt(event(51004, AUTHOR_B, `/cla review imports ${pr().head.sha}`), controls(),
        { request: async () => ({ permission: "read" }) }), /repository maintainer/);
});

test("qualified/malformed commands and bot-wide authorisations cannot create grants", async () => {
    assert.throws(() => checker.parse_command('/cla agree corporate v1.1 {"company":"Synthetic Ltd","exception":"No patents"}'), /only company/);
    assert.throws(() => checker.parse_command("/cla agree individual v1.1 except patents"), /Use \/cla agree/);
    await assert.rejects(receipt(event(51001, AUTHOR_A,
        '/cla agree corporate v1.1 {"company":"Synthetic Ltd","authors":[21002]}'), controls(),
        { request: async () => ({ id: AUTHOR_B, login: "synthetic-bot", type: "Bot" }) }), /specific reviewed/);
});

function receipt_store(conflict = false)
{
    const files = new Map();
    const api = { request: async (method, route, body) => {
        const key = route.split("?")[0];
        if (method === "GET") {
            if (files.has(key)) { return { encoding: "base64", content: files.get(key) }; }
            const error = new Error("Synthetic missing receipt"); error.status = 404; throw error;
        }
        assert.equal(method, "PUT"); assert.equal(body.branch, "contribution-records");
        assert.match(key, /\/assents\/v1\.1\/\d+\.json$/);
        if (conflict && key.endsWith("51002.json")) {
            conflict = false; const error = new Error("Synthetic concurrent write"); error.status = 409; throw error;
        }
        files.set(key, body.content); return {};
    } };
    return { api, files };
}

test("concurrent authors are archived independently with conflict retry, and a replay cannot rewrite assent", async () => {
    const first = await receipt(); const second = await receipt(event(51002, AUTHOR_B));
    const { api, files } = receipt_store(true);
    await Promise.all([checker.append_receipt(api, REPOSITORY, first), checker.append_receipt(api, REPOSITORY, second)]);
    assert.equal(files.size, 2);
    const saved = [...files.values()].map(value => JSON.parse(Buffer.from(value, "base64").toString("utf8")));
    assert.equal(evaluate(controls(), saved, [{ github_id: AUTHOR_A }, { github_id: AUTHOR_B }]).coverage_ok, true);
    assert.equal(await checker.append_receipt(api, REPOSITORY, first), false);
    await assert.rejects(checker.append_receipt(api, REPOSITORY, { ...first, assent_text: "Changed" }), /cannot be rewritten/);
    const altered = { ...first, agreement_text: first.agreement_text + "Changed" };
    assert.throws(() => checker.validate_receipt(altered, controls(), TEXTS), /exact agreement version/);
});

test("an archive failure cannot produce a retained grant", async () => {
    const saved = await receipt();
    const api = { request: async method => {
        const error = new Error("Synthetic archive outage"); error.status = method === "GET" ? 404 : 500; throw error;
    } };
    await assert.rejects(checker.append_receipt(api, REPOSITORY, saved), /archive outage/);
    assert.equal(evaluate(controls(), []).coverage_ok, false);
});

const connection = (nodes, next = null, total = nodes.length) =>
    ({ nodes, totalCount: total, pageInfo: { hasNextPage: next !== null, endCursor: next } });
const actor = id => ({ email: `author-${id}@example.invalid`, name: "Synthetic Author",
    user: { databaseId: id, login: id === AUTHOR_A ? "author-a" : "author-b" } });
const commit = (oid, authors, message = "Synthetic contribution") =>
    ({ commit: { oid, message, author: { email: authors.nodes[0].email }, authors } });

test("complete-author enumeration follows all commit and co-author pages beyond the REST 250-commit cap", async () => {
    const pull = pr(); pull.commits = 251;
    const api = { graphql: async (query, variables) => {
        if (query.includes("$oid:")) { return { repository: { object: { authors: connection([actor(AUTHOR_B)], null, 2) } } }; }
        const offset = variables.after === null ? 0 : Number(variables.after);
        const count = Math.min(100, 251 - offset);
        const nodes = Array.from({ length: count }, (_, index) => commit(
            (offset + index + 1).toString(16).padStart(40, "0"),
            connection([actor(AUTHOR_A)], offset + index === 250 ? "author-next" : null, offset + index === 250 ? 2 : 1)));
        return { repository: { pullRequest: { headRefOid: pull.head.sha,
            commits: connection(nodes, offset + count < 251 ? String(offset + count) : null, 251) } } };
    } };
    const authors = await checker.collect_actors(api, REPOSITORY, pull);
    assert.equal(authors.length, 253); assert.equal(authors.at(-1).github_id, AUTHOR_B);
});

function repository_api(pulls, registry, receipts)
{
    const statuses = new Map();
    const encode = data => ({ encoding: "base64", content: Buffer.from(JSON.stringify(data)).toString("base64") });
    const api = {
        list: async route => route.endsWith("pulls?state=open") ? pulls : [],
        graphql: async (query, variables) => ({ repository: { pullRequest: {
            headRefOid: pulls.find(item => item.number === variables.number).head.sha,
            commits: connection([commit("a", connection([actor(AUTHOR_A)]))]) } } }),
        request: async (method, route, body) => {
            if (route === `/repos/${REPOSITORY}`) { return { id: REPOSITORY_ID, default_branch: "master" }; }
            if (route.includes("/git/ref/heads/")) { return { object: { sha: "snapshot" } }; }
            if (route.includes("/contents/coverage.json")) { return { ...encode(registry), sha: "controls" }; }
            if (route.includes("/git/trees/")) { return { truncated: false, tree: receipts.map((item, index) =>
                ({ type: "blob", path: `assents/v1.1/${item.comment_id}.json`, sha: String(index) })) }; }
            if (route.includes("/git/blobs/")) { return encode(receipts[Number(route.split("/").at(-1))]); }
            if (route.includes("/statuses/")) { statuses.set(`${route.split("/").at(-1)}:${body.context}`, body.state); return {}; }
            if (route.includes("/pulls/")) { return structuredClone(pulls.find(item => route.endsWith(`/pulls/${item.number}`))); }
            if (method === "POST" && route.endsWith("/comments")) { return {}; }
            const error = new Error("Synthetic invalid additional author or provenance path"); error.status = 404; throw error;
        }
    };
    return { api, statuses };
}

test("a PR-local unknown author cannot poison an unrelated covered PR's head", async () => {
    const good = pr(); const bad = pr(8);
    bad.body = bad.body.replace("Additional authors: None", "Additional authors: @does-not-exist");
    const { api, statuses } = repository_api([bad, good], controls(), [await receipt()]);
    assert.equal(await checker.check_repository(api, REPOSITORY, null, "https://example.invalid/run"), false);
    assert.equal(statuses.get(`${good.head.sha}:${checker.status_contexts(0)[0]}`), "success");
    assert.equal(statuses.get(`${good.head.sha}:${checker.status_contexts(0)[1]}`), "success");
    assert.equal(statuses.get(`${bad.head.sha}:${checker.status_contexts(0)[0]}`), "failure");
});

test("the fork-review relay has no permissions or checkout and feeds a trusted-default API-only check", () => {
    const root = path.resolve(__dirname, "../..");
    const relay = fs.readFileSync(path.join(root, ".github/workflows/contribution-review-relay.yml"), "utf8");
    const caller = fs.readFileSync(path.join(root, ".github/workflows/contribution-coverage.yml"), "utf8");
    const shared = fs.readFileSync(path.join(root, ".github/workflows/check-contributions.yml"), "utf8");
    assert.match(relay, /pull_request_review:/); assert.match(relay, /permissions: \{\}/);
    assert.match(relay, /run: 'true'/); assert.doesNotMatch(relay, /uses:|checkout/);
    assert.match(caller, /workflow_run:\n    workflows: \[Contribution review relay\]/);
    assert.match(caller, /github.event.workflow_run.event == 'pull_request_review'/);
    assert.match(shared, /ref: master/); assert.match(shared, /retain-assent:/);
    assert.match(shared, /check:\n[\s\S]*concurrency:/);
    assert.doesNotMatch(caller, /concurrency:/);
});

test("empty registry migration preserves frozen versions and refuses to discard executed agreements", () => {
    const previous = { schema_version: 1, repository_id: REPOSITORY_ID, generation: 0, custodians: [CUSTODIAN],
        agreements: controls().agreements.filter(item => item.id.endsWith("v1.0")),
        individuals: [], corporations: [], corporate_authorizations: [], exceptions: [], material_reviews: [] };
    const next = checker.upgrade_empty_registry(previous, TEXTS);
    assert.equal(next.generation, 0); assert.equal(next.online_signing.schema_version, 1);
    assert.doesNotThrow(() => checker.require_retained_records(previous, next));
    previous.individuals.push({ id: "synthetic-executed-agreement" });
    assert.throws(() => checker.upgrade_empty_registry(previous, TEXTS), /no executed signing evidence/);
});

test("an old in-flight green cannot satisfy a corrected records generation, and interrupted publication retries safely", async () => {
    const registry = controls(); const pull = pr(); const saved = await receipt();
    const { api, statuses } = repository_api([pull], registry, [saved]);
    let ruleset = { name: "Contribution rights", target: "branch", enforcement: "active", bypass_actors: [],
        conditions: { ref_name: { include: ["~DEFAULT_BRANCH"], exclude: [] } },
        rules: [ { type: "non_fast_forward" }, { type: "required_status_checks", parameters: {
            required_status_checks: [
                ...checker.status_contexts(0).map(context => ({ context, integration_id: 15368 })),
                { context: "unrelated-check", integration_id: 18001 }
            ] } } ] };
    let control_reads = 0; let rule_writes = 0;
    const original_request = api.request;
    api.request = async (method, route, body) => {
        if (route.endsWith("/rulesets/9001")) {
            if (method === "PUT") { ruleset = structuredClone(body); rule_writes += 1; }
            return structuredClone(ruleset);
        }
        const answer = await original_request(method, route, body);
        if (route.includes("/contents/coverage.json") && ++control_reads === 2) {
            // The old checker has its last snapshot. Admin advances the merge contract before changing proof.
            await checker.advance_required_generation(api, REPOSITORY, 9001, 0, "master");
            registry.generation = 1;
        }
        return answer;
    };
    assert.equal(await checker.check_repository(api, REPOSITORY, null, "https://example.invalid/run"), true);
    assert.equal(statuses.get(`${pull.head.sha}:${checker.status_contexts(0)[0]}`), "success");
    const required = ruleset.rules[1].parameters.required_status_checks;
    assert.deepEqual(required.slice(0, 2).map(item => item.context), checker.status_contexts(1));
    assert.equal(required.slice(0, 2).every(item => statuses.get(`${pull.head.sha}:${item.context}`) === "success"), false);
    assert.equal(required[2].context, "unrelated-check");
    assert.deepEqual(ruleset.rules[0], { type: "non_fast_forward" });
    await checker.advance_required_generation(api, REPOSITORY, 9001, 0, "master");
    assert.equal(rule_writes, 1);
});

test("generation publication accepts only the explicit owner bypass and preserves it unchanged", async () => {
    const owner = { actor_id: 20025756, actor_type: "User", bypass_mode: "always" };
    for (const actors of [[], [owner],
        [{ ...owner, actor_id: AUTHOR_B }],
        [{ ...owner, actor_type: "Team" }],
        [{ ...owner, actor_type: "RepositoryRole" }],
        [{ ...owner, actor_type: "Integration" }],
        [{ ...owner, bypass_mode: "pull_request" }],
        [owner, { actor_id: AUTHOR_B, actor_type: "User", bypass_mode: "always" }]])
    {
        const ruleset = { name: "Contribution rights", target: "branch", enforcement: "active",
            bypass_actors: actors,
            conditions: { ref_name: { include: ["~DEFAULT_BRANCH"], exclude: [] } },
            rules: [{ type: "required_status_checks", parameters: { required_status_checks:
                checker.status_contexts(1).map(context => ({ context, integration_id: 15368 })) } }] };
        let written;
        const api = { request: async (method, route, body) => {
            assert.equal(route, `/repos/${REPOSITORY}/rulesets/9001`);
            if (method === "PUT") { written = structuredClone(body); }
            return structuredClone(ruleset);
        } };
        const publication = checker.advance_required_generation(api, REPOSITORY, 9001, 1, "master");
        if (actors.length === 0 || actors.length === 1 && actors[0] === owner) {
            await publication;
            assert.equal(JSON.stringify(written.bypass_actors), JSON.stringify(actors));
            assert.deepEqual(written.rules[0].parameters.required_status_checks.map(item => item.context),
                checker.status_contexts(2));
        }
        else {
            await assert.rejects(publication, /only the recorded owner's direct-push bypass/);
            assert.equal(written, undefined);
        }
    }
});
