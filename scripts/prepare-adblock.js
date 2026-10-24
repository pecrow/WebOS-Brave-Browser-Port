'use strict';
var fs = require('fs');
var path = require('path');
var root = path.resolve(__dirname, '..');
fs.copyFileSync(path.join(root, 'node_modules', 'acorn', 'dist', 'acorn.js'),
    path.join(root, 'app', 'acorn.js'));
fs.copyFileSync(path.join(root, 'node_modules', 'acorn', 'LICENSE'),
    path.join(root, 'app', 'acorn.LICENSE'));
