const path = require('path');
const fs = require('fs');

// Keep repository and ref together: forks can publish refs absent upstream.
function resolveExternalConfig(config) {
    const from = config.from || {};
    if (from.type !== 'github' || !/^[A-Za-z0-9_.-]+$/.test(from.owner || '') ||
        !/^[A-Za-z0-9_.-]+$/.test(from.name || '') ||
        typeof from.checkout !== 'string' || !from.checkout || /^[\-]/.test(from.checkout) || /[\s\x00-\x1f]/.test(from.checkout)) {
        throw new Error('Invalid GitHub source in native/external-config.json');
    }
    const repository = `${from.owner}/${from.name}`;
    return { version: from.checkout, repository, url: `https://github.com/${repository}` };
}

if (require.main === module) {
    const configJsonPath = path.join(__dirname, '../../native/external-config.json');
    const source = resolveExternalConfig(JSON.parse(fs.readFileSync(configJsonPath, 'utf8')));
    const field = (process.argv[2] || '--version').replace(/^--/, '');
    if (!Object.prototype.hasOwnProperty.call(source, field)) throw new Error(`Unknown source field: ${field}`);
    console.log(source[field]);
}
module.exports = { resolveExternalConfig };
