#!/usr/bin/ucode
'use strict';

const fs = require('fs');

const DB = '/etc/hh500v-sms.txt';
const HASHES = '/etc/hh500v-sms-hashes';

function read_rows() {
	let raw = '';
	let rows = [];
	let n = 0;

	try { raw = fs.readfile(DB) || ''; } catch (e) { raw = ''; }

	for (let line in split(raw, '\n')) {
		if (!line)
			continue;
		let p = split(line, '|');
		if (length(p) < 5)
			continue;
		/* skip rows an early parse left behind: no sender and no text */
		if ((p[2] ?? '') == '' && trim(join('|', slice(p, 4))) == '')
			continue;
		n++;
		push(rows, {
			id: n,
			epoch: +p[0],
			index: +p[1],
			sender: p[2],
			when: p[3],
			text: join('|', slice(p, 4))
		});
	}
	return rows;
}

let action = ARGV[0] ? ARGV[0] : 'list';

if (action == 'list') {
	let rows = read_rows();
	printf('%J', { total: length(rows), messages: rows });
	exit(0);
}

if (action == 'clear') {
	fs.writefile(DB, '');
	try { fs.unlink(HASHES); } catch (e) { }
	printf('%J', { ok: true, total: 0 });
	exit(0);
}

if (action == 'delete') {
	let want = +ARGV[1];
	let rows = read_rows();
	let keep = [];
	let out = '';

	for (let r in rows)
		if (r.id != want)
			push(keep, r);

	for (let r in keep)
		out += r.epoch + '|' + r.index + '|' + r.sender + '|' + r.when + '|' + r.text + '\n';

	fs.writefile(DB, out);
	printf('%J', { ok: true, total: length(keep) });
	exit(0);
}

printf('%J', { error: 'unknown action' });
