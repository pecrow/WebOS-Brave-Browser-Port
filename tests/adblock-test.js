'use strict';
var assert = require('assert');
var fs = require('fs');
var os = require('os');
var path = require('path');
var builder = require('../app/adblock-resources');
var updater = require('../app/update-adblock');
var configure = require('../scripts/configure-tv-profile');
var revision = new Array(41).join('a');
var base = 'https://raw.githubusercontent.com/gorhill/uBlock/' + revision + '/src/';
var source = 'https://raw.githubusercontent.com/brave/adblock-resources/master/';
var publicKey = new Buffer('test-default-public-key').toString('base64');
var defaultId = updater.componentId(publicKey);
var catalog = [{
    uuid: 'default', title: 'Brave default filters', default_enabled: true,
    hidden: true, platforms: ['LINUX'],
    list_text_component: {component_id: defaultId, base64_public_key: publicKey},
    sources: [{title: 'Test filters', format: 'Standard', url: 'https://example.test/list.txt'}]
}];
var sources = {};
sources['https://api.github.com/repos/brave/brave-core-crx-packager/' +
    'contents/submodules/uBlock?ref=master'] = JSON.stringify({sha: revision});
sources[base + 'js/resources/scriptlets.js'] =
    "import { ArglistParser as __Parser__ } from '../arglist-parser.js';\n" +
    'export const ArglistParser = __Parser__;\n' +
    'function safeSelf() { return {}; }\n' +
    'function setConstant() { return new ArglistParser(); }\n' +
    "registerScriptlet(safeSelf, { name: 'safe-self.fn' });\n" +
    "registerScriptlet(ArglistParser, { name: 'arglist-parser.fn' });\n" +
    "registerScriptlet(setConstant, { name: 'set-constant.js', " +
    "dependencies: [ArglistParser, safeSelf], aliases: ['set.js'] });";
sources[base + 'js/arglist-parser.js'] = 'export class ArglistParser {}';
sources[base + 'js/redirect-resources.js'] =
    "export default new Map([['noop.mp4', {alias: 'noop-video'}], ['empty', {}]]);";
var binary = new Buffer([0, 255, 254, 128, 0, 10]);
sources[base + 'web_accessible_resources/noop.mp4'] = binary;
sources[base + 'web_accessible_resources/empty'] = new Buffer(0);
sources[source + 'dist/resources.json'] = JSON.stringify([{
    name: 'brave-test.js', aliases: [], kind: {mime: 'application/javascript'},
    content: new Buffer('function braveTest() {}').toString('base64')
}]);
sources[source + 'filter_lists/list_catalog.json'] = JSON.stringify(catalog);
sources['https://example.test/list.txt'] =
    '!#if cap_html_filtering\nblocked-by-preprocessor\n!#else\n||ads.example^\n!#endif';

function fetch(address, callback, isBinary) {
    if (!Object.prototype.hasOwnProperty.call(sources, address)) {
        callback(new Error('Unexpected test URL: ' + address));
        return;
    }
    var value = sources[address];
    assert.strictEqual(Buffer.isBuffer(value), !!isBinary);
    callback(null, value);
}
function generate() {
    var result, failure;
    builder.buildResources(fetch, function (error, text) {
        failure = error;
        result = text;
    });
    if (failure) throw failure;
    assert(result, 'Synchronous fixture callback did not complete');
    return JSON.parse(result);
}
var resources = generate();
assert.strictEqual(resources.length, 6);
var constant = resources.filter(function (entry) { return entry.name === 'set-constant.js'; })[0];
assert.deepStrictEqual(constant.dependencies, ['arglist-parser.fn', 'safe-self.fn']);
resources.filter(function (entry) { return !!entry.dependencies; }).forEach(function (entry) {
    builder.parse('(' + new Buffer(entry.content, 'base64').toString('utf8') + ')');
});
var video = resources.filter(function (entry) { return entry.name === 'noop.mp4'; })[0];
assert.deepStrictEqual(new Buffer(video.content, 'base64'), binary);
assert.deepStrictEqual(video.aliases, ['noop-video']);
assert.strictEqual(resources.filter(function (entry) {
    return entry.name === 'empty';
})[0].content, '');
var parserSource = sources[base + 'js/arglist-parser.js'];
sources[base + 'js/arglist-parser.js'] =
    'const First = Second; const Second = First; export {First as ArglistParser};';
assert.throws(generate, /Cyclic public scriptlet/);
sources[base + 'js/arglist-parser.js'] = parserSource;
assert.strictEqual(builder.preprocess('!#if env_firefox\nno\n!#else\nyes\n!#endif', 'Test'), 'yes');
assert.strictEqual(builder.preprocess('!#if unknown\nfirst\n!#else\nsecond\n!#endif', 'Test'),
    'first\nsecond');
assert.throws(function () { builder.preprocess('!#endif', 'Test'); }, /Unbalanced/);
assert.throws(function () { builder.preprocess('!#if env_chromium', 'Test'); }, /Unterminated/);

var optional = JSON.parse(JSON.stringify(catalog[0]));
optional.uuid = 'optional';
optional.hidden = false;
var preference = {brave: {ad_block: {regional_filters: {optional: {enabled: false}}}}};
assert.strictEqual(updater.selectLists(catalog.concat([optional]), preference).length, 1);
assert.throws(function () {
    var invalid = JSON.parse(JSON.stringify(catalog));
    invalid[0].list_text_component.component_id = new Array(33).join('a');
    updater.selectLists(invalid, {});
}, /Invalid public filter component/);

var temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'brave-adblock-test-'));
try {
    var profile = path.join(temporary, 'profile');
    var cache = path.join(temporary, 'cache');
    fs.mkdirSync(profile);
    fs.mkdirSync(path.join(profile, 'Default'));
    fs.writeFileSync(path.join(profile, 'Local State'), JSON.stringify({keep: 'state'}));
    fs.writeFileSync(path.join(profile, 'Default', 'Preferences'), JSON.stringify({
        keep: 'profile',
        profile: {content_settings: {exceptions: {
            autoplay: {'https://other.example:443,*': {setting: 1}}
        }}}
    }));
    var backup = path.join(temporary, 'profile-backup');
    configure(profile, backup);
    var state = JSON.parse(fs.readFileSync(path.join(profile, 'Local State'), 'utf8'));
    var preferences = JSON.parse(fs.readFileSync(path.join(profile, 'Default', 'Preferences'), 'utf8'));
    assert.strictEqual(state.keep, 'state');
    assert.strictEqual(state.performance_tuning.high_efficiency_mode.state, 2);
    assert.strictEqual(state.performance_tuning.high_efficiency_mode.aggressiveness, 1);
    assert.strictEqual(preferences.keep, 'profile');
    assert.strictEqual(preferences.profile.content_settings.exceptions.autoplay[
        'https://other.example:443,*'].setting, 1);
    assert.strictEqual(preferences.profile.content_settings.exceptions.autoplay[
        'https://www.youtube.com:443,*'].setting, 2);
    assert.deepStrictEqual(JSON.parse(fs.readFileSync(path.join(backup, 'Local State'), 'utf8')),
        {keep: 'state'});
    fs.writeFileSync(path.join(profile, 'SingletonLock'), 'test');
    assert.throws(function () {
        configure(profile, path.join(temporary, 'blocked-backup'));
    }, /Close Brave/);
    fs.unlinkSync(path.join(profile, 'SingletonLock'));
    var downloaded, downloadError;
    updater.fetchBundle(profile, cache, fetch, function (error, result) {
        downloadError = error;
        downloaded = result;
    });
    if (downloadError) throw downloadError;
    assert.strictEqual(downloaded.components, 3);
    var pending = JSON.parse(fs.readFileSync(path.join(cache, 'pending.json'), 'utf8'));
    var stagedList = path.join(cache, pending.bundle, defaultId, pending.version, 'list.txt');
    var original = fs.readFileSync(stagedList, 'utf8');
    assert(original.indexOf('||ads.example^') >= 0);
    assert(original.indexOf('blocked-by-preprocessor') < 0);
    fs.appendFileSync(stagedList, 'corrupt');
    assert.throws(function () { updater.applyBundle(profile, cache); }, /checksum/);
    assert(!fs.existsSync(path.join(profile, defaultId)));
    fs.writeFileSync(stagedList, original);
    var failedProfile = path.join(temporary, 'failed-profile');
    var failedCache = path.join(temporary, 'failed-cache');
    fs.mkdirSync(failedProfile);
    fs.cpSync(cache, failedCache, {recursive: true});
    var rename = fs.renameSync;
    var moves = 0;
    try {
        fs.renameSync = function (from, to) {
            if (to.indexOf(failedProfile + path.sep) === 0 && ++moves === 2) {
                throw new Error('Injected component move failure');
            }
            return rename(from, to);
        };
        assert.throws(function () {
            updater.applyBundle(failedProfile, failedCache);
        }, /Injected component move failure/);
    } finally {
        fs.renameSync = rename;
    }
    pending.components.forEach(function (component) {
        assert(!fs.existsSync(path.join(failedProfile, component.id, pending.version)));
        assert(fs.existsSync(path.join(failedCache, pending.bundle, component.id, pending.version)));
    });
    var installed = updater.applyBundle(profile, cache);
    assert.strictEqual(installed.components, 3);
    assert(!fs.existsSync(path.join(cache, 'pending.json')));
    assert.strictEqual(fs.readFileSync(path.join(profile, defaultId, installed.version, 'list.txt'),
        'utf8'), original);
    updater.fetchBundle(profile, cache, function () {
        throw new Error('Fresh data should not download again');
    }, function (error, result) {
        assert.ifError(error);
        assert.deepStrictEqual(result, {cached: true});
    });
    assert.deepStrictEqual(updater.applyBundle(profile, cache), {pending: false});
} finally {
    fs.rmSync(temporary, {recursive: true});
}
['update-adblock.js', 'adblock-resources.js', 'acorn.js'].forEach(function (name) {
    require('../app/acorn').parse(fs.readFileSync(path.join(__dirname, '../app', name), 'utf8'),
        {ecmaVersion: 5, allowReserved: true});
});
console.log('Ad-block generation, binary redirects, preferences, staging, and integrity tests passed.');
