'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { execFileSync, spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { pathToFileURL } = require('node:url');
const script = path.join(__dirname, 'checkout-deps.js');

function fixture(t) {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'dependency-checkout-'));
    t.after(() => fs.rmSync(root, { recursive: true, force: true }));
    const source = path.join(root, 'source with spaces'); fs.mkdirSync(source);
    const git = (cwd, ...args) => execFileSync('git', args, { cwd, encoding: 'utf8', stdio: ['pipe', 'pipe', 'pipe'] }).trim();
    git(source, 'init', '-b', 'fork-only'); git(source, 'config', 'user.name', 'Fixture'); git(source, 'config', 'user.email', 'fixture@example.invalid');
    const commit = (content) => { fs.writeFileSync(path.join(source, 'payload'), content); git(source, 'add', 'payload'); git(source, 'commit', '-m', content); return git(source, 'rev-parse', 'HEAD'); };
    const pinned = commit('pinned'); git(source, 'tag', '-a', 'release-tag', '-m', 'fixture'); const latest = commit('new branch tip');
    git(source, 'branch', 'collision'); git(source, 'tag', '-a', 'collision', pinned, '-m', 'older tag');
    const url = pathToFileURL(source).href;
    const run = (ref, name = 'target with spaces', repository = url) => {
        const target = path.join(root, name);
        const result = spawnSync(process.execPath, [script, repository, ref, target], { encoding: 'utf8' });
        return { ...result, target, head: result.status === 0 ? git(target, 'rev-parse', 'HEAD') : undefined };
    };
    return { root, run, pinned, latest, git };
}
test('branch names fetch the configured tip', (t) => {
    const f = fixture(t); const r = f.run('fork-only'); assert.equal(r.status, 0, r.stderr); assert.equal(r.head, f.latest);
});
test('annotated tags resolve to their commit', (t) => {
    const f = fixture(t); const r = f.run('release-tag'); assert.equal(r.status, 0, r.stderr); assert.equal(r.head, f.pinned);
});
test('pinned SHA stays at the requested ancestor after the branch moves', (t) => {
    const f = fixture(t); const r = f.run(f.pinned); assert.equal(r.status, 0, r.stderr); assert.equal(r.head, f.pinned);
    assert.equal(fs.readFileSync(path.join(r.target, 'payload'), 'utf8'), 'pinned');
    assert.notEqual(r.head, f.latest); assert.equal(f.git(r.target, 'rev-parse', '--is-shallow-repository'), 'true');
});
test('an existing empty destination works', (t) => {
    const f = fixture(t); fs.mkdirSync(path.join(f.root, 'empty')); const r = f.run(f.pinned, 'empty'); assert.equal(r.status, 0, r.stderr);
});
test('a nonempty destination is preserved and fails', (t) => {
    const f = fixture(t); const dir = path.join(f.root, 'existing'); fs.mkdirSync(dir); fs.writeFileSync(path.join(dir, 'sentinel'), 'keep');
    const r = f.run(f.pinned, 'existing'); assert.notEqual(r.status, 0); assert.equal(fs.readFileSync(path.join(dir, 'sentinel'), 'utf8'), 'keep'); assert(!fs.existsSync(path.join(dir, '.git')));
});
test('missing and malformed refs fail instead of falling back to a branch', (t) => {
    const f = fixture(t);
    for (const [i, ref] of ['missing', '1'.repeat(40), '--upload-pack=x', '', 'bad\nref'].entries()) {
        assert.notEqual(f.run(ref, `bad-${i}`).status, 0);
    }
});
test('a missing repository fails rather than claiming a checkout', (t) => {
    const f = fixture(t); assert.notEqual(f.run(f.pinned, 'missing', pathToFileURL(path.join(f.root, 'absent')).href).status, 0);
});

test('failed fetch removes staging and permits retry at the same destination', (t) => {
    const f = fixture(t); const failed = f.run('missing', 'retry');
    assert.notEqual(failed.status, 0); assert(!fs.existsSync(failed.target));
    assert(!fs.readdirSync(f.root).some(name => name.startsWith('.retry-download-')));
    const retry = f.run(f.pinned, 'retry'); assert.equal(retry.status, 0, retry.stderr); assert.equal(retry.head, f.pinned);
});
test('failed fetch preserves an existing empty destination', (t) => {
    const f = fixture(t); const target = path.join(f.root, 'empty-failed'); fs.mkdirSync(target);
    const result = f.run('missing', 'empty-failed'); assert.notEqual(result.status, 0);
    assert.deepEqual(fs.readdirSync(target), []);
    assert(!fs.readdirSync(f.root).some(name => name.startsWith('.empty-failed-download-')));
});

test('a branch takes precedence over a same-named annotated tag', (t) => {
    const f = fixture(t); const result = f.run('collision');
    assert.equal(result.status, 0, result.stderr); assert.equal(result.head, f.latest);
});
test('fully qualified branch and tag refs remain unambiguous', (t) => {
    const f = fixture(t);
    const branch = f.run('refs/heads/collision', 'branch'); const tag = f.run('refs/tags/collision', 'tag');
    assert.equal(branch.status, 0, branch.stderr); assert.equal(branch.head, f.latest);
    assert.equal(tag.status, 0, tag.stderr); assert.equal(tag.head, f.pinned);
});
