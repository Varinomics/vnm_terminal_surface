#!/usr/bin/env node
"use strict";

const crypto = require("node:crypto");
const fs     = require("node:fs");
const path   = require("node:path");

const RECORDS_BRANCH = "contribution-records";
const RECORDS_PATH   = "coverage.json";
const CLA_STATUS     = "cla/coverage";
const MATERIAL_STATUS = "contributions/material-review";
const SIGNATURE_GROUPS = [
    "individuals", "corporations", "corporate_authorizations", "exceptions", "material_reviews"
];
const AGREEMENT_PATHS = {
    "individual-v1.0": "docs/contributions/individual-cla-v1.0.md",
    "corporate-v1.0":  "docs/contributions/corporate-cla-v1.0.md"
};
const SOURCE_ROOT = path.resolve(__dirname, "..");

function require_value(condition, message)
{
    if (!condition) {
        throw new Error(message);
    }
}

function sha256(value)
{
    return crypto.createHash("sha256").update(value).digest("hex");
}

function text_digest(text)
{
    return sha256(text.replace(/\r\n/g, "\n"));
}

function email_digest(email)
{
    require_value(typeof email === "string" && email.trim().includes("@"),
        "An author has no usable commit email; verified identity evidence is required.");
    return sha256(email.trim().toLowerCase());
}

function valid_id(value)
{
    return Number.isSafeInteger(value) && value > 0;
}

function status_contexts(generation)
{
    require_value(Number.isSafeInteger(generation) && generation >= 0,
        "The coverage registry needs a nonnegative generation.");
    return [CLA_STATUS, MATERIAL_STATUS].map(prefix => `${prefix} (records ${generation})`);
}

function valid_hash(value)
{
    return typeof value === "string" && /^[a-f0-9]{64}$/.test(value);
}

function timestamp(value, label)
{
    require_value(typeof value === "string" &&
        /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{3})?Z$/.test(value) &&
        Number.isFinite(Date.parse(value)), `${label} must be a UTC timestamp.`);
    return Date.parse(value);
}

function record_id(value)
{
    return typeof value === "string" && /^[A-Za-z0-9][A-Za-z0-9._:-]{2,159}$/.test(value);
}

function validate_evidence(record, prefix = "evidence")
{
    require_value(record_id(record[`${prefix}_id`]) && valid_hash(record[`${prefix}_sha256`]),
        `${record.id}: an archived ${prefix} receipt and its SHA256 are required.`);
}

function validate_verification(record, registry)
{
    require_value(record_id(record.id), "Every record needs a unique durable ID.");
    require_value(registry.custodians.includes(record.verified_by),
        `${record.id}: verified_by must identify a recorded custodian.`);
    timestamp(record.verified_at, `${record.id}.verified_at`);
    if (record.withdrawn_at !== null) {
        timestamp(record.withdrawn_at, `${record.id}.withdrawn_at`);
    }
}

function validate_subject(record)
{
    require_value(valid_id(record.github_id), `${record.id}: github_id must be a numeric account ID.`);
    require_value(Array.isArray(record.email_sha256) && record.email_sha256.every(valid_hash),
        `${record.id}: email_sha256 must contain verified commit-email hashes.`);
}

function validate_signature(record, registry, agreement_prefix)
{
    validate_verification(record, registry);
    validate_evidence(record);
    const agreement = registry.agreements.find(item => item.id === record.agreement_id);
    require_value(agreement && agreement.id.startsWith(agreement_prefix) &&
        agreement.sha256 === record.agreement_sha256,
        `${record.id}: the signature must identify the exact applicable agreement digest.`);
    require_value(timestamp(record.signed_at, `${record.id}.signed_at`) <=
        timestamp(record.verified_at, `${record.id}.verified_at`),
        `${record.id}: a signature cannot be verified before execution.`);
}

function validate_registry(registry, repository_id, agreement_text)
{
    status_contexts(registry.generation);
    require_value(registry.schema_version === 1 && valid_id(registry.repository_id) &&
        registry.repository_id === repository_id,
        "The coverage registry schema or repository identity does not match.");
    require_value(Array.isArray(registry.custodians) && registry.custodians.length > 0 &&
        registry.custodians.every(valid_id), "The registry needs identified record custodians.");
    require_value(Array.isArray(registry.agreements) && registry.agreements.length > 0,
        "The registry needs versioned agreements.");
    const ids = new Set();
    for (const agreement of registry.agreements) {
        require_value(record_id(agreement.id) && !ids.has(agreement.id),
            "Agreement IDs must be unique.");
        ids.add(agreement.id);
        require_value(AGREEMENT_PATHS[agreement.id] === agreement.path &&
            typeof agreement_text[agreement.id] === "string" &&
            agreement.sha256 === text_digest(agreement_text[agreement.id]),
            `${agreement.id}: agreement text/version SHA256 does not match the published source.`);
    }
    for (const group of SIGNATURE_GROUPS) {
        require_value(Array.isArray(registry[group]), `The registry needs a ${group} array.`);
        for (const record of registry[group]) {
            require_value(record_id(record.id) && !ids.has(record.id), "Record IDs must be unique.");
            ids.add(record.id);
        }
    }
    const aliases = new Map();
    for (const record of registry.individuals) {
        validate_signature(record, registry, "individual-");
        validate_subject(record);
        for (const alias of record.withdrawn_at === null ? record.email_sha256 : []) {
            require_value(!aliases.has(alias) || aliases.get(alias) === record.github_id,
                "A verified commit alias cannot identify two different contributors.");
            aliases.set(alias, record.github_id);
        }
    }
    for (const record of registry.corporations) {
        validate_signature(record, registry, "corporate-");
        require_value(record_id(record.entity_id), `${record.id}: a corporate entity ID is required.`);
        validate_evidence(record, "authority_evidence");
    }
    for (const record of registry.corporate_authorizations) {
        validate_verification(record, registry);
        validate_subject(record);
        validate_evidence(record);
        require_value(registry.corporations.some(item => item.id === record.corporation_id),
            `${record.id}: the corporate signature record is missing.`);
        timestamp(record.valid_from, `${record.id}.valid_from`);
        if (record.valid_until !== null) {
            require_value(timestamp(record.valid_until, `${record.id}.valid_until`) >
                timestamp(record.valid_from, `${record.id}.valid_from`),
                `${record.id}: the authorisation interval is empty.`);
        }
        if (record.ending !== undefined) {
            validate_verification(record.ending, registry);
            validate_evidence(record.ending);
            require_value(record.valid_until !== null &&
                timestamp(record.valid_until, `${record.id}.valid_until`) >=
                timestamp(record.ending.verified_at, `${record.id}.ending.verified_at`),
                `${record.id}: a prospective ending must preserve previously reviewed grants.`);
        }
        require_value(typeof record.work_scope === "string" && record.work_scope.trim().length > 0,
            `${record.id}: the verified corporate work scope is required.`);
    }
    for (const record of registry.exceptions) {
        validate_verification(record, registry);
        validate_evidence(record);
        require_value(valid_id(record.pull_request) && /^[a-f0-9]{40}$/.test(record.head_sha) &&
            (valid_id(record.github_id) !== valid_hash(record.email_sha256)),
            `${record.id}: an exception must name one exact PR/head and one exact author identity.`);
        require_value(["varinomics-owned", "separately-licensed", "mechanical-bot"].includes(record.basis) &&
            typeof record.reason === "string" && record.reason.trim().length > 0,
            `${record.id}: an exception needs its verified rights basis and reason.`);
        require_value(record.rights_irrevocable === true && !("valid_until" in record),
            `${record.id}: exceptions require durable rights; time-limited acceptance is unsupported.`);
    }
    for (const review of registry.material_reviews) {
        validate_verification(review, registry);
        require_value(valid_id(review.pull_request) && /^[a-f0-9]{40}$/.test(review.head_sha) &&
            valid_hash(review.disclosure_sha256) && review.rights_confirmed === true,
            `${review.id}: a review must affirm rights for an exact PR/head and disclosure.`);
        require_value(["original", "imports-approved"].includes(review.disposition) &&
            Array.isArray(review.provenance_paths) && review.provenance_paths.every(item =>
                typeof item === "string" && item.length > 0 && !item.startsWith("/") &&
                !item.split(/[\\/]/).includes("..")),
            `${review.id}: reviewed material needs a valid disposition and provenance paths.`);
        require_value(review.disposition !== "imports-approved" || review.provenance_paths.length > 0,
            `${review.id}: approved imports require tracked origin/licence records.`);
        require_value(Array.isArray(review.individual_github_ids) &&
            review.individual_github_ids.every(valid_id) &&
            Array.isArray(review.corporate_authorization_ids) &&
            review.corporate_authorization_ids.every(id =>
                registry.corporate_authorizations.some(item => item.id === id)),
            `${review.id}: ownership capacity must be explicitly reviewed.`);
    }
    return registry;
}

function is_active(record, now)
{
    // Withdrawal invalidates evidence now; corporate valid_until separately ends future authority.
    return Date.parse(record.verified_at) <= now && record.withdrawn_at === null;
}

function verified_actor_id(actor, registry)
{
    if (valid_id(actor.github_id)) {
        return actor.github_id;
    }
    const hash = email_digest(actor.email);
    const matches = registry.individuals.filter(record =>
        record.withdrawn_at === null && record.email_sha256.includes(hash));
    require_value(matches.length > 0,
        "An unlinked author needs a verified commit-email mapping in the coverage registry.");
    return matches[0].github_id;
}

function evaluate_coverage(pr, actors, registry, now)
{
    const review = registry.material_reviews.findLast(item =>
        item.pull_request === pr.number && item.head_sha === pr.head.sha &&
        item.disclosure_sha256 === text_digest(pr.body || "") && is_active(item, now));
    const missing = [];
    const contribution_time = review ? Date.parse(review.verified_at) : now;
    const declared_corporate = (review?.corporate_authorization_ids || []).map(id =>
        registry.corporate_authorizations.find(item => item.id === id));
    const declared = [
        ...actors,
        ...(review?.individual_github_ids || []).map(github_id => ({ github_id })),
        ...declared_corporate.map(record => ({ github_id: record.github_id }))
    ];
    for (const actor of declared) {
        const exception = registry.exceptions.find(item =>
            item.pull_request === pr.number && item.head_sha === pr.head.sha &&
            item.rights_irrevocable === true && is_active(item, now) &&
            (valid_id(actor.github_id)
                ? item.github_id === actor.github_id
                : item.email_sha256 === email_digest(actor.email)));
        if (exception) {
            continue;
        }
        let id;
        try {
            id = verified_actor_id(actor, registry);
        }
        catch (error) {
            missing.push(error.message);
            continue;
        }
        const individual = registry.individuals.find(item =>
            item.github_id === id && Date.parse(item.signed_at) <= contribution_time && is_active(item, now));
        if (!individual) {
            missing.push(`GitHub account ${id} has no active verified individual CLA.`);
            continue;
        }
        if (!review) {
            missing.push(`GitHub account ${id} needs a maintainer's ownership-capacity review.`);
            continue;
        }
        const corporate = declared_corporate.filter(item => item.github_id === id);
        if (!review.individual_github_ids.includes(id) && corporate.length === 0) {
            missing.push(`GitHub account ${id} needs a maintainer's ownership-capacity review.`);
        }
        // Mixed ownership needs every declared grant; a personal grant cannot replace employer rights.
        for (const authorization of corporate) {
            if (!(Date.parse(authorization.valid_from) <= contribution_time &&
                (authorization.valid_until === null || contribution_time < Date.parse(authorization.valid_until)) &&
                is_active(authorization, now) &&
                (authorization.ending === undefined || is_active(authorization.ending, now)) &&
                registry.corporations.some(signature =>
                    signature.id === authorization.corporation_id &&
                    Date.parse(signature.signed_at) <= contribution_time && is_active(signature, now))))
            {
                missing.push(`GitHub account ${id} has an inactive required corporate authorisation.`);
            }
        }
    }
    const declarations = [...(pr.body || "").matchAll(
        /^Material origin:[ \t]*(original|imported-or-adapted)[ \t]*$/gmi)];
    const material_ok = Boolean(review) && declarations.length === 1 &&
        ((declarations[0][1].toLowerCase() === "original" && review.disposition === "original") ||
         (declarations[0][1].toLowerCase() === "imported-or-adapted" &&
            review.disposition === "imports-approved"));
    return { coverage_ok: missing.length === 0, material_ok, missing: [...new Set(missing)], review };
}

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
        require_value(response.ok, `GitHub API ${method} ${route.split("?")[0]} failed (${response.status}).`);
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

function agreement_texts()
{
    return Object.fromEntries(Object.entries(AGREEMENT_PATHS).map(([id, file]) =>
        [id, fs.readFileSync(path.join(SOURCE_ROOT, file), "utf8")]));
}

async function registry_snapshot(api, repository)
{
    const ref = await api.request("GET", `/repos/${repository}/git/ref/heads/${RECORDS_BRANCH}`);
    const file = await api.request("GET",
        `/repos/${repository}/contents/${RECORDS_PATH}?ref=${ref.object.sha}`);
    require_value(file.type === "file" && file.encoding === "base64" && typeof file.content === "string",
        "The coverage registry could not be read completely.");
    return { sha: ref.object.sha,
        registry: JSON.parse(Buffer.from(file.content, "base64").toString("utf8")), file_sha: file.sha };
}

async function post_status(api, repository, head_sha, context, state, description, target_url)
{
    await api.request("POST", `/repos/${repository}/statuses/${head_sha}`,
        { context, state, description, target_url });
}

async function invalidate_heads(api, repository, pulls, target_url, contexts)
{
    for (const head of new Set(pulls.map(pr => pr.head.sha))) {
        for (const context of contexts) {
            await post_status(api, repository, head, context, "pending",
                "Checking contribution rights and material review.", target_url);
        }
    }
}

async function check_repository(api, repository, requested_number, target_url, now = Date.now())
{
    const repo = await api.request("GET", `/repos/${repository}`);
    const open = await api.list(`/repos/${repository}/pulls?state=open`);
    let pulls = open;
    if (requested_number !== null) {
        const requested = open.find(pr => pr.number === requested_number);
        require_value(requested, "The requested pull request is not open.");
        pulls = open.filter(pr => pr.head.sha === requested.head.sha);
    }
    if (pulls.length === 0) {
        process.stdout.write("No open pull requests need contribution checks.\n");
        return true;
    }
    const outcomes = new Map();
    let contexts = [];
    try {
        const snapshot = await registry_snapshot(api, repository);
        contexts = status_contexts(snapshot.registry.generation);
        await invalidate_heads(api, repository, pulls, target_url, contexts);
        const registry = validate_registry(snapshot.registry, repo.id, agreement_texts());
        for (const listed of pulls) {
            const pr = await api.request("GET", `/repos/${repository}/pulls/${listed.number}`);
            require_value(pr.state === "open" && pr.head.sha === listed.head.sha,
                "An open PR changed while the contribution check was starting; recheck it.");
            const actors = await collect_actors(api, repository, pr);
            const outcome = evaluate_coverage(pr, actors, registry, now);
            if (outcome.material_ok && outcome.review.disposition === "imports-approved") {
                for (const file of outcome.review.provenance_paths) {
                    const encoded = file.split("/").map(encodeURIComponent).join("/");
                    const provenance = await api.request("GET",
                        `/repos/${repository}/contents/${encoded}?ref=${pr.head.sha}`);
                    require_value(provenance.type === "file" && provenance.sha,
                        "Reviewed import provenance must be a tracked file in this PR head.");
                }
            }
            const current = await api.request("GET", `/repos/${repository}/pulls/${pr.number}`);
            require_value(current.state === "open" && current.head.sha === pr.head.sha &&
                text_digest(current.body || "") === text_digest(pr.body || ""),
                "The PR head or disclosure changed during the check; recheck it.");
            const head_outcome = outcomes.get(pr.head.sha) || { coverage_ok: true, material_ok: true };
            head_outcome.coverage_ok &&= outcome.coverage_ok;
            head_outcome.material_ok &&= outcome.material_ok;
            outcomes.set(pr.head.sha, head_outcome);
            process.stdout.write(`PR #${pr.number}: CLA ${outcome.coverage_ok ? "covered" : "not covered"}; ` +
                `material review ${outcome.material_ok ? "approved" : "required"}.\n`);
            for (const failure of outcome.missing) {
                process.stdout.write(`  ${failure}\n`);
            }
        }
        const current_ref = await api.request("GET",
            `/repos/${repository}/git/ref/heads/${RECORDS_BRANCH}`);
        require_value(current_ref.object.sha === snapshot.sha,
            "Contribution records changed during the check; recheck open PRs.");
        for (const [head, outcome] of outcomes) {
            await post_status(api, repository, head, contexts[0],
                outcome.coverage_ok ? "success" : "failure",
                outcome.coverage_ok ? "All declared authors have verified rights coverage." :
                    "Verified author coverage or ownership-capacity review is required.", target_url);
            await post_status(api, repository, head, contexts[1],
                outcome.material_ok ? "success" : "failure",
                outcome.material_ok ? "The exact contribution and disclosed origins were reviewed." :
                    "A matching maintainer material/licence review is required.", target_url);
        }
        return [...outcomes.values()].every(outcome => outcome.coverage_ok && outcome.material_ok);
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

function empty_registry(repository_id, custodians, texts)
{
    return {
        schema_version: 1,
        generation: 0,
        repository_id,
        custodians,
        agreements: Object.entries(AGREEMENT_PATHS).map(([id, file]) =>
            ({ id, path: file, sha256: text_digest(texts[id]) })),
        individuals: [], corporations: [], corporate_authorizations: [], exceptions: [], material_reviews: []
    };
}

function require_retained_records(previous, proposed)
{
    require_value(previous.repository_id === proposed.repository_id,
        "An update cannot change the repository identity.");
    for (const id of previous.custodians) {
        require_value(proposed.custodians.includes(id), "Retain the identities of historical record verifiers.");
    }
    for (const group of ["agreements", ...SIGNATURE_GROUPS]) {
        for (const record of previous[group]) {
            const replacement = proposed[group].find(item => item.id === record.id);
            require_value(replacement, `${record.id}: retained records cannot be deleted.`);
            const retained = { ...replacement };
            if (record.withdrawn_at === null && retained.withdrawn_at !== null) {
                retained.withdrawn_at = null;
            }
            if (group === "corporate_authorizations" && record.valid_until !== retained.valid_until)
            {
                require_value(retained.valid_until !== null &&
                    (record.valid_until === null ||
                        Date.parse(retained.valid_until) < Date.parse(record.valid_until)) &&
                    retained.ending && retained.ending.id !== record.ending?.id,
                    `${record.id}: authority may only be shortened with a new archived ending attestation.`);
                if (record.ending) {
                    require_value(Date.parse(retained.ending.verified_at) >= Date.parse(record.ending.verified_at),
                        `${record.id}: retain the chronology of authority-ending notices.`);
                }
                retained.valid_until = record.valid_until;
                if (record.ending === undefined) {
                    delete retained.ending;
                }
                else {
                    retained.ending = record.ending;
                }
            }
            require_value(canonical_json(record) === canonical_json(retained),
                `${record.id}: retained evidence is immutable; append a correction and withdraw the old record.`);
        }
    }
}

function canonical_json(value)
{
    if (Array.isArray(value)) {
        return `[${value.map(canonical_json).join(",")}]`;
    }
    if (value !== null && typeof value === "object") {
        return `{${Object.keys(value).sort().map(key =>
            `${JSON.stringify(key)}:${canonical_json(value[key])}`).join(",")}}`;
    }
    return JSON.stringify(value);
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
        message: "Create the contribution evidence registry\n\nKeep verified signing evidence separate from product PRs so coverage can be administered without bypassing product merge requirements.",
        tree: tree.sha, parents: []
    });
    await api.request("POST", `/repos/${repository}/git/refs`,
        { ref: `refs/heads/${RECORDS_BRANCH}`, sha: commit.sha });
    process.stdout.write(`Created ${RECORDS_BRANCH} with empty coverage; no signatures were assumed.\n`);
}

async function advance_required_generation(api, repository, ruleset_id, current_generation, default_branch)
{
    const route = `/repos/${repository}/rulesets/${ruleset_id}`;
    const ruleset = await api.request("GET", route);
    require_value(ruleset.target === "branch" && ruleset.enforcement === "active" &&
        (ruleset.bypass_actors || []).length === 0,
        "The contribution ruleset must be active without bypass actors.");
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

async function publish_records(api, repository, filename, ruleset_id)
{
    const repo = await api.request("GET", `/repos/${repository}`);
    const snapshot = await registry_snapshot(api, repository);
    const proposed = JSON.parse(fs.readFileSync(filename, "utf8"));
    require_value(proposed.generation === snapshot.registry.generation,
        "Edit a fresh copy of the current registry before publishing.");
    validate_registry(proposed, repo.id, agreement_texts());
    require_retained_records(snapshot.registry, proposed);
    require_value(valid_id(ruleset_id), "--ruleset must name the installed contribution ruleset ID.");
    await advance_required_generation(api, repository, ruleset_id, snapshot.registry.generation,
        repo.default_branch);
    proposed.generation += 1;
    await api.request("PUT", `/repos/${repository}/contents/${RECORDS_PATH}`, {
        branch: RECORDS_BRANCH,
        sha: snapshot.file_sha,
        content: Buffer.from(JSON.stringify(proposed, null, 2) + "\n").toString("base64"),
        message: "Update verified contribution evidence\n\nRetain executed agreement references, authorisation and material-review decisions for the required contribution checks."
    });
    process.stdout.write(`Published records generation ${proposed.generation}; ` +
        "the CLA-only workflow will recheck open PRs.\n");
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
    if (command === "disclosure-hash") {
        process.stdout.write(text_digest(fs.readFileSync(options.file, "utf8")) + "\n");
        return;
    }
    if (command === "validate-records") {
        const registry = JSON.parse(fs.readFileSync(options.file, "utf8"));
        validate_registry(registry, registry.repository_id, agreement_texts());
        process.stdout.write("Contribution records are structurally valid and match published agreements.\n");
        return;
    }
    const repository = options.repository || process.env.GITHUB_REPOSITORY;
    require_value(typeof repository === "string" && /^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/.test(repository),
        "Use --repository owner/name.");
    const api = new GitHub_api(process.env.GH_TOKEN || process.env.GITHUB_TOKEN);
    if (command === "bootstrap") {
        const custodian = Number(options.custodian);
        require_value(valid_id(custodian), "--custodian must be a verified GitHub numeric account ID.");
        await bootstrap_records(api, repository, custodian);
        return;
    }
    if (command === "publish-records") {
        await publish_records(api, repository, options.file, Number(options.ruleset));
        return;
    }
    require_value(command === "check", "Use check, bootstrap, publish-records, validate-records, email-hash or disclosure-hash.");
    const target_url = process.env.GITHUB_RUN_ID
        ? `https://github.com/${repository}/actions/runs/${process.env.GITHUB_RUN_ID}`
        : `https://github.com/${repository}/actions/workflows/contribution-coverage.yml`;
    const number = options["pull-request"] ? Number(options["pull-request"]) : null;
    require_value(number === null || valid_id(number), "--pull-request must be a positive PR number.");
    if (!await check_repository(api, repository, number, target_url)) {
        process.exitCode = 1;
    }
}

module.exports = {
    AGREEMENT_PATHS, CLA_STATUS, MATERIAL_STATUS, GitHub_api,
    sha256, text_digest, email_digest, validate_registry, empty_registry,
    evaluate_coverage, collect_actors, check_repository, require_retained_records,
    status_contexts, advance_required_generation, publish_records, main
};

if (require.main === module) {
    main(process.argv.slice(2)).catch(error => {
        process.stderr.write(`Contribution check: ${error.message}\n`);
        process.exitCode = 1;
    });
}
