"use strict";

const crypto = require("node:crypto");

const VERSION = "v1.1";
const AGREEMENT_PATHS = {
    "individual-v1.0": "docs/contributions/individual-cla-v1.0.md",
    "corporate-v1.0":  "docs/contributions/corporate-cla-v1.0.md",
    "individual-v1.1": "docs/contributions/individual-cla-v1.1.md",
    "corporate-v1.1":  "docs/contributions/corporate-cla-v1.1.md"
};
const AGREEMENT_IDENTIFIERS = {
    "individual-v1.1": "Varinomics-ICLA-1.1",
    "corporate-v1.1":  "Varinomics-CCLA-1.1"
};

function require_value(condition, message)
{
    if (!condition) {
        throw new Error(message);
    }
}

function sha256(value)       { return crypto.createHash("sha256").update(value).digest("hex"); }
function text_digest(value) { return sha256(value.replace(/\r\n/g, "\n")); }
function valid_id(value)    { return Number.isSafeInteger(value) && value > 0; }
function valid_hash(value)  { return typeof value === "string" && /^[a-f0-9]{64}$/.test(value); }
function valid_head(value)  { return typeof value === "string" && /^[a-f0-9]{40}$/.test(value); }
function record_id(value)   { return typeof value === "string" && /^[A-Za-z0-9][A-Za-z0-9._:-]{2,159}$/.test(value); }

function email_digest(email)
{
    require_value(typeof email === "string" && email.trim().includes("@"),
        "An author needs a GitHub-linked commit email or a verified identity mapping.");
    return sha256(email.trim().toLowerCase());
}

function timestamp(value, label)
{
    require_value(typeof value === "string" &&
        /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{3})?Z$/.test(value) &&
        Number.isFinite(Date.parse(value)), `${label} must be a UTC timestamp.`);
    return Date.parse(value);
}

function status_contexts(generation)
{
    require_value(Number.isSafeInteger(generation) && generation >= 0,
        "The coverage registry needs a nonnegative generation.");
    return ["cla/coverage", "contributions/material-review"].map(prefix => `${prefix} (records ${generation})`);
}

function evidence(record, registry)
{
    require_value(record_id(record.id) && record_id(record.evidence_id) &&
        valid_hash(record.evidence_sha256) && registry.custodians.includes(record.verified_by),
        "An administrative exception or correction needs an identified custodian and retained evidence.");
    timestamp(record.verified_at, `${record.id}.verified_at`);
}

function validate_registry(registry, repository_id, texts)
{
    status_contexts(registry.generation);
    require_value(registry.schema_version === 1 && valid_id(repository_id) &&
        registry.repository_id === repository_id, "The coverage registry belongs to a different repository.");
    require_value(Array.isArray(registry.custodians) && registry.custodians.length > 0 &&
        registry.custodians.every(valid_id), "The registry needs identified record custodians.");
    require_value(Array.isArray(registry.agreements), "The registry needs its versioned agreement digests.");
    const ids = new Set();
    for (const agreement of registry.agreements) {
        require_value(record_id(agreement.id) && !ids.has(agreement.id) &&
            AGREEMENT_PATHS[agreement.id] === agreement.path &&
            typeof texts[agreement.id] === "string" && text_digest(texts[agreement.id]) === agreement.sha256,
            "A versioned agreement does not match its published text.");
        ids.add(agreement.id);
    }
    require_value(Object.keys(AGREEMENT_IDENTIFIERS).every(id => ids.has(id)),
        "The online individual and corporate agreements must be published before signing.");
    const online = registry.online_signing;
    require_value(online && online.schema_version === 1,
        "The repository has not enabled the online-signing capability.");
    for (const group of ["aliases", "exceptions", "invalid_receipts", "authority_endings"]) {
        require_value(Array.isArray(online[group]), `Online signing needs a ${group} array.`);
        for (const record of online[group]) {
            evidence(record, registry);
            require_value(!ids.has(record.id), "Administrative record IDs must be unique.");
            ids.add(record.id);
        }
    }
    const aliases = new Map();
    for (const alias of online.aliases) {
        require_value(valid_hash(alias.email_sha256) && valid_id(alias.github_id),
            "A verified alias must identify one commit-email hash and GitHub account.");
        if (alias.withdrawn_at !== null) {
            timestamp(alias.withdrawn_at, `${alias.id}.withdrawn_at`);
            continue;
        }
        require_value(!aliases.has(alias.email_sha256) || aliases.get(alias.email_sha256) === alias.github_id,
            "An active commit-email alias cannot identify two accounts.");
        aliases.set(alias.email_sha256, alias.github_id);
    }
    for (const exception of online.exceptions) {
        require_value(valid_id(exception.pull_request) && valid_head(exception.head_sha) &&
            (valid_id(exception.github_id) !== valid_hash(exception.email_sha256)) &&
            exception.rights_irrevocable === true && !("valid_until" in exception) &&
            ["varinomics-owned", "separately-licensed", "mechanical-bot"].includes(exception.basis) &&
            typeof exception.reason === "string" && exception.reason.trim().length > 0,
            "An exception needs durable rights for one exact author, PR and head.");
        if (exception.withdrawn_at !== null) {
            timestamp(exception.withdrawn_at, `${exception.id}.withdrawn_at`);
        }
    }
    for (const invalid of online.invalid_receipts) {
        require_value(/^comment-\d+$/.test(invalid.receipt_id) &&
            typeof invalid.reason === "string" && invalid.reason.trim().length > 0,
            "An invalid receipt correction must identify the disputed receipt and reason.");
    }
    for (const ending of online.authority_endings) {
        require_value(/^comment-\d+$/.test(ending.receipt_id) &&
            (ending.github_id === null || valid_id(ending.github_id)) &&
            typeof ending.reason === "string" && ending.reason.trim().length > 0 &&
            !("valid_until" in ending) && !("effective_at" in ending),
            "An authority ending identifies existing company evidence and stops automatic reliance immediately.");
    }
    return registry;
}

function empty_registry(repository_id, custodians, texts)
{
    return {
        schema_version: 1, generation: 0, repository_id, custodians,
        agreements: Object.entries(AGREEMENT_PATHS).map(([id, file]) =>
            ({ id, path: file, sha256: text_digest(texts[id]) })),
        online_signing: { schema_version: 1, aliases: [], exceptions: [], invalid_receipts: [], authority_endings: [] }
    };
}

function upgrade_empty_registry(previous, texts)
{
    require_value(!previous.online_signing && ["individuals", "corporations", "corporate_authorizations",
        "exceptions", "material_reviews"].every(group => Array.isArray(previous[group]) && previous[group].length === 0),
        "Automatic conversion is limited to the deployed registry with no executed signing evidence.");
    const next = empty_registry(previous.repository_id, previous.custodians, texts);
    next.generation = previous.generation;
    for (const agreement of previous.agreements) {
        require_value(next.agreements.some(item => item.id === agreement.id && item.sha256 === agreement.sha256),
            "Preserve every published agreement version during conversion.");
    }
    return next;
}

function parse_command(body)
{
    const text = body.trim();
    if (text === `/cla agree individual ${VERSION}`) {
        return { kind: "individual", agreement_id: "individual-v1.1", authors: [] };
    }
    const company = /^\/cla (agree corporate|authorize) v1\.1 (\{[\s\S]*\})$/.exec(text);
    if (company) {
        let fields;
        try {
            fields = JSON.parse(company[2]);
        }
        catch {
            throw new Error("Use the copyable corporate CLA command with valid JSON company details.");
        }
        require_value(fields && !Array.isArray(fields) && typeof fields.company === "string" &&
            fields.company.trim().length > 0 && fields.company.length <= 300 &&
            !/[\u0000-\u001f]/.test(fields.company), "Provide the exact company legal name.");
        require_value(Object.keys(fields).every(key => ["company", "role", "authors"].includes(key)),
            "Use only company, role and optional authors in the corporate command.");
        require_value(fields.role === undefined || (typeof fields.role === "string" && fields.role.length <= 300),
            "The company signing role must be text.");
        require_value(fields.authors === undefined || (Array.isArray(fields.authors) &&
            fields.authors.every(item => valid_id(item) ||
                (typeof item === "string" && /^@?[A-Za-z0-9][A-Za-z0-9-]{0,38}$/.test(item)))),
            "Authorisations use GitHub handles, such as @alice.");
        require_value(company[1] !== "authorize" || fields.authors?.length > 0,
            "An authorisation must identify at least one GitHub contributor.");
        return { kind: company[1] === "authorize" ? "authorization" : "corporate",
            agreement_id: "corporate-v1.1", company: fields.company.trim(), role: fields.role || "",
            authors: fields.authors || [] };
    }
    const review = /^\/cla review imports ([a-f0-9]{40})$/.exec(text);
    if (review) {
        return { kind: "import-review", head_sha: review[1], authors: [] };
    }
    throw new Error(`Use /cla agree individual ${VERSION}, the corporate example, or the current import-review command.`);
}

async function resolve_authors(api, authors)
{
    const resolved = [];
    for (const author of authors) {
        const user = await api.request("GET", valid_id(author)
            ? `/user/${author}` : `/users/${author.replace(/^@/, "")}`);
        require_value(valid_id(user.id) && typeof user.login === "string", "A named GitHub contributor could not be resolved.");
        require_value(user.type === "User", "Bot authors require a specific reviewed contribution exception.");
        resolved.push({ input: author, github_id: user.id, login: user.login, account_type: user.type });
    }
    return resolved;
}

async function maintainer(api, repository, login)
{
    const permission = await api.request("GET", `/repos/${repository}/collaborators/${encodeURIComponent(login)}/permission`);
    return ["admin", "maintain", "write"].includes(permission.permission);
}

async function capture_receipt(api, repository, event, context, registry, texts)
{
    require_value(context.event_name === "issue_comment" && event.action === "created" &&
        event.issue?.pull_request && event.repository?.id === registry.repository_id &&
        valid_id(event.comment?.id) && valid_id(event.comment?.user?.id) &&
        event.comment.user.id === event.sender?.id && event.comment.user.type === "User",
        "Only an authenticated human's newly created PR comment can record CLA assent.");
    const command = parse_command(event.comment.body);
    if (command.kind === "import-review") {
        require_value(await maintainer(api, repository, event.comment.user.login),
            "An import review must be recorded by a repository maintainer.");
        const pr = await api.request("GET", `/repos/${repository}/pulls/${event.issue.number}`);
        require_value(pr.head.sha === command.head_sha, "Review the current PR head before approving imported material.");
    }
    const receipt = {
        schema_version: 1, id: `comment-${event.comment.id}`, repository_id: registry.repository_id,
        kind: command.kind, pull_request: event.issue.number,
        github_id: event.comment.user.id, login: event.comment.user.login,
        created_at: event.comment.created_at, comment_id: event.comment.id,
        comment_node_id: event.comment.node_id, comment_url: event.comment.html_url,
        assent_text: event.comment.body,
        event: { name: context.event_name, action: event.action, actor_id: event.sender.id,
            run_id: context.run_id, run_attempt: context.run_attempt, workflow_ref: context.workflow_ref },
        authorized_authors: await resolve_authors(api, command.authors)
    };
    if (command.agreement_id) {
        const agreement = registry.agreements.find(item => item.id === command.agreement_id);
        require_value(agreement, "This agreement version is not enabled for this repository.");
        receipt.agreement_id         = command.agreement_id;
        receipt.agreement_identifier = AGREEMENT_IDENTIFIERS[command.agreement_id];
        receipt.agreement_sha256     = agreement.sha256;
        receipt.agreement_text       = texts[command.agreement_id].replace(/\r\n/g, "\n");
    }
    if (command.company) {
        receipt.company = command.company;
        receipt.role    = command.role;
    }
    if (command.head_sha) {
        receipt.head_sha = command.head_sha;
    }
    validate_receipt(receipt, registry, texts);
    return receipt;
}

function validate_receipt(receipt, registry, texts)
{
    require_value(receipt.schema_version === 1 && receipt.repository_id === registry.repository_id &&
        valid_id(receipt.comment_id) && receipt.id === `comment-${receipt.comment_id}` &&
        valid_id(receipt.github_id) && valid_id(receipt.pull_request) &&
        typeof receipt.login === "string" && /^[A-Za-z0-9][A-Za-z0-9-]{0,38}$/.test(receipt.login) &&
        typeof receipt.comment_node_id === "string" && receipt.comment_node_id.length > 0 &&
        typeof receipt.comment_url === "string" && receipt.comment_url.startsWith("https://github.com/") &&
        receipt.event?.name === "issue_comment" && receipt.event.action === "created" &&
        receipt.event.actor_id === receipt.github_id && /^\d+$/.test(receipt.event.run_id) &&
        /^\d+$/.test(receipt.event.run_attempt) && typeof receipt.event.workflow_ref === "string",
        "A signing receipt lacks its authenticated creation event and stable contributor identity.");
    timestamp(receipt.created_at, `${receipt.id}.created_at`);
    const command = parse_command(receipt.assent_text);
    require_value(command.kind === receipt.kind && command.company === receipt.company &&
        (command.role === undefined || command.role === receipt.role) && command.head_sha === receipt.head_sha &&
        Array.isArray(receipt.authorized_authors) && receipt.authorized_authors.every(item =>
            valid_id(item.github_id) && typeof item.login === "string") &&
        receipt.authorized_authors.length === command.authors.length &&
        receipt.authorized_authors.every((item, index) =>
            item.input === command.authors[index] && item.account_type === "User"),
        "A receipt must preserve the exact assent and resolved authorisations.");
    if (command.agreement_id) {
        const agreement = registry.agreements.find(item => item.id === command.agreement_id);
        require_value(agreement && receipt.agreement_id === command.agreement_id &&
            receipt.agreement_identifier === AGREEMENT_IDENTIFIERS[command.agreement_id] &&
            receipt.agreement_sha256 === agreement.sha256 &&
            text_digest(receipt.agreement_text) === agreement.sha256 &&
            text_digest(texts[command.agreement_id]) === agreement.sha256,
            "The retained receipt must match the exact agreement version and complete text.");
    }
    return receipt;
}

function active_receipts(receipts, registry)
{
    const invalid = new Set(registry.online_signing.invalid_receipts.map(item => item.receipt_id));
    return receipts.filter(receipt => !invalid.has(receipt.id));
}

function evaluate_coverage(pr, actors, registry, receipts)
{
    const active = active_receipts(receipts, registry);
    const company_usable = (receipt, id) => !registry.online_signing.authority_endings.some(ending =>
        ending.receipt_id === receipt.id && (ending.github_id === null || ending.github_id === id));
    const missing = [];
    for (const actor of actors) {
        const alias = actor.github_id ? null : registry.online_signing.aliases.find(item =>
            item.withdrawn_at === null && item.email_sha256 === email_digest(actor.email));
        const id = actor.github_id || alias?.github_id;
        const exception = registry.online_signing.exceptions.some(item =>
            item.withdrawn_at === null && item.pull_request === pr.number && item.head_sha === pr.head.sha &&
            (id ? item.github_id === id : item.email_sha256 === email_digest(actor.email)));
        if (exception) {
            continue;
        }
        if (!id) {
            missing.push({ login: null, reason: "An unlinked commit author needs GitHub email association or a verified identity mapping." });
            continue;
        }
        const direct = active.some(receipt => receipt.github_id === id &&
            (receipt.kind === "individual" || (receipt.kind === "corporate" && company_usable(receipt, id))));
        const company = active.some(receipt => ["corporate", "authorization"].includes(receipt.kind) &&
            company_usable(receipt, id) &&
            receipt.authorized_authors.some(author => author.github_id === id) &&
            active.some(signer => signer.kind === "corporate" && signer.github_id === receipt.github_id &&
                signer.company === receipt.company && company_usable(signer, id) &&
                Date.parse(signer.created_at) <= Date.parse(receipt.created_at)));
        if (!direct && !company) {
            missing.push({ github_id: id, login: actor.login || alias?.login || null,
                reason: "This contributor needs an individual assent or an applicable company authorisation." });
        }
    }
    const unique = [...new Map(missing.map(item => [item.github_id || item.reason, item])).values()];
    return { coverage_ok: unique.length === 0, missing: unique };
}

function material_disclosure(body)
{
    const origins = [...body.matchAll(/^Material origin:[ \t]*(original|imported-or-adapted)[ \t]*$/gmi)];
    if (origins.length !== 1) {
        return { origin: null, paths: [] };
    }
    const origin = origins[0][1].toLowerCase();
    const lines = [...body.matchAll(/^Provenance records:[ \t]*(.+)[ \t]*$/gmi)];
    const paths = lines.length === 1 && !/^none$/i.test(lines[0][1].trim())
        ? lines[0][1].split(",").map(item => item.trim().replace(/^`|`$/g, "")) : [];
    require_value(paths.every(item => item.length > 0 && !item.startsWith("/") &&
        !item.split("/").includes("..") && !/[\u0000-\u001f\\]/.test(item)),
        "Provenance records must identify tracked relative file paths.");
    return { origin, paths };
}

async function review_material(api, repository, pr, registry, receipts)
{
    const disclosure = material_disclosure(pr.body || "");
    if (disclosure.origin === "original") {
        return true;
    }
    if (disclosure.origin !== "imported-or-adapted" || disclosure.paths.length === 0) {
        return false;
    }
    for (const file of disclosure.paths) {
        const encoded = file.split("/").map(encodeURIComponent).join("/");
        const record = await api.request("GET", `/repos/${repository}/contents/${encoded}?ref=${pr.head.sha}`);
        require_value(record.type === "file", "An imported-material origin/licence record is not a tracked file.");
    }
    const explicit = active_receipts(receipts, registry).filter(receipt =>
        receipt.kind === "import-review" && receipt.pull_request === pr.number && receipt.head_sha === pr.head.sha);
    for (const receipt of explicit) {
        if (await maintainer(api, repository, receipt.login)) {
            return true;
        }
    }
    const reviews = await api.list(`/repos/${repository}/pulls/${pr.number}/reviews`);
    const latest = new Map();
    for (const review of reviews) {
        if (["APPROVED", "CHANGES_REQUESTED", "DISMISSED"].includes(review.state)) {
            latest.set(review.user.id, review);
        }
    }
    for (const review of latest.values()) {
        if (review.state === "APPROVED" && review.commit_id === pr.head.sha &&
            await maintainer(api, repository, review.user.login))
        {
            return true;
        }
    }
    return false;
}

function signing_prompt(pr, outcome, material_ok)
{
    const root = "https://github.com/Varinomics/vnm_terminal_surface/blob/master/docs/contributions/";
    const who = outcome.missing.map(item => item.login ? `@${item.login}` : "an unlinked author").join(", ") ||
        "one or more declared authors";
    let body = "<!-- varinomics-contribution-check -->\n";
    if (outcome.problem) {
        body += "A declared author or provenance record could not be verified. Correct the PR declaration and check the workflow details.\n\n";
    }
    if (!outcome.coverage_ok) {
        body += `CLA ${VERSION} is needed for ${who}. Sign once in this repository by posting:\n\n` +
            "```text\n/cla agree individual v1.1\n```\n\n" +
            `Read the [individual agreement](${root}individual-cla-v1.1.md) and ` +
            `[signing privacy notice](${root}privacy.md) first.\n\n` +
            "If you have authority to bind a company owning the work, use:\n\n" +
            '```text\n/cla agree corporate v1.1 {"company":"Exact legal entity","role":"Signing authority"}\n```\n\n' +
            `Read the [corporate agreement](${root}corporate-cla-v1.1.md). ` +
            'Add optional `"authors":["@alice","@bob"]` to authorise other company contributors. ' +
            "Each actual signer or authorised contributor must be covered; one person cannot sign another person's rights.\n\n";
        if (outcome.missing.some(item => !item.github_id)) {
            body += "An unlinked author must associate the commit email with their GitHub account or have its identity verified.\n\n";
        }
    }
    if (!material_ok) {
        body += "Declare `Material origin: original` or `Material origin: imported-or-adapted` in the PR description. " +
            "For imports, list `Provenance records: relative/path` with origins, actual licences, notices and obligations. " +
            "A maintainer must review the current head's imported material and approve it, or post:\n\n" +
            `\`\`\`text\n/cla review imports ${pr.head.sha}\n\`\`\`\n\n` +
            "That review command confirms the actual licences and obligations were checked; a CLA signature alone does not approve an import.\n";
    }
    if (outcome.coverage_ok && material_ok) {
        body += "All declared authors are covered. The material-origin check has passed.\n";
    }
    return body;
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

function require_retained_records(previous, proposed)
{
    require_value(previous.repository_id === proposed.repository_id && previous.custodians.every(id =>
        proposed.custodians.includes(id)), "Preserve the repository identity and historical custodians.");
    for (const agreement of previous.agreements) {
        require_value(proposed.agreements.some(item => canonical_json(item) === canonical_json(agreement)),
            "Retain every published agreement version and digest.");
    }
    if (!previous.online_signing) {
        require_value(["individuals", "corporations", "corporate_authorizations", "exceptions", "material_reviews"]
            .every(group => Array.isArray(previous[group]) && previous[group].length === 0),
            "Automatic conversion is limited to the deployed registry with no executed signing evidence.");
        return;
    }
    for (const group of ["aliases", "exceptions", "invalid_receipts", "authority_endings"]) {
        for (const record of previous.online_signing[group]) {
            const replacement = proposed.online_signing[group].find(item => item.id === record.id);
            require_value(replacement, "Retained administrative evidence cannot be deleted.");
            const retained = { ...replacement };
            if (record.withdrawn_at === null && retained.withdrawn_at !== null) {
                retained.withdrawn_at = null;
            }
            require_value(canonical_json(record) === canonical_json(retained),
                "Retained evidence is immutable; append a correction and withdraw the mistaken assertion.");
        }
    }
}

module.exports = {
    VERSION, AGREEMENT_PATHS, require_value, sha256, text_digest, email_digest, valid_id, valid_hash,
    timestamp, status_contexts, validate_registry, empty_registry, upgrade_empty_registry,
    parse_command, capture_receipt, validate_receipt, evaluate_coverage, review_material,
    signing_prompt, require_retained_records, canonical_json
};
