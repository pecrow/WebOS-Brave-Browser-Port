'use strict';
var fs = require('fs');
var path = require('path');

function exists(name) {
    try {
        return fs.lstatSync(name);
    } catch (error) {
        if (error.code !== 'ENOENT') throw error;
        return null;
    }
}

function copy(source, destination) {
    var stat = fs.lstatSync(source);
    if (stat.isDirectory() && !stat.isSymbolicLink()) {
        fs.mkdirSync(destination, 448);
        fs.readdirSync(source).forEach(function (name) {
            copy(path.join(source, name), path.join(destination, name));
        });
    } else if (stat.isFile() && !stat.isSymbolicLink()) {
        fs.writeFileSync(destination, fs.readFileSync(source), {flag: 'wx', mode: 384});
    } else {
        throw new Error('Unsupported file in bundled profile defaults: ' + source);
    }
}

function initialize(profile, defaults) {
    var current = exists(profile);
    if (current) {
        if (!current.isDirectory() || current.isSymbolicLink()) {
            throw new Error('Browser profile is not a regular directory');
        }
        return {created: false};
    }
    ['Local State', path.join('Default', 'Preferences')].forEach(function (name) {
        var file = path.join(defaults, name);
        var stat = fs.lstatSync(file);
        if (!stat.isFile() || stat.isSymbolicLink()) {
            throw new Error('Bundled profile defaults are incomplete');
        }
        JSON.parse(fs.readFileSync(file, 'utf8'));
    });
    var temporary = profile + '.initializing-' + process.pid;
    copy(defaults, temporary);
    fs.renameSync(temporary, profile);
    return {created: true};
}

module.exports = initialize;
if (require.main === module) {
    if (process.argv.length !== 4) {
        throw new Error('Usage: initialize-profile.js PROFILE DEFAULTS');
    }
    console.log('Browser profile: ' + JSON.stringify(
        initialize(process.argv[2], process.argv[3])));
}
