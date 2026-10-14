/* Resolve changed files for both pull requests and branch-creation pushes. */
'use strict';

const { execFileSync } = require('node:child_process');
const fs = require('node:fs');

function git(...args) {
    return execFileSync('git', args, { encoding: 'utf8', stdio: ['pipe', 'pipe', 'pipe'] }).trimEnd();
}

function commit(value, name) {
    if (!/^(?:[0-9a-f]{40}|[0-9a-f]{64})$/i.test(value || '')) {
        throw new Error(`Invalid ${name} commit`);
    }
    return value;
}

function branch(value) {
    if (!value || value.startsWith('-')) throw new Error('Missing or invalid comparison branch');
    git('check-ref-format', `refs/heads/${value}`);
    return `refs/heads/${value}`;
}

function changedFiles(env = process.env) {
    const head = commit(env.GITHUB_SHA, 'head');
    let base;
    if (env.GITHUB_BASE_REF) {
        git('fetch', '--no-tags', '--depth=1', 'origin', branch(env.GITHUB_BASE_REF));
        base = git('rev-parse', 'FETCH_HEAD');
    } else {
        const event = JSON.parse(fs.readFileSync(env.GITHUB_EVENT_PATH, 'utf8'));
        const before = commit(event.before, 'before');
        if (/^0+$/.test(before)) {
            // A new branch has no before object. Compare the entire branch with
            // its merge-base, not just HEAD's parent (a push can add many commits).
            const defaultBranch = event.repository?.default_branch;
            const ref = branch(defaultBranch);
            const remoteRef = `refs/remotes/origin/${defaultBranch}`;
            const args = ['fetch', '--no-tags'];
            if (git('rev-parse', '--is-shallow-repository') === 'true') args.push('--unshallow');
            git(...args, 'origin', head, `+${ref}:${remoteRef}`);
            if (event.ref === ref) {
                // The initial push of the default branch has no earlier tree.
                base = git('hash-object', '-t', 'tree', '--stdin');
            } else {
                base = git('merge-base', head, remoteRef);
            }
        } else {
            git('fetch', '--no-tags', '--depth=1', 'origin', before);
            base = before;
        }
    }
    const files = git('diff', '--name-only', '-z', '--diff-filter=d', base, head);
    return files ? files.split('\0').filter(Boolean) : [];
}

if (require.main === module) {
    try {
        process.stdout.write(JSON.stringify(changedFiles()));
    } catch (error) {
        process.stderr.write(`Cannot determine ESLint input: ${error.message}\n`);
        process.exitCode = 1;
    }
}
module.exports = { changedFiles };
