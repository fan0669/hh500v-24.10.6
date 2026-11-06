#!/usr/bin/ucode
/* hh500v-ca.uc - JSON for the "HH500V 载波" LuCI page.
 *
 * Data sources (both written by hh500v-sig-cache.sh during the boot AT window,
 * the only moment this GM800 answers AT):
 *   /etc/hh500v-cells.txt    raw AT+ZCELLINFO? reply, one line per camped cell
 *   /etc/hh500v-modem-cfg    key=value cache (band/arfcn/pci/rsrp/..., zsnt)
 *
 * Nothing here talks to the modem: AT is dead once the MBIM session is up, so
 * the page shows the cached snapshot of the most recent boot.
 */

'use strict';

import { readfile } from 'fs';

const CELLS  = '/etc/hh500v-cells.txt';
const CFG    = '/etc/hh500v-modem-cfg';
const PUSHED = '/etc/hh500v-netmode-pushed';

function kv(path, key) {
	let raw = readfile(path);
	if (!raw)
		return '';

	let want = key + '=', val = '';
	let lines = split(raw, '\n');

	for (let i = 0; i < length(lines); i++) {
		let line = trim(replace(lines[i], '\r', ''));
		if (substr(line, 0, length(want)) == want)
			val = substr(line, length(want));
	}
	return val;
}

function cell_lines() {
	let raw = readfile(CELLS), out = [], lte = 0, nr = 0;

	if (!raw)
		return { lines: out, lte: 0, nr: 0 };

	let rows = split(raw, '\n');

	for (let i = 0; i < length(rows); i++) {
		let line = trim(replace(rows[i], '\r', ''));
		if (!length(line))
			continue;
		push(out, line);
		if (match(line, /NR5G/i))
			nr++;
		else if (match(line, /LTE/i))
			lte++;
	}
	return { lines: out, lte: lte, nr: nr };
}

let c = cell_lines();
let zsnt = kv(CFG, 'zsnt');
let act = kv(CFG, 'act');
let head = substr(zsnt, 0, 2);

let verdict, verdict_class;

if (c.nr >= 2) {
	verdict = 'NR 载波聚合已启用（NR 侧看到 ' + c.nr + ' 个小区）';
	verdict_class = 'success';
}
else if (c.lte >= 2) {
	verdict = 'LTE 载波聚合已启用（LTE 侧看到 ' + c.lte + ' 个小区）';
	verdict_class = 'success';
}
else if (c.lte >= 1 && c.nr >= 1) {
	verdict = 'EN-DC（NSA）已启用：LTE 锚点 + NR 双连接';
	verdict_class = 'success';
}
else if (c.nr == 1 || c.lte == 1) {
	verdict = '单载波：当前没有聚合（基站只给了 1 个载波）';
	verdict_class = 'info';
}
else {
	verdict = '还没有小区缓存：下次开机、模组驻网后会自动写入';
	verdict_class = 'warning';
}

let lock_lte = kv(CFG, 'locked_lte');
let lock_nr = kv(CFG, 'locked_nr');
let lock_wcdma = kv(CFG, 'locked_wcdma');
let locked_any = length(lock_lte) + length(lock_nr) + length(lock_wcdma);

let mode_ok = (head == '7,' || head == '8,' || head == '0,');
let mode_text;

if (head == '7,')
	mode_text = '仅 5G：SA 的 NR 聚合可用，但 NSA(EN-DC) 与 LTE CA 被禁';
else if (head == '8,' || head == '0,')
	mode_text = '允许 LTE+NR：SA、NSA(EN-DC)、LTE CA 都能用';
else
	mode_text = '未设置或未知：' + (zsnt || '-');

let readiness = [
	{
		label: '频段锁',
		ok: (locked_any == 0),
		detail: (locked_any == 0)
			? '无锁，辅载波不会被我们自己挡住'
			: 'LTE=' + (lock_lte || '-') + ' NR=' + (lock_nr || '-') +
			  ' WCDMA=' + (lock_wcdma || '-') + '（可用 hh500v-band.sh clear all 清掉）'
	},
	{
		label: '网络模式',
		ok: mode_ok,
		detail: mode_text
	},
	{
		label: '小区采集',
		ok: (length(c.lines) > 0),
		detail: (length(c.lines) > 0)
			? '已记录 ' + length(c.lines) + ' 行（/etc/hh500v-cells.txt）'
			: '还没有，下次开机 AT 窗口自动写入'
	},
	{
		label: 'CA 禁用开关',
		ok: true,
		detail: '本固件没有此开关，AT+QCFG="nr5g_disable_ca" 回 0（未禁用）'
	}
];

printf('%J\n', {
	has_data: (length(c.lines) > 0),
	cells_lte: c.lte,
	cells_nr: c.nr,
	cells: c.lines,
	act: act,
	band: kv(CFG, 'band'),
	arfcn: kv(CFG, 'earfcn'),
	pci: kv(CFG, 'pci'),
	rsrp: kv(CFG, 'rsrp'),
	rsrq: kv(CFG, 'rsrq'),
	sinr: kv(CFG, 'sinr'),
	tac: kv(CFG, 'tac'),
	csq: kv(CFG, 'csq'),
	reg: kv(CFG, 'reg'),
	zsnt: zsnt,
	zsnt_pushed: kv(PUSHED, 'zsnt'),
	verdict: verdict,
	verdict_class: verdict_class,
	readiness: readiness
});
