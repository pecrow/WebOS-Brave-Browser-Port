'use strict';
var acorn = require('./acorn');
var crypto = require('crypto');

var UBO_REF = 'https://api.github.com/repos/brave/brave-core-crx-packager/' +
    'contents/submodules/uBlock?ref=master';
var BRAVE = 'https://raw.githubusercontent.com/brave/adblock-resources/master/dist/resources.json';
var CONDITIONS = {
    ext_ublock: true, ext_devbuild: false, env_devbuild: false,
    env_chromium: true, env_edge: false, env_firefox: false,
    env_legacy: false, env_safari: false, cap_html_filtering: false,
    cap_user_stylesheet: true, 'false': false, ext_abp: false,
    adguard: false, adguard_app_android: false, adguard_app_ios: false,
    adguard_app_mac: false, adguard_app_windows: false,
    adguard_ext_android_cb: false, adguard_ext_chromium: true,
    adguard_ext_edge: false, adguard_ext_firefox: false,
    adguard_ext_opera: true, adguard_ext_safari: false
};

function preprocess(text, title) {
    var stack = [], active = true;
    var result = text.split(/\r?\n/).filter(function (line) {
        var trimmed = line.trim();
        var match = /^!#if (!?)(.*)$/.exec(trimmed);
        if (match) {
            var value = CONDITIONS[match[2]];
            if (value !== undefined && match[1]) value = !value;
            stack.push({parent: active, value: value, otherwise: false});
            active = active && value !== false;
            return false;
        }
        if (trimmed === '!#else' || trimmed === '!#endif') {
            if (!stack.length) throw new Error('Unbalanced public filter condition');
            var frame = stack[stack.length - 1];
            if (trimmed === '!#else') {
                if (frame.otherwise) throw new Error('Duplicate public filter else');
                frame.otherwise = true;
                active = frame.parent && frame.value !== true;
            } else {
                active = stack.pop().parent;
            }
            return false;
        }
        return active && (!/\+js\(brave-/.test(line) ||
            /^Brave /.test(title) || title === 'Experimental ad blocker');
    }).join('\n');
    if (stack.length) throw new Error('Unterminated public filter condition');
    return result;
}

function parse(text) {
    return acorn.parse(text, {ecmaVersion: 'latest', sourceType: 'module'});
}

function properties(node) {
    if (!node || node.type !== 'ObjectExpression') {
        throw new Error('Unsupported public resource metadata');
    }
    var values = {};
    node.properties.forEach(function (property) {
        if (property.type !== 'Property' || property.computed || property.kind !== 'init') {
            throw new Error('Unsupported public resource property');
        }
        values[property.key.name || property.key.value] = property.value;
    });
    return values;
}

function literal(node) {
    if (node.type === 'Literal') return node.value;
    if (node.type === 'ArrayExpression') return node.elements.map(literal);
    throw new Error('Public resource metadata is not a literal');
}

function declarations(module, node) {
    if (!node) return;
    if (node.type === 'FunctionDeclaration' || node.type === 'ClassDeclaration') {
        module.functions[node.id ? node.id.name : 'default'] = module.text.slice(node.start, node.end);
    }
    if (node.type === 'VariableDeclaration') {
        node.declarations.forEach(function (declaration) {
            var value = declaration.init;
            if (declaration.id.type === 'Identifier' && value &&
                (value.type === 'FunctionExpression' || value.type === 'ArrowFunctionExpression' ||
                    value.type === 'ClassExpression')) {
                module.functions[declaration.id.name] =
                    module.text.slice(value.start, value.end);
            } else if (declaration.id.type === 'Identifier' && value &&
                       value.type === 'Identifier') {
                module.imports[declaration.id.name] = {
                    file: module.filename, name: value.name
                };
            }
        });
    }
}

function modulePath(parent, relative) {
    if (!/^\.{1,2}\//.test(relative)) throw new Error('Unsupported public scriptlet import');
    var parts = parent.split('/');
    parts.pop();
    relative.split('/').forEach(function (part) {
        if (part === '..') {
            if (!parts.length) throw new Error('Public scriptlet import escapes its source');
            parts.pop();
        } else if (part !== '.') {
            parts.push(part);
        }
    });
    var result = parts.join('/');
    if (!/^[a-zA-Z0-9_./-]+\.js$/.test(result)) {
        throw new Error('Invalid public scriptlet module path');
    }
    return result;
}

function scriptletResources(modules) {
    function resolve(module, name, depth) {
        if (depth > 20) throw new Error('Cyclic public scriptlet function import');
        if (module.functions[name]) return module.functions[name];
        var imported = module.imports[name];
        if (!imported) throw new Error('Missing public scriptlet function: ' + name);
        return resolve(modules[imported.file], imported.name, depth + 1);
    }
    var registrations = [], functions = {};
    Object.keys(modules).forEach(function (filename) {
        var module = modules[filename];
        module.ast.body.forEach(function (node) {
            if (node.type !== 'ExpressionStatement' ||
                node.expression.type !== 'CallExpression') return;
            var call = node.expression, target = call.callee;
            var registration = target.type === 'Identifier' && target.name === 'registerScriptlet';
            registration = registration || (target.type === 'MemberExpression' &&
                target.property.name === 'push' && target.object.type === 'Identifier' &&
                ['builtinScriptlets', 'registeredScriptlets'].indexOf(target.object.name) >= 0);
            if (!registration) return;
            var direct = target.type === 'Identifier';
            if (call.arguments.length !== (direct ? 2 : 1)) {
                throw new Error('Unsupported scriptlet registration in ' + filename);
            }
            var metadata = properties(call.arguments[direct ? 1 : 0]);
            var fn = direct ? call.arguments[0] : metadata.fn;
            var code;
            if (fn.type === 'Identifier') code = resolve(module, fn.name, 0);
            else if (fn.type === 'FunctionExpression' || fn.type === 'ArrowFunctionExpression' ||
                fn.type === 'ClassExpression') {
                code = module.text.slice(fn.start, fn.end);
            } else throw new Error('Unsupported scriptlet function expression');
            var name = literal(metadata.name);
            functions[crypto.createHash('sha256').update(code).digest('hex')] = name;
            registrations.push({
                module: module, metadata: metadata, code: code, name: name
            });
        });
    });
    return registrations.map(function (entry) {
        var metadata = entry.metadata;
        var dependencies = metadata.dependencies ? metadata.dependencies.elements.map(function (node) {
            if (node.type === 'Literal') return literal(node);
            if (node.type !== 'Identifier') throw new Error('Unsupported public resource dependency');
            var code = resolve(entry.module, node.name, 0);
            var name = functions[crypto.createHash('sha256').update(code).digest('hex')];
            if (!name) throw new Error('Unregistered public resource dependency: ' + node.name);
            return name;
        }) : [];
        return {
                name: entry.name,
                aliases: metadata.aliases ? literal(metadata.aliases) : [],
                kind: {mime: 'application/javascript'},
                content: new Buffer(entry.code, 'utf8').toString('base64'),
                dependencies: dependencies
        };
    });
}

function buildResources(fetch, callback) {
    fetch(UBO_REF, function (error, refText) {
        if (error) return callback(error);
        var base;
        try {
            var revision = JSON.parse(refText).sha;
            if (!/^[0-9a-f]{40}$/.test(revision)) throw new Error('Invalid public uBlock revision');
            base = 'https://raw.githubusercontent.com/gorhill/uBlock/' + revision + '/src/';
        } catch (failure) { callback(failure); return; }
        var modules = {}, count = 0;
        function load(filename, done) {
            if (modules[filename]) { done(); return; }
            if (++count > 120) { done(new Error('Public scriptlet module limit exceeded')); return; }
            fetch(base + 'js/' + filename, function (loadError, text) {
                if (loadError) return done(loadError);
                var module = {
                    text: text, filename: filename, functions: {}, imports: {}
                }, imports = [];
                try {
                    module.ast = parse(text);
                    modules[filename] = module;
                    module.ast.body.forEach(function (node) {
                        declarations(module,
                            node.type === 'ExportNamedDeclaration' ||
                            node.type === 'ExportDefaultDeclaration' ? node.declaration : node);
                        if (node.type === 'ExportDefaultDeclaration') {
                            var declaration = node.declaration;
                            if (declaration.type === 'Identifier') {
                                module.imports['default'] = {file: filename, name: declaration.name};
                            } else {
                                module.functions['default'] = text.slice(declaration.start, declaration.end);
                            }
                        }
                        if (node.type === 'ExportNamedDeclaration' && !node.declaration) {
                            var exportedFile = node.source ?
                                modulePath(filename, node.source.value) : filename;
                            if (node.source) imports.push(exportedFile);
                            node.specifiers.forEach(function (specifier) {
                                if (exportedFile === filename &&
                                    specifier.exported.name === specifier.local.name) return;
                                module.imports[specifier.exported.name] = {
                                    file: exportedFile, name: specifier.local.name
                                };
                            });
                        }
                        if (node.type !== 'ImportDeclaration') return;
                        var file = modulePath(filename, node.source.value);
                        imports.push(file);
                        node.specifiers.forEach(function (specifier) {
                            if (specifier.type === 'ImportNamespaceSpecifier') return;
                            module.imports[specifier.local.name] = {
                                file: file, name: specifier.type === 'ImportDefaultSpecifier' ?
                                    'default' : specifier.imported.name
                            };
                        });
                    });
                } catch (failure) { done(failure); return; }
                var next = 0;
                function advance(importError) {
                    if (importError) { done(importError); return; }
                    if (next === imports.length) { done(); return; }
                    load(imports[next++], advance);
                }
                advance();
            });
        }
        load('resources/scriptlets.js', function (loadError) {
            if (loadError) return callback(loadError);
            var resources;
            try { resources = scriptletResources(modules); }
            catch (failure) { callback(failure); return; }
            fetch(base + 'js/redirect-resources.js', function (redirectError, text) {
                if (redirectError) return callback(redirectError);
                var redirects;
                try {
                    var exported = parse(text).body.filter(function (node) {
                        return node.type === 'ExportDefaultDeclaration';
                    })[0];
                    var map = exported && exported.declaration;
                    if (!map || map.type !== 'NewExpression' || map.callee.name !== 'Map' ||
                        map.arguments.length !== 1 || map.arguments[0].type !== 'ArrayExpression') {
                        throw new Error('Unsupported public redirect resource table');
                    }
                    redirects = map.arguments[0].elements.map(function (node) {
                        var name = literal(node.elements[0]);
                        var settings = properties(node.elements[1]);
                        return {name: name, aliases: settings.alias ? literal(settings.alias) : []};
                    }).filter(function (item) { return item.name !== 'google-ima-dai.js'; });
                } catch (failure) { callback(failure); return; }
                var index = 0;
                function nextRedirect(resourceError) {
                    if (resourceError) return callback(resourceError);
                    if (index === redirects.length) { finish(); return; }
                    var entry = redirects[index++];
                    if (!/^[a-zA-Z0-9_.-]+$/.test(entry.name)) {
                        callback(new Error('Invalid redirect resource filename')); return;
                    }
                    fetch(base + 'web_accessible_resources/' + entry.name, function (fileError, content) {
                        if (fileError) return callback(fileError);
                        var extension = entry.name.split('.').pop();
                        var mime = {js: 'application/javascript', json: 'application/json',
                            css: 'text/css', html: 'text/html', txt: 'text/plain',
                            xml: 'text/xml', gif: 'image/gif', png: 'image/png',
                            mp3: 'audio/mpeg', mp4: 'video/mp4', empty: 'text/plain'}[extension];
                        if (!mime) { callback(new Error('Unsupported redirect resource MIME type')); return; }
                        resources.push({
                            name: entry.name,
                            aliases: typeof entry.aliases === 'string' ? [entry.aliases] : entry.aliases,
                            kind: {mime: mime}, content: new Buffer(content).toString('base64')
                        });
                        nextRedirect();
                    }, true);
                }
                function finish() {
                    fetch(BRAVE, function (braveError, braveText) {
                        if (braveError) return callback(braveError);
                        try {
                            var brave = JSON.parse(braveText);
                            if (!Array.isArray(brave) || !brave.length) throw new Error('Empty Brave resources');
                            resources = resources.concat(brave);
                            var names = {};
                            resources.forEach(function (entry) {
                                if (!entry.name || names[entry.name]) throw new Error('Duplicate public resource');
                                names[entry.name] = true;
                            });
                            resources.forEach(function (entry) {
                                (entry.dependencies || []).forEach(function (dependency) {
                                    if (!names[dependency]) throw new Error('Missing public resource dependency');
                                });
                            });
                            if (!names['safe-self.fn'] || !names['set-constant.js']) {
                                throw new Error('Public uBlock scriptlet library is incomplete');
                            }
                            callback(null, JSON.stringify(resources), {uBlockRevision: revision});
                        } catch (failure) { callback(failure); }
                    });
                }
                nextRedirect();
            });
        });
    });
}

module.exports = {
    preprocess: preprocess,
    buildResources: buildResources,
    scriptletResources: scriptletResources,
    parse: parse,
    declarations: declarations
};
