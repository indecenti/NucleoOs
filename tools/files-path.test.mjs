// Unit tests for the Files browser path helpers (web/device/apps/files.js),
// the navigation logic mirrored by firmware app_files.cpp (go_up / descend).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { joinPath, parentPath } from '../web/device/apps/files.js';

test('descend from root', () => assert.equal(joinPath('/', 'data'), '/data/'));
test('descend nested', () => assert.equal(joinPath('/data/', 'Music'), '/data/Music/'));
test('descend tolerates missing trailing slash', () => assert.equal(joinPath('/data', 'Music'), '/data/Music/'));

test('parent of root is root', () => assert.equal(parentPath('/'), '/'));
test('parent of first level is root', () => assert.equal(parentPath('/data/'), '/'));
test('parent of nested', () => assert.equal(parentPath('/data/Music/'), '/data/'));
test('parent tolerates missing trailing slash', () => assert.equal(parentPath('/data/Music'), '/data/'));
test('descend then up round-trips', () => assert.equal(parentPath(joinPath('/data/', 'Pictures')), '/data/'));
