'use strict';
var fs = require('fs');
var path = require('path');
var crypto = require('crypto');
var childProcess = require('child_process');
var url = require('url');
var resourceBuilder = require('./adblock-resources');

var SOURCE = 'https://raw.githubusercontent.com/brave/adblock-resources/master/';
var CATALOG_ID = 'gkboaolpopklhgplhaaiboijnklogmbc';
var RESOURCES_ID = 'mfddibmblmbccpadfndgakiopmmhebop';
var DAY = 86400000;
var DEFAULT_LISTS = [
    'AC023D22-AE88-4060-A978-4FEEEC4221693',
    '2F3DCE16-A19A-493C-A88F-2E110FBD37D6',
    'A5E6EC21-F01F-4547-9F0A-1EE1C3F2AE8D',
    '84960ADD-1CC1-419F-81FC-9F116F5205CC',
    'FB626316-6CC8-4447-884B-F5A37C29B0AE'
];

function hash(data) {
    return crypto.createHash('sha256').update(data).digest('hex');
}

function directory(name) {
    if (!fs.existsSync(name)) {
        fs.mkdirSync(name, 448);
    }
    var stat = fs.lstatSync(name);
    if (!stat.isDirectory() || stat.isSymbolicLink()) {
        throw new Error('Unsafe ad-block update directory');
    }
}

function readJson(name, missing) {
    if (!fs.existsSync(name) && missing !== undefined) return missing;
    return JSON.parse(fs.readFileSync(name, 'utf8'));
}

function atomicJson(name, data) {
    var temporary = name + '.new-' + process.pid;
    fs.writeFileSync(temporary, JSON.stringify(data), {flag: 'wx', mode: 384});
    fs.renameSync(temporary, name);
}

function validateUrl(address) {
    var parsed = url.parse(address);
    if (parsed.protocol !== 'https:' || parsed.auth || parsed.port ||
        !parsed.hostname || parsed.hostname.indexOf('.') < 0 ||
        /^[0-9.]+$/.test(parsed.hostname) ||
        /(^|\.)(localhost|local)$/.test(parsed.hostname)) {
        throw new Error('Unsupported public filter source URL');
    }
}

function download(address, callback, binary) {
    validateUrl(address);
    var executable = process.platform === 'win32' ? 'curl.exe' : '/usr/bin/curl';
    childProcess.execFile(executable, [
        '--fail', '--silent', '--show-error', '--location',
        '--proto', '=https', '--proto-redir', '=https',
        '--connect-timeout', '5', '--max-time', '20', address
    ], {timeout: 25000, maxBuffer: 16 * 1024 * 1024, encoding: binary ? null : 'utf8'},
    function (error, stdout, stderr) {
        if (error) {
            callback(new Error('Public filter download failed for ' +
                url.parse(address).hostname + ': ' + (stderr || error.message).trim()));
            return;
        }
        if ((!binary && !stdout) || (!binary && /^\s*(<!doctype|<html)/i.test(stdout))) {
            callback(new Error('Public filter source returned an invalid document'));
            return;
        }
        callback(null, stdout);
    });
}

function componentId(publicKey) {
    return hash(new Buffer(publicKey, 'base64')).slice(0, 32)
        .replace(/[0-9a-f]/g, function (digit) {
            return String.fromCharCode(97 + parseInt(digit, 16));
        });
}

function selectLists(catalog, state) {
    if (!Array.isArray(catalog) || !catalog.length) {
        throw new Error('Public filter catalog is empty');
    }
    var brave = state.brave || {};
    var settings = brave.ad_block && brave.ad_block.regional_filters || {};
    var locale = state.intl && state.intl.app_locale || 'en-US';
    var language = locale.toLowerCase().split('-')[0];
    var selected = catalog.filter(function (entry) {
        if (entry.platforms && entry.platforms.length &&
            entry.platforms.indexOf('LINUX') < 0) return false;
        var preference = settings[entry.uuid];
        if (entry.default_enabled && entry.hidden) return true;
        if (preference) return preference.enabled === true;
        return entry.default_enabled ||
            DEFAULT_LISTS.indexOf(entry.uuid) >= 0 ||
            (entry.langs || []).some(function (lang) {
                return lang.toLowerCase().split('-')[0] === language;
            });
    });
    if (!selected.some(function (entry) { return entry.uuid === 'default'; })) {
        throw new Error('Public catalog does not include the default Brave filters');
    }
    selected.forEach(function (entry) {
        var component = entry.list_text_component;
        if (!component || !/^[a-p]{32}$/.test(component.component_id) ||
            componentId(component.base64_public_key) !== component.component_id ||
            !Array.isArray(entry.sources) || !entry.sources.length) {
            throw new Error('Invalid public filter component metadata');
        }
        entry.sources.forEach(function (source) {
            validateUrl(source.url);
            if (source.format !== 'Standard') {
                throw new Error('Unsupported public filter source format');
            }
        });
    });
    return selected;
}

function fetchBundle(profile, cache, fetch, callback) {
    directory(cache);
    var state = readJson(path.join(profile, 'Local State'), {});
    var selection = hash(JSON.stringify({
        resourceSchema: 2,
        locale: state.intl && state.intl.app_locale || 'en-US',
        lists: state.brave && state.brave.ad_block &&
            state.brave.ad_block.regional_filters || {}
    }));
    var pending = readJson(path.join(cache, 'pending.json'), null);
    var previous = pending || readJson(path.join(cache, 'installed.json'), null);
    if (previous && (typeof previous.createdAt !== 'number' ||
                    !isFinite(previous.createdAt) || previous.createdAt < 0)) {
        throw new Error('Invalid previous ad-block update timestamp');
    }
    if (previous && previous.schema === 2 && previous.selection === selection &&
        Date.now() >= previous.createdAt && Date.now() - previous.createdAt < DAY) {
        callback(null, {cached: true});
        return;
    }
    fetch(SOURCE + 'filter_lists/list_catalog.json', function (error, catalogText) {
        if (error) return callback(error);
        var catalog, selected;
        try {
            catalog = JSON.parse(catalogText);
            selected = selectLists(catalog, state);
        } catch (failure) {
            callback(failure);
            return;
        }
        resourceBuilder.buildResources(fetch, function (resourceError, resourcesText, provenance) {
            if (resourceError) return callback(resourceError);
            var stage, record;
            try {
                var resources = JSON.parse(resourcesText);
                if (!Array.isArray(resources) || !resources.length) {
                    throw new Error('Public scriptlet resource library is empty');
                }
                var now = Math.max(Date.now(), previous ? previous.createdAt + 1 : 0);
                var version = '1.' + Math.floor(now / DAY) + '.' +
                    Math.floor(now / 1000) % 86400 + '.' + now % 1000;
                var bundle = 'bundle-' + now + '-' + crypto.randomBytes(6).toString('hex');
                stage = path.join(cache, bundle);
                directory(stage);
                record = {
                    schema: 2, bundle: bundle, version: version, createdAt: now,
                    selection: selection, sources: provenance, components: []
                };
                addComponent(RESOURCES_ID, 'Brave Ad Block Resources Library',
                    'resources.json', resourcesText);
            } catch (failure) {
                callback(failure);
                return;
            }
            var index = 0;
            function addComponent(id, name, filename, text) {
                directory(path.join(stage, id));
                var destination = path.join(stage, id, record.version);
                directory(destination);
                var files = {};
                files[filename] = text;
                files['manifest.json'] = JSON.stringify({
                    manifest_version: 2, name: name, version: record.version
                });
                var checks = [];
                Object.keys(files).forEach(function (file) {
                    var content = new Buffer(files[file], 'utf8');
                    fs.writeFileSync(path.join(destination, file), content,
                        {flag: 'wx', mode: 384});
                    checks.push({name: file, sha256: hash(content)});
                });
                record.components.push({id: id, files: checks});
            }
            function nextList() {
                if (index === selected.length) {
                    try {
                        addComponent(CATALOG_ID, 'Brave Ad Block List Catalog',
                            'list_catalog.json', catalogText);
                        atomicJson(path.join(cache, 'pending.json'), record);
                        callback(null, {components: record.components.length});
                    } catch (failure) {
                        callback(failure);
                    }
                    return;
                }
                var entry = selected[index++];
                var sourceIndex = 0;
                var pieces = [];
                function nextSource() {
                    if (sourceIndex === entry.sources.length) {
                        try {
                            addComponent(entry.list_text_component.component_id,
                                entry.title, 'list.txt', pieces.join('\n\n') + '\n');
                            nextList();
                        } catch (failure) {
                            callback(failure);
                        }
                        return;
                    }
                    var source = entry.sources[sourceIndex++];
                    fetch(source.url, function (sourceError, content) {
                        if (sourceError) return callback(sourceError);
                        try {
                            pieces.push('! Source: ' + source.title + '\n' +
                                resourceBuilder.preprocess(content, entry.title));
                        } catch (failure) {
                            callback(failure);
                            return;
                        }
                        nextSource();
                    });
                }
                nextSource();
            }
            nextList();
        });
    });
}

function applyBundle(profile, cache) {
    directory(profile);
    directory(cache);
    var pointer = path.join(cache, 'pending.json');
    var record = readJson(pointer, null);
    if (!record) return {pending: false};
    if (record.schema !== 2 ||
        !/^bundle-[0-9]+-[0-9a-f]{12}$/.test(record.bundle) ||
        !/^[0-9]+(\.[0-9]+){3}$/.test(record.version) ||
        !Array.isArray(record.components) || record.components.length < 3) {
        throw new Error('Invalid staged ad-block update');
    }
    var stage = path.join(cache, record.bundle);
    var componentIds = {};
    record.components.forEach(function (component) {
        if (!/^[a-p]{32}$/.test(component.id) ||
            componentIds[component.id] ||
            !Array.isArray(component.files) || component.files.length !== 2) {
            throw new Error('Invalid staged filter component');
        }
        componentIds[component.id] = true;
        var source = path.join(stage, component.id, record.version);
        var target = path.join(profile, component.id, record.version);
        var location = fs.existsSync(target) ? target : source;
        component.files.forEach(function (file) {
            if (!/^(manifest\.json|resources\.json|list_catalog\.json|list\.txt)$/.test(file.name) ||
                !/^[0-9a-f]{64}$/.test(file.sha256) ||
                hash(fs.readFileSync(path.join(location, file.name))) !== file.sha256) {
                throw new Error('Staged ad-block checksum verification failed');
            }
        });
    });
    if (!componentIds[CATALOG_ID] || !componentIds[RESOURCES_ID]) {
        throw new Error('Staged ad-block update is missing required components');
    }
    var moved = [];
    try {
        record.components.forEach(function (component) {
            var parent = path.join(profile, component.id);
            directory(parent);
            var source = path.join(stage, component.id, record.version);
            var target = path.join(parent, record.version);
            if (!fs.existsSync(target)) {
                fs.renameSync(source, target);
                moved.push({source: source, target: target});
            }
        });
        atomicJson(path.join(cache, 'installed.json'), record);
    } catch (failure) {
        var rollbackErrors = [];
        moved.reverse().forEach(function (move) {
            try { fs.renameSync(move.target, move.source); }
            catch (rollbackError) { rollbackErrors.push(rollbackError.message); }
        });
        if (rollbackErrors.length) {
            throw new Error(failure.message + '; rollback failed: ' + rollbackErrors.join('; '));
        }
        throw failure;
    }
    var latest = readJson(pointer, null);
    if (latest && latest.bundle === record.bundle) fs.unlinkSync(pointer);
    return {components: record.components.length, version: record.version};
}

module.exports = {
    download: download,
    componentId: componentId,
    selectLists: selectLists,
    fetchBundle: fetchBundle,
    applyBundle: applyBundle
};

if (require.main === module) {
    var mode = process.argv[2], profile = process.argv[3], cache = process.argv[4];
    if (!profile || !cache || (mode !== 'fetch' && mode !== 'apply')) {
        throw new Error('Usage: update-adblock.js fetch|apply PROFILE CACHE');
    }
    if (mode === 'apply') {
        console.log('Ad-block update: ' + JSON.stringify(applyBundle(profile, cache)));
    } else {
        fetchBundle(profile, cache, function (address, callback, binary) {
            try {
                fs.lstatSync(path.join(profile, 'SingletonLock'));
            } catch (error) {
                if (error.code !== 'ENOENT') { callback(error); return; }
                download(address, callback, binary);
                return;
            }
            callback(new Error('Browser is open; ad-block download deferred until its next close'));
        }, function (error, result) {
            if (error) {
                console.error('Ad-block update failed: ' + error.message);
                process.exitCode = 1;
                return;
            }
            console.log('Ad-block download: ' + JSON.stringify(result));
        });
    }
}
