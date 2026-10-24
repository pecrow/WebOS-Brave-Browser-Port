'use strict';
var fs = require('fs');
var path = require('path');
var defaults = require('../app/profile-defaults');
var updater = require('../app/update-adblock');

if (process.argv.length !== 3) {
    throw new Error('Usage: prepare-profile.js NEW_DEFAULTS_DIRECTORY');
}
var root = process.argv[2];
fs.mkdirSync(root);
var profile = path.join(root, 'profile');
var cache = path.join(root, 'adblock-cache');
fs.mkdirSync(profile);
fs.mkdirSync(path.join(profile, 'Default'));
var state = {}, preferences = {};
defaults(state, preferences);
fs.writeFileSync(path.join(profile, 'Local State'), JSON.stringify(state), {flag: 'wx'});
fs.writeFileSync(path.join(profile, 'Default', 'Preferences'),
    JSON.stringify(preferences), {flag: 'wx'});
updater.fetchBundle(profile, cache, updater.download, function (error) {
    if (error) {
        console.error('Bundled profile preparation failed: ' + error.message);
        process.exitCode = 1;
        return;
    }
    console.log('Bundled public filters: ' +
        JSON.stringify(updater.applyBundle(profile, cache)));
});
