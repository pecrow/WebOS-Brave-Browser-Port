'use strict';
var assert = require('assert');
var fs = require('fs');
var os = require('os');
var path = require('path');
var acorn = require('acorn');
var applyDefaults = require('../app/profile-defaults');
var initialize = require('../app/initialize-profile');
var root = fs.mkdtempSync(path.join(os.tmpdir(), 'brave-profile-'));

try {
    var seed = path.join(root, 'seed');
    fs.mkdirSync(seed);
    fs.mkdirSync(path.join(seed, 'Default'));
    var state = {}, preferences = {};
    applyDefaults(state, preferences);
    fs.writeFileSync(path.join(seed, 'Local State'), JSON.stringify(state));
    fs.writeFileSync(path.join(seed, 'Default', 'Preferences'), JSON.stringify(preferences));
    fs.mkdirSync(path.join(seed, 'filter-component'));
    fs.writeFileSync(path.join(seed, 'filter-component', 'list.txt'), '||ads.example^\n');
    var profile = path.join(root, 'profile');
    assert.deepStrictEqual(initialize(profile, seed), {created: true});
    var actualState = JSON.parse(fs.readFileSync(path.join(profile, 'Local State'), 'utf8'));
    assert.strictEqual(actualState.performance_tuning.high_efficiency_mode.state, 2);
    assert.strictEqual(actualState.performance_tuning.high_efficiency_mode.aggressiveness, 1);
    var actualPreferences = JSON.parse(
        fs.readFileSync(path.join(profile, 'Default', 'Preferences'), 'utf8'));
    assert.strictEqual(actualPreferences.profile.content_settings.exceptions.autoplay[
        'https://www.youtube.com:443,*'].setting, 2);
    assert.strictEqual(fs.readFileSync(path.join(profile, 'filter-component', 'list.txt'),
        'utf8'), '||ads.example^\n');
    if (process.platform !== 'win32') {
        assert.strictEqual(fs.statSync(profile).mode & 511, 448);
        assert.strictEqual(fs.statSync(path.join(profile, 'Local State')).mode & 511, 384);
    }
    fs.writeFileSync(path.join(profile, 'Local State'), '{"keep":"user changes"}');
    assert.deepStrictEqual(initialize(profile, seed), {created: false});
    assert.strictEqual(fs.readFileSync(path.join(profile, 'Local State'), 'utf8'),
        '{"keep":"user changes"}');

    fs.writeFileSync(path.join(seed, 'Default', 'Preferences'), '{broken');
    var invalid = path.join(root, 'invalid');
    assert.throws(function () { initialize(invalid, seed); }, SyntaxError);
    assert.strictEqual(fs.existsSync(invalid), false);
    fs.writeFileSync(path.join(seed, 'Default', 'Preferences'), '{}');
    if (process.platform !== 'win32') {
        var linked = path.join(root, 'linked');
        fs.symlinkSync(profile, linked);
        assert.throws(function () { initialize(linked, seed); }, /regular directory/);
        fs.symlinkSync(path.join(root, 'outside'), path.join(seed, 'unsafe'));
        var unsafe = path.join(root, 'unsafe');
        assert.throws(function () { initialize(unsafe, seed); }, /Unsupported file/);
        assert.strictEqual(fs.existsSync(unsafe), false);
    }
    ['initialize-profile.js', 'profile-defaults.js'].forEach(function (name) {
        acorn.parse(fs.readFileSync(path.join(__dirname, '..', 'app', name), 'utf8'),
            {ecmaVersion: 5});
    });
    console.log('Fresh-profile defaults, bundled filters, preservation, and ES5 checks passed.');
} finally {
    fs.rmSync(root, {recursive: true});
}
