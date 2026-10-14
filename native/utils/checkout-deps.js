'use strict';

const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');

// Unlike clone --branch, fetch accepts a branch, tag, or an exact commit SHA.
function checkoutDependency(url, ref, targetDir) {
    for (const [name, value] of [['repository', url], ['ref', ref]]) {
        if (typeof value !== 'string' || !value || value.startsWith('-') || /[\x00-\x20]/.test(value)) {
            throw new Error(`Invalid dependency ${name}`);
        }
    }
    const target = path.resolve(targetDir);
    if (fs.existsSync(target) && (fs.lstatSync(target).isSymbolicLink() || fs.readdirSync(target).length !== 0)) {
        throw new Error(`Dependency destination is not empty: ${target}`);
    }
    fs.mkdirSync(path.dirname(target), { recursive: true });
    const staging = fs.mkdtempSync(path.join(path.dirname(target), `.${path.basename(target)}-download-`));
    try {
        const git = (...args) => execFileSync('git', args, { cwd: staging, stdio: 'inherit' });
        git('init', '--quiet');
        git('remote', 'add', 'origin', url);
        const isCommit = /^(?:[a-f0-9]{40}|[a-f0-9]{64})$/i.test(ref);
        let fetchRef = ref;
        if (!isCommit && !ref.startsWith('refs/')) {
            // Match clone --branch semantics: a branch wins over a same-named
            // tag. A bare fetch ref otherwise silently prefers the tag.
            git('check-ref-format', `refs/heads/${ref}`);
            const branchRef = `refs/heads/${ref}`;
            const tagRef = `refs/tags/${ref}`;
            const advertised = execFileSync('git', ['ls-remote', '--refs', 'origin', branchRef, tagRef], {
                cwd: staging, encoding: 'utf8',
            }).trim().split('\n').map(line => line.split('\t')[1]);
            if (advertised.includes(branchRef)) fetchRef = branchRef;
            else if (advertised.includes(tagRef)) fetchRef = tagRef;
            else throw new Error(`Dependency ref not found: ${ref}`);
        }
        git('fetch', '--no-tags', '--depth', '1', 'origin', fetchRef);
        git('checkout', '--detach', 'FETCH_HEAD');
        const head = execFileSync('git', ['rev-parse', 'HEAD'], { cwd: staging, encoding: 'utf8' }).trim();
        if (isCommit && head.toLowerCase() !== ref.toLowerCase()) {
            throw new Error('Dependency checkout does not match the pinned commit');
        }
        // Publish only a complete checkout. A failed fetch leaves the original
        // empty target untouched, so the same command can be retried safely.
        if (fs.existsSync(target)) fs.rmdirSync(target);
        fs.renameSync(staging, target);
        return head;
    } finally {
        fs.rmSync(staging, { recursive: true, force: true });
    }
}

if (require.main === module) {
    try {
        if (process.argv.length !== 5) throw new Error('Usage: checkout-deps.js <repository> <ref> <destination>');
        console.log(`Dependency checkout: ${checkoutDependency(...process.argv.slice(2))}`);
    } catch (error) {
        console.error(error.message);
        process.exitCode = 1;
    }
}
module.exports = { checkoutDependency };
