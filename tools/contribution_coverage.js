#!/usr/bin/env node
"use strict";

const fs   = require("node:fs");
const path = require("node:path");
const assent = require("./contribution_assent.js");
const { AGREEMENT_PATHS, require_value, sha256, text_digest, email_digest, valid_id,
    status_contexts, validate_registry, empty_registry, require_retained_records } = assent;
const RECORDS_BRANCH = "contribution-records";
const RECORDS_PATH   = "coverage.json";
const RECEIPTS_PATH  = "assents/v1.1";
const SOURCE_ROOT   = path.resolve(__dirname, "..");
class GitHub_api
{
    constructor(token)
    {
        require_value(typeof token === "string" && token.length > 0, "A GitHub API token is required.");
        this.token = token;
    }

    async request(method, route, body)
    {
        const response = await fetch(`https://api.github.com${route}`, {
            method,
            headers: {
                Authorization: `Bearer ${this.token}`,
                Accept: "application/vnd.github+json",
                "Content-Type": "application/json",
                "X-GitHub-Api-Version": "2022-11-28",
                "User-Agent": "Varinomics-contribution-coverage"
            },
            body: body === undefined ? undefined : JSON.stringify(body),
            signal: AbortSignal.timeout(30000)
        });
        if (!response.ok) {
            const error = new Error(`GitHub API ${method} ${route.split("?")[0]} failed (${response.status}).`);
            error.status = response.status;
            throw error;
        }
        return response.status === 204 ? null : response.json();
    }

    async graphql(query, variables)
    {
        const response = await this.request("POST", "/graphql", { query, variables });
        require_value(!response.errors && response.data, "GitHub could not return complete author metadata.");
        return response.data;
    }

    async list(route)
    {
        const items = [];
        for (let page = 1; ; ++page) {
            const separator = route.includes("?") ? "&" : "?";
            const batch = await this.request("GET", `${route}${separator}per_page=100&page=${page}`);
            require_value(Array.isArray(batch), "GitHub returned an incomplete list response.");
            items.push(...batch);
            if (batch.length < 100) {
                return items;
            }
        }
    }
}

const AUTHOR_FIELDS = `totalCount pageInfo { hasNextPage endCursor }
    nodes { email name user { databaseId login } }`;
const PR_AUTHORS_QUERY = `query($owner:String!, $name:String!, $number:Int!, $after:String) {
    repository(owner:$owner, name:$name) {
        pullRequest(number:$number) {
            headRefOid
            commits(first:100, after:$after) {
                totalCount pageInfo { hasNextPage endCursor }
                nodes { commit { oid message author { email }
                    authors(first:100) { ${AUTHOR_FIELDS} }
                } }
            }
        }
    }
}`;
const MORE_AUTHORS_QUERY = `query($owner:String!, $name:String!, $oid:GitObjectID!, $after:String) {
    repository(owner:$owner, name:$name) {
        object(oid:$oid) { ... on Commit { authors(first:100, after:$after) { ${AUTHOR_FIELDS} } } }
    }
}`;

function advance_cursor(connection, previous, label)
{
    require_value(connection && Array.isArray(connection.nodes) && connection.pageInfo &&
        Number.isSafeInteger(connection.totalCount), `${label} metadata is incomplete.`);
    if (!connection.pageInfo.hasNextPage) {
        return null;
    }
    require_value(typeof connection.pageInfo.endCursor === "string" &&
        connection.pageInfo.endCursor !== previous && connection.nodes.length > 0,
        `${label} pagination did not advance.`);
    return connection.pageInfo.endCursor;
}

async function collect_actors(api, repository, pr)
{
    const [owner, name] = repository.split("/");
    const actors = [{ github_id: pr.user.id, login: pr.user.login }];
    const seen_commits = new Set();
    const seen_pages   = new Set();
    let after = null;
    let expected_commits;
    do {
        const data = await api.graphql(PR_AUTHORS_QUERY, { owner, name, number: pr.number, after });
        const pull = data.repository?.pullRequest;
        require_value(pull && pull.headRefOid === pr.head.sha, "The PR head changed during author enumeration.");
        const commits = pull.commits;
        const next = advance_cursor(commits, after, "PR commits");
        expected_commits ??= commits.totalCount;
        require_value(commits.totalCount === expected_commits, "The PR commit count changed during enumeration.");
        for (const item of commits.nodes) {
            const commit = item.commit;
            require_value(commit && !seen_commits.has(commit.oid), "PR commit pagination repeated a commit.");
            seen_commits.add(commit.oid);
            const authors = [...commit.authors.nodes];
            const author_count = commit.authors.totalCount;
            let author_cursor = advance_cursor(commit.authors, null, "Commit authors");
            const author_pages = new Set();
            while (author_cursor !== null) {
                require_value(!author_pages.has(author_cursor), "Commit author pagination repeated a page.");
                author_pages.add(author_cursor);
                const more = await api.graphql(MORE_AUTHORS_QUERY,
                    { owner, name, oid: commit.oid, after: author_cursor });
                const page = more.repository?.object?.authors;
                const cursor = advance_cursor(page, author_cursor, "Commit authors");
                require_value(page.totalCount === author_count, "The commit author count changed during enumeration.");
                authors.push(...page.nodes);
                author_cursor = cursor;
            }
            require_value(authors.length === author_count && authors.length > 0 &&
                authors[0].email === commit.author?.email,
                "GitHub returned an incomplete commit author/co-author list.");
            // GitHub recognises trailers; an attribution elsewhere in a message must not disappear.
            for (const line of commit.message.split(/\r?\n/)) {
                if (/^Co-authored-by\s*:/i.test(line)) {
                    const match = /^Co-authored-by:[ \t]*[^<>]+<([^<>]+)>[ \t]*$/i.exec(line);
                    require_value(match, "A Co-authored-by attribution is malformed; correct the author identity.");
                    if (!authors.some(actor => actor.email.toLowerCase() === match[1].trim().toLowerCase())) {
                        authors.push({ email: match[1].trim(), user: null });
                    }
                }
            }
            for (const author of authors) {
                actors.push({ github_id: author.user?.databaseId, email: author.email,
                    login: author.user?.login });
            }
        }
        require_value(next === null || !seen_pages.has(next), "PR commit pagination repeated a page.");
        seen_pages.add(next);
        after = next;
    }
    while (after !== null);
    require_value(seen_commits.size === expected_commits && expected_commits === pr.commits &&
        expected_commits > 0, "The complete PR commit list could not be verified.");
    return actors;
}

async function advance_required_generation(api, repository, ruleset_id, current_generation, default_branch)
{
    const route = `/repos/${repository}/rulesets/${ruleset_id}`;
    const ruleset = await api.request("GET", route);
    const bypass_actors = ruleset.bypass_actors || [];
    const owner_bypass = bypass_actors.length === 1 &&
        bypass_actors[0].actor_type === "User" && bypass_actors[0].actor_id === 20025756 &&
        bypass_actors[0].bypass_mode === "always";
    require_value(ruleset.target === "branch" && ruleset.enforcement === "active" &&
        (bypass_actors.length === 0 || owner_bypass),
        "The contribution ruleset must be active with no bypass or only the recorded owner's direct-push bypass.");
    const refs = ruleset.conditions?.ref_name;
    require_value(refs && Array.isArray(refs.include) && refs.exclude?.length === 0 &&
        (refs.include.includes("~DEFAULT_BRANCH") || refs.include.includes(`refs/heads/${default_branch}`)),
        "The contribution ruleset must protect the default branch without exclusions.");
    const required = ruleset.rules.filter(rule => rule.type === "required_status_checks");
    require_value(required.length === 1, "The contribution ruleset needs one required-status rule.");
    const checks = required[0].parameters.required_status_checks;
    const current = status_contexts(current_generation);
    const next    = status_contexts(current_generation + 1);
    const has_current = current.every(context => checks.some(check => check.context === context));
    const has_next    = next.every(context => checks.some(check => check.context === context));
    require_value(has_current !== has_next,
        "The contribution ruleset does not identify the current or interrupted-next records generation.");
    const selected = has_current ? current : next;
    require_value(selected.every(context => checks.some(check =>
        check.context === context && valid_id(check.integration_id))),
        "Required contribution checks must be bound to their GitHub Actions source app.");
    if (has_next) {
        return;
    }
    required[0].parameters.required_status_checks = checks.map(check => {
        const index = current.indexOf(check.context);
        return index < 0 ? check : { ...check, context: next[index] };
    });
    // This is the publication boundary: old evidence can no longer satisfy the merge contract.
    await api.request("PUT", route, {
        name: ruleset.name, target: ruleset.target, enforcement: ruleset.enforcement,
        bypass_actors: ruleset.bypass_actors || [], conditions: ruleset.conditions, rules: ruleset.rules
    });
}

function agreement_texts()
{
    return Object.fromEntries(Object.entries(AGREEMENT_PATHS).map(([id, file]) =>
        [id, fs.readFileSync(path.join(SOURCE_ROOT, file), "utf8")]));
}

function decode_file(file)
{
    require_value(file.encoding === "base64" && typeof file.content === "string",
        "The complete signing evidence could not be read.");
    return JSON.parse(Buffer.from(file.content, "base64").toString("utf8"));
}

async function registry_snapshot(api, repository)
{
    const ref = await api.request("GET", `/repos/${repository}/git/ref/heads/${RECORDS_BRANCH}`);
    const file = await api.request("GET", `/repos/${repository}/contents/${RECORDS_PATH}?ref=${ref.object.sha}`);
    return { sha: ref.object.sha, registry: decode_file(file), file_sha: file.sha };
}

async function read_receipts(api, repository, snapshot, texts)
{
    const tree = await api.request("GET", `/repos/${repository}/git/trees/${snapshot.sha}?recursive=1`);
    require_value(Array.isArray(tree.tree) && tree.truncated === false,
        "The complete signing-receipt inventory could not be read.");
    const receipts = [];
    for (const entry of tree.tree) {
        if (entry.path.startsWith(`${RECEIPTS_PATH}/`) && entry.type === "blob") {
            require_value(new RegExp(`^${RECEIPTS_PATH}/\\d+\\.json$`).test(entry.path),
                "The signing evidence contains an unexpected receipt path.");
            const file = await api.request("GET", `/repos/${repository}/git/blobs/${entry.sha}`);
            const receipt = assent.validate_receipt(decode_file(file), snapshot.registry, texts);
            require_value(entry.path === `${RECEIPTS_PATH}/${receipt.comment_id}.json`,
                "The signing-receipt identity does not match its immutable path.");
            receipts.push(receipt);
        }
    }
    return receipts;
}

async function append_receipt(api, repository, receipt)
{
    const route = `/repos/${repository}/contents/${RECEIPTS_PATH}/${receipt.comment_id}.json`;
    for (let attempt = 0; attempt < 5; ++attempt) {
        try {
            const existing = await api.request("GET", `${route}?ref=${RECORDS_BRANCH}`);
            const retained = decode_file(existing);
            require_value(retained.github_id === receipt.github_id && retained.assent_text === receipt.assent_text &&
                retained.created_at === receipt.created_at && retained.agreement_sha256 === receipt.agreement_sha256 &&
                retained.head_sha === receipt.head_sha,
                "An independently retained creation receipt cannot be rewritten.");
            return false;
        }
        catch (error) {
            if (error.status !== 404) {
                throw error;
            }
        }
        try {
            await api.request("PUT", route, {
                branch: RECORDS_BRANCH,
                message: `Retain authenticated contribution assent ${receipt.comment_id}`,
                content: Buffer.from(JSON.stringify(receipt, null, 2) + "\n").toString("base64")
            });
            return true;
        }
        catch (error) {
            if (error.status !== 409 || attempt === 4) {
                throw error;
            }
            await new Promise(resolve => setTimeout(resolve, 100 * 2 ** attempt));
        }
    }
}

async function ingest_comment(api, repository, event, context)
{
    try {
        const snapshot = await registry_snapshot(api, repository);
        const texts = agreement_texts();
        validate_registry(snapshot.registry, event.repository?.id, texts);
        const receipt = await assent.capture_receipt(api, repository, event, context, snapshot.registry, texts);
        await append_receipt(api, repository, receipt);
        process.stdout.write(`Retained authenticated ${receipt.kind} creation receipt.\n`);
    }
    catch (error) {
        if (event.issue?.pull_request && /^\/cla /.test(event.comment?.body || "")) {
            await api.request("POST", `/repos/${repository}/issues/${event.issue.number}/comments`, {
                body: `The CLA command was not recorded. ${error.message}`
            });
        }
        throw error;
    }
}

async function additional_actors(api, body)
{
    const lines = [...body.matchAll(/^Additional authors:[ \t]*(.*)$/gmi)];
    require_value(lines.length <= 1, "Use one Additional authors line.");
    if (lines.length === 0 || /^(none)?$/i.test(lines[0][1].trim())) {
        return [];
    }
    const handles = lines[0][1].split(",").map(item => item.trim().replace(/^@/, ""));
    require_value(handles.every(item => /^[A-Za-z0-9][A-Za-z0-9-]{0,38}$/.test(item)),
        "Additional authors use comma-separated GitHub handles, such as @alice, @bob.");
    const actors = [];
    for (const handle of handles) {
        const user = await api.request("GET", `/users/${handle}`);
        require_value(valid_id(user.id), "An additional author could not be identified.");
        actors.push({ github_id: user.id, login: user.login });
    }
    return actors;
}

async function update_prompt(api, repository, pr, body)
{
    const comments = await api.list(`/repos/${repository}/issues/${pr.number}/comments`);
    const marker = "<!-- varinomics-contribution-check -->";
    const existing = comments.find(comment => comment.user?.login === "github-actions[bot]" &&
        comment.user.type === "Bot" && comment.body.startsWith(marker));
    if (existing?.body === body) {
        return;
    }
    if (existing) {
        await api.request("PATCH", `/repos/${repository}/issues/comments/${existing.id}`, { body });
    }
    else {
        await api.request("POST", `/repos/${repository}/issues/${pr.number}/comments`, { body });
    }
}

async function post_status(api, repository, head_sha, context, state, description, target_url)
{
    await api.request("POST", `/repos/${repository}/statuses/${head_sha}`,
        { context, state, description, target_url });
}

async function check_repository(api, repository, requested_number, target_url)
{
    const repo = await api.request("GET", `/repos/${repository}`);
    const open = await api.list(`/repos/${repository}/pulls?state=open`);
    let pulls = open;
    if (requested_number !== null) {
        const requested = open.find(pr => pr.number === requested_number);
        require_value(requested, "The requested PR is not open.");
        pulls = open.filter(pr => pr.head.sha === requested.head.sha);
    }
    if (pulls.length === 0) {
        process.stdout.write("No open PRs need contribution checks.\n");
        return true;
    }
    let contexts = [];
    try {
        const snapshot = await registry_snapshot(api, repository);
        contexts = status_contexts(snapshot.registry.generation);
        for (const head of new Set(pulls.map(pr => pr.head.sha))) {
            for (const context of contexts) {
                await post_status(api, repository, head, context, "pending", "Checking contribution rights.", target_url);
            }
        }
        const texts = agreement_texts();
        const registry = validate_registry(snapshot.registry, repo.id, texts);
        const receipts = await read_receipts(api, repository, snapshot, texts);
        const outcomes = new Map();
        for (const listed of pulls) {
            let pull = listed;
            let coverage = { coverage_ok: false, missing: [] };
            let material_ok = false;
            try {
                pull = await api.request("GET", `/repos/${repository}/pulls/${listed.number}`);
                require_value(pull.state === "open" && pull.head.sha === listed.head.sha,
                    "The PR head changed before checking; its next event will recheck the new head.");
                const results = await Promise.allSettled([
                    (async () => {
                        const actors = [...await collect_actors(api, repository, pull),
                            ...await additional_actors(api, pull.body || "")];
                        return assent.evaluate_coverage(pull, actors, registry, receipts);
                    })(),
                    assent.review_material(api, repository, pull, registry, receipts)
                ]);
                if (results[0].status === "fulfilled") {
                    coverage = results[0].value;
                }
                if (results[1].status === "fulfilled") {
                    material_ok = results[1].value;
                }
                for (const result of results) {
                    if (result.status === "rejected") {
                        coverage.problem = true;
                        process.stderr.write(`PR #${pull.number}: ${result.reason.message}\n`);
                    }
                }
                const current = await api.request("GET", `/repos/${repository}/pulls/${pull.number}`);
                require_value(current.state === "open" && current.head.sha === pull.head.sha &&
                    text_digest(current.body || "") === text_digest(pull.body || ""),
                    "The PR head or disclosure changed during checking; the next event will recheck it.");
                await update_prompt(api, repository, pull, assent.signing_prompt(pull, coverage, material_ok));
            }
            catch (error) {
                coverage.coverage_ok = false;
                material_ok = false;
                process.stderr.write(`PR #${listed.number}: ${error.message}\n`);
            }
            const combined = outcomes.get(listed.head.sha) || { coverage_ok: true, material_ok: true };
            combined.coverage_ok &&= coverage.coverage_ok;
            combined.material_ok &&= material_ok;
            outcomes.set(listed.head.sha, combined);
            process.stdout.write(`PR #${listed.number}: CLA ${coverage.coverage_ok ? "covered" : "assent needed"}; ` +
                `origins ${material_ok ? "checked" : "review needed"}.\n`);
        }
        const latest = await registry_snapshot(api, repository);
        require_value(assent.canonical_json(latest.registry) === assent.canonical_json(snapshot.registry),
            "Administrative rights evidence changed during checking; recheck under its new generation.");
        for (const [head, outcome] of outcomes) {
            await post_status(api, repository, head, contexts[0], outcome.coverage_ok ? "success" : "failure",
                outcome.coverage_ok ? "All declared authors have contribution rights coverage." :
                    "CLA assent is needed; follow the copyable PR comment.", target_url);
            await post_status(api, repository, head, contexts[1], outcome.material_ok ? "success" : "failure",
                outcome.material_ok ? "The material-origin and licence-review requirements are met." :
                    "Disclose origins and obtain current-head import review.", target_url);
        }
        return [...outcomes.values()].every(item => item.coverage_ok && item.material_ok);
    }
    catch (error) {
        for (const head of new Set(pulls.map(pr => pr.head.sha))) {
            let failed_contexts = contexts;
            if (failed_contexts.length === 0) {
                const prior = await api.list(`/repos/${repository}/commits/${head}/statuses`);
                failed_contexts = [...new Set(prior.map(item => item.context).filter(context =>
                    /^(cla\/coverage|contributions\/material-review) \(records \d+\)$/.test(context)))];
            }
            for (const context of failed_contexts) {
                await post_status(api, repository, head, context, "failure",
                    "Contribution evidence could not be verified; see the workflow log.", target_url);
            }
        }
        throw error;
    }
}

async function publish_records(api, repository, filename, ruleset_id)
{
    const repo = await api.request("GET", `/repos/${repository}`);
    const snapshot = await registry_snapshot(api, repository);
    const proposed = JSON.parse(fs.readFileSync(filename, "utf8"));
    require_value(proposed.generation === snapshot.registry.generation,
        "Edit a fresh copy of the current registry before publishing.");
    validate_registry(proposed, repo.id, agreement_texts());
    require_retained_records(snapshot.registry, proposed);
    require_value(valid_id(ruleset_id), "--ruleset must identify the installed contribution ruleset.");
    await advance_required_generation(api, repository, ruleset_id, snapshot.registry.generation, repo.default_branch);
    proposed.generation += 1;
    await api.request("PUT", `/repos/${repository}/contents/${RECORDS_PATH}`, {
        branch: RECORDS_BRANCH, sha: snapshot.file_sha,
        content: Buffer.from(JSON.stringify(proposed, null, 2) + "\n").toString("base64"),
        message: "Update controlled contribution rights evidence\n\nKeep corrections separate from additive online assent and advance the required merge generation before changing existing rights decisions."
    });
    process.stdout.write(`Published administrative records generation ${proposed.generation}.\n`);
}

async function bootstrap_records(api, repository, custodian)
{
    const repo = await api.request("GET", `/repos/${repository}`);
    const registry = empty_registry(repo.id, [custodian], agreement_texts());
    validate_registry(registry, repo.id, agreement_texts());
    const workflow = fs.readFileSync(path.join(SOURCE_ROOT, ".github/workflows/contribution-coverage.yml"), "utf8");
    const tree = await api.request("POST", `/repos/${repository}/git/trees`, { tree: [
        { path: RECORDS_PATH, mode: "100644", type: "blob", content: JSON.stringify(registry, null, 2) + "\n" },
        { path: ".github/workflows/contribution-coverage.yml", mode: "100644", type: "blob", content: workflow }
    ] });
    const commit = await api.request("POST", `/repos/${repository}/git/commits`, {
        message: "Create empty versioned online contribution records", tree: tree.sha, parents: []
    });
    await api.request("POST", `/repos/${repository}/git/refs`,
        { ref: `refs/heads/${RECORDS_BRANCH}`, sha: commit.sha });
}

function parse_options(args)
{
    const options = {};
    for (let index = 0; index < args.length; index += 2) {
        require_value(args[index].startsWith("--") && args[index + 1] !== undefined,
            "Options use --name value pairs.");
        const key = args[index].slice(2);
        require_value(options[key] === undefined, `Duplicate option --${key}.`);
        options[key] = args[index + 1];
    }
    return options;
}

async function main(args)
{
    const command = args[0];
    const options = parse_options(args.slice(1));
    if (command === "email-hash") {
        process.stdout.write(email_digest(options.email) + "\n");
        return;
    }
    if (command === "upgrade-empty-registry") {
        const previous = JSON.parse(fs.readFileSync(options.file, "utf8"));
        const upgraded = assent.upgrade_empty_registry(previous, agreement_texts());
        fs.writeFileSync(options.output, JSON.stringify(upgraded, null, 2) + "\n", "utf8");
        return;
    }
    if (command === "validate-records") {
        const registry = JSON.parse(fs.readFileSync(options.file, "utf8"));
        validate_registry(registry, registry.repository_id, agreement_texts());
        process.stdout.write("Online signing controls match the published agreement versions.\n");
        return;
    }
    const repository = options.repository || process.env.GITHUB_REPOSITORY;
    require_value(typeof repository === "string" && /^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/.test(repository),
        "Use --repository owner/name.");
    const api = new GitHub_api(process.env.GH_TOKEN || process.env.GITHUB_TOKEN);
    if (command === "bootstrap") {
        require_value(valid_id(Number(options.custodian)), "--custodian must be a verified GitHub account ID.");
        await bootstrap_records(api, repository, Number(options.custodian));
        return;
    }
    if (command === "publish-records") {
        await publish_records(api, repository, options.file, Number(options.ruleset));
        return;
    }
    if (command === "ingest") {
        const event = JSON.parse(fs.readFileSync(process.env.GITHUB_EVENT_PATH, "utf8"));
        await ingest_comment(api, repository, event, { event_name: process.env.GITHUB_EVENT_NAME,
            run_id: process.env.GITHUB_RUN_ID, run_attempt: process.env.GITHUB_RUN_ATTEMPT,
            workflow_ref: process.env.GITHUB_WORKFLOW_REF });
        return;
    }
    require_value(command === "check", "Use check, ingest, bootstrap, publish-records, validate-records or upgrade-empty-registry.");
    const target_url = process.env.GITHUB_RUN_ID
        ? `https://github.com/${repository}/actions/runs/${process.env.GITHUB_RUN_ID}`
        : `https://github.com/${repository}/actions/workflows/contribution-coverage.yml`;
    const number = options["pull-request"] ? Number(options["pull-request"]) : null;
    require_value(number === null || valid_id(number), "--pull-request must identify an open PR.");
    if (!await check_repository(api, repository, number, target_url)) {
        process.exitCode = 1;
    }
}

module.exports = { ...assent, GitHub_api, collect_actors, agreement_texts, registry_snapshot,
    read_receipts, append_receipt, ingest_comment, additional_actors, check_repository,
    advance_required_generation, publish_records, main };

if (require.main === module) {
    main(process.argv.slice(2)).catch(error => {
        process.stderr.write(`Contribution check: ${error.message}\n`);
        process.exitCode = 1;
    });
}
