'use strict';
var fs = require('fs');
var path = require('path');
var applyDefaults = require('../app/profile-defaults');

function exists(name) {
    try {
        fs.lstatSync(name);
        return true;
    } catch (error) {
        if (error.code !== 'ENOENT') throw error;
        return false;
    }
}

function configure(profile, backup) {
    if (exists(path.join(profile, 'SingletonLock'))) {
        throw new Error('Close Brave before configuring its profile');
    }
    var stateFile = path.join(profile, 'Local State');
    var preferencesFile = path.join(profile, 'Default', 'Preferences');
    var files = [stateFile, preferencesFile].map(function (name) {
        var stat = fs.lstatSync(name);
        if (!stat.isFile() || stat.isSymbolicLink()) {
            throw new Error('Unexpected browser preference file');
        }
        var bytes = fs.readFileSync(name);
        return {name: name, stat: stat, bytes: bytes, value: JSON.parse(bytes)};
    });
    var state = files[0].value;
    var preferences = files[1].value;
    applyDefaults(state, preferences);
    fs.mkdirSync(backup, 448);
    files.forEach(function (file) {
        fs.writeFileSync(path.join(backup, path.basename(file.name)), file.bytes,
            {flag: 'wx', mode: 384});
    });
    files.forEach(function (file) {
        var temporary = file.name + '.memory-tuning-' + process.pid;
        fs.writeFileSync(temporary, JSON.stringify(file.value), {flag: 'wx', mode: 384});
        if (process.platform !== 'win32') {
            fs.chownSync(temporary, file.stat.uid, file.stat.gid);
            fs.chmodSync(temporary, file.stat.mode & 511);
        }
        fs.renameSync(temporary, file.name);
    });
    return {memorySaver: 'enabled', aggressiveness: 'medium', youtubeAutoplay: 'blocked'};
}

module.exports = configure;
if (require.main === module) {
    if (process.argv.length !== 4) {
        throw new Error('Usage: configure-tv-profile.js PROFILE NEW_BACKUP_DIRECTORY');
    }
    console.log(JSON.stringify(configure(process.argv[2], process.argv[3])));
}
