'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { execFileSync, spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const helper = path.join(__dirname, 'get-eslint-files.js');

function fixture(t) {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'eslint-diff-'));
    t.after(() => fs.rmSync(root, { recursive: true, force: true }));
    const source = path.join(root, 'source');
    const remote = path.join(root, 'remote.git');
    fs.mkdirSync(source);
    const git = (cwd, ...args) => execFileSync('git', args, { cwd, encoding: 'utf8', stdio: ['pipe', 'pipe', 'pipe'] }).trim();
    git(source, 'init', '-b', 'main');
    git(source, 'config', 'user.name', 'Fixture');
    git(source, 'config', 'user.email', 'fixture@example.invalid');
    const write = (name, content) => { fs.mkdirSync(path.dirname(path.join(source, name)), { recursive: true }); fs.writeFileSync(path.join(source, name), content); };
    const commit = (message) => { git(source, 'add', '-A'); git(source, 'commit', '-m', message); return git(source, 'rev-parse', 'HEAD'); };
    write('cocos/old.ts', 'old\n');
    write('README.md', 'unchanged\n');
    const base = commit('base');
    git(root, 'clone', '--bare', source, remote);
    git(source, 'remote', 'add', 'origin', remote);
    git(source, 'checkout', '-b', 'feature');
    write('cocos/first.ts', 'first\n');
    const first = commit('first');
    write('pal/file with space.ts', 'space\n');
    write('exports/second.ts', 'second\n');
    fs.unlinkSync(path.join(source, 'cocos/old.ts'));
    const head = commit('second');
    git(source, 'push', 'origin', 'feature');
    const checkout = path.join(root, 'checkout');
    git(root, 'clone', '--depth=1', '--branch', 'feature', `file://${remote}`, checkout);
    const run = (event, extra = {}) => {
        const eventPath = path.join(root, 'event.json');
        fs.writeFileSync(eventPath, JSON.stringify(event));
        return spawnSync(process.execPath, [helper], { cwd: checkout, encoding: 'utf8', env: { ...process.env, GITHUB_SHA: head, GITHUB_BASE_REF: '', GITHUB_EVENT_PATH: eventPath, ...extra } });
    };
    return { run, git, checkout, head, base, first };
}

test('new shallow branch includes all pushed commits, excludes deletions and keeps spaces', (t) => {
    const f = fixture(t);
    const r = f.run({ before: '0'.repeat(40), ref: 'refs/heads/feature', repository: { default_branch: 'main' } });
    assert.equal(r.status, 0, r.stderr);
    assert.deepEqual(JSON.parse(r.stdout), ['cocos/first.ts', 'exports/second.ts', 'pal/file with space.ts']);
});
test('ordinary push uses before, not the branch merge-base', (t) => {
    const f = fixture(t); const r = f.run({ before: f.first });
    assert.equal(r.status, 0, r.stderr);
    assert.deepEqual(JSON.parse(r.stdout), ['exports/second.ts', 'pal/file with space.ts']);
});
test('pull request uses its base branch', (t) => {
    const f = fixture(t); const r = f.run({}, { GITHUB_BASE_REF: 'main' });
    assert.equal(r.status, 0, r.stderr);
    assert.deepEqual(JSON.parse(r.stdout), ['cocos/first.ts', 'exports/second.ts', 'pal/file with space.ts']);
});
test('initial default-branch push includes the complete tracked tree', (t) => {
    const f = fixture(t); const r = f.run({ before: '0'.repeat(40), ref: 'refs/heads/feature', repository: { default_branch: 'feature' } });
    assert.equal(r.status, 0, r.stderr);
    assert.deepEqual(JSON.parse(r.stdout), ['README.md', 'cocos/first.ts', 'exports/second.ts', 'pal/file with space.ts']);
});
test('empty comparison succeeds with an explicit empty list', (t) => {
    const f = fixture(t); const r = f.run({ before: f.head });
    assert.equal(r.status, 0, r.stderr); assert.deepEqual(JSON.parse(r.stdout), []);
});
test('missing comparison branch fails rather than silently skipping lint', (t) => {
    const f = fixture(t); const r = f.run({ before: '0'.repeat(40), repository: { default_branch: 'missing' } });
    assert.notEqual(r.status, 0); assert.equal(r.stdout, '');
});
test('invalid and unavailable before objects both fail', (t) => {
    const f = fixture(t);
    for (const before of ['--all', '1'.repeat(40)]) {
        const r = f.run({ before }); assert.notEqual(r.status, 0); assert.equal(r.stdout, '');
    }
});

const hasPowerShell = spawnSync('pwsh', ['-NoProfile', '-Command', 'exit 0']).status === 0;
if (process.env.GITHUB_ACTIONS === 'true') assert.ok(hasPowerShell, 'CI must run the PowerShell wrapper tests');
function wrapperFixture(t, files, nodeExit = 0, firstLintExit = 0) {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'eslint-wrapper-'));
    t.after(() => fs.rmSync(root, { recursive: true, force: true }));
    const nodeScript = `#!/bin/sh\nprintf '%s' '${JSON.stringify(files)}'\nexit ${nodeExit}\n`;
    const npxScript = `#!/bin/sh\nprintf '%s|%s|%s\\n' "$#" "$1" "$2" >> "$LINT_LOG"\nif [ "$2" = "cocos/first.ts" ]; then exit ${firstLintExit}; fi\nexit 0\n`;
    fs.writeFileSync(path.join(root, 'node'), nodeScript, { mode: 0o755 });
    fs.writeFileSync(path.join(root, 'npx'), npxScript, { mode: 0o755 });
    const log = path.join(root, 'lint.log');
    const result = spawnSync('pwsh', ['-NoProfile', '-NonInteractive', '-File', path.join(__dirname, 'eslint.ps1')], {
        encoding: 'utf8', env: { ...process.env, PATH: `${root}${path.delimiter}${process.env.PATH}`, LINT_LOG: log },
    });
    return { result, log: fs.existsSync(log) ? fs.readFileSync(log, 'utf8') : '' };
}
test('PowerShell preserves a failed lint when a later lint succeeds, and quotes spaces', { skip: !hasPowerShell }, (t) => {
    const f = wrapperFixture(t, ['cocos/first.ts', 'pal/file with space.ts'], 0, 1);
    assert.notEqual(f.result.status, 0);
    assert.equal(f.log, '2|eslint|cocos/first.ts\n2|eslint|pal/file with space.ts\n');
});
test('PowerShell rejects file discovery failure without invoking lint', { skip: !hasPowerShell }, (t) => {
    const f = wrapperFixture(t, [], 1);
    assert.notEqual(f.result.status, 0); assert.equal(f.log, '');
});
test('PowerShell accepts empty and non-TypeScript changes without stale failure', { skip: !hasPowerShell }, (t) => {
    for (const files of [[], ['README.md', 'cocos/native.cpp', 'cocos-extra/file.ts']]) {
        const f = wrapperFixture(t, files);
        assert.equal(f.result.status, 0, f.result.stderr); assert.equal(f.log, '');
    }
});
