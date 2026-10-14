const assert = require('assert/strict');
const { execFileSync } = require('child_process');
const fs = require('fs');
const path = require('path');
const { test } = require('node:test');
const { resolveExternalConfig } = require('./get-native-external-version');
const root = path.resolve(__dirname, '../..');
const script = path.join(__dirname, 'get-native-external-version.js');

test('repository, URL and checkout come from one config', () => {
    const from = { type: 'github', owner: 'fixture-owner', name: 'dependency', checkout: 'fork-only-branch' };
    assert.deepEqual(resolveExternalConfig({ from }), {
        version: 'fork-only-branch', repository: 'fixture-owner/dependency', url: 'https://github.com/fixture-owner/dependency'
    });
});
test('CLI works in root and native working directories', () => {
    const { from } = JSON.parse(fs.readFileSync(path.join(root, 'native/external-config.json')));
    for (const cwd of [root, path.join(root, 'native')]) {
        for (const [arg, expected] of [['--version', from.checkout], ['--repository', `${from.owner}/${from.name}`], ['--url', `https://github.com/${from.owner}/${from.name}`]]) {
            assert.equal(execFileSync(process.execPath, [script, arg], { cwd, encoding: 'utf8' }).trim(), expected);
        }
    }
});
test('reject malformed repository or ref instead of emitting shell options', () => {
    const from = { type: 'github', owner: 'owner', name: 'repo', checkout: 'main' };
    for (const patch of [{ owner: '../elsewhere' }, { name: 'bad\nrepo' }, { checkout: '--upload-pack=x' }, { checkout: '' }, { type: 'unhandled' }]) {
        assert.throws(() => resolveExternalConfig({ from: { ...from, ...patch } }));
    }
});
test('all external workflows consume configured repository', () => {
    for (const name of fs.readdirSync(__dirname).filter(name => name.endsWith('.yml'))) {
        const workflow = fs.readFileSync(path.join(__dirname, name), 'utf8');
        assert(!workflow.includes('https://github.com/cocos/cocos-engine-external'), name);
        assert(!workflow.includes('repository: cocos/cocos-engine-external'), name);
        for (const block of workflow.split(/\n\s*- name:/)) {
            assert(!block.includes('git clone --branch "$EXT_VERSION"'), name);
            if (block.includes('checkout-deps.js')) assert(block.includes('--url'), name);
            if (block.includes('archive --format=tar FETCH_HEAD')) {
                assert(block.includes('remote set-url origin "$EXT_REPOSITORY"'), name);
                assert(block.includes('fetch --prune origin "$EXT_VERSION"'), name);
            }
        }
    }
});
